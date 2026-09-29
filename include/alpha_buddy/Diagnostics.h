#pragma once

#include <cstddef>

#include "alpha_buddy/ProbeTypes.h"

namespace alpha_buddy {

const char* probeStageName(ProbeStage stage);
const char* probeStatusName(ProbeStatus status);
const char* transportErrorName(TransportError error);
bool formatProbeEvent(const ProbeEvent& event, char* output, size_t capacity);

}  // namespace alpha_buddy

