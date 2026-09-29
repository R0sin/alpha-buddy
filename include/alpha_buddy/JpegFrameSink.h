#pragma once

#include <cstddef>
#include <cstdint>

#include "alpha_buddy/IPtpIpCommandPort.h"

namespace alpha_buddy {

class JpegFrameSink final : public IDataSink {
public:
    explicit JpegFrameSink(size_t maximumBytes);
    ~JpegFrameSink() override;

    JpegFrameSink(const JpegFrameSink&) = delete;
    JpegFrameSink& operator=(const JpegFrameSink&) = delete;

    bool begin(size_t expectedBytes) override;
    bool write(const uint8_t* data, size_t length) override;
    bool finish() override;

    void reset();
    bool validJpeg() const;
    const uint8_t* data() const;
    size_t size() const;
    size_t capacity() const;
    uint16_t width() const;
    uint16_t height() const;

private:
    bool reserve(size_t requestedBytes);
    bool normalizeJpegPayload();
    bool parseDimensions();
    void release();

    uint8_t* _buffer = nullptr;
    size_t _size = 0;
    size_t _capacity = 0;
    size_t _maximumBytes = 0;
    bool _finished = false;
    bool _rejected = false;
    uint16_t _width = 0;
    uint16_t _height = 0;
};

}  // namespace alpha_buddy
