/* Copyright (c) 2015 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

/**
    Three Wire Packet types (From BLUETOOTH SPECIFICATION V4.2 [Vol 4, Part D], 8.X

    |Type  | Name            | Byte pattern
    -------+-----------------+--------------------------------
    | 15   | SYNC MESSAGE    | 0x01 0x7e
    | 15   | SYNC RESPONSE   | 0x02 0x7d
    | 15   | CONFIG MESSAGE  | 0x03 0xfc CONFIGURATION_FIELD
    | 15   | CONFIG RESPONSE | 0x04 0x7b CONFIGURATION_FIELD
    | 15   | WAKEUP MESSAGE  | 0x05 0xfa
    | 15   | WOKEN MESSAGE   | 0x06 0xf9
    | 15   | SLEEP MESSAGE   | 0x07 0x78
*/

#include "h5_transport.h"
#include "sd_rpc_types.h"
#include "nrf_error.h"

#include "h5.h"
#include "slip.h"

#include <stdint.h>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>
#include <map>

#include <exception>

// Constants use for state machine states UNINITIALIZED and INITIALIZED
const auto NON_ACTIVE_STATE_TIMEOUT = std::chrono::milliseconds(250);  // Duration to wait until resending a packet
const uint8_t PACKET_RETRANSMISSIONS = 4;                              // Number of times to send reliable packets before giving in

// Other constants
const auto OPEN_WAIT_TIMEOUT = std::chrono::milliseconds(2000);   // Duration to wait for state ACTIVE after open is called
const auto RESET_WAIT_DURATION = std::chrono::milliseconds(300);  // Duration to wait before continuing UART communication after reset is sent to target


#pragma region Public methods
H5Transport::H5Transport(Transport *_nextTransportLayer, uint32_t retransmission_timeout)
    : Transport(),
    seqNum(0), ackNum(0), c0Found(false),
    unprocessedData(), incomingPacketCount(0), outgoingPacketCount(0),
    errorPacketCount(0), currentState(STATE_START), stateMachineThread(nullptr)
{
    this->nextTransportLayer = _nextTransportLayer;
    retransmissionTimeout = std::chrono::milliseconds(retransmission_timeout);

    setupStateMachine();
}

H5Transport::~H5Transport()
{
    delete nextTransportLayer;
}

uint32_t H5Transport::open(status_cb_t status_callback, data_cb_t data_callback, log_cb_t log_callback)
{
    if (currentState != STATE_START)
    {
        log("Not able to open, current state is not valid");
        return NRF_ERROR_INTERNAL;
    }

    startStateMachine();
    auto _exitCriterias = dynamic_cast<StartExitCriterias*>(exitCriterias[STATE_START]);

    auto errorCode = Transport::open(status_callback, data_callback, log_callback);
    lastPacket.clear();

    if (errorCode != NRF_SUCCESS)
    {
        _exitCriterias->ioResourceError = true;
        syncWaitCondition.notify_all();
        return errorCode;
    }

    status_callback = std::bind(&H5Transport::statusHandler, this, std::placeholders::_1, std::placeholders::_2);
    data_callback = std::bind(&H5Transport::dataHandler, this, std::placeholders::_1, std::placeholders::_2);
    errorCode = nextTransportLayer->open(status_callback, data_callback, log_callback);

    if (errorCode != NRF_SUCCESS)
    {
        _exitCriterias->ioResourceError = true;
        syncWaitCondition.notify_all();
        return NRF_ERROR_INTERNAL;
    }

    _exitCriterias->isOpened = true;
    syncWaitCondition.notify_all();

    if (waitForState(STATE_ACTIVE, OPEN_WAIT_TIMEOUT))
    {
        return NRF_SUCCESS;
    }
    else
    {
        return NRF_ERROR_TIMEOUT;
    }
}

uint32_t H5Transport::close()
{
    exitCriterias[currentState]->close = true;
    stopStateMachine();

    auto errorCode1 = nextTransportLayer->close();
    auto errorCode2 = Transport::close();

    if (errorCode1 != NRF_SUCCESS)
    {
        return errorCode1;
    }
    else
    {
        return errorCode2;
    }
}

