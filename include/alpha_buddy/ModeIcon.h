#pragma once

#include <array>
#include <cstring>
#include "alpha_buddy/CameraModes.h"

namespace alpha_buddy {

// 20x15 monochrome artwork; shared with the native visual check.
struct ModeIcon {
    std::array<uint32_t, 15> rows{};
    bool unknown = false;
    void rect(int x, int y, int w, int h) {
        for (int j = y; j < y + h && j < 15; ++j)
            for (int i = x; i < x + w && i < 20; ++i)
                if (i >= 0 && j >= 0) rows[j] |= uint32_t{1} << i;
    }
    void frame(int x, int y, int w, int h) {
        rect(x, y, w, 2); rect(x, y + h - 2, w, 2);
        rect(x, y, 2, h); rect(x + w - 2, y, 2, h);
    }
    void text(const char* label) {
        const int length = int(std::strlen(label));
        const int sx = length > 2 ? 1 : 2;
        const int left = (20 - (length * 4 - 1) * sx) / 2;
        for (int c = 0; c < length; ++c) {
            const char* glyph = "111001010000010"; // ?
            switch (label[c]) {
                case 'P': glyph = "110101110100100"; break;
                case 'A': glyph = "010101111101101"; break;
                case 'S': glyph = "111100111001111"; break;
                case 'M': glyph = "101111111101101"; break;
                case 'C': glyph = "111100100100111"; break;
                case 'D': glyph = "110101101101110"; break;
                case 'F': glyph = "111100110100100"; break;
                case 'U': glyph = "101101101101111"; break;
                case 'T': glyph = "111010010010010"; break;
                case 'O': glyph = "111101101101111"; break;
                case 'N': glyph = "101111111111101"; break;
                case '-': glyph = "000000111000000"; break;
            }
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 3; ++x)
                    if (glyph[y * 3 + x] == '1') rect(left + (c * 4 + x) * sx, 2 + y * 2, sx, 2);
        }
    }
};

inline ModeIcon modeIcon(unsigned slot, uint8_t value) {
    ModeIcon icon;
    icon.unknown = value == 0;
    if (value == 0) { icon.text("?"); return icon; }
    if (slot == 0) {
        const char* labels[] = {"?", "P", "A", "S", "M", "AUTO", "SCN"};
        if (value < 7) icon.text(labels[value]);
        else { icon.unknown = true; icon.text("?"); }
    } else if (slot == 1) {
        const char* labels[] = {"?", "AF-S", "AF-C", "AF-A", "DMF", "MF"};
        if (value < 6) icon.text(labels[value]);
        else { icon.unknown = true; icon.text("?"); }
    } else {
        if (value != uint8_t(SilentMode::Off) && value != uint8_t(SilentMode::On)) {
            icon.unknown = true; icon.text("?"); return icon;
        }
        // Musical note; a diagonal slash marks Sony Silent Mode.
        icon.rect(10, 1, 2, 11);
        icon.rect(12, 2, 3, 2);
        icon.rect(14, 4, 3, 3);
        icon.rect(13, 7, 3, 2);
        icon.rect(5, 10, 7, 3);
        icon.rect(6, 9, 5, 1);
        icon.rect(6, 13, 4, 1);
        if (value == uint8_t(SilentMode::On)) {
            // One-pixel black separation keeps the slash distinct from the note.
            for (int i = 0; i < 14; ++i) {
                const int x = 2 + i, y = i;
                for (int row = y - 1; row <= y + 2; ++row)
                    if (row >= 0 && row < 15) icon.rows[row] &= ~(uint32_t{15} << (x - 1));
            }
            for (int i = 0; i < 14; ++i) icon.rect(2 + i, i, 2, 2);
        }
    }
    return icon;
}

} // namespace alpha_buddy
