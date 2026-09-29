#pragma once

#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/IShutterInput.h"
#include "alpha_buddy/LandscapeOrientationTracker.h"

namespace alpha_buddy {

class StickS3ShutterInput final : public IShutterInput {
public:
    StickS3ShutterInput();
    ~StickS3ShutterInput() override;

    StickS3ShutterInput(const StickS3ShutterInput&) = delete;
    StickS3ShutterInput& operator=(const StickS3ShutterInput&) = delete;

    bool begin();
    void end();
    ShutterInputEvents poll() override;
    bool takeActivity();
    bool takeGridToggle();
    bool held() const;
    bool cancelPending() const;
    bool cancelCounting() const;
    void observeOrientation(bool resolved, LandscapeOrientation screen,
                            bool valid, LandscapeOrientation observed, uint32_t now);

private:
    class Impl;
    Impl* _impl = nullptr;
};

}  // namespace alpha_buddy

#endif