uint32_t H5Transport::send(std::vector<uint8_t> &data)
{
    if (currentState != STATE_ACTIVE) {
        return NRF_ERROR_INVALID_STATE;
    }

    // max theoretical length of encoded packet, aditional 6 bytes h5 encoding and all bytes escaped + 2 packet encapsuling
    std::vector<uint8_t> h5EncodedPacket;

    h5_encode(data,
              h5EncodedPacket,
              seqNum,
              ackNum,
              true,
              true,
              VENDOR_SPECIFIC_PACKET);

    std::vector<uint8_t> encodedPacket;
    slip_encode(h5EncodedPacket, encodedPacket);

    auto remainingRetransmissions = PACKET_RETRANSMISSIONS;

    lastPacket.clear();
    lastPacket = encodedPacket;

    std::unique_lock<std::mutex> ackGuard(ackMutex);

    while (remainingRetransmissions--)
    {
        logPacket(true, h5EncodedPacket);
        nextTransportLayer->send(lastPacket);

        auto status = ackWaitCondition.wait_for(ackGuard, std::chrono::milliseconds(retransmissionTimeout));

        if (status == std::cv_status::no_timeout)
        {
            lastPacket.clear();
            return NRF_SUCCESS;
        }
    }

    lastPacket.clear();

    return NRF_ERROR_TIMEOUT;
}
#pragma endregion Public methods

#pragma region Processing incoming data from UART
void H5Transport::processPacket(std::vector<uint8_t> &packet)
{
    uint8_t seq_num;
    uint8_t ack_num;
    bool reliable_packet;
    h5_pkt_type_t packet_type;

    std::vector<uint8_t> slipPayload;
    auto err_code = slip_decode(packet, slipPayload);

    if (err_code != NRF_SUCCESS)
    {
        errorPacketCount++;
        return;
    }

    logPacket(false, slipPayload);

    std::vector<uint8_t> h5Payload;

    err_code = h5_decode(slipPayload, h5Payload,
        &seq_num,
        &ack_num,
        &reliable_packet,
        &packet_type);

    if (err_code != NRF_SUCCESS)
    {
        errorPacketCount++;
        return;
    }

    if (currentState == STATE_RESET)
    {
        // Ignore packets packets received in this state.
        syncWaitCondition.notify_all();
        return;
    }

    if (packet_type == LINK_CONTROL_PACKET)
    {
        auto isSyncPacket = h5Payload[0] == syncFirstByte && h5Payload[1] == syncSecondByte;
        auto isSyncResponsePacket = h5Payload[0] == syncRspFirstByte && h5Payload[1] == syncRspSecondByte;
        auto isSyncConfigPacket = h5Payload[0] == syncConfigFirstByte && h5Payload[1] == syncConfigSecondByte;
        auto isSyncConfigResponsePacket = h5Payload[0] == syncConfigRspFirstByte && h5Payload[1] == syncConfigRspSecondByte;

        if (currentState == STATE_UNINITIALIZED)
        {
            if (isSyncResponsePacket) {
                dynamic_cast<UninitializedExitCriterias*>(exitCriterias[currentState])->syncRspReceived = true;
                syncWaitCondition.notify_all();
            }

            if (isSyncPacket) {
                sendControlPacket(CONTROL_PKT_SYNC_RESPONSE);
            }
        }
        else if (currentState == STATE_INITIALIZED)
        {
            auto exit = dynamic_cast<InitializedExitCriterias*>(exitCriterias[currentState]);

            if (isSyncConfigResponsePacket) {
                exit->syncConfigRspReceived = true;
                syncWaitCondition.notify_all();
            }

            if (isSyncConfigPacket)
            {
                exit->syncConfigReceived = true;
                sendControlPacket(CONTROL_PKT_SYNC_CONFIG_RESPONSE);
                exit->syncConfigRspSent = true;
                syncWaitCondition.notify_all();
            }

            if (isSyncPacket) {
                sendControlPacket(CONTROL_PKT_SYNC_RESPONSE);
            }
        }
        else if (currentState == STATE_ACTIVE)
        {
            auto exit = dynamic_cast<ActiveExitCriterias*>(exitCriterias[currentState]);

            if (isSyncPacket)
            {
                exit->syncReceived = true;
                syncWaitCondition.notify_all();
            }
        }
    }
    else if (packet_type == VENDOR_SPECIFIC_PACKET)
    {
        if (currentState == STATE_ACTIVE)
        {
            if (reliable_packet)
            {
                if (seq_num == ackNum)
                {
                    incrementAckNum();
                    sendControlPacket(CONTROL_PKT_ACK);
                    dataCallback(h5Payload.data(), h5Payload.size());
                }
                else
                {
                    dynamic_cast<ActiveExitCriterias*>(exitCriterias[currentState])->irrecoverableSyncError = true;
                    syncWaitCondition.notify_all();
                }
            }
        }
    }
    else if (packet_type == ACK_PACKET)
    {
        if (ack_num == seqNum + 1)
        {
            // Received a packet with valid ack_num, inform threads that wait the command is received on the other end
            std::lock_guard<std::mutex> ackGuard(ackMutex);
            incrementSeqNum();
            ackWaitCondition.notify_all();
        }
        else if (ack_num == seqNum)
        {
            // Discard packet, we assume that we have received a reply from a previous packet
        }
        else
        {
            dynamic_cast<ActiveExitCriterias*>(exitCriterias[currentState])->irrecoverableSyncError = true;
            syncWaitCondition.notify_all();
        }
    }
}

