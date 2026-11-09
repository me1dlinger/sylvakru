#include "usb_dsd.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// 与 Kotlin UsbDsdTest 逐用例对拍：手工构造 KB 级的最小 DSF/DFF 头验证
// 解析、位序与块交错（不提交任何版权音频）。临时文件写到 argv[1]（默认当前目录）。

namespace {

std::string g_temp_dir = ".";

std::string tempPath(const char* name) {
    return g_temp_dir + "/" + name;
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file != nullptr);
    if (!bytes.empty()) {
        assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    }
    assert(std::fclose(file) == 0);
}

int64_t fileLength(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    assert(file != nullptr);
    std::fseek(file, 0, SEEK_END);
    const int64_t length = std::ftell(file);
    std::fclose(file);
    return length;
}

void append(std::vector<uint8_t>& out, const char* text) {
    while (*text != '\0') {
        out.push_back(static_cast<uint8_t>(*text++));
    }
}

void writeIntLe(std::vector<uint8_t>& out, int32_t value) {
    for (int index = 0; index < 4; ++index) {
        out.push_back(static_cast<uint8_t>((static_cast<uint32_t>(value) >> (index * 8)) & 0xff));
    }
}

void writeLongLe(std::vector<uint8_t>& out, int64_t value) {
    for (int index = 0; index < 8; ++index) {
        out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (index * 8)) & 0xff));
    }
}

void writeIntBe(std::vector<uint8_t>& out, int32_t value) {
    for (int index = 3; index >= 0; --index) {
        out.push_back(static_cast<uint8_t>((static_cast<uint32_t>(value) >> (index * 8)) & 0xff));
    }
}

void writeShortBe(std::vector<uint8_t>& out, int32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void writeLongBe(std::vector<uint8_t>& out, int64_t value) {
    for (int index = 7; index >= 0; --index) {
        out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (index * 8)) & 0xff));
    }
}

std::vector<uint8_t> buildDsfBytes(
    int channels,
    int sample_rate,
    int bits_per_sample,
    int block_size,
    const std::vector<std::vector<uint8_t>>& channel_data) {
    const int bytes_per_channel = static_cast<int>(channel_data[0].size());
    const int blocks = (bytes_per_channel + block_size - 1) / block_size;
    std::vector<uint8_t> audio;
    for (int block = 0; block < blocks; ++block) {
        for (int channel = 0; channel < channels; ++channel) {
            const std::vector<uint8_t>& data = channel_data[channel];
            for (int index = 0; index < block_size; ++index) {
                const size_t offset = static_cast<size_t>(block) * block_size + index;
                audio.push_back(offset < data.size() ? data[offset] : 0);
            }
        }
    }

    std::vector<uint8_t> out;
    append(out, "DSD ");
    writeLongLe(out, 28);
    writeLongLe(out, 28 + 52 + 12 + static_cast<int64_t>(audio.size()));
    writeLongLe(out, 0);  // 无 ID3 元数据
    append(out, "fmt ");
    writeLongLe(out, 52);
    writeIntLe(out, 1);  // formatVersion
    writeIntLe(out, 0);  // formatId = DSD raw
    writeIntLe(out, 2);  // channelType
    writeIntLe(out, channels);
    writeIntLe(out, sample_rate);
    writeIntLe(out, bits_per_sample);
    writeLongLe(out, bytes_per_channel * 8LL);  // sampleCount（每通道位数）
    writeIntLe(out, block_size);
    writeIntLe(out, 0);  // reserved
    append(out, "data");
    writeLongLe(out, 12 + static_cast<int64_t>(audio.size()));
    out.insert(out.end(), audio.begin(), audio.end());
    return out;
}

std::string buildDsf(
    const char* name,
    int channels,
    int sample_rate,
    int bits_per_sample,
    int block_size,
    const std::vector<std::vector<uint8_t>>& channel_data) {
    const std::string path = tempPath(name);
    writeFile(path, buildDsfBytes(channels, sample_rate, bits_per_sample, block_size, channel_data));
    return path;
}

