#include <unity.h>
#include "alpha_buddy/GridExtent.h"
#include "alpha_buddy/SideButtonDoubleClick.h"
#include "alpha_buddy/FlipCancelGesture.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "alpha_buddy/Diagnostics.h"
#include "alpha_buddy/DisconnectedShutdown.h"
#include "alpha_buddy/JpegFrameSink.h"
#include "alpha_buddy/LandscapeOrientationTracker.h"
#include "alpha_buddy/PtpIpProtocol.h"
#include "alpha_buddy/SonyLiveViewProbe.h"
#include "alpha_buddy/SonyLiveViewStream.h"
#include "alpha_buddy/ModeIcon.h"

using namespace alpha_buddy;

extern "C" void setUp() {}
extern "C" void tearDown() {}

namespace {

void test_grid_reaches_image_edges_in_both_orientations() {
    // 640x424 JPEG decoded at quarter size, displayed in a 204x135 viewport.
    const float zoom = 135.0f / 106;
    const GridExtent y = gridExtent(67.5f, 106, zoom);
    TEST_ASSERT_EQUAL_INT32(0, y.first); // Previous pixel-center bounds started at 1.
    TEST_ASSERT_EQUAL_INT32(134, y.last);
    const GridExtent x = gridExtent(102, 160, zoom);
    TEST_ASSERT_EQUAL_INT32(1, x.first);
    TEST_ASSERT_EQUAL_INT32(203, x.last);
    const GridExtent flipped = gridExtent(138, 160, zoom);
    TEST_ASSERT_EQUAL_INT32(x.first + 36, flipped.first);
    TEST_ASSERT_EQUAL_INT32(x.last + 36, flipped.last);

    // Width-limited 16:9 preview leaves black bars; grid must stay in the image.
    const GridExtent wide = gridExtent(102, 160, 204.0f / 160);
    const GridExtent letterbox = gridExtent(67.5f, 90, 204.0f / 160);
    TEST_ASSERT_EQUAL_INT32(0, wide.first);
    TEST_ASSERT_EQUAL_INT32(203, wide.last);
    TEST_ASSERT_EQUAL_INT32(11, letterbox.first);
    TEST_ASSERT_EQUAL_INT32(124, letterbox.last);
}


void test_flip_cancel_requires_continuity_and_latches_until_next_press() {
    FlipCancelGesture gesture;
    const auto normal = LandscapeOrientation::Rotation1;
    const auto reverse = LandscapeOrientation::Rotation3;
    gesture.begin(true, normal);
    for (uint32_t t = 0; t <= 1000; t += 100) gesture.observe(true, reverse, t);
    TEST_ASSERT_FALSE(gesture.cancelled());
    TEST_ASSERT_TRUE(gesture.counting(1000));
    TEST_ASSERT_FALSE(gesture.counting(1151)); // Stale IMU must not keep flashing.
    gesture.observe(true, reverse, 1040);
    TEST_ASSERT_TRUE(gesture.cancelled());
    TEST_ASSERT_FALSE(gesture.counting(1040));
    gesture.observe(true, normal, 1080);
    gesture.observe(false, normal, 1120);
    TEST_ASSERT_TRUE(gesture.cancelled());

    gesture.begin(true, normal);
    TEST_ASSERT_FALSE(gesture.cancelled());
    for (uint32_t t = 0; t <= 900; t += 100) gesture.observe(true, reverse, t);
    gesture.observe(false, reverse, 940);
    TEST_ASSERT_FALSE(gesture.counting(940));
    for (uint32_t t = 980; t <= 1980; t += 100) gesture.observe(true, reverse, t);
    TEST_ASSERT_FALSE(gesture.cancelled());
    gesture.observe(true, normal, 2020); // Returning early also resets.
    gesture.observe(true, reverse, 2060);
    gesture.observe(true, reverse, 4000); // Missing IMU samples cannot cancel.
    TEST_ASSERT_FALSE(gesture.cancelled());

    gesture.begin(false, normal); // Unresolved startup orientation is not a gesture.
    for (uint32_t t = 0; t <= 2000; t += 100) gesture.observe(true, reverse, t);
    TEST_ASSERT_FALSE(gesture.cancelled());
    gesture.begin(true, reverse);
    constexpr uint32_t start = UINT32_MAX - 500;
    for (uint32_t t = 0; t <= 1100; t += 100) gesture.observe(true, normal, start + t);
    TEST_ASSERT_TRUE(gesture.cancelled());
}

void test_side_button_double_click_timing_holds_and_wrap() {
    SideButtonDoubleClick click;
    TEST_ASSERT_FALSE(click.update(true, 0));
    TEST_ASSERT_FALSE(click.update(false, 50));
    TEST_ASSERT_FALSE(click.update(true, 350));
    TEST_ASSERT_TRUE(click.update(false, 650)); // Both 300ms boundaries accepted.
    TEST_ASSERT_FALSE(click.update(false, 660));
    TEST_ASSERT_FALSE(click.update(true, 700));
    TEST_ASSERT_FALSE(click.update(false, 750)); // Triple click is only one toggle.
    TEST_ASSERT_FALSE(click.update(true, 1051)); // Gap expired.
    TEST_ASSERT_FALSE(click.update(false, 1100));
    TEST_ASSERT_FALSE(click.update(true, 1150));
    TEST_ASSERT_FALSE(click.update(false, 1451)); // Long second press cancels.
    TEST_ASSERT_FALSE(click.update(true, 1500));
    TEST_ASSERT_FALSE(click.update(false, 1550));
    TEST_ASSERT_FALSE(click.update(true, 1600));
    TEST_ASSERT_TRUE(click.update(false, 1650));

    SideButtonDoubleClick wrap;
    TEST_ASSERT_FALSE(wrap.update(true, UINT32_MAX - 100));
    TEST_ASSERT_FALSE(wrap.update(false, UINT32_MAX - 50));
    TEST_ASSERT_FALSE(wrap.update(true, 20));
    TEST_ASSERT_TRUE(wrap.update(false, 60));
    SideButtonDoubleClick hold;
    TEST_ASSERT_FALSE(hold.update(true, 0));
    TEST_ASSERT_FALSE(hold.update(true, 1000));
    TEST_ASSERT_FALSE(hold.update(false, 1100));
    TEST_ASSERT_FALSE(hold.update(true, 1150));
    TEST_ASSERT_FALSE(hold.update(false, 1200));
}

void test_disconnected_shutdown_deadline_resets_and_clock_wrap() {
    DisconnectedShutdown timer;
    TEST_ASSERT_FALSE(timer.update(0, false, false, false));
    // Failed handshakes/retries do not extend the original deadline.
    TEST_ASSERT_FALSE(timer.update(5000, false, false, false));
    TEST_ASSERT_FALSE(timer.update(299999, false, false, false));
    TEST_ASSERT_TRUE(timer.update(300000, false, false, false));

    // Input at the deadline wins; its new timeout is a full five minutes.
    TEST_ASSERT_FALSE(timer.update(300000, false, false, true));
    TEST_ASSERT_FALSE(timer.update(599999, false, false, false));
    TEST_ASSERT_TRUE(timer.update(600000, false, false, false));

    // An established session stays alive without requiring frames or input.
    TEST_ASSERT_FALSE(timer.update(600000, true, false, false));
    TEST_ASSERT_FALSE(timer.update(1200000, true, false, false));
    TEST_ASSERT_FALSE(timer.update(1200001, false, false, false));
    TEST_ASSERT_FALSE(timer.update(1500000, false, false, false));
    TEST_ASSERT_TRUE(timer.update(1500001, false, false, false));

    // USB insertion at the deadline inhibits shutdown; unplug starts fresh.
    TEST_ASSERT_FALSE(timer.update(1500001, false, true, false));
    TEST_ASSERT_FALSE(timer.update(2400000, false, true, false));
    TEST_ASSERT_FALSE(timer.update(2400001, false, false, false));
    TEST_ASSERT_FALSE(timer.update(2700000, false, false, false));
    TEST_ASSERT_TRUE(timer.update(2700001, false, false, false));

    timer = DisconnectedShutdown{};
    constexpr uint32_t start = UINT32_MAX - 1000U;
    TEST_ASSERT_FALSE(timer.update(start, false, false, false));
    TEST_ASSERT_FALSE(timer.update(start + 299999U, false, false, false));
    TEST_ASSERT_TRUE(timer.update(start + 300000U, false, false, false));
}

class CapturingTrace final : public IProbeTrace {
public:
    void record(const ProbeEvent& event) override { events.push_back(event); }
    std::vector<ProbeEvent> events;
};

class FakePort final : public IPtpIpCommandPort {
public:
    PortOpenResult open(const PtpSessionConfig&) override {
        ++openCalls;
        PortOpenResult result;
        result.elapsedMs = 4;
        if (openCalls <= failOpenCount) {
            result.failedStage = ProbeStage::InitCommand;
            result.transportError = TransportError::Timeout;
            return result;
        }
        result.ok = true;
        result.failedStage = ProbeStage::Complete;
        return result;
    }

