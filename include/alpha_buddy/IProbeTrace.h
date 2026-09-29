#pragma once

#include "alpha_buddy/ProbeTypes.h"

namespace alpha_buddy {

class IProbeTrace {
public:
    virtual ~IProbeTrace() = default;
    virtual void record(const ProbeEvent& event) = 0;
};

}  // namespace alpha_buddy

