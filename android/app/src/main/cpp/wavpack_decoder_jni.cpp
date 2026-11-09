// UsbWavPackNative 的 JNI 边界：把 WavPackDecoder 以不透明句柄暴露给 Kotlin。
// 句柄由 create/destroy 管理所有权，其余调用均假定单线程（USB 独占解码线程）使用。
#include <jni.h>

#include <cstdint>
#include <string>

#include "wavpack_decoder.h"

namespace {

// 除解码器本体外，记录最近一次 readFrames 的错误与流结束状态，
// 避免 JNI 返回值里同时打包帧数/错误/EOS 三种信息。
struct WavPackHandle {
    sylvakru::WavPackDecoder decoder;
    std::string last_error;
    bool end_of_stream = false;
};

WavPackHandle* fromHandle(jlong handle) {
    return reinterpret_cast<WavPackHandle*>(handle);
}

jstring nullableError(JNIEnv* env, const std::string& message) {
    if (message.empty()) {
        return nullptr;
    }
    return env->NewStringUTF(message.c_str());
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_create(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new WavPackHandle());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_open(
    JNIEnv* env,
    jobject,
    jlong handle,
    jstring path) {
    auto* holder = fromHandle(handle);
    if (holder == nullptr || path == nullptr) {
        return nullableError(env, "Invalid WavPack decoder handle.");
    }
    const char* path_chars = env->GetStringUTFChars(path, nullptr);
    if (path_chars == nullptr) {
        return nullableError(env, "Failed to read WavPack path.");
    }
    const auto result = holder->decoder.open(path_chars);
    env->ReleaseStringUTFChars(path, path_chars);
    holder->end_of_stream = false;
    holder->last_error.clear();
    return nullableError(env, result.ok() ? std::string() : result.message);
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_streamInfo(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* holder = fromHandle(handle);
    jlong values[4] = {0, 0, 0, 0};
    if (holder != nullptr) {
        const auto& info = holder->decoder.streamInfo();
        values[0] = static_cast<jlong>(info.sample_rate);
        values[1] = static_cast<jlong>(info.channels);
        values[2] = static_cast<jlong>(info.valid_bits_per_sample);
        values[3] = static_cast<jlong>(info.total_frames);
    }
    jlongArray result = env->NewLongArray(4);
    if (result != nullptr) {
        env->SetLongArrayRegion(result, 0, 4, values);
    }
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_readFrames(
    JNIEnv* env,
    jobject,
    jlong handle,
    jobject buffer,
    jint capacity_frames) {
    auto* holder = fromHandle(handle);
    if (holder == nullptr) {
        return -1;
    }
    if (buffer == nullptr || capacity_frames <= 0) {
        holder->last_error = "WavPack target buffer is invalid.";
        return -1;
    }
    auto* output = static_cast<int32_t*>(env->GetDirectBufferAddress(buffer));
    const jlong capacity_bytes = env->GetDirectBufferCapacity(buffer);
    const auto& info = holder->decoder.streamInfo();
    const jlong required_bytes = static_cast<jlong>(capacity_frames) *
        info.channels * static_cast<jlong>(sizeof(int32_t));
    if (output == nullptr || capacity_bytes < required_bytes) {
        holder->last_error = "WavPack target buffer is not a large enough direct buffer.";
        return -1;
    }
    const auto read = holder->decoder.readFrames(
        output,
        static_cast<uint32_t>(capacity_frames));
    if (!read.ok()) {
        holder->last_error = read.message;
        return -1;
    }
    holder->end_of_stream = read.end_of_stream;
    holder->last_error.clear();
    return static_cast<jint>(read.frames);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_endOfStream(
    JNIEnv*,
    jobject,
    jlong handle) {
    auto* holder = fromHandle(handle);
    return (holder != nullptr && holder->end_of_stream) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_lastError(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* holder = fromHandle(handle);
    if (holder == nullptr) {
        return nullableError(env, "Invalid WavPack decoder handle.");
    }
    return nullableError(env, holder->last_error);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_seekToFrame(
    JNIEnv* env,
    jobject,
    jlong handle,
    jlong frame) {
    auto* holder = fromHandle(handle);
    if (holder == nullptr || frame < 0) {
        return nullableError(env, "Invalid WavPack decoder handle.");
    }
    const auto result = holder->decoder.seekToFrame(static_cast<uint64_t>(frame));
    if (result.ok()) {
        holder->end_of_stream = false;
        return nullptr;
    }
    return nullableError(env, result.message);
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbWavPackNative_destroy(
    JNIEnv*,
    jobject,
    jlong handle) {
    delete fromHandle(handle);
}