    OperationResult execute(uint16_t operationCode,
                            const uint32_t*,
                            size_t,
                            IDataSink* sink) override {
        operations.push_back(operationCode);
        OperationResult result;
        result.transportOk = true;
        result.responseCode = 0x2001;
        result.elapsedMs = 2;
        if (operationCode == 0x9209) {
            result.transportOk = modeTransportOk;
            result.responseCode = modeResponse;
            result.transportError = modeTransportOk ? TransportError::None : TransportError::SocketClosed;
            if (result.transportOk && modeResponse == 0x2001 && sink && !modeData.empty()) {
                sink->begin(modeData.size());
                sink->write(modeData.data(), modeData.size());
                sink->finish();
            }
            return result;
        }
        if (operationCode == SonyLiveViewProbe::kOperationGetObjectInfo &&
            failGetObjectInfoTransportCalls > 0) {
            --failGetObjectInfoTransportCalls;
            result.transportOk = false;
            result.transportError = TransportError::SocketClosed;
            result.responseCode = 0;
            return result;
        }
        if (operationCode == SonyLiveViewProbe::kOperationGetObject &&
            deniedGetObjectCalls > 0) {
            --deniedGetObjectCalls;
            result.responseCode = 0x200F;
            return result;
        }
        if (operationCode == SonyLiveViewProbe::kOperationGetObject && sink != nullptr) {
            const uint8_t valid[] = {
                0xFF, 0xD8,
                0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x02, 0x00, 0x03,
                0x01, 0x01, 0x11, 0x00,
                0xFF, 0xD9,
            };
            const uint8_t invalid[] = {
                0x00, 0xD8,
                0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x02, 0x00, 0x03,
                0x01, 0x01, 0x11, 0x00,
                0xFF, 0xD9,
            };
            const uint8_t* payload = invalidJpeg ? invalid : valid;
            sink->begin(sizeof(valid));
            sink->write(payload, sizeof(valid));
            sink->finish();
            result.dataBytes = sizeof(valid);
        }
        return result;
    }