void H5Transport::statusHandler(sd_rpc_app_status_t code, const char * error)
{
    if (code == IO_RESOURCES_UNAVAILABLE)
    {
        exitCriterias[currentState]->ioResourceError = true;
        syncWaitCondition.notify_all();
    }

    statusCallback(code, error);
}

void H5Transport::dataHandler(uint8_t *data, size_t length)
{
    std::vector<uint8_t> packet;

    // Check if we have any data from before that has not been processed.
    // If so add the remaining data from previous callback(s) to this packet
    if (!unprocessedData.empty())
    {
        packet.insert(packet.begin(), unprocessedData.begin(), unprocessedData.end());
    }

    for (size_t i = 0; i < length; i++)
    {
        packet.push_back(data[i]);

        if (data[i] == 0xC0)
        {
            if (c0Found)
            {
                // End of packet found

                // If we have two 0xC0 after another we assume it is the beginning of a new packet, and not the end
                if (packet.size() == 2)
                {
                    packet.clear();
                    packet.push_back(0xc0);
                    continue;
                }

                processPacket(packet);

                packet.clear();
                unprocessedData.clear();
                c0Found = false;
            }
            else
            {
                // Start of packet found
                c0Found = true;

                // Clear previous data from packet since data before the start of packet is irrelevant.
                packet.clear();
                packet.push_back(0xC0);
            }
        }
    }

    if (!packet.empty())
    {
        unprocessedData.clear();
        unprocessedData.insert(unprocessedData.begin(), packet.begin(), packet.end());
    }
}

void H5Transport::incrementSeqNum()
{
    seqNum++;
    seqNum = seqNum & 0x07;
}

void H5Transport::incrementAckNum()
{
    ackNum++;
    ackNum = ackNum & 0x07;
}

#pragma endregion Processing of incoming packets from UART

