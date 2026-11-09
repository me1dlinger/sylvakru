#pragma once

#include <cstdint>
#include <vector>

namespace sylvakru {

// 数字音量线性增益的 Q16.16 定点满刻度（1.0），与 Kotlin UNITY_GAIN_Q16 一致。
inline constexpr int kPcmUnityGainQ16 = 65536;

// PcmIsoPacketizer 的纯计算核心，从引擎 Kotlin 内部类逐行为对齐下沉：
// 槽位/位深转换、数字音量、暂停恢复淡入、切换尾部淡出、反馈驱动的包长
// 状态机。线程模型/缓冲/URB 节奏/日志仍在 Kotlin 侧（对拍测试
// tests/usb_pcm_packetizer_test.cpp）。
//
// 音量安全红线：所有增益运算必须与 Kotlin 原实现一个 bit 不差；
// DoP 路径由调用方以 unity 增益进入，转换只做槽位对齐移位（低位补零），
// 绝不允许触碰采样值本身。
class PcmPacketizerCore {
public:
    PcmPacketizerCore(
        int sample_rate,
        int packets_per_second,
        int channels,
        int input_bytes_per_sample,
        int input_bit_depth,
        int usb_bytes_per_sample,
        int usb_bit_resolution,
        int feedback_output_packet_divisor);

    // 对应 Kotlin applyFadeInIfNeeded(convertPcmToUsbSlots(data))：
    // 返回 false 表示满刻度直通且无淡入，输出未生成，调用方沿用原缓冲
    // （含不足一帧的尾巴，与 Kotlin 直通语义一致）；返回 true 时 out 为
    // 转换（+淡入）结果。无论直通与否都会捕获最后一帧的 USB 域采样。
    bool process(const uint8_t* data, int length, int gain_q16, std::vector<uint8_t>& out);

    // 暂停恢复时对续播数据做短淡入；seek 的 reset() 不清计数，
    // 暂停中 seek 再恢复同样有淡入护住拼接点。
    void beginFadeIn(int total_frames);

    // 旧流淡出到静音的尾部（fade + silence 帧，USB 槽位域）。
    // 无已捕获的上一帧时输出空，调用方改写整段静音。
    void transitionTail(int fade_frames, int silence_frames, std::vector<uint8_t>& out);

    struct PacketSize {
        int packet_bytes = 0;
        // 反馈路径的实际/名义每包帧数（Q16.16），仅 state != 0 时有效
        int output_feedback_q16 = 0;
        int nominal_q16 = 0;
        // 0 = 无反馈值；1 = 反馈被接受；2 = 反馈越界被拒（回落名义速率）
        int state = 0;
    };

    // 对应 Kotlin nextPacketBytes()：反馈值在 [名义-1/8, 名义+1/2] 窗口内
    // 按 Q16 余数累积出整帧数；越界或整帧数为 0 时回落到名义采样率余数路径。
    PacketSize nextPacketBytes(int feedback_q16);

    // 对应 Kotlin reset()：清包长余数与最后一帧，不清淡入计数。
    void reset();

    int bytesPerFrame() const { return bytes_per_frame_; }

private:
    int32_t readSignedLittleEndian(const uint8_t* data, int offset, int bytes, int bit_depth) const;
    void writeLittleEndian(uint8_t* data, int offset, int bytes, int32_t value) const;
    int32_t sampleForUsbTransition(int32_t sample, int gain_q16) const;
    void applyFadeInIfNeeded(std::vector<uint8_t>& data);

    const int sample_rate_;
    const int packets_per_second_;
    const int channels_;
    const int input_bytes_per_sample_;
    const int input_bit_depth_;
    const int usb_bytes_per_sample_;
    const int usb_bit_resolution_;
    const int feedback_output_packet_divisor_;
    const int bytes_per_frame_;
    const int input_bytes_per_frame_;

    std::vector<int32_t> last_usb_samples_;
    bool has_last_usb_frame_ = false;
    int fade_in_total_frames_ = 0;
    int fade_in_frames_done_ = 0;
    int sample_remainder_ = 0;
    long long feedback_remainder_q16_ = 0;
};

// 恢复播放的逐帧淡入增益：从 0 线性升到满刻度（与 Kotlin pcmFadeInGainQ16 一致）。
int pcmFadeInGainQ16(int frame_index, int total_frames);

// 源位深域施加 Q16 线性增益后做 USB 槽位对齐移位（与 Kotlin
// pcmSampleForUsbTransition 一致，负数为算术移位、放大移位按补码回绕）。
int32_t pcmSampleForUsbTransition(
    int32_t sample,
    int input_bit_depth,
    int usb_bit_resolution,
    int gain_q16);

// 最后一帧线性淡出 fade_frames 帧再垫 silence_frames 帧零（与 Kotlin
// pcmFadeToSilence 一致）。要求 last_samples 非空、fade_frames > 0。
std::vector<int32_t> pcmFadeToSilence(
    const std::vector<int32_t>& last_samples,
    int fade_frames,
    int silence_frames);

}  // namespace sylvakru
