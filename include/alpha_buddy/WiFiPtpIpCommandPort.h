#pragma once

#ifndef ALPHA_BUDDY_NATIVE

#include <WiFi.h>

#include "alpha_buddy/IPtpIpCommandPort.h"

namespace alpha_buddy {

class WiFiPtpIpCommandPort final : public IPtpIpCommandPort {
public:
    PortOpenResult open(const PtpSessionConfig& config) override;
    OperationResult execute(uint16_t operationCode,
                            const uint32_t* parameters,
                            size_t parameterCount,
                            IDataSink* sink) override;
    OperationResult executeWithData(uint16_t operationCode,
                                    const uint32_t* parameters,
                                    size_t parameterCount,
                                    const uint8_t* data,
                                    size_t dataBytes) override;
    void close() override;

    const char* cameraName() const;

private:
    struct PacketView {
        uint8_t* data = nullptr;
        size_t length = 0;
    };

    bool connectSockets(uint32_t ipv4, uint16_t port, uint32_t timeoutMs);
    bool sendPacket(WiFiClient& socket, const uint8_t* data, size_t length);
    bool readExact(WiFiClient& socket, uint8_t* output, size_t length, uint32_t timeoutMs);
    bool readPacket(WiFiClient& socket, PacketView& packet, uint32_t timeoutMs);
    bool handleProbe(WiFiClient& socket, const PacketView& packet);
    OperationResult executeInternal(uint16_t operationCode,
                                    const uint32_t* parameters,
                                    size_t parameterCount,
                                    IDataSink* sink,
                                    const uint8_t* dataOut = nullptr,
                                    size_t dataOutBytes = 0);
    OperationResult readOperationResult(uint32_t transactionId,
                                        bool expectData,
                                        IDataSink* sink,
                                        uint32_t startedAtMs);

    WiFiClient _commandSocket;
    WiFiClient _eventSocket;
    uint32_t _timeoutMs = 5000;
    uint32_t _connectionId = 0;
    uint32_t _sessionId = 1;
    uint32_t _transactionId = 1;
    char _cameraName[128] = {};
    uint8_t _packetBuffer[2048] = {};
};

}  // namespace alpha_buddy

#endif