    OperationResult executeWithData(uint16_t operationCode,
                                    const uint32_t* parameters,
                                    size_t parameterCount,
                                    const uint8_t* data,
                                    size_t dataBytes) override {
        operations.push_back(operationCode);
        writtenParameters.assign(parameters, parameters + parameterCount);
        writtenData.assign(data, data + dataBytes);
        if (parameterCount == 1 && dataBytes == 2) controls.push_back({parameters[0], data[0]});
        OperationResult result;
        result.transportOk = writeTransportOk;
        result.responseCode = writeResponse;
        if (rejectAfRelease && parameterCount == 1 && parameters[0] == 0xD2C1 &&
            dataBytes == 2 && data[0] == 1) result.responseCode = 0x200F;
        result.transportError = writeTransportOk ? TransportError::None : TransportError::SocketClosed;
        result.elapsedMs = 2;
        return result;
    }

    void close() override { ++closeCalls; }

    int failOpenCount = 0;
    bool modeTransportOk = true;
    uint16_t modeResponse = 0x2001;
    std::vector<uint8_t> modeData;
    bool writeTransportOk = true;
    bool rejectAfRelease = false;
    uint16_t writeResponse = 0x2001;
    std::vector<uint32_t> writtenParameters;
    std::vector<uint8_t> writtenData;
    std::vector<std::array<uint32_t, 2>> controls;
    bool invalidJpeg = false;
    int deniedGetObjectCalls = 0;
    int failGetObjectInfoTransportCalls = 0;
    int openCalls = 0;
    int closeCalls = 0;
    std::vector<uint16_t> operations;
};

class FakePresenter final : public IFramePresenter {
public:
    bool present(const uint8_t*, size_t, uint16_t, uint16_t) override {
        ++calls;
        return accept && calls <= acceptFrames;
    }

    void setConnection(CameraConnection connection) override { connections.push_back(connection); }
    void setPreviewPaused(bool paused) override { pauses.push_back(paused); }
    void setCameraModes(CameraModes modes) override { modeUpdates.push_back(modes.packed()); }

    std::vector<CameraConnection> connections;
    std::vector<bool> pauses;
    std::vector<uint32_t> modeUpdates;
    bool accept = true;
    uint32_t acceptFrames = UINT32_MAX;
    uint32_t calls = 0;
};


std::vector<uint8_t> modeReply(uint16_t focus = 0x8004) {
    // Captured target-camera types and current values, with empty enum lists.
    std::vector<uint8_t> data;
    const auto put = [&](uint32_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) data.push_back(uint8_t(value >> (8 * i)));
    };
    put(3, 4); put(0, 4);
    const uint32_t props[][3] = {{0x500E, 6, 0x00020003}, {0x500A, 4, focus}, {0xD0DB, 2, 2}};
    for (const auto& prop : props) {
        put(prop[0], 2); put(prop[1], 2); put(0x0200, 2);
        const unsigned width = 1u << ((prop[1] - 1) / 2);
        put(0, width); put(prop[2], width);
        put(2, 1); put(0, 2); put(0, 2);
    }
    return data;
}

void test_camera_mode_wire_values_and_rejects_partial_snapshots() {
    const auto data = modeReply();
    CameraModes modes;
    TEST_ASSERT_TRUE(decodeCameraModes(data.data(), data.size(), modes));
    TEST_ASSERT_EQUAL(ExposureMode::A, modes.exposure);
    TEST_ASSERT_EQUAL(FocusMode::C, modes.focus);
    TEST_ASSERT_EQUAL(SilentMode::On, modes.silent);
    auto shutterOnly = data;
    shutterOnly[shutterOnly.size() - 13] = 0xDF; // ShutterType 0xD0DF is not SilentMode.
    TEST_ASSERT_TRUE(decodeCameraModes(shutterOnly.data(), shutterOnly.size(), modes));
    TEST_ASSERT_EQUAL(SilentMode::Unknown, modes.silent);
    TEST_ASSERT_EQUAL(ExposureMode::A, modes.exposure);
    for (size_t n = 0; n < data.size(); ++n) {
        TEST_ASSERT_FALSE(decodeCameraModes(data.data(), n, modes));
        TEST_ASSERT_EQUAL_UINT32(0, modes.packed());
    }
    auto trailing = data; trailing.push_back(0);
    TEST_ASSERT_FALSE(decodeCameraModes(trailing.data(), trailing.size(), modes));
    TEST_ASSERT_EQUAL(ExposureMode::Unknown, exposureMode(0x8020));
    TEST_ASSERT_EQUAL(FocusMode::Unknown, focusMode(999));
    TEST_ASSERT_EQUAL(SilentMode::Off, silentMode(1));
    TEST_ASSERT_EQUAL(SilentMode::On, silentMode(2));
    TEST_ASSERT_EQUAL(SilentMode::Unknown, silentMode(3));
    TEST_ASSERT_EQUAL(SilentMode::Unknown, silentMode(0));
    TEST_ASSERT_EQUAL_UINT32(0, visibleCameraModes(CameraConnection::Disconnected, 0x010202, 10, 9));
    TEST_ASSERT_EQUAL_UINT32(0, visibleCameraModes(CameraConnection::Connected, 0x010202, 4000, 0));
    TEST_ASSERT_EQUAL_UINT32(0x010202, visibleCameraModes(CameraConnection::Connected, 0x010202, 4, UINT32_MAX - 10));
}