#pragma region  State machine
void H5Transport::setupStateMachine()
{
    stateActions[STATE_START] = [&]() -> h5_state_t {
        auto exit = dynamic_cast<StartExitCriterias*>(exitCriterias[STATE_START]);
        exit->reset();

        std::unique_lock<std::mutex> syncGuard(syncMutex);

        while (!exit->isFullfilled())
        {
            syncWaitCondition.wait(syncGuard);
        }

        if (exit->ioResourceError)
        {
            return STATE_FAILED;
        }
        else if (exit->isOpened)
        {
            return STATE_RESET;
        }
        else
        {
            return STATE_FAILED;
        }
    };

    stateActions[STATE_RESET] = [&]() -> h5_state_t {
        auto exit = dynamic_cast<ResetExitCriterias*>(exitCriterias[STATE_RESET]);
        exit->reset();

        std::unique_lock<std::mutex> syncGuard(syncMutex);

        while (!exit->isFullfilled())
        {
            sendControlPacket(CONTROL_PKT_RESET);
            statusCallback(RESET_PERFORMED, "Target Reset performed");
            exit->resetSent = true;
            syncWaitCondition.wait_for(syncGuard, RESET_WAIT_DURATION);
        }

        if (!exit->isFullfilled())
        {
            return STATE_FAILED;
        }
        else
        {
            return STATE_UNINITIALIZED;
        }
    };

    stateActions[STATE_UNINITIALIZED] = [&]() -> h5_state_t
    {
        auto exit = dynamic_cast<UninitializedExitCriterias*>(exitCriterias[STATE_UNINITIALIZED]);
        exit->reset();

        uint8_t syncRetransmission = PACKET_RETRANSMISSIONS;
        std::unique_lock<std::mutex> syncGuard(syncMutex);

        while (!exit->isFullfilled() && syncRetransmission--)
        {
            sendControlPacket(CONTROL_PKT_SYNC);
            exit->syncSent = true;
            syncWaitCondition.wait_for(syncGuard, NON_ACTIVE_STATE_TIMEOUT);
        }

        if (exit->isFullfilled())
        {
            return STATE_INITIALIZED;
        }
        else
        {
            return STATE_FAILED;
        }
    };

    stateActions[STATE_INITIALIZED] = [&]() -> h5_state_t
    {
        auto exit = dynamic_cast<InitializedExitCriterias*>(exitCriterias[STATE_INITIALIZED]);
        exit->reset();

        uint8_t syncRetransmission = PACKET_RETRANSMISSIONS;
        std::unique_lock<std::mutex> syncGuard(syncMutex);

        // Send a package immediately
        sendControlPacket(CONTROL_PKT_SYNC_CONFIG);
        exit->syncConfigSent = true;

        while (!exit->isFullfilled() && syncRetransmission > 0)
        {
            auto status = syncWaitCondition.wait_for(syncGuard, NON_ACTIVE_STATE_TIMEOUT);

            if (status == std::cv_status::timeout)
            {
                sendControlPacket(CONTROL_PKT_SYNC_CONFIG);
                syncRetransmission--;
            }
        }

        if (exit->syncConfigSent && exit->syncConfigRspReceived
            && exit->syncConfigReceived && exit->syncConfigRspSent)
        {
            return STATE_ACTIVE;
        }
        else
        {
            return STATE_FAILED;
        }
    };

    stateActions[STATE_ACTIVE] = [&]() -> h5_state_t
    {
        seqNum = 0;
        ackNum = 0;

        std::unique_lock<std::mutex> syncGuard(syncMutex);
        auto exit = dynamic_cast<ActiveExitCriterias*>(exitCriterias[STATE_ACTIVE]);
        exit->reset();

        statusHandler(CONNECTION_ACTIVE, "Connection active");

        while (!exit->isFullfilled())
        {
            syncWaitCondition.wait(syncGuard);
        }

        if (exit->syncReceived || exit->irrecoverableSyncError)
        {
            return STATE_RESET;
        }
        else if (exit->close)
        {
            return STATE_START;
        }
        else if (exit->ioResourceError)
        {
            return STATE_FAILED;
        }
        else
        {
            return STATE_FAILED;
        }
    };

    stateActions[STATE_FAILED] = [&]() -> h5_state_t
    {
        log("Giving up! I can not provide you a way of your failed state!");
        return STATE_FAILED;
    };

    // Setup exit criterias
    exitCriterias[STATE_START] = new StartExitCriterias();
    exitCriterias[STATE_RESET] = new ResetExitCriterias();
    exitCriterias[STATE_UNINITIALIZED] = new UninitializedExitCriterias();
    exitCriterias[STATE_INITIALIZED] = new InitializedExitCriterias();
    exitCriterias[STATE_ACTIVE] = new ActiveExitCriterias();
}

void H5Transport::startStateMachine()
{
    runStateMachine = true;
    currentState = STATE_START;

    if (stateMachineThread == nullptr)
    {
        stateMachineThread = new std::thread(std::bind(&H5Transport::stateMachineWorker, this));
    }
}

void H5Transport::stopStateMachine()
{
    runStateMachine = false;
    syncWaitCondition.notify_all(); // Notify state machine thread

    if (stateMachineThread != nullptr)
    {
        // Check if stateMachineThread is stopping itself
        if (std::this_thread::get_id() == stateMachineThread->get_id())
        {
            stateMachineThread = nullptr;
            return;
        }

        stateMachineThread->join();
        delete stateMachineThread;
        stateMachineThread = nullptr;
    }
}

