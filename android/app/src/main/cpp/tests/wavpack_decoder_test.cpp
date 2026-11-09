#include "wavpack_decoder.h"

#include <wavpack.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 测试不依赖二进制 fixture：用 libwavpack 编码 API 现场生成 .wv，再往返验证。
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kFrames = 512;

std::string tempPath(const char* name) {
    return std::string("sylvakru_wavpack_test_") + name;
}

int writeBlock(void* id, void* data, int32_t byte_count) {
    auto* file = static_cast<std::FILE*>(id);
    return std::fwrite(data, 1, static_cast<size_t>(byte_count), file) ==
        static_cast<size_t>(byte_count);
}

// 24-bit 立体声斜坡样本：覆盖正负值与低位模式，全程保持在 24-bit 范围内
std::vector<int32_t> referenceSamples() {
    std::vector<int32_t> samples;
    samples.reserve(kFrames * kChannels);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const int32_t base = static_cast<int32_t>(frame) * 16001 - (1 << 22);
        samples.push_back(base);
        samples.push_back(-base - 1);
    }
    return samples;
}

void encodeReferenceFile(const std::string& path, bool float_data) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    assert(file != nullptr);
    WavpackContext* context = WavpackOpenFileOutput(writeBlock, file, nullptr);
    assert(context != nullptr);

    WavpackConfig config = {};
    config.sample_rate = static_cast<int32_t>(kSampleRate);
    config.num_channels = static_cast<int>(kChannels);
    config.channel_mask = 3;
    config.bits_per_sample = float_data ? 32 : 24;
    config.bytes_per_sample = float_data ? 4 : 3;
    if (float_data) {
        // float_norm_exp 非零即按 IEEE float 打包（127 = 标准归一化）
        config.float_norm_exp = 127;
    }
    assert(WavpackSetConfiguration(context, &config, kFrames));
    assert(WavpackPackInit(context));

    auto samples = referenceSamples();
    if (float_data) {
        for (auto& sample : samples) {
            const float value = static_cast<float>(sample) / (1 << 23);
            std::memcpy(&sample, &value, sizeof(sample));
        }
    }
    assert(WavpackPackSamples(context, samples.data(), kFrames));
    assert(WavpackFlushSamples(context));
    WavpackCloseFile(context);
    std::fclose(file);
}

void verifiesMissingFile() {
    sylvakru::WavPackDecoder decoder;
    const auto result = decoder.open(tempPath("missing.wv"));
    assert(!result.ok());
    assert(result.error == sylvakru::WavPackError::kOpenFailed);
}

void verifies24BitRoundTrip() {
    const std::string path = tempPath("pcm24.wv");
    encodeReferenceFile(path, false);

    sylvakru::WavPackDecoder decoder;
    assert(decoder.open(path).ok());
    const auto& info = decoder.streamInfo();
    assert(info.sample_rate == kSampleRate);
    assert(info.channels == kChannels);
    assert(info.valid_bits_per_sample == 24);
    assert(info.total_frames == kFrames);

    std::vector<int32_t> decoded(kFrames * kChannels);
    const auto read = decoder.readFrames(decoded.data(), kFrames);
    assert(read.ok());
    assert(read.frames == kFrames);
    assert(read.end_of_stream);
    assert(decoded == referenceSamples());

    decoder.close();
    std::remove(path.c_str());
}

void verifiesSeek() {
    const std::string path = tempPath("seek.wv");
    encodeReferenceFile(path, false);

    sylvakru::WavPackDecoder decoder;
    assert(decoder.open(path).ok());
    constexpr uint32_t kSeekFrame = 256;
    assert(decoder.seekToFrame(kSeekFrame).ok());

    std::vector<int32_t> decoded((kFrames - kSeekFrame) * kChannels);
    const auto read = decoder.readFrames(decoded.data(), kFrames - kSeekFrame);
    assert(read.ok());
    assert(read.frames == kFrames - kSeekFrame);

    const auto reference = referenceSamples();
    const std::vector<int32_t> expected(
        reference.begin() + kSeekFrame * kChannels,
        reference.end());
    assert(decoded == expected);

    decoder.close();
    std::remove(path.c_str());
}

void verifiesFloatUnsupported() {
    const std::string path = tempPath("float.wv");
    encodeReferenceFile(path, true);

    sylvakru::WavPackDecoder decoder;
    const auto result = decoder.open(path);
    assert(!result.ok());
    assert(result.error == sylvakru::WavPackError::kUnsupportedFormat);

    std::remove(path.c_str());
}

}  // namespace

int main() {
    verifiesMissingFile();
    verifies24BitRoundTrip();
    verifiesSeek();
    verifiesFloatUnsupported();
    std::printf("wavpack_decoder_test passed\n");
    return 0;
}