void test_stream_mode_read_success_rejection_and_transport_failure() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        FakePort port; port.modeData = modeReply();
        if (scenario == 1) port.modeResponse = 0x2005;
        if (scenario == 2) port.modeTransportOk = false;
        CapturingTrace trace;
        SonyLiveViewStream stream(port, trace);
        JpegFrameSink frame(128);
        FakePresenter presenter; presenter.acceptFrames = 1;
        StreamOptions options; options.readCameraModes = true;
        const auto report = stream.run({}, frame, presenter, options);
        TEST_ASSERT_EQUAL_UINT32(1, port.closeCalls);
        TEST_ASSERT_EQUAL_UINT32(0, presenter.modeUpdates.front());
        TEST_ASSERT_EQUAL_UINT32(scenario == 0 ? 0x020202 : 0, presenter.modeUpdates.back());
        if (scenario == 2) {
            TEST_ASSERT_EQUAL_UINT32(0, report.framesPresented);
            TEST_ASSERT_EQUAL(TransportError::SocketClosed, report.transportError);
        } else TEST_ASSERT_GREATER_THAN_UINT32(0, report.framesPresented);
    }
}

void test_viewfinder_hint_timeout_pause_and_connection_priority() {
    TEST_ASSERT_TRUE(canDisplayPreview(CameraConnection::Connected, 1, 1));
    TEST_ASSERT_FALSE(canDisplayPreview(CameraConnection::Disconnected, 1, 1));
    TEST_ASSERT_FALSE(canDisplayPreview(CameraConnection::Connecting, 1, 1));
    TEST_ASSERT_FALSE(canDisplayPreview(CameraConnection::Connected, 1, 2));
    TEST_ASSERT_EQUAL(ViewfinderHint::None,
        viewfinderHint(CameraConnection::Connected, false, 1500, 500, 0));
    TEST_ASSERT_EQUAL(ViewfinderHint::Stalled,
        viewfinderHint(CameraConnection::Connected, false, 1501, 500, 0));
    TEST_ASSERT_EQUAL(ViewfinderHint::None,
        viewfinderHint(CameraConnection::Connected, true, 9000, 500, 0));
    TEST_ASSERT_EQUAL(ViewfinderHint::None,
        viewfinderHint(CameraConnection::Connected, false, 9500, 500, 9000));
    TEST_ASSERT_EQUAL(ViewfinderHint::Stalled,
        viewfinderHint(CameraConnection::Connected, false, 10001, 500, 9000));
    TEST_ASSERT_EQUAL(ViewfinderHint::None,
        viewfinderHint(CameraConnection::Connected, false, 10001, 10000, 9000));
    TEST_ASSERT_EQUAL(ViewfinderHint::Waiting,
        viewfinderHint(CameraConnection::Disconnected, true, 9000, 500, 0));
    TEST_ASSERT_EQUAL(ViewfinderHint::Waiting,
        viewfinderHint(CameraConnection::Connecting, false, 9000, 500, 0));
    TEST_ASSERT_EQUAL(ViewfinderHint::None,
        viewfinderHint(CameraConnection::Connected, false, 100, UINT32_MAX - 100, UINT32_MAX - 100));
    TEST_ASSERT_EQUAL(ViewfinderHint::Stalled,
        viewfinderHint(CameraConnection::Connected, false, 901, UINT32_MAX - 100, UINT32_MAX - 100));
}

void test_stream_connection_lifecycle_and_capture_pause() {
    class Shutter final : public IShutterInput {
    public:
        ShutterInputEvents poll() override {
            ++calls;
            return {calls == 1, calls == 2};
        }
        int calls = 0;
    } shutter;
    FakePort port;
    CapturingTrace trace;
    FakePresenter presenter;
    presenter.acceptFrames = 2;
    JpegFrameSink frame(64);
    SonyLiveViewStream stream(port, trace);
    const auto report = stream.run({}, frame, presenter, {}, &shutter);
    TEST_ASSERT_EQUAL_UINT32(1, report.shutterSuccesses);
    TEST_ASSERT_EQUAL_UINT32(3, presenter.connections.size());
    TEST_ASSERT_EQUAL(CameraConnection::Connecting, presenter.connections[0]);
    TEST_ASSERT_EQUAL(CameraConnection::Connected, presenter.connections[1]);
    TEST_ASSERT_EQUAL(CameraConnection::Disconnected, presenter.connections[2]);
    TEST_ASSERT_EQUAL_UINT32(4, presenter.pauses.size());
    TEST_ASSERT_TRUE(presenter.pauses[0]);
    TEST_ASSERT_FALSE(presenter.pauses[1]);
    TEST_ASSERT_TRUE(presenter.pauses[2]);
    TEST_ASSERT_FALSE(presenter.pauses[3]);

    port.failOpenCount = 2;
    presenter.connections.clear();
    stream.run({}, frame, presenter, {});
    TEST_ASSERT_EQUAL_UINT32(2, presenter.connections.size());
    TEST_ASSERT_EQUAL(CameraConnection::Connecting, presenter.connections[0]);
    TEST_ASSERT_EQUAL(CameraConnection::Disconnected, presenter.connections[1]);
}

