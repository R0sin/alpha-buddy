#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/WiFiPtpIpCommandPort.h"

#include <algorithm>
#include <cstdint>

#include "alpha_buddy/PtpIpProtocol.h"

namespace alpha_buddy {

namespace {

constexpr uint16_t kOperationOpenSession = 0x1002;
constexpr uint16_t kResponseOk = 0x2001;
constexpr size_t kHeaderBytes = 8;
constexpr size_t kDataPacketPrefixBytes = 12;
constexpr size_t kMaximumPacketBytes = 4U * 1024U * 1024U;

uint32_t elapsedSince(uint32_t startedAtMs) {
    return millis() - startedAtMs;
}

}  // namespace

PortOpenResult WiFiPtpIpCommandPort::open(const PtpSessionConfig& config) {
    close();
    PortOpenResult result;
    const uint32_t startedAt = millis();
    _timeoutMs = config.timeoutMs;

    if (!connectSockets(config.cameraIpv4, config.port, config.timeoutMs)) {
        result.failedStage = ProbeStage::SocketConnect;
        result.transportError = TransportError::SocketConnect;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }

    uint8_t request[320] = {};
    const size_t commandBytes = ptpip::buildInitCommandRequest(config.guid,
                                                               config.friendlyName,
                                                               request,
                                                               sizeof(request));
    if (commandBytes == 0 || !sendPacket(_commandSocket, request, commandBytes)) {
        result.failedStage = ProbeStage::InitCommand;
        result.transportError = TransportError::WriteFailed;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }

    PacketView packet;
    if (!readPacket(_commandSocket, packet, _timeoutMs)) {
        result.failedStage = ProbeStage::InitCommand;
        result.transportError = TransportError::UnexpectedPacket;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }
    if (!ptpip::parseInitCommandAck(packet.data,
                                    packet.length,
                                    _connectionId,
                                    _cameraName,
                                    sizeof(_cameraName))) {
        ptpip::PacketHeader header;
        const bool headerOk = ptpip::parseHeader(packet.data, packet.length, header);
        const uint32_t detail = packet.length >= 12 ? ptpip::readLe32(packet.data + 8) : 0;
        Serial.printf("{\"event\":\"ap_init_reply\",\"header_valid\":%s,"
                      "\"packet_length\":%lu,\"packet_type\":%lu,"
                      "\"detail\":%lu}\n",
                      headerOk ? "true" : "false",
                      static_cast<unsigned long>(packet.length),
                      headerOk ? static_cast<unsigned long>(header.type) : 0UL,
                      static_cast<unsigned long>(detail));
        result.failedStage = ProbeStage::InitCommand;
        result.transportError = TransportError::UnexpectedPacket;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }

    const size_t eventBytes = ptpip::buildInitEventRequest(_connectionId, request, sizeof(request));
    if (eventBytes == 0 || !sendPacket(_eventSocket, request, eventBytes)) {
        result.failedStage = ProbeStage::InitEvent;
        result.transportError = TransportError::WriteFailed;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }
    if (!readPacket(_eventSocket, packet, _timeoutMs)) {
        result.failedStage = ProbeStage::InitEvent;
        result.transportError = TransportError::Timeout;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }
    ptpip::PacketHeader header;
    if (!ptpip::parseHeader(packet.data, packet.length, header) ||
        header.type != ptpip::PacketType::InitEventAck) {
        result.failedStage = ProbeStage::InitEvent;
        result.transportError = TransportError::UnexpectedPacket;
        result.elapsedMs = elapsedSince(startedAt);
        close();
        return result;
    }

    _sessionId = 1;
    _transactionId = 1;
    const uint32_t openParameters[2] = {_transactionId, _sessionId};
    OperationResult openSession = executeInternal(kOperationOpenSession,
                                                  openParameters,
                                                  2,
                                                  nullptr);
    result.responseCode = openSession.responseCode;
    result.elapsedMs = elapsedSince(startedAt);
    if (!openSession.transportOk || openSession.responseCode != kResponseOk) {
        result.failedStage = ProbeStage::OpenSession;
        result.transportError = openSession.transportError;
        close();
        return result;
    }

    result.ok = true;
    result.failedStage = ProbeStage::Complete;
    result.transportError = TransportError::None;
    return result;
}

OperationResult WiFiPtpIpCommandPort::execute(uint16_t operationCode,
                                               const uint32_t* parameters,
                                               size_t parameterCount,
                                               IDataSink* sink) {
    return executeInternal(operationCode, parameters, parameterCount, sink);
}

OperationResult WiFiPtpIpCommandPort::executeWithData(uint16_t operationCode,
                                                       const uint32_t* parameters,
                                                       size_t parameterCount,
                                                       const uint8_t* data,
                                                       size_t dataBytes) {
    return executeInternal(operationCode,
                           parameters,
                           parameterCount,
                           nullptr,
                           data,
                           dataBytes);
}

void WiFiPtpIpCommandPort::close() {
    _commandSocket.stop();
    _eventSocket.stop();
    _connectionId = 0;
    _sessionId = 1;
    _transactionId = 1;
    _cameraName[0] = '\0';
}

const char* WiFiPtpIpCommandPort::cameraName() const {
    return _cameraName;
}

bool WiFiPtpIpCommandPort::connectSockets(uint32_t ipv4,
                                          uint16_t port,
                                          uint32_t timeoutMs) {
    const IPAddress address(ipv4);
    if (!_commandSocket.connect(address, port, timeoutMs)) {
        return false;
    }
    if (!_eventSocket.connect(address, port, timeoutMs)) {
        _commandSocket.stop();
        return false;
    }
    _commandSocket.setTimeout(timeoutMs);
    _eventSocket.setTimeout(timeoutMs);
    return true;
}

bool WiFiPtpIpCommandPort::sendPacket(WiFiClient& socket,
                                      const uint8_t* data,
                                      size_t length) {
    if (!socket.connected() || data == nullptr || length == 0) {
        return false;
    }
    return socket.write(data, length) == length;
}

bool WiFiPtpIpCommandPort::readExact(WiFiClient& socket,
                                     uint8_t* output,
                                     size_t length,
                                     uint32_t timeoutMs) {
    const uint32_t startedAt = millis();
    size_t received = 0;
    while (received < length && elapsedSince(startedAt) < timeoutMs) {
        if (!socket.connected() && socket.available() == 0) {
            return false;
        }
        const int available = socket.available();
        if (available <= 0) {
            delay(1);
            continue;
        }
        const size_t wanted = std::min(length - received, static_cast<size_t>(available));
        const int count = socket.read(output + received, wanted);
        if (count <= 0) {
            delay(1);
            continue;
        }
        received += static_cast<size_t>(count);
    }
    return received == length;
}

bool WiFiPtpIpCommandPort::readPacket(WiFiClient& socket,
                                      PacketView& packet,
                                      uint32_t timeoutMs) {
    if (!readExact(socket, _packetBuffer, kHeaderBytes, timeoutMs)) {
        return false;
    }
    ptpip::PacketHeader header;
    if (!ptpip::parseHeader(_packetBuffer, kHeaderBytes, header) ||
        header.length > sizeof(_packetBuffer)) {
        return false;
    }
    const size_t remaining = header.length - kHeaderBytes;
    if (remaining > 0 && !readExact(socket,
                                    _packetBuffer + kHeaderBytes,
                                    remaining,
                                    timeoutMs)) {
        return false;
    }
    packet.data = _packetBuffer;
    packet.length = header.length;
    return true;
}

bool WiFiPtpIpCommandPort::handleProbe(WiFiClient& socket, const PacketView& packet) {
    ptpip::PacketHeader header;
    if (!ptpip::parseHeader(packet.data, packet.length, header) ||
        header.type != ptpip::PacketType::ProbeRequest) {
        return false;
    }
    uint8_t response[8] = {};
    const size_t responseBytes = ptpip::buildProbeResponse(response, sizeof(response));
    return responseBytes > 0 && sendPacket(socket, response, responseBytes);
}

OperationResult WiFiPtpIpCommandPort::executeInternal(uint16_t operationCode,
                                                       const uint32_t* parameters,
                                                       size_t parameterCount,
                                                       IDataSink* sink,
                                                       const uint8_t* dataOut,
                                                       size_t dataOutBytes) {
    OperationResult result;
    const uint32_t startedAt = millis();
    if ((dataOut == nullptr) != (dataOutBytes == 0) ||
        (sink != nullptr && dataOutBytes > 0)) {
        result.transportError = TransportError::InvalidPacket;
        return result;
    }
    if (!_commandSocket.connected()) {
        result.transportError = TransportError::SocketClosed;
        return result;
    }
    uint8_t request[64] = {};
    const uint32_t transactionId = _transactionId++;
    const size_t requestBytes = ptpip::buildOperationRequest(operationCode,
                                                             transactionId,
                                                             dataOutBytes > 0
                                                                 ? ptpip::DataPhaseInfo::DataOut
                                                                 : ptpip::DataPhaseInfo::NoDataOrDataIn,
                                                             parameters,
                                                             parameterCount,
                                                             request,
                                                             sizeof(request));
    if (requestBytes == 0 || !sendPacket(_commandSocket, request, requestBytes)) {
        result.transportError = TransportError::WriteFailed;
        result.elapsedMs = elapsedSince(startedAt);
        return result;
    }
    if (dataOutBytes > 0) {
        const size_t startBytes = ptpip::buildStartDataPacket(transactionId,
                                                              dataOutBytes,
                                                              request,
                                                              sizeof(request));
        if (startBytes == 0 || !sendPacket(_commandSocket, request, startBytes)) {
            result.transportError = TransportError::WriteFailed;
            result.elapsedMs = elapsedSince(startedAt);
            return result;
        }
        const size_t payloadPacketBytes = ptpip::buildDataPacket(transactionId,
                                                                 dataOut,
                                                                 dataOutBytes,
                                                                 request,
                                                                 sizeof(request));
        if (payloadPacketBytes == 0 ||
            !sendPacket(_commandSocket, request, payloadPacketBytes)) {
            result.transportError = dataOutBytes > sizeof(request) - 12
                ? TransportError::PacketTooLarge
                : TransportError::WriteFailed;
            result.elapsedMs = elapsedSince(startedAt);
            return result;
        }
        const size_t endBytes = ptpip::buildEndDataPacket(transactionId,
                                                          request,
                                                          sizeof(request));
        if (endBytes == 0 || !sendPacket(_commandSocket, request, endBytes)) {
            result.transportError = TransportError::WriteFailed;
            result.elapsedMs = elapsedSince(startedAt);
            return result;
        }
    }
    return readOperationResult(transactionId,
                               sink != nullptr,
                               sink,
                               startedAt);
}

OperationResult WiFiPtpIpCommandPort::readOperationResult(uint32_t transactionId,
                                                           bool expectData,
                                                           IDataSink* sink,
                                                           uint32_t startedAtMs) {
    OperationResult result;
    bool sinkStarted = false;
    bool dataComplete = !expectData;
    uint64_t expectedBytes = 0;
    bool expectedLengthKnown = false;
    size_t receivedBytes = 0;

    while (elapsedSince(startedAtMs) < _timeoutMs) {
        uint8_t headerBytes[kHeaderBytes] = {};
        if (!readExact(_commandSocket, headerBytes, sizeof(headerBytes), _timeoutMs)) {
            result.transportError = _commandSocket.connected()
                ? TransportError::Timeout
                : TransportError::SocketClosed;
            result.elapsedMs = elapsedSince(startedAtMs);
            return result;
        }
        ptpip::PacketHeader header;
        if (!ptpip::parseHeader(headerBytes, sizeof(headerBytes), header) ||
            header.length > kMaximumPacketBytes) {
            result.transportError = TransportError::InvalidPacket;
            result.elapsedMs = elapsedSince(startedAtMs);
            return result;
        }
        const size_t payloadBytes = header.length - kHeaderBytes;

        if (header.type == ptpip::PacketType::StartData) {
            if (payloadBytes != 12 ||
                !readExact(_commandSocket, _packetBuffer + kHeaderBytes, payloadBytes, _timeoutMs)) {
                result.transportError = TransportError::InvalidPacket;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            std::copy(headerBytes, headerBytes + kHeaderBytes, _packetBuffer);
            uint32_t dataTransaction = 0;
            if (!ptpip::parseStartData(_packetBuffer,
                                       header.length,
                                       dataTransaction,
                                       expectedBytes) ||
                dataTransaction != transactionId) {
                result.transportError = TransportError::InvalidPacket;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            expectedLengthKnown = expectedBytes != UINT64_MAX;
            if (expectedLengthKnown && expectedBytes > kMaximumPacketBytes) {
                result.transportError = TransportError::PacketTooLarge;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            const size_t sinkExpectedBytes = expectedLengthKnown
                ? static_cast<size_t>(expectedBytes)
                : 0;
            if (sink != nullptr && !sink->begin(sinkExpectedBytes)) {
                result.transportError = TransportError::SinkRejected;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            sinkStarted = sink != nullptr;
            continue;
        }

        if (header.type == ptpip::PacketType::Data ||
            header.type == ptpip::PacketType::EndData) {
            if (payloadBytes < 4) {
                result.transportError = TransportError::InvalidPacket;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            uint8_t transactionBytes[4] = {};
            if (!readExact(_commandSocket, transactionBytes, sizeof(transactionBytes), _timeoutMs) ||
                ptpip::readLe32(transactionBytes) != transactionId) {
                result.transportError = TransportError::InvalidPacket;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            size_t remaining = payloadBytes - sizeof(transactionBytes);
            while (remaining > 0) {
                const size_t chunk = std::min(remaining, sizeof(_packetBuffer));
                if (!readExact(_commandSocket, _packetBuffer, chunk, _timeoutMs)) {
                    result.transportError = TransportError::Timeout;
                    result.elapsedMs = elapsedSince(startedAtMs);
                    return result;
                }
                if (sinkStarted && !sink->write(_packetBuffer, chunk)) {
                    result.transportError = TransportError::SinkRejected;
                    result.elapsedMs = elapsedSince(startedAtMs);
                    return result;
                }
                receivedBytes += chunk;
                remaining -= chunk;
            }
            if (header.type == ptpip::PacketType::EndData) {
                dataComplete = true;
                if (sinkStarted && !sink->finish()) {
                    result.transportError = TransportError::SinkRejected;
                    result.elapsedMs = elapsedSince(startedAtMs);
                    return result;
                }
            }
            continue;
        }

        if (payloadBytes > sizeof(_packetBuffer) - kHeaderBytes) {
            result.transportError = TransportError::PacketTooLarge;
            result.elapsedMs = elapsedSince(startedAtMs);
            return result;
        }
        std::copy(headerBytes, headerBytes + kHeaderBytes, _packetBuffer);
        if (payloadBytes > 0 &&
            !readExact(_commandSocket,
                       _packetBuffer + kHeaderBytes,
                       payloadBytes,
                       _timeoutMs)) {
            result.transportError = TransportError::Timeout;
            result.elapsedMs = elapsedSince(startedAtMs);
            return result;
        }
        PacketView packet;
        packet.data = _packetBuffer;
        packet.length = header.length;
        if (header.type == ptpip::PacketType::ProbeRequest) {
            if (!handleProbe(_commandSocket, packet)) {
                result.transportError = TransportError::WriteFailed;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            continue;
        }
        if (header.type == ptpip::PacketType::CancelData) {
            result.transportError = TransportError::UnexpectedPacket;
            result.elapsedMs = elapsedSince(startedAtMs);
            return result;
        }
        if (header.type == ptpip::PacketType::OperationResponse) {
            if (!ptpip::parseOperationResponse(packet.data, packet.length, result.responseCode)) {
                result.transportError = TransportError::InvalidPacket;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            if (expectedLengthKnown && !dataComplete &&
                expectedBytes > 0 && receivedBytes < expectedBytes) {
                result.transportError = TransportError::InvalidPacket;
                result.elapsedMs = elapsedSince(startedAtMs);
                return result;
            }
            result.transportOk = true;
            result.transportError = TransportError::None;
            result.dataBytes = receivedBytes;
            result.elapsedMs = elapsedSince(startedAtMs);
            return result;
        }

        result.transportError = TransportError::UnexpectedPacket;
        result.elapsedMs = elapsedSince(startedAtMs);
        return result;
    }

    result.transportError = TransportError::Timeout;
    result.elapsedMs = elapsedSince(startedAtMs);
    return result;
}

}  // namespace alpha_buddy

#endif
