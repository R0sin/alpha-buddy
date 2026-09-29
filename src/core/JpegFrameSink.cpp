#include "alpha_buddy/JpegFrameSink.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifndef ALPHA_BUDDY_NATIVE
#include <esp_heap_caps.h>
#endif

namespace alpha_buddy {

namespace {

constexpr size_t kFallbackInitialBytes = 256U * 1024U;

void* allocateBytes(size_t bytes) {
#ifdef ALPHA_BUDDY_NATIVE
    return std::malloc(bytes);
#else
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

void freeBytes(void* pointer) {
#ifdef ALPHA_BUDDY_NATIVE
    std::free(pointer);
#else
    heap_caps_free(pointer);
#endif
}

}  // namespace

JpegFrameSink::JpegFrameSink(size_t maximumBytes) : _maximumBytes(maximumBytes) {}

JpegFrameSink::~JpegFrameSink() {
    release();
}

bool JpegFrameSink::begin(size_t expectedBytes) {
    reset();
    if (expectedBytes > _maximumBytes) {
        _rejected = true;
        return false;
    }
    const size_t initialBytes = expectedBytes > 0
        ? expectedBytes
        : std::min(kFallbackInitialBytes, _maximumBytes);
    return reserve(initialBytes);
}

bool JpegFrameSink::write(const uint8_t* data, size_t length) {
    if (_rejected || _finished || data == nullptr) {
        return false;
    }
    if (length > _maximumBytes - _size) {
        _rejected = true;
        return false;
    }
    const size_t required = _size + length;
    if (required > _capacity) {
        size_t nextCapacity = _capacity == 0 ? kFallbackInitialBytes : _capacity;
        while (nextCapacity < required && nextCapacity < _maximumBytes) {
            nextCapacity = std::min(nextCapacity * 2, _maximumBytes);
        }
        if (!reserve(nextCapacity)) {
            _rejected = true;
            return false;
        }
    }
    std::memcpy(_buffer + _size, data, length);
    _size += length;
    return true;
}

bool JpegFrameSink::finish() {
    _finished = true;
    if (!normalizeJpegPayload() || !parseDimensions()) {
        _rejected = true;
    }
    // A complete transport payload was accepted even when it was not a JPEG.
    // Returning false here would make the PTP/IP reader abandon the operation
    // before consuming OperationResponse, desynchronizing the shared session.
    // Content validation remains available through validJpeg().
    return true;
}

void JpegFrameSink::reset() {
    _size = 0;
    _finished = false;
    _rejected = false;
    _width = 0;
    _height = 0;
}

bool JpegFrameSink::validJpeg() const {
    return !_rejected && _finished && _size >= 4 &&
           _buffer[0] == 0xFF && _buffer[1] == 0xD8 &&
           _buffer[_size - 2] == 0xFF && _buffer[_size - 1] == 0xD9;
}

const uint8_t* JpegFrameSink::data() const {
    return _buffer;
}

size_t JpegFrameSink::size() const {
    return _size;
}

size_t JpegFrameSink::capacity() const {
    return _capacity;
}

uint16_t JpegFrameSink::width() const {
    return _width;
}

uint16_t JpegFrameSink::height() const {
    return _height;
}

bool JpegFrameSink::reserve(size_t requestedBytes) {
    if (requestedBytes == 0) {
        return true;
    }
    if (requestedBytes > _maximumBytes) {
        return false;
    }
    if (requestedBytes <= _capacity) {
        return true;
    }
    auto* replacement = static_cast<uint8_t*>(allocateBytes(requestedBytes));
    if (replacement == nullptr) {
        return false;
    }
    if (_buffer != nullptr && _size > 0) {
        std::memcpy(replacement, _buffer, _size);
    }
    release();
    _buffer = replacement;
    _capacity = requestedBytes;
    return true;
}

bool JpegFrameSink::normalizeJpegPayload() {
    if (_buffer == nullptr || _size < 4) {
        return false;
    }
    size_t start = _size;
    for (size_t index = 0; index + 1 < _size; ++index) {
        if (_buffer[index] == 0xFF && _buffer[index + 1] == 0xD8) {
            start = index;
            break;
        }
    }
    if (start == _size) {
        return false;
    }
    size_t end = 0;
    for (size_t index = start + 2; index + 1 < _size; ++index) {
        if (_buffer[index] == 0xFF && _buffer[index + 1] == 0xD9) {
            end = index + 2;
            break;
        }
    }
    if (end == 0) {
        return false;
    }
    const size_t jpegBytes = end - start;
    if (start > 0) {
        std::memmove(_buffer, _buffer + start, jpegBytes);
    }
    _size = jpegBytes;
    return true;
}

bool JpegFrameSink::parseDimensions() {
    if (_buffer == nullptr || _size < 10 || _buffer[0] != 0xFF || _buffer[1] != 0xD8) {
        return false;
    }

    size_t cursor = 2;
    while (cursor + 3 < _size) {
        if (_buffer[cursor] != 0xFF) {
            ++cursor;
            continue;
        }
        while (cursor < _size && _buffer[cursor] == 0xFF) {
            ++cursor;
        }
        if (cursor >= _size) {
            return false;
        }
        const uint8_t marker = _buffer[cursor++];
        if (marker == 0xD9 || marker == 0xDA) {
            return false;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            continue;
        }
        if (cursor + 1 >= _size) {
            return false;
        }
        const uint16_t segmentBytes = static_cast<uint16_t>(
            (static_cast<uint16_t>(_buffer[cursor]) << 8U) | _buffer[cursor + 1]);
        if (segmentBytes < 2 || cursor + segmentBytes > _size) {
            return false;
        }
        const bool isStartOfFrame =
            (marker >= 0xC0 && marker <= 0xC3) ||
            (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) ||
            (marker >= 0xCD && marker <= 0xCF);
        if (isStartOfFrame) {
            if (segmentBytes < 7) {
                return false;
            }
            _height = static_cast<uint16_t>(
                (static_cast<uint16_t>(_buffer[cursor + 3]) << 8U) | _buffer[cursor + 4]);
            _width = static_cast<uint16_t>(
                (static_cast<uint16_t>(_buffer[cursor + 5]) << 8U) | _buffer[cursor + 6]);
            return _width > 0 && _height > 0;
        }
        cursor += segmentBytes;
    }
    return false;
}

void JpegFrameSink::release() {
    if (_buffer != nullptr) {
        freeBytes(_buffer);
        _buffer = nullptr;
    }
    _capacity = 0;
}

}  // namespace alpha_buddy