void test_stream_mf_and_cancel_never_send_unwanted_controls() {
    for (bool mf : {false, true}) {
        for (bool cancel : {false, true}) {
            class Shutter final : public IShutterInput {
            public:
                ShutterInputEvents poll() override {
                    ++calls;
                    if (calls == 2) {
                        for (const auto& control : port->controls)
                            TEST_ASSERT_NOT_EQUAL(0xD2C2, control[0]);
                    }
                    return {calls == 1, calls == 3, calls == 3 && cancel};
                }
                FakePort* port = nullptr;
                bool cancel = false;
                int calls = 0;
            } shutter;
            shutter.cancel = cancel;
            FakePort port; port.modeData = modeReply(mf ? 1 : 0x8004);
            shutter.port = &port;
            CapturingTrace trace;
            FakePresenter presenter; presenter.acceptFrames = 2;
            JpegFrameSink frame(64);
            StreamOptions options; options.readCameraModes = true;
            SonyLiveViewStream stream(port, trace);
            const auto report = stream.run({}, frame, presenter, options, &shutter);
            TEST_ASSERT_EQUAL_UINT32(cancel ? 0 : 1, report.shutterAttempts);
            TEST_ASSERT_EQUAL_UINT32(cancel ? 0 : 1, report.shutterSuccesses);
            std::vector<std::array<uint32_t, 2>> expected;
            if (!mf) expected.push_back({0xD2C1, 2});
            if (!cancel) {
                expected.push_back({0xD2C2, 2});
                expected.push_back({0xD2C2, 1});
            }
            if (!mf) expected.push_back({0xD2C1, 1});
            TEST_ASSERT_TRUE(expected == port.controls);
        }
    }
}

void test_cancel_cleanup_failure_closes_session_without_capture() {
    class Shutter final : public IShutterInput {
    public:
        ShutterInputEvents poll() override { return {true, true, true}; }
    } shutter;
    FakePort port; port.rejectAfRelease = true;
    CapturingTrace trace;
    FakePresenter presenter;
    JpegFrameSink frame(64);
    SonyLiveViewStream stream(port, trace);
    const auto report = stream.run({}, frame, presenter, {}, &shutter);
    TEST_ASSERT_EQUAL_UINT32(0, report.shutterAttempts);
    TEST_ASSERT_EQUAL_UINT32(1, port.closeCalls);
    TEST_ASSERT_EQUAL(ProbeStage::Autofocus, report.failedStage);
    TEST_ASSERT_EQUAL_UINT16(0x200F, report.responseCode);
    TEST_ASSERT_EQUAL_UINT32(2, port.controls.size());
    for (const auto& control : port.controls) TEST_ASSERT_EQUAL_UINT32(0xD2C1, control[0]);
}

void test_stream_requests_small_live_view_once_and_tolerates_rejection() {
    for (const uint16_t response : {0x2001, 0x200A}) {
        FakePort port;
        port.writeResponse = response;
        CapturingTrace trace;
        FakePresenter presenter;
        presenter.acceptFrames = 3;
        JpegFrameSink frame(64);
        StreamOptions options;
        options.preferLowResolution = true;
        SonyLiveViewStream stream(port, trace);
        const auto report = stream.run({}, frame, presenter, options);
        TEST_ASSERT_EQUAL_UINT32(1, port.writtenParameters.size());
        TEST_ASSERT_EQUAL_HEX32(0xD26A, port.writtenParameters[0]);
        TEST_ASSERT_EQUAL_UINT32(1, port.writtenData.size());
        TEST_ASSERT_EQUAL_UINT8(1, port.writtenData[0]);
        TEST_ASSERT_EQUAL_HEX16(0x9205, port.operations[7]);
        TEST_ASSERT_EQUAL_UINT32(4, presenter.calls);
        TEST_ASSERT_EQUAL_UINT32(7 + 1 + 2 * presenter.calls, port.operations.size());
        TEST_ASSERT_EQUAL_UINT32(3, report.framesPresented);
        TEST_ASSERT_EQUAL_UINT8(response == 0x2001 ? 0 : 1, report.warnings);
    }
}

void test_stream_quality_transport_failure_closes_session() {
    FakePort port;
    port.writeTransportOk = false;
    CapturingTrace trace;
    FakePresenter presenter;
    presenter.accept = false;
    JpegFrameSink frame(64);
    StreamOptions options;
    options.preferLowResolution = true;
    SonyLiveViewStream stream(port, trace);
    const auto report = stream.run({}, frame, presenter, options);
    TEST_ASSERT_EQUAL(TransportError::SocketClosed, report.transportError);
    TEST_ASSERT_EQUAL_UINT32(0, presenter.calls);
    TEST_ASSERT_EQUAL_UINT32(1, port.closeCalls);
    TEST_ASSERT_EQUAL_UINT32(2, presenter.connections.size());
    TEST_ASSERT_EQUAL(CameraConnection::Disconnected, presenter.connections.back());
}

void test_init_command_packet_matches_wire_shape() {
    uint8_t guid[16] = {};
    for (size_t index = 0; index < sizeof(guid); ++index) {
        guid[index] = static_cast<uint8_t>(index);
    }
    uint8_t packet[128] = {};
    const size_t length = ptpip::buildInitCommandRequest(guid,
                                                         "alpha-buddy",
                                                         packet,
                                                         sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(length, ptpip::readLe32(packet));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ptpip::PacketType::InitCommandRequest),
                             ptpip::readLe32(packet + 4));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(guid, packet + 8, sizeof(guid));
    TEST_ASSERT_EQUAL_UINT8('a', packet[24]);
    TEST_ASSERT_EQUAL_UINT8(0, packet[25]);
    TEST_ASSERT_EQUAL_UINT32(ptpip::kProtocolVersion, ptpip::readLe32(packet + length - 4));
}