std::vector<uint8_t> buildDffBytes(
    int channels,
    int sample_rate,
    const char* compression,
    const std::vector<uint8_t>& audio) {
    std::vector<uint8_t> prop;
    append(prop, "SND ");
    append(prop, "FS  ");
    writeLongBe(prop, 4);
    writeIntBe(prop, sample_rate);
    append(prop, "CHNL");
    writeLongBe(prop, 2 + channels * 4LL);
    writeShortBe(prop, channels);
    for (int channel = 0; channel < channels; ++channel) {
        append(prop, "SLFT");
    }
    append(prop, "CMPR");
    writeLongBe(prop, 5);  // 4 字节压缩类型 + 1 字节名字长度（奇数长度验证偶数对齐）
    append(prop, compression);
    prop.push_back(0);
    prop.push_back(0);  // 对齐填充

    std::vector<uint8_t> body;
    append(body, "DSD ");
    append(body, "FVER");
    writeLongBe(body, 4);
    writeIntBe(body, 0x01050000);
    append(body, "PROP");
    writeLongBe(body, static_cast<int64_t>(prop.size()) - 1);  // 声明大小不含对齐填充字节
    body.insert(body.end(), prop.begin(), prop.end());
    append(body, std::string(compression) == "DST " ? "DST " : "DSD ");
    writeLongBe(body, static_cast<int64_t>(audio.size()));
    body.insert(body.end(), audio.begin(), audio.end());

    std::vector<uint8_t> out;
    append(out, "FRM8");
    writeLongBe(out, static_cast<int64_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::string buildDff(
    const char* name,
    int channels,
    int sample_rate,
    const char* compression,
    const std::vector<uint8_t>& audio) {
    const std::string path = tempPath(name);
    writeFile(path, buildDffBytes(channels, sample_rate, compression, audio));
    return path;
}

std::vector<uint8_t> readAll(sylvakru::DsdFileReader& reader) {
    std::vector<uint8_t> out;
    uint8_t buffer[16];
    while (true) {
        const int count = reader.read(buffer, static_cast<int>(sizeof(buffer)));
        if (count < 0) {
            break;
        }
        out.insert(out.end(), buffer, buffer + count);
    }
    return out;
}

uint8_t reverseBits(uint8_t byte) {
    int value = byte;
    int reversed = 0;
    for (int bit = 0; bit < 8; ++bit) {
        reversed = (reversed << 1) | (value & 1);
        value >>= 1;
    }
    return static_cast<uint8_t>(reversed);
}

// ---- DSF ----

void dsfLsbFirstBlocksAreInterleavedAndBitReversed() {
    // 2 通道、块大小 4、每通道 6 字节 → 2 个块组（第二块组半满）
    const std::vector<uint8_t> left = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const std::vector<uint8_t> right = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    const std::string path = buildDsf("sample_lsb.dsf", 2, 2822400, 1, 4, {left, right});

    sylvakru::DsdFileReader reader;
    assert(reader.open(path).ok());
    assert(reader.formatName() == "dsf");
    assert(reader.sampleRate() == 2822400);
    assert(reader.channels() == 2);
    assert(reader.dsdMultiple() == 64);
    assert(reader.dopFrameRate() == 176400);
    // 6 字节 × 8 位 / 2822400 Hz ≈ 0.017 ms → 截断为 0
    assert(reader.durationMs() == 48 * 1000LL / 2822400);

    const std::vector<uint8_t> output = readAll(reader);
    // LSB-first 位反转后逐字节交错：L0 R0 L1 R1 …
    std::vector<uint8_t> expected(12);
    for (int index = 0; index < 6; ++index) {
        expected[index * 2] = reverseBits(left[index]);
        expected[index * 2 + 1] = reverseBits(right[index]);
    }
    assert(output == expected);
}

void dsfMsbFirstBytesPassThrough() {
    const std::vector<uint8_t> left = {0x0F, 0x33};
    const std::vector<uint8_t> right = {0xF0, 0xCC};
    const std::string path = buildDsf("sample_msb.dsf", 2, 5644800, 8, 2, {left, right});

    sylvakru::DsdFileReader reader;
    assert(reader.open(path).ok());
    assert(reader.dsdMultiple() == 128);
    assert(readAll(reader) == (std::vector<uint8_t>{0x0F, 0xF0, 0x33, 0xCC}));
}

void dsfSeekAlignsToBlockBoundary() {
    std::vector<uint8_t> per_channel(4096 * 3);
    for (size_t index = 0; index < per_channel.size(); ++index) {
        per_channel[index] = static_cast<uint8_t>(index % 251);
    }
    const std::string path =
        buildDsf("sample_seek.dsf", 2, 2822400, 8, 4096, {per_channel, per_channel});

    sylvakru::DsdFileReader reader;
    assert(reader.open(path).ok());
    // 目标位置换算后落在块中间，应对齐回块边界
    const int64_t target_ms = 4100LL * 8000 / 2822400 + 1;
    const int64_t actual_ms = reader.seekTo(target_ms);
    assert(actual_ms == 4096LL * 8000 / 2822400);

    uint8_t buffer[8];
    assert(reader.read(buffer, 8) == 8);
    // 对齐到第二块起点：每通道第 4096 字节
    assert(buffer[0] == per_channel[4096]);
    assert(buffer[1] == per_channel[4096]);
}

// ---- DFF ----

void dffInterleavedBytesPassThrough() {
    const std::vector<uint8_t> audio = {0x01, 0x11, 0x02, 0x12, 0x03, 0x13};
    const std::string path = buildDff("sample.dff", 2, 2822400, "DSD ", audio);

    sylvakru::DsdFileReader reader;
    assert(reader.open(path).ok());
    assert(reader.formatName() == "dff");
    assert(reader.sampleRate() == 2822400);
    assert(reader.channels() == 2);
    assert(readAll(reader) == audio);
}

void dffSeekAlignsToDopFramePair() {
    std::vector<uint8_t> audio(64);
    for (size_t index = 0; index < audio.size(); ++index) {
        audio[index] = static_cast<uint8_t>(index);
    }
    // 用 8000 Hz 的假速率让 1 ms 恰好等于每通道 1 字节，便于构造奇数目标位置
    const std::string path = buildDff("sample_seek.dff", 2, 8000, "DSD ", audio);

    sylvakru::DsdFileReader reader;
    assert(reader.open(path).ok());
    // 每通道 32 字节；定位到第 3 字节应对齐回第 2 字节（DoP 双字节边界）
    assert(reader.seekTo(3) == 2);
    uint8_t buffer[4];
    assert(reader.read(buffer, 4) == 4);
    // 交错流里每通道第 2 字节从下标 4 开始
    assert(buffer[0] == 4 && buffer[1] == 5 && buffer[2] == 6 && buffer[3] == 7);
}

void dffStreamingUsesDeclaredSizeAndGatesReads() {
    std::vector<uint8_t> audio(64);
    for (size_t index = 0; index < audio.size(); ++index) {
        audio[index] = static_cast<uint8_t>(index);
    }
    const std::vector<uint8_t> full_bytes = buildDffBytes(2, 8000, "DSD ", audio);
    const size_t data_start = full_bytes.size() - audio.size();
    // 只写入头部 + 前 4 字节数据，模拟仍在下载的 .part 文件
    const std::string path = tempPath("stream.dff");
    writeFile(path, std::vector<uint8_t>(
        full_bytes.begin(), full_bytes.begin() + data_start + 4));

    sylvakru::DsdFileReader reader;
    assert(reader.open(path, /*streaming=*/true).ok());
    // 时长按 chunk 头声明值取，而不是当前文件长度（每通道 32 字节 → 32 ms）
    assert(reader.durationMs() == 32);
    assert(reader.canReadAt(fileLength(path)));
    uint8_t buffer[4];
    assert(reader.read(buffer, 4) == 4);
    // 下一帧还没下载到，不能读（否则会被误判成文件结尾）
    assert(!reader.canReadAt(fileLength(path)));
    // 模拟下载完成：其余数据就位后能一直读到声明的结尾
    writeFile(path, full_bytes);
    assert(reader.canReadAt(fileLength(path)));
    assert(readAll(reader).size() == 60);
}

void dsfStreamingCanReadAtRequiresFullBlockGroup() {
    std::vector<uint8_t> per_channel(8);
    for (size_t index = 0; index < per_channel.size(); ++index) {
        per_channel[index] = static_cast<uint8_t>(index + 1);
    }
    const std::vector<uint8_t> full_bytes =
        buildDsfBytes(2, 2822400, 8, 4, {per_channel, per_channel});
    const size_t data_start = full_bytes.size() - 16;
    // 第一个块组完整，第二个块组缺最后 1 字节
    const std::string path = tempPath("stream.dsf");
    writeFile(path, std::vector<uint8_t>(
        full_bytes.begin(), full_bytes.begin() + data_start + 8 + 7));

    sylvakru::DsdFileReader reader;
    assert(reader.open(path, /*streaming=*/true).ok());
    assert(reader.canReadAt(fileLength(path)));
    uint8_t buffer[8];
    assert(reader.read(buffer, 8) == 8);
    // 第二块组按 planar 布局需要 (channels-1)×blockSize+valid 字节齐全
    assert(!reader.canReadAt(fileLength(path)));
    writeFile(path, full_bytes);
    assert(reader.canReadAt(fileLength(path)));
    assert(reader.read(buffer, 8) == 8);
}

void dffDstCompressionIsRejected() {
    const std::string path =
        buildDff("sample_dst.dff", 2, 2822400, "DST ", std::vector<uint8_t>(8, 0));
    sylvakru::DsdFileReader reader;
    const auto result = reader.open(path);
    assert(!result.ok());
    assert(result.message.find("DST") != std::string::npos);
}

void nonDsdRateHasNoMultiple() {
    const std::string path =
        buildDff("sample_rate.dff", 2, 3072000, "DSD ", std::vector<uint8_t>(4, 0));
    sylvakru::DsdFileReader reader;
    assert(reader.open(path).ok());
    assert(reader.dsdMultiple() == 0);
}

// ---- DoP ----

void dopMarkerAlternatesAndBytesArePackedLittleEndian() {
    sylvakru::DopPacketizer packetizer(2);
    // 两帧：帧 1 取 L=0xA1/0xA2、R=0xB1/0xB2；帧 2 取 L=0xC1/0xC2、R=0xD1/0xD2
    const uint8_t input[] = {0xA1, 0xB1, 0xA2, 0xB2, 0xC1, 0xD1, 0xC2, 0xD2};
    std::vector<uint8_t> output;
    packetizer.encode(input, 8, output);
    // 24-bit 小端：低字节=时间靠后的 DSD 字节，中字节=时间靠前，高字节=标记
    const std::vector<uint8_t> expected = {
        0xA2, 0xA1, 0x05,
        0xB2, 0xB1, 0x05,
        0xC2, 0xC1, 0xFA,
        0xD2, 0xD1, 0xFA,
    };
    assert(output == expected);
}

void dopCarryKeepsPartialFrameAcrossWrites() {
    sylvakru::DopPacketizer packetizer(2);
    const uint8_t first_input[] = {0xA1, 0xB1, 0xA2};
    std::vector<uint8_t> first;
    packetizer.encode(first_input, 3, first);
    assert(first.empty());
    const uint8_t second_input[] = {0xB2};
    std::vector<uint8_t> second;
    packetizer.encode(second_input, 1, second);
    const std::vector<uint8_t> expected = {
        0xA2, 0xA1, 0x05,
        0xB2, 0xB1, 0x05,
    };
    assert(second == expected);
}

void dopSilenceUses0x69AndKeepsMarkerPhase() {
    sylvakru::DopPacketizer packetizer(2);
    const uint8_t input[] = {0xA1, 0xB1, 0xA2, 0xB2};
    std::vector<uint8_t> encoded;
    packetizer.encode(input, 4, encoded);
    std::vector<uint8_t> silence;
    packetizer.encodeSilence(2, silence);
    const std::vector<uint8_t> expected = {
        0x69, 0x69, 0xFA,
        0x69, 0x69, 0xFA,
        0x69, 0x69, 0x05,
        0x69, 0x69, 0x05,
    };
    assert(silence == expected);
}

void dopDrainPadsTailWithSilence() {
    sylvakru::DopPacketizer packetizer(2);
    const uint8_t input[] = {0xA1, 0xB1};
    std::vector<uint8_t> encoded;
    packetizer.encode(input, 2, encoded);
    std::vector<uint8_t> drained;
    packetizer.drain(drained);
    const std::vector<uint8_t> expected = {
        0x69, 0xA1, 0x05,
        0x69, 0xB1, 0x05,
    };
    assert(drained == expected);
    packetizer.drain(drained);
    assert(drained.empty());
}

void dopResetRestoresMarkerPhase() {
    sylvakru::DopPacketizer packetizer(1);
    const uint8_t input[] = {0x01, 0x02};
    std::vector<uint8_t> output;
    packetizer.encode(input, 2, output);
    packetizer.reset();
    const uint8_t next[] = {0x03, 0x04};
    packetizer.encode(next, 2, output);
    assert(output[2] == 0x05);
}

// ---- Native DSD ----

void nativeU32leGroupsPerChannelBytesInTimeOrder() {
    sylvakru::NativeDsdPacketizer packetizer(2, 4, /*big_endian=*/false);
    // 交错流 L0 R0 L1 R1 L2 R2 L3 R3
    const uint8_t input[] = {0x01, 0x11, 0x02, 0x12, 0x03, 0x13, 0x04, 0x14};
    std::vector<uint8_t> output;
    packetizer.encode(input, 8, output);
    // u32le：每声道连续 4 字节按时间正序 [L0..L3][R0..R3]
    assert(output == (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x11, 0x12, 0x13, 0x14}));
}

void nativeU32beReversesBytesWithinSlot() {
    sylvakru::NativeDsdPacketizer packetizer(2, 4, /*big_endian=*/true);
    const uint8_t input[] = {0x01, 0x11, 0x02, 0x12, 0x03, 0x13, 0x04, 0x14};
    std::vector<uint8_t> output;
    packetizer.encode(input, 8, output);
    // u32be：LSB（最早字节）在 word 高地址，组内倒序
    assert(output == (std::vector<uint8_t>{0x04, 0x03, 0x02, 0x01, 0x14, 0x13, 0x12, 0x11}));
}

void nativeU16leCarryAcrossWrites() {
    sylvakru::NativeDsdPacketizer packetizer(2, 2, /*big_endian=*/false);
    const uint8_t first[] = {0x01, 0x11, 0x02};
    std::vector<uint8_t> output;
    packetizer.encode(first, 3, output);
    assert(output.empty());
    const uint8_t second[] = {0x12};
    packetizer.encode(second, 1, output);
    assert(output == (std::vector<uint8_t>{0x01, 0x02, 0x11, 0x12}));
}

void nativeSilenceAndDrainUse0x69() {
    sylvakru::NativeDsdPacketizer packetizer(2, 2, /*big_endian=*/false);
    std::vector<uint8_t> silence;
    packetizer.encodeSilence(2, silence);
    assert(silence == std::vector<uint8_t>(8, 0x69));
    const uint8_t input[] = {0x01, 0x11};
    std::vector<uint8_t> encoded;
    packetizer.encode(input, 2, encoded);
    std::vector<uint8_t> drained;
    packetizer.drain(drained);
    assert(drained == (std::vector<uint8_t>{0x01, 0x69, 0x11, 0x69}));
    packetizer.drain(drained);
    assert(drained.empty());
}

void nativeBytesPerSampleMapping() {
    assert(sylvakru::nativeDsdBytesPerSample("u8") == 1);
    assert(sylvakru::nativeDsdBytesPerSample("u16le") == 2);
    assert(sylvakru::nativeDsdBytesPerSample("u32le") == 4);
    assert(sylvakru::nativeDsdBytesPerSample("u32be") == 4);
    assert(sylvakru::nativeDsdBytesPerSample("") == 0);
    assert(sylvakru::nativeDsdBytesPerSample("dop") == 0);
}

void missingFileFailsToOpen() {
    sylvakru::DsdFileReader reader;
    const auto result = reader.open(tempPath("missing.dsf"));
    assert(!result.ok());
    assert(result.error == sylvakru::DsdError::kOpenFailed);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        g_temp_dir = argv[1];
    }
#define RUN(test)                        \
    do {                                 \
        std::fprintf(stderr, #test "\n"); \
        test();                          \
    } while (false)
    RUN(dsfLsbFirstBlocksAreInterleavedAndBitReversed);
    RUN(dsfMsbFirstBytesPassThrough);
    RUN(dsfSeekAlignsToBlockBoundary);
    RUN(dffInterleavedBytesPassThrough);
    RUN(dffSeekAlignsToDopFramePair);
    RUN(dffStreamingUsesDeclaredSizeAndGatesReads);
    RUN(dsfStreamingCanReadAtRequiresFullBlockGroup);
    RUN(dffDstCompressionIsRejected);
    RUN(nonDsdRateHasNoMultiple);
    RUN(dopMarkerAlternatesAndBytesArePackedLittleEndian);
    RUN(dopCarryKeepsPartialFrameAcrossWrites);
    RUN(dopSilenceUses0x69AndKeepsMarkerPhase);
    RUN(dopDrainPadsTailWithSilence);
    RUN(dopResetRestoresMarkerPhase);
    RUN(nativeU32leGroupsPerChannelBytesInTimeOrder);
    RUN(nativeU32beReversesBytesWithinSlot);
    RUN(nativeU16leCarryAcrossWrites);
    RUN(nativeSilenceAndDrainUse0x69);
    RUN(nativeBytesPerSampleMapping);
    RUN(missingFileFailsToOpen);
#undef RUN
    std::fprintf(stderr, "all tests passed\n");
    return 0;
}
