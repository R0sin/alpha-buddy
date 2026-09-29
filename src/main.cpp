#ifndef ALPHA_BUDDY_NATIVE

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <cstring>

#include "alpha_buddy/Diagnostics.h"
#include "alpha_buddy/AccessPoint.h"
#include "alpha_buddy/JpegFrameSink.h"
#include "alpha_buddy/SerialProbeTrace.h"
#include "alpha_buddy/SonyLiveViewProbe.h"
#ifndef ALPHA_BUDDY_AP_SPIKE
#include "alpha_buddy/SonyLiveViewStream.h"
#include "alpha_buddy/StickS3FramePresenter.h"
#include "alpha_buddy/StickS3ShutterInput.h"
#endif
#include "alpha_buddy/WiFiPtpIpCommandPort.h"

namespace {

using alpha_buddy::kApSsid;
using alpha_buddy::apPassword;
constexpr uint8_t kApChannel = 1;
constexpr uint32_t kProbeRetryMs = 5000;
constexpr uint32_t kHeartbeatMs = 2000;
constexpr size_t kMaximumJpegBytes = 4U * 1024U * 1024U;
#ifndef ALPHA_BUDDY_AP_SPIKE
constexpr char kMilestone[] = "ap_live_view_runtime";
#else
constexpr char kMilestone[] = "ap_mode_spike";
#endif

volatile uint32_t assignedClientIpv4 = 0;
uint32_t activeClientIpv4 = 0;
uint32_t nextProbeAtMs = 0;
uint32_t nextHeartbeatAtMs = 0;
bool probeSucceeded = false;
bool apReady = false;

alpha_buddy::SerialProbeTrace trace;
alpha_buddy::JpegFrameSink frame(kMaximumJpegBytes);
#ifndef ALPHA_BUDDY_AP_SPIKE
alpha_buddy::StickS3ShutterInput shutterInput;
alpha_buddy::StickS3FramePresenter presenter;
#endif

void fillStableGuid(uint8_t guid[16]) {
    static constexpr uint8_t prefix[10] = {
        0x61, 0x6C, 0x70, 0x68, 0x61, 0x2D, 0x62, 0x75, 0x64, 0x79,
    };
    std::memcpy(guid, prefix, sizeof(prefix));
    esp_efuse_mac_get_default(guid + sizeof(prefix));
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
#ifndef ALPHA_BUDDY_AP_SPIKE
    if (event == ARDUINO_EVENT_WIFI_AP_START ||
        event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        presenter.setWaitingForWifi(true);
    } else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED ||
               event == ARDUINO_EVENT_WIFI_AP_STOP) {
        presenter.setWaitingForWifi(false);
    }
#endif
    if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
        assignedClientIpv4 = info.wifi_ap_staipassigned.ip.addr;
    } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        assignedClientIpv4 = 0;
#ifndef ALPHA_BUDDY_AP_SPIKE
        presenter.setConnection(alpha_buddy::CameraConnection::Disconnected);
#endif
    }
}

void emitHeartbeat(const char* state) {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextHeartbeatAtMs) < 0) {
        return;
    }
    nextHeartbeatAtMs = now + kHeartbeatMs;
    Serial.printf("{\"event\":\"ap_heartbeat\",\"state\":\"%s\"," 
                  "\"uptime_ms\":%lu}\n",
                  state,
                  static_cast<unsigned long>(now));
}

alpha_buddy::PtpSessionConfig makeSession(uint32_t cameraIpv4) {
    alpha_buddy::PtpSessionConfig session;
    session.cameraIpv4 = cameraIpv4;
    session.timeoutMs = 10000;
    session.friendlyName = "alpha-buddy";
    fillStableGuid(session.guid);
    return session;
}

#ifdef ALPHA_BUDDY_AP_SPIKE
alpha_buddy::ProbeReport runProbe(uint32_t cameraIpv4) {
    alpha_buddy::WiFiPtpIpCommandPort port;
    alpha_buddy::SonyLiveViewProbe probe(port, trace);
    alpha_buddy::ProbeOptions options;
    options.maximumAttempts = 1;
    return probe.run(makeSession(cameraIpv4), frame, options);
}

void emitSummary(uint32_t cameraIpv4, const alpha_buddy::ProbeReport& report) {
    const String cameraIp = IPAddress(cameraIpv4).toString();
    Serial.printf("{\"event\":\"ap_spike_summary\",\"success\":%s,"
                  "\"camera_ip\":\"%s\",\"failed_stage\":\"%s\","
                  "\"transport_error\":\"%s\",\"opcode\":\"0x%04X\","
                  "\"response\":\"0x%04X\",\"jpeg_bytes\":%lu,"
                  "\"jpeg_width\":%u,\"jpeg_height\":%u,\"elapsed_ms\":%lu}\n",
                  report.success ? "true" : "false",
                  cameraIp.c_str(),
                  alpha_buddy::probeStageName(report.failedStage),
                  alpha_buddy::transportErrorName(report.transportError),
                  static_cast<unsigned>(report.operationCode),
                  static_cast<unsigned>(report.responseCode),
                  static_cast<unsigned long>(report.jpegBytes),
                  static_cast<unsigned>(report.jpegWidth),
                  static_cast<unsigned>(report.jpegHeight),
                  static_cast<unsigned long>(report.elapsedMs));
}
#else
alpha_buddy::StreamReport runPreview(uint32_t cameraIpv4) {
    alpha_buddy::WiFiPtpIpCommandPort port;
    alpha_buddy::SonyLiveViewStream stream(port, trace);
    alpha_buddy::StreamOptions options;
    options.durationMs = 0;
    options.preferLowResolution = true;
    options.readCameraModes = true;
    return stream.run(makeSession(cameraIpv4),
                      frame,
                      presenter,
                      options,
                      &shutterInput);
}