void test_operation_packet_encodes_data_phase_and_parameters() {
    const uint32_t parameter = SonyLiveViewProbe::kLegacyLiveViewObjectHandle;
    uint8_t packet[64] = {};
    const size_t length = ptpip::buildOperationRequest(0x1009,
                                                       7,
                                                       ptpip::DataPhaseInfo::NoDataOrDataIn,
                                                       &parameter,
                                                       1,
                                                       packet,
                                                       sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(22, length);
    TEST_ASSERT_EQUAL_UINT32(1, ptpip::readLe32(packet + 8));
    TEST_ASSERT_EQUAL_HEX16(0x1009, ptpip::readLe16(packet + 12));
    TEST_ASSERT_EQUAL_UINT32(7, ptpip::readLe32(packet + 14));
    TEST_ASSERT_EQUAL_HEX32(parameter, ptpip::readLe32(packet + 18));
}

void test_data_out_packets_match_ptpip_wire_shape() {
    constexpr uint32_t transactionId = 9;
    const uint8_t payload[] = {0x02, 0x00};
    uint8_t packet[32] = {};

    size_t length = ptpip::buildStartDataPacket(transactionId,
                                                sizeof(payload),
                                                packet,
                                                sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(20, length);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ptpip::PacketType::StartData),
                             ptpip::readLe32(packet + 4));
    TEST_ASSERT_EQUAL_UINT32(transactionId, ptpip::readLe32(packet + 8));
    TEST_ASSERT_EQUAL_UINT64(sizeof(payload), ptpip::readLe64(packet + 12));

    length = ptpip::buildDataPacket(transactionId,
                                    payload,
                                    sizeof(payload),
                                    packet,
                                    sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(14, length);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ptpip::PacketType::Data),
                             ptpip::readLe32(packet + 4));
    TEST_ASSERT_EQUAL_UINT32(transactionId, ptpip::readLe32(packet + 8));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, packet + 12, sizeof(payload));

    length = ptpip::buildEndDataPacket(transactionId, packet, sizeof(packet));
    TEST_ASSERT_EQUAL_UINT32(12, length);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ptpip::PacketType::EndData),
                             ptpip::readLe32(packet + 4));
    TEST_ASSERT_EQUAL_UINT32(transactionId, ptpip::readLe32(packet + 8));
}

void test_jpeg_sink_grows_and_validates_markers() {
    JpegFrameSink sink(32);
    const uint8_t first[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08};
    const uint8_t second[] = {0x00, 0x02, 0x00, 0x03, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9};
    TEST_ASSERT_TRUE(sink.begin(4));
    TEST_ASSERT_TRUE(sink.write(first, sizeof(first)));
    TEST_ASSERT_TRUE(sink.write(second, sizeof(second)));
    TEST_ASSERT_TRUE(sink.finish());
    TEST_ASSERT_EQUAL_UINT32(17, sink.size());
    TEST_ASSERT_TRUE(sink.validJpeg());
    TEST_ASSERT_EQUAL_UINT16(3, sink.width());
    TEST_ASSERT_EQUAL_UINT16(2, sink.height());
}

void test_jpeg_sink_rejects_declared_oversize_frame() {
    JpegFrameSink sink(5);
    TEST_ASSERT_FALSE(sink.begin(6));
}

void test_jpeg_sink_accepts_complete_non_jpeg_transport_payload() {
    JpegFrameSink sink(64);
    const uint8_t placeholder[] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_TRUE(sink.begin(sizeof(placeholder)));
    TEST_ASSERT_TRUE(sink.write(placeholder, sizeof(placeholder)));
    TEST_ASSERT_TRUE(sink.finish());
    TEST_ASSERT_FALSE(sink.validJpeg());
}

void test_jpeg_sink_extracts_jpeg_from_ptp_metadata() {
    JpegFrameSink sink(64);
    const uint8_t wrapped[] = {
        0x10, 0x20, 0x30,
        0xFF, 0xD8,
        0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x04, 0x00, 0x05,
        0x01, 0x01, 0x11, 0x00,
        0xFF, 0xD9,
        0x40, 0x50,
    };
    TEST_ASSERT_TRUE(sink.begin(sizeof(wrapped)));
    TEST_ASSERT_TRUE(sink.write(wrapped, sizeof(wrapped)));
    TEST_ASSERT_TRUE(sink.finish());
    TEST_ASSERT_EQUAL_UINT8(0xFF, sink.data()[0]);
    TEST_ASSERT_EQUAL_UINT8(0xD8, sink.data()[1]);
    TEST_ASSERT_EQUAL_UINT16(5, sink.width());
    TEST_ASSERT_EQUAL_UINT16(4, sink.height());
    TEST_ASSERT_EQUAL_UINT32(17, sink.size());
}

void test_probe_retries_open_and_gets_single_jpeg() {
    FakePort port;
    port.failOpenCount = 2;
    CapturingTrace trace;
    SonyLiveViewProbe probe(port, trace);
    JpegFrameSink frame(128);
    PtpSessionConfig session;

    const ProbeReport report = probe.run(session, frame);

    TEST_ASSERT_TRUE(report.success);
    TEST_ASSERT_EQUAL_UINT8(3, report.attempts);
    TEST_ASSERT_EQUAL_UINT32(3, port.openCalls);
    TEST_ASSERT_EQUAL_UINT32(3, port.closeCalls);
    TEST_ASSERT_EQUAL_UINT32(9, port.operations.size());
    TEST_ASSERT_EQUAL_HEX16(SonyLiveViewProbe::kOperationGetObjectInfo,
                            port.operations[port.operations.size() - 2]);
    TEST_ASSERT_EQUAL_HEX16(SonyLiveViewProbe::kOperationGetObject, port.operations.back());
    TEST_ASSERT_EQUAL_UINT32(17, report.jpegBytes);
    TEST_ASSERT_EQUAL_UINT16(3, report.jpegWidth);
    TEST_ASSERT_EQUAL_UINT16(2, report.jpegHeight);
}

