#pragma once

#include <cstdint>

namespace alpha_buddy {

// Receives debounced state; a hold is not a click. Emits on second release.
class SideButtonDoubleClick {
public:
    bool update(bool down, uint32_t now) {
        if (_waiting && !_down && now - _releasedAt > 300) _waiting = false;
        if (down == _down) return false;
        _down = down;
        if (down) {
            _pressedAt = now;
            return false;
        }
        if (now - _pressedAt > 300) {
            _waiting = false;
            return false;
        }
        if (_waiting) {
            _waiting = false;
            return true;
        }
        _releasedAt = now;
        _waiting = true;
        return false;
    }

private:
    bool _down = false;
    bool _waiting = false;
    uint32_t _pressedAt = 0;
    uint32_t _releasedAt = 0;
};

}  // namespace alpha_buddy
