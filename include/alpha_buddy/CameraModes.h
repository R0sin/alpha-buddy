#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "alpha_buddy/IPtpIpCommandPort.h"
#include "alpha_buddy/ViewfinderStatus.h"

namespace alpha_buddy {

enum class ExposureMode : uint8_t { Unknown, P, A, S, M, Auto, Scene };
enum class FocusMode : uint8_t { Unknown, S, C, A, DMF, MF };
enum class SilentMode : uint8_t { Unknown, Off, On };

struct CameraModes {
    ExposureMode exposure = ExposureMode::Unknown;
    FocusMode focus = FocusMode::Unknown;
    SilentMode silent = SilentMode::Unknown;
    uint32_t packed() const {
        return uint32_t(exposure) | (uint32_t(focus) << 8) | (uint32_t(silent) << 16);
    }
};

inline ExposureMode exposureMode(uint32_t value) {
    // Sony encodes an additional program category in bits 16..19.
    if (value > 0x000FFFFF) return ExposureMode::Unknown;
    switch (value & 0xFFFF) {
        case 1: case 0x8053: return ExposureMode::M;
        case 2: case 0x8050: return ExposureMode::P;
        case 3: case 0x8051: return ExposureMode::A;
        case 4: case 0x8052: return ExposureMode::S;
        case 0x8000: case 0x8001: return ExposureMode::Auto;
        case 7: case 0x8011: case 0x8012: case 0x8013: case 0x8014:
        case 0x8015: case 0x8016: case 0x8017: case 0x8018: return ExposureMode::Scene;
        default: return ExposureMode::Unknown;
    }
}

inline FocusMode focusMode(uint32_t value) {
    switch (value) {
        case 1: return FocusMode::MF;
        case 2: return FocusMode::S;
        case 0x8004: return FocusMode::C;
        case 0x8005: return FocusMode::A;
        case 0x8006: return FocusMode::DMF;
        default: return FocusMode::Unknown;
    }
}

// Sony SilentMode is independent of shutter type (libgphoto2 PTP mapping).
inline SilentMode silentMode(uint32_t value) {
    if (value == 1) return SilentMode::Off;
    if (value == 2) return SilentMode::On;
    return SilentMode::Unknown;
}

inline uint32_t visibleCameraModes(CameraConnection connection, uint32_t modes,
                                   uint32_t now, uint32_t updatedAt) {
    return connection == CameraConnection::Connected && now - updatedAt <= 3500
        ? modes : 0;
}

// Bounds-checked Sony descriptor reader. The target camera uses two enum lists.
class SonyPropertyReader {
public:
    SonyPropertyReader(const uint8_t* data, size_t size) : _data(data), _size(size) {}
    bool ok() const { return _ok; }
    bool done() const { return _ok && _offset == _size; }
    uint32_t integer(size_t size) {
        const auto* bytes = take(size);
        uint32_t value = 0;
        if (bytes) for (size_t i = 0; i < size && i < 4; ++i) value |= uint32_t(bytes[i]) << (i * 8);
        return value;
    }
    uint32_t value(uint16_t type) {
        if (type == 0xFFFF) { const uint32_t count = integer(1); take(count * 2); return 0; }
        const uint16_t base = type & ~0x4000;
        if (base < 1 || base > 10 || (type != base && type != (base | 0x4000))) {
            _ok = false; return 0;
        }
        const size_t width = size_t{1} << ((base - 1) / 2);
        if (type & 0x4000) {
            const uint32_t count = integer(4);
            if (count > (_size - _offset) / width) { _ok = false; return 0; }
            take(count * width); return 0;
        }
        return integer(width);
    }
private:
    const uint8_t* take(size_t size) {
        if (!_ok || !_data || size > _size - _offset) { _ok = false; return nullptr; }
        const auto* start = _data + _offset; _offset += size; return start;
    }
    const uint8_t* _data;
    size_t _size, _offset = 0;
    bool _ok = true;
};

inline bool decodeCameraModes(const uint8_t* data, size_t size, CameraModes& output) {
    output = {};
    CameraModes modes;
    SonyPropertyReader reader(data, size);
    const uint32_t count = reader.integer(4);
    reader.integer(4);
    if (count > size / 7) return false;
    uint8_t seen = 0;
    for (uint32_t i = 0; i < count && reader.ok(); ++i) {
        const uint16_t code = uint16_t(reader.integer(2));
        const uint16_t type = uint16_t(reader.integer(2));
        reader.integer(2); // get/set and enabled flags
        reader.value(type);
        const uint32_t current = reader.value(type);
        const uint8_t bit = code == 0x500E ? 1 : (code == 0x500A ? 2 : (code == 0xD0DB ? 4 : 0));
        if (!code || (seen & bit)) return false;
        seen |= bit;
        if (type == 2 || type == 4 || type == 6) {
            if (bit == 1) modes.exposure = exposureMode(current);
            if (bit == 2) modes.focus = focusMode(current);
            if (bit == 4 && type == 2) modes.silent = silentMode(current);
        }
        const uint32_t form = reader.integer(1);
        if (form == 1) {
            for (int n = 0; n < 3; ++n) reader.value(type);
        } else if (form == 2) {
            for (int list = 0; list < 2 && reader.ok(); ++list) {
                const uint32_t n = reader.integer(2);
                for (uint32_t j = 0; j < n && reader.ok(); ++j) reader.value(type);
            }
        } else if (form != 0) return false;
    }
    if (!reader.done()) return false;
    output = modes;
    return true;
}

class CameraModeSink final : public IDataSink {
public:
    ~CameraModeSink() override { free(_data); }
    bool begin(size_t expected) override {
        _size = 0;
        if (!_data) _data = static_cast<uint8_t*>(malloc(65536));
        return _data && expected <= 65536;
    }
    bool write(const uint8_t* data, size_t size) override {
        if (!_data || size > 65536 - _size) return false;
        memcpy(_data + _size, data, size); _size += size; return true;
    }
    bool finish() override { return true; } // Always consume the operation response.
    bool decode(CameraModes& modes) const { return decodeCameraModes(_data, _size, modes); }
    void reset() { _size = 0; }
private:
    uint8_t* _data = nullptr;
    size_t _size = 0;
};

} // namespace alpha_buddy
