#include "usb_pcm_packetizer.h"

#include <algorithm>

namespace sylvakru {

int pcmFadeInGainQ16(int frame_index, int total_frames) {
    if (total_frames <= 0) {
        return kPcmUnityGainQ16;
    }
    if (frame_index >= total_frames) {
        return kPcmUnityGainQ16;
    }
    const long long clamped = std::max(frame_index, 0);
    return static_cast<int>((clamped << 16) / total_frames);
}

int32_t pcmSampleForUsbTransition(
    int32_t sample,
    int input_bit_depth,
    int usb_bit_resolution,
    int gain_q16) {
    const long long gain = std::min(std::max(gain_q16, 0), kPcmUnityGainQ16);
    // 源位深域施加线性增益（64 位防溢出，算术右移与 Kotlin shr 一致）
    const int32_t adjusted =
        static_cast<int32_t>((static_cast<long long>(sample) * gain) >> 16);
    if (usb_bit_resolution >= input_bit_depth) {
        // 放大移位按补码回绕（与 Kotlin shl 一致），用无符号位移避开 UB
        return static_cast<int32_t>(
            static_cast<uint32_t>(adjusted) << (usb_bit_resolution - input_bit_depth));
    }
    return adjusted >> (input_bit_depth - usb_bit_resolution);
}

std::vector<int32_t> pcmFadeToSilence(
    const std::vector<int32_t>& last_samples,
    int fade_frames,
    int silence_frames) {
    if (last_samples.empty() || fade_frames <= 0 || silence_frames < 0) {
        return {};
    }
    const size_t channels = last_samples.size();
    std::vector<int32_t> result(
        static_cast<size_t>(fade_frames + silence_frames) * channels, 0);
    const int denominator = std::max(fade_frames - 1, 1);
    for (int frame = 0; frame < fade_frames; ++frame) {
        const int numerator = std::max(fade_frames - 1 - frame, 0);
        for (size_t channel = 0; channel < channels; ++channel) {
            result[frame * channels + channel] = static_cast<int32_t>(
                (static_cast<long long>(last_samples[channel]) * numerator) / denominator);
        }
    }
    return result;
}

PcmPacketizerCore::PcmPacketizerCore(
    int sample_rate,
    int packets_per_second,
    int channels,
    int input_bytes_per_sample,
    int input_bit_depth,
    int usb_bytes_per_sample,
    int usb_bit_resolution,
    int feedback_output_packet_divisor)
    : sample_rate_(sample_rate),
      packets_per_second_(packets_per_second),
      channels_(channels),
      input_bytes_per_sample_(input_bytes_per_sample),
      input_bit_depth_(input_bit_depth),
      usb_bytes_per_sample_(usb_bytes_per_sample),
      usb_bit_resolution_(usb_bit_resolution),
      feedback_output_packet_divisor_(feedback_output_packet_divisor),
      bytes_per_frame_(channels * usb_bytes_per_sample),
      input_bytes_per_frame_(channels * input_bytes_per_sample),
      last_usb_samples_(static_cast<size_t>(channels), 0) {}

int32_t PcmPacketizerCore::readSignedLittleEndian(
    const uint8_t* data,
    int offset,
    int bytes,
    int bit_depth) const {
    uint32_t value = 0;
    for (int index = 0; index < bytes; ++index) {
        value |= static_cast<uint32_t>(data[offset + index]) << (index * 8);
    }
    const int shift = std::min(std::max(32 - bit_depth, 0), 31);
    return static_cast<int32_t>(value << shift) >> shift;
}

void PcmPacketizerCore::writeLittleEndian(
    uint8_t* data,
    int offset,
    int bytes,
    int32_t value) const {
    for (int index = 0; index < bytes; ++index) {
        data[offset + index] =
            static_cast<uint8_t>((static_cast<uint32_t>(value) >> (index * 8)) & 0xff);
    }
}

void PcmPacketizerCore::beginFadeIn(int total_frames) {
    fade_in_total_frames_ = total_frames;
    fade_in_frames_done_ = 0;
}

// 在 USB slot 域就地施加逐帧淡入（与 Kotlin applyFadeInIfNeeded 一致）。
void PcmPacketizerCore::applyFadeInIfNeeded(std::vector<uint8_t>& data) {
    if (fade_in_total_frames_ == 0 || fade_in_frames_done_ >= fade_in_total_frames_) {
        return;
    }
    const int frames = static_cast<int>(data.size()) / bytes_per_frame_;
    int offset = 0;
    int frame = 0;
    while (frame < frames && fade_in_frames_done_ < fade_in_total_frames_) {
        const int gain_q16 = pcmFadeInGainQ16(fade_in_frames_done_, fade_in_total_frames_);
        for (int channel = 0; channel < channels_; ++channel) {
            const int32_t sample = readSignedLittleEndian(
                data.data(), offset, usb_bytes_per_sample_, usb_bit_resolution_);
            const int32_t faded =
                static_cast<int32_t>((static_cast<long long>(sample) * gain_q16) >> 16);
            writeLittleEndian(data.data(), offset, usb_bytes_per_sample_, faded);
            offset += usb_bytes_per_sample_;
        }
        ++fade_in_frames_done_;
        ++frame;
    }
}

bool PcmPacketizerCore::process(
    const uint8_t* data,
    int length,
    int gain_q16,
    std::vector<uint8_t>& out) {
    const bool apply_gain = gain_q16 < kPcmUnityGainQ16;
    const int frames = length / input_bytes_per_frame_;
    if (frames > 0) {
        // 捕获最后一帧的 USB 域采样（含增益、不含淡入），供切换尾部淡出使用
        int input_offset = (frames - 1) * input_bytes_per_frame_;
        for (int channel = 0; channel < channels_; ++channel) {
            const int32_t sample = readSignedLittleEndian(
                data, input_offset, input_bytes_per_sample_, input_bit_depth_);
            last_usb_samples_[channel] = sylvakru::pcmSampleForUsbTransition(
                sample, input_bit_depth_, usb_bit_resolution_, gain_q16);
            input_offset += input_bytes_per_sample_;
        }
        has_last_usb_frame_ = true;
    }

    // 满刻度且无需重排位深时零拷贝直通，保持位完美（DoP 亦走此路径或纯移位路径）。
    const bool passthrough = !apply_gain &&
        input_bytes_per_sample_ == usb_bytes_per_sample_ &&
        input_bit_depth_ == usb_bit_resolution_;
    const bool fade_active =
        fade_in_total_frames_ != 0 && fade_in_frames_done_ < fade_in_total_frames_;
    if (passthrough && !fade_active) {
        return false;
    }

    if (passthrough) {
        // 直通布局但淡入进行中：拷贝后就地淡入（Kotlin 在原缓冲上就地改）
        out.assign(data, data + length);
    } else {
        out.assign(static_cast<size_t>(frames) * bytes_per_frame_, 0);
        int input_offset = 0;
        int output_offset = 0;
        const int samples_per_frame = input_bytes_per_frame_ / input_bytes_per_sample_;
        for (int frame = 0; frame < frames; ++frame) {
            for (int index = 0; index < samples_per_frame; ++index) {
                const int32_t sample = readSignedLittleEndian(
                    data, input_offset, input_bytes_per_sample_, input_bit_depth_);
                // 在源位深域施加线性增益再做 slot 对齐移位
                const int32_t shifted = sylvakru::pcmSampleForUsbTransition(
                    sample, input_bit_depth_, usb_bit_resolution_, gain_q16);
                writeLittleEndian(out.data(), output_offset, usb_bytes_per_sample_, shifted);
                input_offset += input_bytes_per_sample_;
                output_offset += usb_bytes_per_sample_;
            }
        }
    }
    applyFadeInIfNeeded(out);
    return true;
}

void PcmPacketizerCore::transitionTail(
    int fade_frames,
    int silence_frames,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!has_last_usb_frame_) {
        return;
    }
    const std::vector<int32_t> samples =
        pcmFadeToSilence(last_usb_samples_, fade_frames, silence_frames);
    out.assign(samples.size() * usb_bytes_per_sample_, 0);
    for (size_t index = 0; index < samples.size(); ++index) {
        writeLittleEndian(
            out.data(),
            static_cast<int>(index) * usb_bytes_per_sample_,
            usb_bytes_per_sample_,
            samples[index]);
    }
}

