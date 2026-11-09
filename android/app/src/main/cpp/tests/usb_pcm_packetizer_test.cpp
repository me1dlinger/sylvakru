#include "usb_pcm_packetizer.h"

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <vector>

// 与引擎 Kotlin PcmIsoPacketizer / UsbStreamTransition 热路径纯函数逐用例对拍。
// 金标准数值取自 Kotlin 现实现的逐行推导与 UsbStreamTransitionTest 既有断言。

namespace {

using sylvakru::PcmPacketizerCore;

std::vector<uint8_t> le16(std::initializer_list<int32_t> samples) {
    std::vector<uint8_t> out;
    for (const int32_t sample : samples) {
        out.push_back(static_cast<uint8_t>(sample & 0xff));
        out.push_back(static_cast<uint8_t>((sample >> 8) & 0xff));
    }
    return out;
}

int32_t readLe(const std::vector<uint8_t>& data, int offset, int bytes) {
    uint32_t value = 0;
    for (int index = 0; index < bytes; ++index) {
        value |= static_cast<uint32_t>(data[offset + index]) << (index * 8);
    }
    const int shift = 32 - bytes * 8;
    return shift > 0 ? static_cast<int32_t>(value << shift) >> shift
                     : static_cast<int32_t>(value);
}

// ---- 纯函数（UsbStreamTransitionTest 原断言值） ----

void fadeInGainRampsLinearly() {
    assert(sylvakru::pcmFadeInGainQ16(0, 8) == 0);
    assert(sylvakru::pcmFadeInGainQ16(4, 8) == 32768);
    assert(sylvakru::pcmFadeInGainQ16(8, 8) == 65536);
    assert(sylvakru::pcmFadeInGainQ16(9, 8) == 65536);
    for (int index = 0; index < 8; ++index) {
        assert(sylvakru::pcmFadeInGainQ16(index, 8) <= sylvakru::pcmFadeInGainQ16(index + 1, 8));
    }
}

void sampleTransitionShiftsAndGains() {
    assert(sylvakru::pcmSampleForUsbTransition(32, 16, 24, 32768) == 4096);
    assert(sylvakru::pcmSampleForUsbTransition(-32, 16, 24, 32768) == -4096);
    assert(sylvakru::pcmSampleForUsbTransition(32, 16, 16, 65536) == 32);
    assert(sylvakru::pcmSampleForUsbTransition(0x123456, 24, 16, 65536) == 0x1234);
    assert(sylvakru::pcmSampleForUsbTransition(INT32_MAX, 32, 32, 65536) == INT32_MAX);
    // 负数半增益按算术右移向下取整（Kotlin shr 语义）
    assert(sylvakru::pcmSampleForUsbTransition(-3, 16, 16, 32768) == -2);
    // 增益越界钳位到 [0, 65536]
    assert(sylvakru::pcmSampleForUsbTransition(100, 16, 16, 999999) == 100);
    assert(sylvakru::pcmSampleForUsbTransition(100, 16, 16, -5) == 0);
}

void fadeToSilenceRampsLastFrame() {
    const auto tail = sylvakru::pcmFadeToSilence({1000, -1000}, 5, 2);
    assert(tail.size() == 14);
    assert(tail[0] == 1000 && tail[1] == -1000);
    assert(tail[2] == 750 && tail[3] == -750);
    assert(tail[4] == 500 && tail[5] == -500);
    assert(tail[6] == 250 && tail[7] == -250);
    assert(tail[8] == 0 && tail[9] == 0);
    assert(tail[10] == 0 && tail[12] == 0);
    // fadeFrames=1 时分母钳位为 1、分子为 0：单帧直接归零
    const auto single = sylvakru::pcmFadeToSilence({4096}, 1, 0);
    assert(single.size() == 1 && single[0] == 0);
    // 32 位满幅样本乘分子须走 64 位不溢出（Kotlin Long 语义）
    const auto wide = sylvakru::pcmFadeToSilence({INT32_MAX, -INT32_MAX}, 9, 1);
    assert(wide.front() == INT32_MAX && wide[1] == -INT32_MAX);
    assert(wide[wide.size() - 1] == 0 && wide[wide.size() - 2] == 0);
}

// ---- process：直通/转换/增益/淡入 ----

void passthroughKeepsBufferUntouched() {
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 2, 16, 1);
    const auto input = le16({0x1234, -0x1234, 0x0777, -0x0777});
    std::vector<uint8_t> out;
    assert(!core.process(input.data(), static_cast<int>(input.size()), 65536, out));
    // 直通也要捕获最后一帧：尾部淡出应从 0x0777/-0x0777 开始
    core.transitionTail(1, 1, out);
    assert(out.size() == 8);
    assert(readLe(out, 0, 2) == 0);  // fade=1 帧直接归零
}

