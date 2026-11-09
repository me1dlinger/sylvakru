#pragma once

#include <cstdint>
#include <string>

namespace sylvakru {

enum class WavPackError {
    kNone,
    kOpenFailed,
    kInvalidStream,
    kUnsupportedFormat,
    kDecodeFailed,
    kSeekFailed,
    kInvalidBuffer,
};

struct WavPackResult {
    WavPackError error = WavPackError::kNone;
    std::string message;

    bool ok() const { return error == WavPackError::kNone; }
};

struct WavPackStreamInfo {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t valid_bits_per_sample = 0;
    uint64_t total_frames = 0;
};

struct WavPackReadResult {
    WavPackError error = WavPackError::kNone;
    uint32_t frames = 0;
    bool end_of_stream = false;
    std::string message;

    bool ok() const { return error == WavPackError::kNone; }
};

class WavPackDecoder {
public:
    WavPackDecoder();
    ~WavPackDecoder();
    WavPackDecoder(const WavPackDecoder&) = delete;
    WavPackDecoder& operator=(const WavPackDecoder&) = delete;

    WavPackResult open(const std::string& path);
    const WavPackStreamInfo& streamInfo() const;
    WavPackReadResult readFrames(int32_t* output, uint32_t capacity_frames);
    WavPackResult seekToFrame(uint64_t frame);
    void close();

private:
    struct State;
    State* state_;
};

}  // namespace sylvakru
