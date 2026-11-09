#pragma once

#include <cstdint>
#include <string>

namespace sylvakru {

enum class FlacError {
    kNone,
    kOpenFailed,
    kInvalidStream,
    kUnsupportedFormat,
    kDecodeFailed,
    kSeekFailed,
    kInvalidBuffer,
};

struct FlacResult {
    FlacError error = FlacError::kNone;
    std::string message;

    bool ok() const { return error == FlacError::kNone; }
};

struct FlacStreamInfo {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t valid_bits_per_sample = 0;
    uint64_t total_frames = 0;
};

struct FlacReadResult {
    FlacError error = FlacError::kNone;
    uint32_t frames = 0;
    bool end_of_stream = false;
    std::string message;

    bool ok() const { return error == FlacError::kNone; }
};

class FlacDecoder {
public:
    FlacDecoder();
    ~FlacDecoder();
    FlacDecoder(const FlacDecoder&) = delete;
    FlacDecoder& operator=(const FlacDecoder&) = delete;

    FlacResult open(const std::string& path);
    const FlacStreamInfo& streamInfo() const;
    FlacReadResult readFrames(int32_t* output, uint32_t capacity_frames);
    FlacResult seekToFrame(uint64_t frame);
    void close();

private:
    struct State;
    State* state_;
};

}  // namespace sylvakru