void passthroughKeepsPartialFrameTail() {
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 2, 16, 1);
    const std::vector<uint8_t> input = {0x01, 0x02, 0x03};  // 不足一帧
    std::vector<uint8_t> out;
    assert(!core.process(input.data(), 3, 65536, out));  // 原样直通（尾巴保留在调用方）
}

void convertsBitDepthUpShift() {
    // 16-bit 输入 → 24-bit/3 字节槽位：左移 8，低字节补零
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 3, 24, 1);
    const auto input = le16({0x1234, -0x1234});
    std::vector<uint8_t> out;
    assert(core.process(input.data(), static_cast<int>(input.size()), 65536, out));
    assert(out.size() == 6);
    assert(readLe(out, 0, 3) == 0x123400);
    assert(readLe(out, 3, 3) == -0x123400);
    assert(out[0] == 0x00 && out[3] == 0x00);
}

void dopBitsSurviveSlotAlignment() {
    // DoP 红线：24-bit DoP 帧（含 0x05/0xFA 标记）以 unity 增益进 32-bit 槽位，
    // 高 3 字节逐位不变、低字节全零——任何数值触碰都会破坏标记
    PcmPacketizerCore core(176400, 8000, 2, 3, 24, 4, 32, 1);
    const std::vector<uint8_t> input = {
        0xA2, 0xA1, 0x05, 0xB2, 0xB1, 0x05,
        0xC2, 0xC1, 0xFA, 0xD2, 0xD1, 0xFA,
    };
    std::vector<uint8_t> out;
    assert(core.process(input.data(), static_cast<int>(input.size()), 65536, out));
    assert(out.size() == 16);
    const uint8_t expected[] = {
        0x00, 0xA2, 0xA1, 0x05, 0x00, 0xB2, 0xB1, 0x05,
        0x00, 0xC2, 0xC1, 0xFA, 0x00, 0xD2, 0xD1, 0xFA,
    };
    for (int index = 0; index < 16; ++index) {
        assert(out[index] == expected[index]);
    }
}

void gainAppliesInSourceDomain() {
    PcmPacketizerCore core(48000, 8000, 1, 2, 16, 2, 16, 1);
    const auto input = le16({1000, -1000, -3});
    std::vector<uint8_t> out;
    assert(core.process(input.data(), static_cast<int>(input.size()), 32768, out));
    assert(out.size() == 6);
    assert(readLe(out, 0, 2) == 500);
    assert(readLe(out, 2, 2) == -500);
    assert(readLe(out, 4, 2) == -2);  // 算术右移向下取整
    // 零增益全零
    assert(core.process(input.data(), static_cast<int>(input.size()), 0, out));
    assert(readLe(out, 0, 2) == 0 && readLe(out, 2, 2) == 0);
}

void conversionDropsPartialFrameTail() {
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 3, 24, 1);
    const std::vector<uint8_t> input = {0x01, 0x02, 0x03, 0x04, 0x05};  // 1 帧 + 1 字节
    std::vector<uint8_t> out;
    assert(core.process(input.data(), 5, 65536, out));
    assert(out.size() == 6);  // 只保留完整帧
}

void fadeInRampsAcrossWrites() {
    PcmPacketizerCore core(48000, 8000, 1, 2, 16, 2, 16, 1);
    core.beginFadeIn(4);
    const auto first = le16({0x4000, 0x4000});
    std::vector<uint8_t> out;
    // 直通布局但淡入进行中：拷贝后就地淡入
    assert(core.process(first.data(), static_cast<int>(first.size()), 65536, out));
    assert(readLe(out, 0, 2) == 0);       // 帧 0：增益 0
    assert(readLe(out, 2, 2) == 0x1000);  // 帧 1：增益 16384
    assert(core.process(first.data(), static_cast<int>(first.size()), 65536, out));
    assert(readLe(out, 0, 2) == 0x2000);  // 帧 2：增益 32768
    assert(readLe(out, 2, 2) == 0x3000);  // 帧 3：增益 49152
    // 淡入完成后恢复零拷贝直通
    assert(!core.process(first.data(), static_cast<int>(first.size()), 65536, out));
    // seek 的 reset() 不清淡入计数：reset 后仍保持已完成状态
    core.reset();
    assert(!core.process(first.data(), static_cast<int>(first.size()), 65536, out));
}

void transitionTailUsesGainedLastFrame() {
    PcmPacketizerCore core(48000, 8000, 1, 2, 16, 2, 16, 1);
    std::vector<uint8_t> out;
    core.transitionTail(2, 1, out);
    assert(out.empty());  // 尚无已捕获帧：调用方改写整段静音
    const auto input = le16({1000});
    std::vector<uint8_t> converted;
    core.process(input.data(), static_cast<int>(input.size()), 32768, converted);
    core.transitionTail(2, 1, out);
    assert(out.size() == 6);
    assert(readLe(out, 0, 2) == 500);  // 最后一帧按增益后的 USB 域值起淡
    assert(readLe(out, 2, 2) == 0);
    assert(readLe(out, 4, 2) == 0);
    // reset 清掉最后一帧
    core.reset();
    core.transitionTail(2, 1, out);
    assert(out.empty());
}

