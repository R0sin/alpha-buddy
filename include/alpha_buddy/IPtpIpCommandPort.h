#pragma once

#include <cstddef>
#include <cstdint>

#include "alpha_buddy/ProbeTypes.h"

namespace alpha_buddy {

struct PtpSessionConfig {
    uint32_t cameraIpv4 = 0;
    uint16_t port = 15740;
    uint32_t timeoutMs = 5000;
    uint8_t guid[16] = {};
    const char* friendlyName = "alpha-buddy";
};

class IDataSink {
public:
    virtual ~IDataSink() = default;
    virtual bool begin(size_t expectedBytes) = 0;
    virtual bool write(const uint8_t* data, size_t length) = 0;
    virtual bool finish() = 0;
};

class IPtpIpCommandPort {
public:
    virtual ~IPtpIpCommandPort() = default;
    virtual PortOpenResult open(const PtpSessionConfig& config) = 0;
    virtual OperationResult execute(uint16_t operationCode,
                                    const uint32_t* parameters,
                                    size_t parameterCount,
                                    IDataSink* sink) = 0;
    virtual OperationResult executeWithData(uint16_t operationCode,
                                            const uint32_t* parameters,
                                            size_t parameterCount,
                                            const uint8_t* data,
                                            size_t dataBytes) = 0;
    virtual void close() = 0;
};

}  // namespace alpha_buddy
