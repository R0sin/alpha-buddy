"""Run the production HUD refresh method without a camera or ESP32.

Requires g++ on PATH, or the Visual Studio C++ developer shell (cl).
Only hardware drawing is stubbed; the method is extracted unchanged so this
checks the no-JPEG path, rather than a duplicate of its rotation logic.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/ui/StickS3FramePresenter.cpp").read_text()
start = source.index("    void refreshHud() {")
end = source.index("    void drawConnectionHint(", start)
method = source[start:end]

harness = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include "alpha_buddy/AccessPoint.h"
#include "alpha_buddy/LandscapeOrientationTracker.h"
#include "alpha_buddy/CameraModes.h"
using namespace alpha_buddy;
namespace alpha_buddy { char apPassword[9] = "a2b3c4d5"; }
constexpr int kStatusWidth = 36, TFT_BLACK = 0, TFT_WHITE = 65535;
namespace fonts { int Font0; }
uint32_t millis() { return 2000; }
struct FakeDisplay {
    int rotation = 1, clears = 0;
    int areaClears = 0, cursorX = 0;
    std::string text;
    int width() { return 240; }
    int height() { return 135; }
    void setRotation(int value) { rotation = value; }
    void fillScreen(int) { ++clears; text.clear(); }
    void fillRect(int, int, int, int, int) { ++areaClears; text.clear(); }
    void setFont(int*) {}
    void setTextSize(int) {}
    void setTextColor(int, int) {}
    void setCursor(int x, int) { cursorX = x; }
    void printf(const char* format, const char* value) {
        char line[80];
        std::snprintf(line, sizeof(line), format, value);
        assert(cursorX + std::string(line).size() * 12 <= (rotation == 1 ? 204 : 240));
        text += line;
    }
    void startWrite() {}
    void endWrite() {}
    void waitDMA() {}
};
struct { FakeDisplay Display; } M5;
struct Presenter {
    std::atomic<CameraConnection> _connection{CameraConnection::Disconnected};
    std::atomic<bool> _waitingForWifi{false};
    bool _credentialsVisible = false;
    std::atomic<bool> _previewPaused{false};
    std::atomic<uint32_t> _resumedAtMs{0}, _session{0}, _cameraModes{0}, _modesUpdatedAt{0};
    uint32_t _lastFrameMs = 0, _visibleSession = 0, _drawnModes = 0;
    bool _previewVisible = false, _hudDirty = true, _layoutDirty = false;
    bool _blink = false, _modesVisible = false, _cancelPending = false;
    ViewfinderHint _hint = ViewfinderHint::None;
    LandscapeOrientationTracker _orientation;
    LandscapeOrientation _appliedOrientation = LandscapeOrientation::Rotation1;
    struct { bool cancelPending() { return false; } } _shutterInput;
    int _drawnBorderColor = 0, blits = 0, hudDraws = 0;
    int frameBorderColor() { return 0; }
    void blitCanvas() {
        assert(canDisplayPreview(_connection.load(), _visibleSession, _session.load()));
        ++blits;
    }
    void drawBatteryHud(LandscapeOrientation, bool, int, int) { ++hudDraws; }
    void drawCameraModes(bool, uint32_t) {}
    void drawConnectionHint(bool) {}
    // PRODUCTION_METHOD
};
int main() {
    // No draw(JPEG) call: disconnected, handshake, connected but no first frame,
    // stalled/paused retained frame, and a retained frame from an old session.
    for (int scenario = 0; scenario < 7; ++scenario) {
        M5.Display = {};
        Presenter p;
        p._connection = scenario == 0 ? CameraConnection::Disconnected :
                        scenario == 1 ? CameraConnection::Connecting : CameraConnection::Connected;
        p._previewVisible = scenario >= 3;
        p._previewPaused = scenario == 4;
        if (scenario == 5) p._session = 1;
        if (scenario == 6) p._connection = CameraConnection::Disconnected;
        p.refreshHud();
        p._orientation.update(-0.9f, 0.0f, 0, false);
        p._layoutDirty = true;
        p.refreshHud();
        assert(M5.Display.rotation == 3);
        assert(p._appliedOrientation == LandscapeOrientation::Rotation3);
        assert(p.hudDraws == 2);
        assert(p.blits == ((scenario == 3 || scenario == 4) ? 1 : 0));
        const int clears = M5.Display.clears;
        p.refreshHud();
        assert(M5.Display.clears == clears && p.hudDraws == 2);
        // Back to the original orientation without any new frame.
        for (uint32_t now = 40; now <= 1200; now += 40)
            p._orientation.update(0.9f, 0.0f, now, false);
        p._layoutDirty = true;
        p.refreshHud();
        assert(M5.Display.rotation == 1 && p.hudDraws == 3);
        assert(p.blits == ((scenario == 3 || scenario == 4) ? 2 : 0));
    }
    M5.Display = {};
    Presenter p;
    p._waitingForWifi = true;
    p.refreshHud();
    assert(M5.Display.text == "SSID alpha-buddyPASS a2b3c4d5");
    p._orientation.update(-0.9f, 0.0f, 0, false);
    p._layoutDirty = true;
    p.refreshHud();
    assert(M5.Display.rotation == 3 && M5.Display.cursorX == 42);
    p._waitingForWifi = false; // Wi-Fi association, before PTP starts.
    p.refreshHud();
    assert(M5.Display.text.empty() && !p._credentialsVisible);
    p._connection = CameraConnection::Connecting;
    p.refreshHud();
    p._connection = CameraConnection::Disconnected; // PTP retry with Wi-Fi intact.
    p.refreshHud();
    assert(M5.Display.text.empty());
    p._waitingForWifi = true;
    p.refreshHud();
    assert(M5.Display.text == "SSID alpha-buddyPASS a2b3c4d5");
    puts("PASS: rotation, retained preview, invalid sessions, and Wi-Fi credentials");
}
'''.replace("    // PRODUCTION_METHOD", method)

with tempfile.TemporaryDirectory(prefix="alpha-buddy-refresh-") as directory:
    folder = Path(directory)
    cpp = folder / "refresh.cpp"
    cpp.write_text(harness)
    executable = folder / "refresh.exe"
    if shutil.which("g++"):
        command = ["g++", "-std=c++17", "-I", str(ROOT / "include"), str(cpp), "-o", str(executable)]
    else:
        command = ["cl", "/nologo", "/std:c++17", "/EHsc", "/I" + str(ROOT / "include"),
                   str(cpp), "/Fe" + str(executable)]
    subprocess.run(command, cwd=folder, check=True)
    subprocess.run([str(executable)], cwd=folder, check=True)
