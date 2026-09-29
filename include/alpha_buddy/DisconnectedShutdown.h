#pragma once

#include <cstdint>

namespace alpha_buddy {

class DisconnectedShutdown {
public:
    static constexpr uint32_t kTimeoutMs = 5U * 60U * 1000U;

    bool update(uint32_t now, bool connected, bool externalPower, bool activity) {
        if (connected || externalPower) {
            _waiting = false;
            return false;
        }
        if (!_waiting || activity) {
            _waiting = true;
            _waitingSinceMs = now;
        }
        return now - _waitingSinceMs >= kTimeoutMs;
    }

private:
    bool _waiting = false;
    uint32_t _waitingSinceMs = 0;
};

}  // namespace alpha_buddy
