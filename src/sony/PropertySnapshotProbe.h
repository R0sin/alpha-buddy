#pragma once

#include <cstdlib>
#include <cstring>
#include <Arduino.h>
#include "alpha_buddy/IPtpIpCommandPort.h"

namespace alpha_buddy {

// Diagnostic only: retain the complete reply, including unknown properties.
class PropertySnapshotSink final : public IDataSink {
public:
    static constexpr size_t capacity = 65536;
    uint8_t* data = static_cast<uint8_t*>(malloc(capacity));
    size_t size = 0;
    ~PropertySnapshotSink() override { free(data); }
    bool begin(size_t expected) override {
        size = 0;
        return data && expected <= capacity;
    }
    bool write(const uint8_t* bytes, size_t length) override {
        if (!data || length > capacity - size) return false;
        memcpy(data + size, bytes, length);
        size += length;
        return true;
    }
    bool finish() override { return true; }
};

inline OperationResult capturePropertySnapshot(IPtpIpCommandPort& port) {
    PropertySnapshotSink sink;
    Serial.println("{\"event\":\"property_snapshot\",\"kind\":\"begin\"}");
    const OperationResult result = port.execute(0x9209, nullptr, 0, &sink);
    if (result.transportOk && result.responseCode == 0x2001) {
        constexpr char digits[] = "0123456789abcdef";
        for (size_t offset = 0; offset < sink.size; offset += 128) {
            const size_t count = sink.size - offset < 128 ? sink.size - offset : 128;
            char hex[257];
            for (size_t i = 0; i < count; ++i) {
                hex[2 * i] = digits[sink.data[offset + i] >> 4];
                hex[2 * i + 1] = digits[sink.data[offset + i] & 15];
            }
            hex[2 * count] = 0;
            Serial.printf("{\"event\":\"property_snapshot\",\"kind\":\"data\","
                          "\"offset\":%u,\"hex\":\"%s\"}\n", unsigned(offset), hex);
        }
    }
    Serial.printf("{\"event\":\"property_snapshot\",\"kind\":\"end\","
                  "\"transport_ok\":%s,\"response\":%u,\"bytes\":%u}\n",
                  result.transportOk ? "true" : "false", result.responseCode,
                  unsigned(sink.size));
    return result;
}

} // namespace alpha_buddy