PcmPacketizerCore::PacketSize PcmPacketizerCore::nextPacketBytes(int feedback_q16) {
    PacketSize result;
    if (feedback_q16 > 0) {
        const int output_feedback_q16 = feedback_q16 / feedback_output_packet_divisor_;
        const int nominal_frames_q16 = static_cast<int>(
            (static_cast<long long>(sample_rate_) << 16) / packets_per_second_);
        const int min_feedback_q16 = nominal_frames_q16 - (nominal_frames_q16 / 8);
        const int max_feedback_q16 = nominal_frames_q16 + (nominal_frames_q16 / 2);
        result.output_feedback_q16 = output_feedback_q16;
        result.nominal_q16 = nominal_frames_q16;
        if (output_feedback_q16 >= min_feedback_q16 &&
            output_feedback_q16 <= max_feedback_q16) {
            result.state = 1;
            feedback_remainder_q16_ += output_feedback_q16;
            const int frames = static_cast<int>(
                static_cast<unsigned long long>(feedback_remainder_q16_) >> 16);
            feedback_remainder_q16_ &= 0xffff;
            if (frames > 0) {
                result.packet_bytes = std::max(bytes_per_frame_, frames * bytes_per_frame_);
                return result;
            }
        } else {
            result.state = 2;
        }
    }

    sample_remainder_ += sample_rate_;
    const int frames = sample_remainder_ / packets_per_second_;
    sample_remainder_ %= packets_per_second_;
    result.packet_bytes = std::max(bytes_per_frame_, frames * bytes_per_frame_);
    return result;
}

void PcmPacketizerCore::reset() {
    sample_remainder_ = 0;
    feedback_remainder_q16_ = 0;
    std::fill(last_usb_samples_.begin(), last_usb_samples_.end(), 0);
    has_last_usb_frame_ = false;
}

}  // namespace sylvakru
