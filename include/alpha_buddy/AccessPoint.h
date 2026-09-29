#pragma once

namespace alpha_buddy {

constexpr char kApSsid[] = "alpha-buddy";
// Initialized once before the display task and Wi-Fi start; then read-only.
extern char apPassword[9];
bool initializeApPassword();

}  // namespace alpha_buddy
