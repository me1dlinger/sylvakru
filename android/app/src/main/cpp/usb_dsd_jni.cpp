// UsbDsdNative 的 JNI 边界：把 DsdFileReader 与 DoP/Native 打包器以不透明句柄
// 暴露给 Kotlin。句柄由 create/destroy 管理所有权；读取器假定单线程（USB 独占
// 解码线程）使用，打包器的写线程与静音填充线程由引擎先 join 再启动保证互斥。
#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "usb_dsd.h"

namespace {

struct DsdReaderHandle {
    sylvakru::DsdFileReader reader;
    // JNI 读取的中转缓冲，复用避免每次 read 分配
    std::vector<uint8_t> scratch;
};

struct DsdEncoderHandle {
    std::unique_ptr<sylvakru::DsdStreamEncoder> encoder;
    // 仅 DoP 打包器需要 reset；native 打包器时为 nullptr
    sylvakru::DopPacketizer* dop = nullptr;
    // 编码输出的复用缓冲
    std::vector<uint8_t> out;
};

DsdReaderHandle* readerFromHandle(jlong handle) {
    return reinterpret_cast<DsdReaderHandle*>(handle);
}

DsdEncoderHandle* encoderFromHandle(jlong handle) {
    return reinterpret_cast<DsdEncoderHandle*>(handle);
}

jstring nullableError(JNIEnv* env, const std::string& message) {
    if (message.empty()) {
        return nullptr;
    }
    return env->NewStringUTF(message.c_str());
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
Java_com_afalphy_sylvakru_UsbDsdNative_readerCreate(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new DsdReaderHandle());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerOpen(
    JNIEnv* env,
    jobject,
    jlong handle,
    jstring path,
    jboolean streaming) {
    auto* holder = readerFromHandle(handle);
    if (holder == nullptr || path == nullptr) {
        return nullableError(env, "Invalid DSD reader handle.");
    }
    const char* path_chars = env->GetStringUTFChars(path, nullptr);
    if (path_chars == nullptr) {
        return nullableError(env, "Failed to read DSD path.");
    }
    const auto result = holder->reader.open(path_chars, streaming == JNI_TRUE);
    env->ReleaseStringUTFChars(path, path_chars);
    return nullableError(env, result.ok() ? std::string() : result.message);
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerInfo(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* holder = readerFromHandle(handle);
    jlong values[3] = {0, 0, 0};
    if (holder != nullptr) {
        values[0] = holder->reader.sampleRate();
        values[1] = holder->reader.channels();
        values[2] = holder->reader.durationMs();
    }
    jlongArray result = env->NewLongArray(3);
    if (result != nullptr) {
        env->SetLongArrayRegion(result, 0, 3, values);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerFormatName(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* holder = readerFromHandle(handle);
    return env->NewStringUTF(holder != nullptr ? holder->reader.formatName().c_str() : "");
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerRead(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray out) {
    auto* holder = readerFromHandle(handle);
    if (holder == nullptr || out == nullptr) {
        return -1;
    }
    const jsize capacity = env->GetArrayLength(out);
    if (capacity <= 0) {
        return -1;
    }
    holder->scratch.resize(static_cast<size_t>(capacity));
    const int count = holder->reader.read(holder->scratch.data(), capacity);
    if (count > 0) {
        env->SetByteArrayRegion(
            out, 0, count, reinterpret_cast<const jbyte*>(holder->scratch.data()));
    }
    return count;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerCanReadAt(
    JNIEnv*,
    jobject,
    jlong handle,
    jlong file_length) {
    auto* holder = readerFromHandle(handle);
    return (holder != nullptr && holder->reader.canReadAt(file_length)) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerSeekTo(
    JNIEnv*,
    jobject,
    jlong handle,
    jlong position_ms) {
    auto* holder = readerFromHandle(handle);
    return holder != nullptr ? holder->reader.seekTo(position_ms) : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerPositionMs(
    JNIEnv*,
    jobject,
    jlong handle) {
    auto* holder = readerFromHandle(handle);
    return holder != nullptr ? holder->reader.positionMs() : 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_readerDestroy(
    JNIEnv*,
    jobject,
    jlong handle) {
    delete readerFromHandle(handle);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_dopCreate(
    JNIEnv*,
    jobject,
    jint channels) {
    auto* holder = new DsdEncoderHandle();
    auto dop = std::make_unique<sylvakru::DopPacketizer>(channels);
    holder->dop = dop.get();
    holder->encoder = std::move(dop);
    return reinterpret_cast<jlong>(holder);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_nativeDsdCreate(
    JNIEnv*,
    jobject,
    jint channels,
    jint bytes_per_sample,
    jboolean big_endian) {
    auto* holder = new DsdEncoderHandle();
    holder->encoder = std::make_unique<sylvakru::NativeDsdPacketizer>(
        channels, bytes_per_sample, big_endian == JNI_TRUE);
    return reinterpret_cast<jlong>(holder);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_encoderEncode(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray data,
    jint length) {
    auto* holder = encoderFromHandle(handle);
    if (holder == nullptr || data == nullptr) {
        return toByteArray(env, {});
    }
    const jsize available = env->GetArrayLength(data);
    if (length < 0 || length > available) {
        length = available;
    }
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (bytes == nullptr) {
        return toByteArray(env, {});
    }
    holder->encoder->encode(reinterpret_cast<const uint8_t*>(bytes), length, holder->out);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return toByteArray(env, holder->out);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_encoderEncodeSilence(
    JNIEnv* env,
    jobject,
    jlong handle,
    jint frames) {
    auto* holder = encoderFromHandle(handle);
    if (holder == nullptr || frames < 0) {
        return toByteArray(env, {});
    }
    holder->encoder->encodeSilence(frames, holder->out);
    return toByteArray(env, holder->out);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_encoderDrain(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* holder = encoderFromHandle(handle);
    if (holder == nullptr) {
        return toByteArray(env, {});
    }
    holder->encoder->drain(holder->out);
    return toByteArray(env, holder->out);
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_dopReset(
    JNIEnv*,
    jobject,
    jlong handle) {
    auto* holder = encoderFromHandle(handle);
    if (holder != nullptr && holder->dop != nullptr) {
        holder->dop->reset();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_encoderDestroy(
    JNIEnv*,
    jobject,
    jlong handle) {
    delete encoderFromHandle(handle);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbDsdNative_nativeDsdBytesPerSample(
    JNIEnv* env,
    jobject,
    jstring format) {
    if (format == nullptr) {
        return 0;
    }
    const char* format_chars = env->GetStringUTFChars(format, nullptr);
    if (format_chars == nullptr) {
        return 0;
    }
    const int bytes = sylvakru::nativeDsdBytesPerSample(format_chars);
    env->ReleaseStringUTFChars(format, format_chars);
    return bytes;
}