// ---- nextPacketBytes：名义速率余数与反馈窗口 ----

void nominalPacketSizeCarriesRemainder() {
    PcmPacketizerCore core(44100, 1000, 2, 2, 16, 2, 16, 1);
    const int bytes_per_frame = 4;
    long long total_frames = 0;
    for (int packet = 0; packet < 1000; ++packet) {
        const auto size = core.nextPacketBytes(0);
        assert(size.state == 0);
        assert(size.packet_bytes % bytes_per_frame == 0);
        total_frames += size.packet_bytes / bytes_per_frame;
    }
    assert(total_frames == 44100);  // 1 秒整帧数不丢不多
}

void feedbackWithinWindowDrivesPacketSize() {
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 2, 16, 1);
    const int nominal = (48000LL << 16) / 8000;  // 393216 = 6.0 帧
    const int feedback = 425984;                 // 6.5 帧，窗口内
    auto size = core.nextPacketBytes(feedback);
    assert(size.state == 1);
    assert(size.output_feedback_q16 == feedback);
    assert(size.nominal_q16 == nominal);
    assert(size.packet_bytes == 6 * 4);  // 6 帧，余 0.5 帧
    size = core.nextPacketBytes(feedback);
    assert(size.packet_bytes == 7 * 4);  // 0.5+6.5 → 7 帧
}

void feedbackOutOfWindowFallsBackToNominal() {
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 2, 16, 1);
    // 12 帧/包远超 名义+1/2 上限 → 拒绝并回落名义路径
    const auto size = core.nextPacketBytes(12 << 16);
    assert(size.state == 2);
    assert(size.packet_bytes == 6 * 4);
}

void feedbackAcceptedButSubFrameFallsThrough() {
    // 名义 1.0 帧/包：反馈 60000（约 0.92 帧）在窗口内但不足一帧，
    // 余数保留同时回落名义路径出包（与 Kotlin fallthrough 语义一致）
    PcmPacketizerCore core(8000, 8000, 1, 2, 16, 2, 16, 1);
    auto size = core.nextPacketBytes(60000);
    assert(size.state == 1);
    assert(size.packet_bytes == 1 * 2);  // 名义路径：1 帧
    // 第二次反馈余数凑满一整帧，从反馈路径出包
    size = core.nextPacketBytes(60000);
    assert(size.state == 1);
    assert(size.packet_bytes == 1 * 2);
}

void feedbackDivisorScalesOutputPackets() {
    PcmPacketizerCore core(48000, 8000, 2, 2, 16, 2, 16, 2);
    // 除数 2：原始反馈 13 帧 → 输出包 6.5 帧，窗口内
    const auto size = core.nextPacketBytes(13 << 16);
    assert(size.state == 1);
    assert(size.output_feedback_q16 == (13 << 16) / 2);
    assert(size.packet_bytes == 6 * 4);
}

void resetClearsRemaindersButNotFade() {
    PcmPacketizerCore core(44100, 1000, 1, 2, 16, 2, 16, 1);
    core.nextPacketBytes(0);  // 产生 100 的采样率余数
    core.reset();
    long long total = 0;
    for (int packet = 0; packet < 1000; ++packet) {
        total += core.nextPacketBytes(0).packet_bytes / 2;
    }
    assert(total == 44100);  // 余数被清零后重新从整秒对齐
}

}  // namespace

int main() {
#define RUN(test)                         \
    do {                                  \
        std::fprintf(stderr, #test "\n"); \
        test();                           \
    } while (false)
    RUN(fadeInGainRampsLinearly);
    RUN(sampleTransitionShiftsAndGains);
    RUN(fadeToSilenceRampsLastFrame);
    RUN(passthroughKeepsBufferUntouched);
    RUN(passthroughKeepsPartialFrameTail);
    RUN(convertsBitDepthUpShift);
    RUN(dopBitsSurviveSlotAlignment);
    RUN(gainAppliesInSourceDomain);
    RUN(conversionDropsPartialFrameTail);
    RUN(fadeInRampsAcrossWrites);
    RUN(transitionTailUsesGainedLastFrame);
    RUN(nominalPacketSizeCarriesRemainder);
    RUN(feedbackWithinWindowDrivesPacketSize);
    RUN(feedbackOutOfWindowFallsBackToNominal);
    RUN(feedbackAcceptedButSubFrameFallsThrough);
    RUN(feedbackDivisorScalesOutputPackets);
    RUN(resetClearsRemaindersButNotFade);
#undef RUN
    std::fprintf(stderr, "all tests passed\n");
    return 0;
}
