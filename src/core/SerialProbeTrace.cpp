#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/SerialProbeTrace.h"

#include <Arduino.h>

#include "alpha_buddy/Diagnostics.h"

namespace alpha_buddy {

void SerialProbeTrace::record(const ProbeEvent& event) {
    char line[320] = {};
    if (formatProbeEvent(event, line, sizeof(line))) {
        Serial.println(line);
    } else {
        Serial.println("{\"event\":\"gate_a\",\"status\":\"format_error\"}");
    }
}

}  // namespace alpha_buddy

#endif