// Event Thread
void H5Transport::stateMachineWorker()
{
    h5_state_t nextState;

    while (currentState != STATE_FAILED && runStateMachine == true)
    {
        nextState = stateActions[currentState]();
        logStateTransition(currentState, nextState);

        currentState = nextState;

        // Inform interested parties that new state is about to be entered
        stateWaitCondition.notify_all();
    }
}

bool H5Transport::waitForState(h5_state_t state, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(stateMutex);
    stateWaitCondition.wait_for(lock, timeout, [&] { return currentState == state; });

    if (currentState != state)
    {
        return false;
    }
    else
    {
        return true;
    }
}

#pragma endregion State machine related methods

#pragma region Sending packet types

void H5Transport::sendControlPacket(control_pkt_type type)
{
    static std::map<control_pkt_type, std::vector<uint8_t>> pkt_pattern = {
        {CONTROL_PKT_RESET, {}},
        {CONTROL_PKT_SYNC, {syncFirstByte, syncSecondByte}},
        {CONTROL_PKT_SYNC_RESPONSE, {syncRspFirstByte, syncRspSecondByte}},
        {CONTROL_PKT_SYNC_CONFIG, {syncConfigFirstByte, syncConfigSecondByte, syncConfigField}},
        {CONTROL_PKT_SYNC_CONFIG_RESPONSE, {syncConfigRspFirstByte, syncConfigRspSecondByte, syncConfigField}}
    };

    h5_pkt_type_t h5_packet;

    switch (type)
    {
    case CONTROL_PKT_RESET:
        h5_packet = RESET_PACKET;
        break;
    case CONTROL_PKT_SYNC:
    case CONTROL_PKT_SYNC_RESPONSE:
    case CONTROL_PKT_SYNC_CONFIG:
    case CONTROL_PKT_SYNC_CONFIG_RESPONSE:
        h5_packet = LINK_CONTROL_PACKET;
        break;
    case CONTROL_PKT_ACK:
        h5_packet = ACK_PACKET;
        break;
    default:
        h5_packet = LINK_CONTROL_PACKET;
    }

    auto payload = pkt_pattern[type];
    std::vector<uint8_t> h5Packet;

    h5_encode(payload,
        h5Packet,
        0,
        type == CONTROL_PKT_ACK ? ackNum : 0,
        false,
        false,
        h5_packet);

    std::vector<uint8_t> slipPacket;
    slip_encode(h5Packet, slipPacket);

    logPacket(true, h5Packet);

    nextTransportLayer->send(slipPacket);
}

#pragma endregion Methods related to sending packet types defined in the Three Wire Standard

#pragma region Debugging
std::string H5Transport::stateToString(h5_state_t state)
{
    static std::map<h5_state_t, std::string> stateString{
        { STATE_UNKNOWN, "STATE_UNKNOWN" },
        { STATE_START, "STATE_START" },
        { STATE_UNINITIALIZED, "STATE_UNINITIALIZED" },
        { STATE_ACTIVE, "STATE_ACTIVE" },
        { STATE_FAILED, "STATE_FAILED" },
        { STATE_RESET, "STATE_RESET" },
        { STATE_INITIALIZED, "STATE_INITIALIZED" }
    };

    return stateString[state];
}

std::string H5Transport::pktTypeToString(h5_pkt_type_t pktType)
{
    static std::map<h5_pkt_type_t, std::string> pktTypeString{
        { ACK_PACKET, "ACK" },
        { HCI_COMMAND_PACKET, "HCI_COMMAND_PACKET" },
        { ACL_DATA_PACKET, "ACL_DATA_PACKET" },
        { SYNC_DATA_PACKET, "SYNC_DATA_PACKET" },
        { HCI_EVENT_PACKET, "HCI_EVENT_PACKET" },
        { RESET_PACKET, "RESERVED_5" },
        { VENDOR_SPECIFIC_PACKET, "VENDOR_SPECIFIC" },
        { LINK_CONTROL_PACKET, "LINK_CONTROL_PACKET" },
    };

    return pktTypeString[pktType];
}

std::string H5Transport::asHex(std::vector<uint8_t> &packet) const
{
    std::stringstream hex;

    for_each(packet.begin(), packet.end(), [&](uint8_t byte){
        hex << std::setfill('0') << std::setw(2) << std::hex << +byte << " ";
    });

    return hex.str();
}

