"""Exercise production password initialization with fake NVS and entropy APIs.

Run with Python from a Visual Studio developer shell or with g++ on PATH.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKES = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <string>
using String = std::string;
inline bool openOk = true, writeOk = true, readbackOk = true;
inline bool keyExists = false, entropy = false;
inline unsigned writes = 0, randomCalls = 0, sample = 0, enables = 0, closes = 0;
inline String stored;
struct Preferences {
    bool begin(const char* name, bool readOnly) {
        assert(String(name) == "alpha-buddy-ap" && !readOnly);
        return openOk;
    }
    bool isKey(const char* key) { assert(String(key) == "password"); return keyExists; }
    String getString(const char*, const char*) { return readbackOk ? stored : String(); }
    size_t putString(const char*, const char* value) {
        ++writes;
        if (!writeOk) return 0;
        stored = value;
        keyExists = true;
        return stored.size();
    }
    void end() { ++closes; }
};
inline void bootloader_random_enable() { assert(!entropy); entropy = true; ++enables; }
inline void bootloader_random_disable() { assert(entropy); entropy = false; }
inline uint32_t esp_random() { assert(entropy); ++randomCalls; return sample++ % 256; }
'''
HARNESS = r'''
#include "Preferences.h"
#include "alpha_buddy/AccessPoint.h"
#include <cstring>
#include <cstdio>
using namespace alpha_buddy;
int main() {
    // Start in the rejection tail (248..255), then accept the first 8 symbols.
    sample = 248;
    assert(initializeApPassword());
    assert(String(apPassword) == "abcdefgh" && stored == apPassword);
    assert(randomCalls == 16 && writes == 1 && enables == 1 && !entropy);
    const String first = apPassword;
    std::memset(apPassword, 0, sizeof(apPassword)); // Simulated reboot RAM.
    assert(initializeApPassword() && first == apPassword);
    assert(randomCalls == 16 && writes == 1); // No regeneration or flash wear.
    // Cover every accepted symbol, length, and the next device/erased store.
    for (unsigned seed = 0; seed < 256; ++seed) {
        keyExists = false;
        sample = seed;
        assert(initializeApPassword() && std::strlen(apPassword) == 8);
        for (unsigned i = 0; i < 8; ++i)
            assert(std::strchr("abcdefghjkmnpqrstuvwxyz23456789", apPassword[i]));
    }
    for (const char* invalid : {"", "short", "alphabuddy", "ab0defgh", "ab1defgh",
                               "abidefgh", "abldefgh", "abodefgh", "ABCDEFGH"}) {
        stored = invalid;
        keyExists = true;
        const auto previousWrites = writes;
        assert(!initializeApPassword() && apPassword[0] == '\0');
        assert(writes == previousWrites); // Never silently change an existing key.
    }
    keyExists = false;
    writeOk = false;
    assert(!initializeApPassword() && apPassword[0] == '\0' && !entropy);
    writeOk = true;
    readbackOk = false;
    assert(!initializeApPassword() && apPassword[0] == '\0');
    openOk = false;
    const auto previousCalls = randomCalls;
    assert(!initializeApPassword() && randomCalls == previousCalls);
    puts("PASS: random alphabet, rejection sampling, persistence, and storage failures");
}
'''

with tempfile.TemporaryDirectory(prefix="alpha-buddy-password-") as directory:
    folder = Path(directory)
    (folder / "Preferences.h").write_text(FAKES)
    for name in ("bootloader_random.h", "esp_system.h"):
        (folder / name).write_text('#include "Preferences.h"\n')
    cpp = folder / "check.cpp"
    cpp.write_text(HARNESS)
    executable = folder / "check.exe"
    production = ROOT / "src/core/AccessPoint.cpp"
    if shutil.which("g++"):
        command = ["g++", "-std=c++17", "-I", str(folder), "-I", str(ROOT / "include"),
                   str(cpp), str(production), "-o", str(executable)]
    else:
        command = ["cl", "/nologo", "/std:c++17", "/EHsc", "/I" + str(folder),
                   "/I" + str(ROOT / "include"), str(cpp), str(production),
                   "/Fe" + str(executable)]
    subprocess.run(command, cwd=folder, check=True)
    subprocess.run([str(executable)], cwd=folder, check=True)
