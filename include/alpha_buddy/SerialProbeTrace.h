#pragma once

#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/IProbeTrace.h"

namespace alpha_buddy {

class SerialProbeTrace final : public IProbeTrace {
public:
    void record(const ProbeEvent& event) override;
};

}  // namespace alpha_buddy

#endif