std::string H5Transport::hciPacketLinkControlToString(std::vector<uint8_t> payload) const
{
    std::stringstream retval;

    auto configToString = [](uint8_t config)
    {
        std::stringstream info;
        info << " sliding-window-size:" << (config & 0x07);
        info << " out-of-frame:" << ((config & 0x08) ? "1" : "0");
        info << " data-integrity-check-type:" << ((config & 0x0f) ? "1" : "0");
        info << " version-number:" << ((config & 0x0e) >> 5) << " ";
        return info.str();
    };

    if (payload.size() >= 2)
    {
        retval << "[";

        if (payload[0] == syncFirstByte && payload[1] == syncSecondByte) retval << "SYNC";
        if (payload[0] == syncRspFirstByte && payload[1] == syncRspSecondByte) retval << "SYNC_RESP";

        if (payload[0] == syncConfigFirstByte && payload[1] == syncConfigSecondByte && payload.size() == 3)
        {
            retval << "CONFIG [" << configToString(payload[2]) << "]";
        }
        if (payload[0] == syncConfigRspFirstByte && payload[1] == syncConfigRspSecondByte)
        {
            retval << "CONFIG_RESP [" << configToString(payload[2]) << "]";
        }

        if (payload[0] == 0x05 && payload[1] == 0xfa) retval << "WAKEUP";
        if (payload[0] == 0x06 && payload[1] == 0xf9) retval << "WOKEN";
        if (payload[0] == 0x07 && payload[1] == 0x78) retval << "SLEEP";

        retval << "]";
    }

    return retval.str();
}

std::string H5Transport::h5PktToString(bool out, std::vector<uint8_t> &h5Packet) const
{
    std::vector<uint8_t> payload;

    uint8_t seq_num;
    uint8_t ack_num;
    bool reliable_packet;
    h5_pkt_type_t packet_type;

    auto err_code = h5_decode(h5Packet, payload,
        &seq_num,
        &ack_num,
        &reliable_packet,
        &packet_type);

    std::stringstream count;

    if (out)
    {
        count << std::setw(8) << outgoingPacketCount << " -> ";
    }
    else
    {
        count << std::setw(5) << incomingPacketCount << "/" << std::setw(2) << errorPacketCount << " <- ";
    }

    std::stringstream retval;
    retval
        << count.str()
        << " [" << asHex(payload) << "]" << std::endl
        << std::setw(20) << "type:" << std::setw(20) << pktTypeToString(packet_type)
        << " reliable:" << std::setw(3) << (reliable_packet ? "yes" : "no")
        << " seq#:" << std::hex << +seq_num << " ack#:" << std::hex << +ack_num
        << " status:" << err_code;

    if (packet_type == LINK_CONTROL_PACKET)
    {
        retval << std::endl << std::setw(15) << "" << hciPacketLinkControlToString(payload);
    }

    return retval.str();
}

void H5Transport::logPacket(bool outgoing, std::vector<uint8_t> &packet)
{
    if (outgoing)
    {
        outgoingPacketCount++;
    }
    else
    {
        incomingPacketCount++;
    }

    std::string logLine = h5PktToString(outgoing, packet).c_str();

    if (this->logCallback != nullptr)
    {
        this->logCallback(SD_RPC_LOG_DEBUG, logLine);
    }
    else
    {
        std::clog << logLine << std::endl;
    }
}

void H5Transport::log(std::string &logLine) const
{
    if (this->logCallback != nullptr)
    {
        this->logCallback(SD_RPC_LOG_DEBUG, logLine);
    }
    else
    {
        std::clog << logLine << std::endl;
    }
}

void H5Transport::log(char const *logLine) const
{
    auto _logLine = std::string(logLine);
    log(_logLine);
}

void H5Transport::logStateTransition(h5_state_t from, h5_state_t to) const
{
    std::stringstream logLine;
    logLine << "State change: " << stateToString(from) << " -> " << stateToString(to) << std::endl;

    if (this->logCallback != nullptr)
    {
        this->logCallback(SD_RPC_LOG_DEBUG, logLine.str());
    }
    else
    {
        std::clog << logLine.str() << std::endl;
    }
}

#pragma endregion Debugging related methods