void emitStreamSummary(uint32_t cameraIpv4,
                       const alpha_buddy::StreamReport& report) {
    const String cameraIp = IPAddress(cameraIpv4).toString();
    Serial.printf("{\"event\":\"ap_stream_summary\",\"camera_ip\":\"%s\","
                  "\"failed_stage\":\"%s\",\"transport_error\":\"%s\","
                  "\"opcode\":\"0x%04X\",\"response\":\"0x%04X\","
                  "\"frames\":%lu,\"frame_failures\":%lu,\"elapsed_ms\":%lu}\n",
                  cameraIp.c_str(),
                  alpha_buddy::probeStageName(report.failedStage),
                  alpha_buddy::transportErrorName(report.transportError),
                  static_cast<unsigned>(report.operationCode),
                  static_cast<unsigned>(report.responseCode),
                  static_cast<unsigned long>(report.framesPresented),
                  static_cast<unsigned long>(report.frameFailures),
                  static_cast<unsigned long>(report.elapsedMs));
}
#endif

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(500);

    const bool passwordReady = alpha_buddy::initializeApPassword();

    auto m5Config = M5.config();
    M5.begin(m5Config);
    M5.update();

    Serial.printf("{\"event\":\"boot\",\"project\":\"alpha-buddy\","
                  "\"milestone\":\"%s\"}\n",
                  kMilestone);
    if (!passwordReady) {
        Serial.println("{\"event\":\"ap_setup\",\"success\":false,"
                       "\"failed_stage\":\"ap_password_storage\"}");
        return;
    }
    if (!psramFound()) {
        Serial.println("{\"event\":\"ap_setup\",\"success\":false,"
                       "\"failed_stage\":\"psram\"}");
        return;
    }
#ifndef ALPHA_BUDDY_AP_SPIKE
    if (!shutterInput.begin() || !presenter.begin(shutterInput)) {
        Serial.println("{\"event\":\"ap_setup\",\"success\":false,"
                       "\"failed_stage\":\"runtime_io\"}");
        return;
    }
#endif

    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP);
    apReady = WiFi.softAP(kApSsid, apPassword, kApChannel, false, 1);
    Serial.printf("{\"event\":\"ap_setup\",\"success\":%s,"
                  "\"ssid\":\"%s\",\"ap_ip\":\"%s\",\"channel\":%u}\n",
                  apReady ? "true" : "false",
                  kApSsid,
                  WiFi.softAPIP().toString().c_str(),
                  static_cast<unsigned>(kApChannel));
}

void loop() {
    M5.update();
    if (!apReady) {
        emitHeartbeat("ap_failed");
        delay(20);
        return;
    }

    const uint32_t observedIpv4 = assignedClientIpv4;
    if (observedIpv4 == 0) {
        if (activeClientIpv4 != 0) {
            Serial.println("{\"event\":\"ap_client\",\"status\":\"disconnected\"}");
            activeClientIpv4 = 0;
            probeSucceeded = false;
            nextProbeAtMs = 0;
        }
        emitHeartbeat("waiting_for_camera");
        delay(20);
        return;
    }

    if (observedIpv4 != activeClientIpv4) {
        activeClientIpv4 = observedIpv4;
        probeSucceeded = false;
        nextProbeAtMs = 0;
        Serial.printf("{\"event\":\"ap_client\",\"status\":\"ready\","
                      "\"camera_ip\":\"%s\"}\n",
                      IPAddress(activeClientIpv4).toString().c_str());
    }

#ifdef ALPHA_BUDDY_AP_SPIKE
    if (probeSucceeded) {
        emitHeartbeat("probe_passed");
        delay(20);
        return;
    }
#endif

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextProbeAtMs) < 0) {
        emitHeartbeat("retry_wait");
        delay(20);
        return;
    }

#ifdef ALPHA_BUDDY_AP_SPIKE
    Serial.printf("{\"event\":\"ap_probe\",\"status\":\"started\","
                  "\"camera_ip\":\"%s\"}\n",
                  IPAddress(activeClientIpv4).toString().c_str());
    const alpha_buddy::ProbeReport report = runProbe(activeClientIpv4);
    emitSummary(activeClientIpv4, report);
    probeSucceeded = report.success;
#else
    Serial.printf("{\"event\":\"ap_stream\",\"status\":\"started\","
                  "\"camera_ip\":\"%s\"}\n",
                  IPAddress(activeClientIpv4).toString().c_str());
    presenter.setCaptureFeedback(alpha_buddy::CaptureFeedback::Live);
    const alpha_buddy::StreamReport report = runPreview(activeClientIpv4);
    emitStreamSummary(activeClientIpv4, report);
    presenter.setCaptureFeedback(alpha_buddy::CaptureFeedback::Failure);
#endif
    nextProbeAtMs = millis() + kProbeRetryMs;
}

#endif