void test_probe_stops_after_three_failures() {
    FakePort port;
    port.failOpenCount = 9;
    CapturingTrace trace;
    SonyLiveViewProbe probe(port, trace);
    JpegFrameSink frame(128);
    PtpSessionConfig session;

    const ProbeReport report = probe.run(session, frame);

    TEST_ASSERT_FALSE(report.success);
    TEST_ASSERT_EQUAL_UINT8(3, report.attempts);
    TEST_ASSERT_EQUAL_UINT32(3, port.openCalls);
    TEST_ASSERT_EQUAL_UINT32(3, port.closeCalls);
    TEST_ASSERT_EQUAL(ProbeStage::InitCommand, report.failedStage);
}

void test_invalid_jpeg_fails_at_validation() {
    FakePort port;
    port.invalidJpeg = true;
    CapturingTrace trace;
    SonyLiveViewProbe probe(port, trace);
    JpegFrameSink frame(128);
    PtpSessionConfig session;

    const ProbeReport report = probe.run(session, frame, ProbeOptions{1});

    TEST_ASSERT_FALSE(report.success);
    TEST_ASSERT_EQUAL(ProbeStage::JpegValidation, report.failedStage);
}

void test_diagnostics_have_fixed_schema_without_secret_fields() {
    ProbeEvent event;
    event.stage = ProbeStage::LiveViewRequest;
    event.status = ProbeStatus::Failed;
    event.operationCode = 0x1009;
    event.responseCode = 0x2005;
    char output[320] = {};

    TEST_ASSERT_TRUE(formatProbeEvent(event, output, sizeof(output)));
    const std::string text(output);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, text.find("live_view_request"));
    TEST_ASSERT_EQUAL(std::string::npos, text.find("password"));
    TEST_ASSERT_EQUAL(std::string::npos, text.find("ssid"));
    TEST_ASSERT_EQUAL(std::string::npos, text.find("guid"));
}

void test_stream_reuses_one_session_for_multiple_frames() {
    FakePort port;
    CapturingTrace trace;
    SonyLiveViewStream stream(port, trace);
    JpegFrameSink frame(128);
    FakePresenter presenter;
    PtpSessionConfig session;
    StreamOptions options;
    options.durationMs = 2;

    const StreamReport report = stream.run(session, frame, presenter, options);

    TEST_ASSERT_TRUE(report.success);
    TEST_ASSERT_EQUAL_UINT32(1, port.openCalls);
    TEST_ASSERT_EQUAL_UINT32(1, port.closeCalls);
    TEST_ASSERT_GREATER_THAN_UINT32(0, report.framesPresented);
    TEST_ASSERT_EQUAL_UINT32(report.framesPresented, presenter.calls);
    TEST_ASSERT_EQUAL_UINT32(7 + 2 * report.framesPresented, port.operations.size());
}

void test_stream_stops_when_presenter_rejects_frame() {
    FakePort port;
    CapturingTrace trace;
    SonyLiveViewStream stream(port, trace);
    JpegFrameSink frame(128);
    FakePresenter presenter;
    presenter.accept = false;
    PtpSessionConfig session;
    StreamOptions options;
    options.durationMs = 1000;

    const StreamReport report = stream.run(session, frame, presenter, options);

    TEST_ASSERT_FALSE(report.success);
    TEST_ASSERT_EQUAL_UINT32(1, report.frameFailures);
    TEST_ASSERT_EQUAL_UINT32(1, presenter.calls);
    TEST_ASSERT_EQUAL(ProbeStage::JpegValidation, report.failedStage);
}

void test_stream_retries_temporary_get_object_access_denied() {
    FakePort port;
    port.deniedGetObjectCalls = 2;
    CapturingTrace trace;
    SonyLiveViewStream stream(port, trace);
    JpegFrameSink frame(128);
    FakePresenter presenter;
    PtpSessionConfig session;
    StreamOptions options;
    options.durationMs = 2;

    const StreamReport report = stream.run(session, frame, presenter, options);

    TEST_ASSERT_TRUE(report.success);
    TEST_ASSERT_GREATER_THAN_UINT32(0, report.framesPresented);
    TEST_ASSERT_EQUAL_HEX16(0x2001, report.responseCode);
}

void test_stream_stops_immediately_when_ptp_transport_dies() {
    FakePort port;
    port.failGetObjectInfoTransportCalls = 1;
    CapturingTrace trace;
    SonyLiveViewStream stream(port, trace);
    JpegFrameSink frame(128);
    FakePresenter presenter;
    PtpSessionConfig session;
    StreamOptions options;
    options.durationMs = 1000;

    const StreamReport report = stream.run(session, frame, presenter, options);

    TEST_ASSERT_FALSE(report.success);
    TEST_ASSERT_EQUAL_UINT32(1, report.frameFailures);
    TEST_ASSERT_EQUAL_UINT32(8, port.operations.size());
    TEST_ASSERT_EQUAL(TransportError::SocketClosed, report.transportError);
}

