#pragma once

namespace alpha_buddy {

struct ShutterInputEvents {
    bool pressed = false;
    bool released = false;
    bool cancelled = false; // Latched decision attached to this release.
};

class IShutterInput {
public:
    virtual ~IShutterInput() = default;
    virtual ShutterInputEvents poll() = 0;
};

}  // namespace alpha_buddy
