// UsbPcmNative 的 JNI 边界：把 PcmPacketizerCore 以不透明句柄暴露给 Kotlin
// PcmIsoPacketizer。句柄由 create/destroy 管理所有权；调用均来自单个 USB
// 独占写线程（引擎先 join 再切换），无并发访问。
// - process 返回 null 表示满刻度直通（调用方沿用原缓冲）
// - transitionTail 返回空数组表示尚无已捕获帧（调用方改写整段静音）
// - nextPacketBytes 返回 [包字节数, 实际反馈Q16, 名义Q16, 状态]，
//   状态 0=无反馈 1=接受 2=拒绝
#include <jni.h>

#include <cstdint>
#include <vector>

#include "usb_pcm_packetizer.h"

namespace {

struct PcmPacketizerHandle {
    PcmPacketizerHandle(
        int sample_rate,
        int packets_per_second,
        int channels,
        int input_bytes_per_sample,
        int input_bit_depth,
        int usb_bytes_per_sample,
        int usb_bit_resolution,
        int feedback_output_packet_divisor)
        : core(
              sample_rate,
              packets_per_second,
              channels,
              input_bytes_per_sample,
              input_bit_depth,
              usb_bytes_per_sample,
              usb_bit_resolution,
              feedback_output_packet_divisor) {}

    sylvakru::PcmPacketizerCore core;
    // 转换输出的复用缓冲
    std::vector<uint8_t> out;
};

PcmPacketizerHandle* fromHandle(jlong handle) {
    return reinterpret_cast<PcmPacketizerHandle*>(handle);
}

jbyteArray toByteArray(JNIEnv* env, const std::vector<uint8_t>& bytes) {
    jbyteArray result = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (result != nullptr && !bytes.empty()) {
        env->SetByteArrayRegion(
            result,
            0,
            static_cast<jsize>(bytes.size()),
            reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return result;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_create(
    JNIEnv*,
    jobject,
    jint sample_rate,
    jint packets_per_second,
    jint channels,
    jint input_bytes_per_sample,
    jint input_bit_depth,
    jint usb_bytes_per_sample,
    jint usb_bit_resolution,
    jint feedback_output_packet_divisor) {
    return reinterpret_cast<jlong>(new PcmPacketizerHandle(
        sample_rate,
        packets_per_second,
        channels,
        input_bytes_per_sample,
        input_bit_depth,
        usb_bytes_per_sample,
        usb_bit_resolution,
        feedback_output_packet_divisor));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_process(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray data,
    jint gain_q16) {
    auto* holder = fromHandle(handle);
    if (holder == nullptr || data == nullptr) {
        return toByteArray(env, {});
    }
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (bytes == nullptr) {
        return toByteArray(env, {});
    }
    const bool converted = holder->core.process(
        reinterpret_cast<const uint8_t*>(bytes),
        static_cast<int>(env->GetArrayLength(data)),
        gain_q16,
        holder->out);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    if (!converted) {
        // 满刻度直通：返回 null，调用方沿用原缓冲
        return nullptr;
    }
    return toByteArray(env, holder->out);
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_beginFadeIn(
    JNIEnv*,
    jobject,
    jlong handle,
    jint total_frames) {
    auto* holder = fromHandle(handle);
    if (holder != nullptr) {
        holder->core.beginFadeIn(total_frames);
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_transitionTail(
    JNIEnv* env,
    jobject,
    jlong handle,
    jint fade_frames,
    jint silence_frames) {
    auto* holder = fromHandle(handle);
    if (holder == nullptr) {
        return toByteArray(env, {});
    }
    holder->core.transitionTail(fade_frames, silence_frames, holder->out);
    return toByteArray(env, holder->out);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_nextPacketBytes(
    JNIEnv* env,
    jobject,
    jlong handle,
    jint feedback_q16) {
    jint values[4] = {0, 0, 0, 0};
    auto* holder = fromHandle(handle);
    if (holder != nullptr) {
        const auto size = holder->core.nextPacketBytes(feedback_q16);
        values[0] = size.packet_bytes;
        values[1] = size.output_feedback_q16;
        values[2] = size.nominal_q16;
        values[3] = size.state;
    }
    jintArray result = env->NewIntArray(4);
    if (result != nullptr) {
        env->SetIntArrayRegion(result, 0, 4, values);
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_reset(JNIEnv*, jobject, jlong handle) {
    auto* holder = fromHandle(handle);
    if (holder != nullptr) {
        holder->core.reset();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbPcmNative_destroy(JNIEnv*, jobject, jlong handle) {
    delete fromHandle(handle);
}
