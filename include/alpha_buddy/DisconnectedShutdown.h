#pragma once

#include <cstdint>

namespace alpha_buddy {

class DisconnectedShutdown {
public:
    static constexpr uint32_t kTimeoutMs = 5U * 60U * 1000U;

    void setClientCount(uint32_t now, uint8_t count) {
        const bool connected = count != 0;
        if (_connected && !connected) {
            _waitingSinceMs = now;
        }
        _connected = connected;
    }

    bool shouldShutdown(uint32_t now) const {
        return !_connected && now - _waitingSinceMs >= kTimeoutMs;
    }

private:
    bool _connected = false;
    uint32_t _waitingSinceMs = 0;
};

}  // namespace alpha_buddy
