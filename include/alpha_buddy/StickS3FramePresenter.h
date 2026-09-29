#pragma once

#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/IFramePresenter.h"

namespace alpha_buddy {

class StickS3ShutterInput;

struct FramePresentationStats {
    uint32_t framesDisplayed = 0;
    uint32_t failures = 0;
    uint32_t totalDisplayMs = 0;
    uint32_t maximumDisplayMs = 0;
};

class StickS3FramePresenter final : public IFramePresenter {
public:
    StickS3FramePresenter();
    ~StickS3FramePresenter() override;

    StickS3FramePresenter(const StickS3FramePresenter&) = delete;
    StickS3FramePresenter& operator=(const StickS3FramePresenter&) = delete;

    bool begin(StickS3ShutterInput& shutterInput);
    bool present(const uint8_t* jpeg,
                 size_t jpegBytes,
                 uint16_t width,
                 uint16_t height) override;
    void setCaptureFeedback(CaptureFeedback feedback) override;
    void setConnection(CameraConnection connection) override;
    void setWaitingForWifi(bool waiting);
    void setPreviewPaused(bool paused) override;
    void setCameraModes(CameraModes modes) override;
    FramePresentationStats end();

private:
    class Impl;
    Impl* _impl = nullptr;
    FramePresentationStats _lastStats;
};

}  // namespace alpha_buddy

#endif