void test_landscape_orientation_is_immediate_then_stable_and_lockable() {
    LandscapeOrientationTracker tracker;

    TEST_ASSERT_EQUAL(LandscapeOrientation::Rotation1,
                      tracker.update(0.9f, 0.1f, 0, false));
    TEST_ASSERT_TRUE(tracker.resolved());

    for (uint32_t now = 40; now < 400; now += 40) {
        TEST_ASSERT_EQUAL(LandscapeOrientation::Rotation1,
                          tracker.update(-0.9f, 0.1f, now, true));
    }

    TEST_ASSERT_EQUAL(LandscapeOrientation::Rotation1,
                      tracker.update(-0.9f, 0.1f, 400, false));
    TEST_ASSERT_EQUAL(LandscapeOrientation::Rotation1,
                      tracker.update(-0.9f, 0.1f, 560, false));
    TEST_ASSERT_EQUAL(LandscapeOrientation::Rotation3,
                      tracker.update(-0.9f, 0.1f, 600, false));

    TEST_ASSERT_EQUAL(LandscapeOrientation::Rotation3,
                      tracker.update(0.1f, 0.1f, 2000, false));
}

void test_sticks3_gravity_mapping_matches_display_orientation() {
    const LandscapeGravity gravity = stickS3LandscapeGravity(0.1f, 0.9f);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, gravity.verticalG);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.1f, gravity.horizontalG);
}

void test_hud_uses_display_rotation_without_local_mirroring() {
    constexpr int32_t width = 24;
    constexpr int32_t height = 12;

    const HudRect batteryTip = orientHudRect(
        LandscapeOrientation::Rotation3,
        width,
        height,
        HudRect{21, 1, 3, 6});
    const HudPoint boltTop = orientHudPoint(
        LandscapeOrientation::Rotation3,
        width,
        height,
        HudPoint{12, 2});

    TEST_ASSERT_EQUAL_INT32(21, batteryTip.x);
    TEST_ASSERT_EQUAL_INT32(1, batteryTip.y);
    TEST_ASSERT_EQUAL_INT32(12, boltTop.x);
    TEST_ASSERT_EQUAL_INT32(2, boltTop.y);
    // Every pixel of each mode icon must keep its local position. The display
    // controller already rotates the complete image, including these glyphs.
    for (unsigned slot = 0; slot < 3; ++slot) {
        for (uint8_t mode = 0; mode <= 6; ++mode) {
            const ModeIcon icon = modeIcon(slot, mode);
            for (int y = 0; y < 15; ++y) {
                for (int x = 0; x < 20; ++x) {
                    if (!(icon.rows[y] & (uint32_t{1} << x))) continue;
                    for (auto orientation : {LandscapeOrientation::Rotation1,
                                             LandscapeOrientation::Rotation3}) {
                        const HudPoint p = orientHudPoint(orientation, 20, 15, {x, y});
                        TEST_ASSERT_EQUAL_INT32(x, p.x);
                        TEST_ASSERT_EQUAL_INT32(y, p.y);
                    }
                }
            }
        }
    }
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_grid_reaches_image_edges_in_both_orientations);
    RUN_TEST(test_side_button_double_click_timing_holds_and_wrap);
    RUN_TEST(test_flip_cancel_requires_continuity_and_latches_until_next_press);
    RUN_TEST(test_stream_mf_and_cancel_never_send_unwanted_controls);
    RUN_TEST(test_cancel_cleanup_failure_closes_session_without_capture);
    RUN_TEST(test_disconnected_shutdown_deadline_resets_and_clock_wrap);
    RUN_TEST(test_camera_mode_wire_values_and_rejects_partial_snapshots);
    RUN_TEST(test_stream_mode_read_success_rejection_and_transport_failure);
    RUN_TEST(test_viewfinder_hint_timeout_pause_and_connection_priority);
    RUN_TEST(test_stream_connection_lifecycle_and_capture_pause);
    RUN_TEST(test_stream_requests_small_live_view_once_and_tolerates_rejection);
    RUN_TEST(test_stream_quality_transport_failure_closes_session);
    RUN_TEST(test_init_command_packet_matches_wire_shape);
    RUN_TEST(test_operation_packet_encodes_data_phase_and_parameters);
    RUN_TEST(test_data_out_packets_match_ptpip_wire_shape);
    RUN_TEST(test_jpeg_sink_grows_and_validates_markers);
    RUN_TEST(test_jpeg_sink_rejects_declared_oversize_frame);
    RUN_TEST(test_jpeg_sink_accepts_complete_non_jpeg_transport_payload);
    RUN_TEST(test_jpeg_sink_extracts_jpeg_from_ptp_metadata);
    RUN_TEST(test_probe_retries_open_and_gets_single_jpeg);
    RUN_TEST(test_probe_stops_after_three_failures);
    RUN_TEST(test_invalid_jpeg_fails_at_validation);
    RUN_TEST(test_diagnostics_have_fixed_schema_without_secret_fields);
    RUN_TEST(test_stream_reuses_one_session_for_multiple_frames);
    RUN_TEST(test_stream_stops_when_presenter_rejects_frame);
    RUN_TEST(test_stream_retries_temporary_get_object_access_denied);
    RUN_TEST(test_stream_stops_immediately_when_ptp_transport_dies);
    RUN_TEST(test_landscape_orientation_is_immediate_then_stable_and_lockable);
    RUN_TEST(test_sticks3_gravity_mapping_matches_display_orientation);
    RUN_TEST(test_hud_uses_display_rotation_without_local_mirroring);
    return UNITY_END();
}
