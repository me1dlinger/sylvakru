#include "wavpack_decoder.h"

#include <wavpack.h>

#include <algorithm>

namespace sylvakru {

struct WavPackDecoder::State {
    WavpackContext* context = nullptr;
    WavPackStreamInfo info;
    uint64_t current_frame = 0;
    int base_error_count = 0;
    bool end_of_stream = false;
};

WavPackDecoder::WavPackDecoder() : state_(new State()) {}

WavPackDecoder::~WavPackDecoder() {
    close();
    delete state_;
}

WavPackResult WavPackDecoder::open(const std::string& path) {
    close();
    char error_buffer[81] = {0};
    // OPEN_WVC：hybrid 文件旁有 .wvc 校正文件时无损还原，没有则按主文件解。
    // 不带 DSD 标志：DSD .wv 直接打开失败，调用方回退共享输出。
    state_->context = WavpackOpenFileInput(
        path.c_str(),
        error_buffer,
        OPEN_WVC | OPEN_FILE_UTF8,
        0);
    if (state_->context == nullptr) {
        return {
            WavPackError::kOpenFailed,
            std::string("Failed to open WavPack file: ") + error_buffer + ".",
        };
    }
    const int mode = WavpackGetMode(state_->context);
    if ((mode & MODE_FLOAT) != 0) {
        close();
        return {
            WavPackError::kUnsupportedFormat,
            "Floating point WavPack files are not supported.",
        };
    }
    const int64_t total_frames = WavpackGetNumSamples64(state_->context);
    state_->info = {
        WavpackGetSampleRate(state_->context),
        static_cast<uint32_t>(WavpackGetNumChannels(state_->context)),
        static_cast<uint32_t>(WavpackGetBitsPerSample(state_->context)),
        total_frames > 0 ? static_cast<uint64_t>(total_frames) : 0,
    };
    if (state_->info.sample_rate == 0) {
        close();
        return {WavPackError::kUnsupportedFormat, "Unsupported WavPack sample rate."};
    }
    if (state_->info.channels < 1 || state_->info.channels > 2) {
        close();
        return {WavPackError::kUnsupportedFormat, "Unsupported WavPack channel count."};
    }
    if (state_->info.valid_bits_per_sample < 8 ||
        state_->info.valid_bits_per_sample > 32) {
        close();
        return {WavPackError::kUnsupportedFormat, "Unsupported WavPack bit depth."};
    }
    state_->base_error_count = WavpackGetNumErrors(state_->context);
    return {};
}

const WavPackStreamInfo& WavPackDecoder::streamInfo() const {
    return state_->info;
}

WavPackReadResult WavPackDecoder::readFrames(int32_t* output, uint32_t capacity_frames) {
    if (state_->context == nullptr) {
        return {WavPackError::kInvalidStream, 0, false, "WavPack decoder is not open."};
    }
    if (output == nullptr || capacity_frames == 0) {
        return {WavPackError::kInvalidBuffer, 0, false, "WavPack target buffer is invalid."};
    }

    const uint32_t frames = WavpackUnpackSamples(state_->context, output, capacity_frames);
    // 与 libFLAC 路径同语义：解码错误立即上报停播，绝不静默吞块继续
    const int error_count = WavpackGetNumErrors(state_->context);
    if (error_count > state_->base_error_count) {
        state_->base_error_count = error_count;
        const char* message = WavpackGetErrorMessage(state_->context);
        return {
            WavPackError::kDecodeFailed,
            frames,
            false,
            std::string("WavPack decode failed: ") +
                (message != nullptr && message[0] != '\0' ? message : "block CRC error") +
                ".",
        };
    }
    state_->current_frame += frames;
    if (frames == 0) {
        state_->end_of_stream = true;
    }
    const bool known_end = state_->info.total_frames > 0 &&
        state_->current_frame >= state_->info.total_frames;
    return {WavPackError::kNone, frames, state_->end_of_stream || known_end, {}};
}

WavPackResult WavPackDecoder::seekToFrame(uint64_t frame) {
    if (state_->context == nullptr) {
        return {WavPackError::kInvalidStream, "WavPack decoder is not open."};
    }
    if (!WavpackSeekSample64(state_->context, static_cast<int64_t>(frame))) {
        return {WavPackError::kSeekFailed, "Failed to seek WavPack stream."};
    }
    state_->current_frame = frame;
    state_->end_of_stream = false;
    return {};
}

void WavPackDecoder::close() {
    if (state_->context != nullptr) {
        WavpackCloseFile(state_->context);
    }
    state_->context = nullptr;
    state_->info = {};
    state_->current_frame = 0;
    state_->base_error_count = 0;
    state_->end_of_stream = false;
}

}  // namespace sylvakru
