#pragma once

#include <cstddef>
#include <cstdint>
#include "alpha_buddy/ViewfinderStatus.h"
#include "alpha_buddy/CameraModes.h"

namespace alpha_buddy {

enum class CaptureFeedback : uint8_t {
    Live,
    Autofocus,
    Capturing,
    Success,
    Failure,
};

class IFramePresenter {
public:
    virtual ~IFramePresenter() = default;
    virtual bool present(const uint8_t* jpeg,
                         size_t jpegBytes,
                         uint16_t width,
                         uint16_t height) = 0;
    virtual void setCaptureFeedback(CaptureFeedback) {}
    virtual void setConnection(CameraConnection) {}
    virtual void setPreviewPaused(bool) {}
    virtual void setCameraModes(CameraModes) {}
};

}  // namespace alpha_buddy
