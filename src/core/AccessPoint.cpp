#ifndef ALPHA_BUDDY_NATIVE

#include "alpha_buddy/AccessPoint.h"

#include <Preferences.h>
#include <bootloader_random.h>
#include <esp_system.h>
#include <cstring>

namespace alpha_buddy {

char apPassword[9] = {};

bool initializeApPassword() {
    Preferences settings;
    if (!settings.begin("alpha-buddy-ap", false)) return false;

    constexpr char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    constexpr unsigned alphabetSize = sizeof(alphabet) - 1;
    static_assert(alphabetSize == 31, "Password alphabet must have 31 characters");
    bool success = false;
    if (settings.isKey("password")) {
        const String saved = settings.getString("password", "");
        success = saved.length() == 8;
        for (unsigned i = 0; success && i < 8; ++i) {
            success = std::strchr(alphabet, saved[i]) != nullptr && saved[i] != '\0';
        }
        if (success) std::memcpy(apPassword, saved.c_str(), sizeof(apPassword));
    } else {
        // Called before M5/ADC and Wi-Fi initialization. Supply hardware entropy
        // while generating the password, then release the ADC before normal use.
        bootloader_random_enable();
        for (unsigned i = 0; i < 8;) {
            const unsigned sample = esp_random() & 0xFF;
            // Reject the tail so each of the 31 characters is equally likely.
            if (sample < 256U - 256U % alphabetSize) {
                apPassword[i++] = alphabet[sample % alphabetSize];
            }
        }
        bootloader_random_disable();
        apPassword[8] = '\0';
        success = settings.putString("password", apPassword) == 8 &&
                  settings.getString("password", "") == apPassword;
    }
    settings.end();
    // Never advertise an unsaved password or silently replace a corrupt one.
    if (!success) std::memset(apPassword, 0, sizeof(apPassword));
    return success;
}

}  // namespace alpha_buddy

#endif
