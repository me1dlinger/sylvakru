// UsbUacNative 的 JNI 边界：四个无状态解析函数，输入 raw descriptors 字节数组，
// 输出扁平 IntArray（结构化解码留在引擎原函数体内完成）。
// - parseStreamingFormats：每条 10 槽位 [ifNum, alt, protocol, terminalLink,
//   formatType, channels, subslotSize, bitResolution, bmFormats, hasBmFormats]，
//   -1 表示字段缺失（对应 Kotlin null）
// - findClockSource：[hasClockSource(0/1), terminalLink(-1=null), clockSourceId(-1=null)]
// - parseVolumeFeatures：每条 6 槽位 [uac2(0/1), controlInterface, unitId,
//   sourceId, channel, writable(0/1)]
// - parseOutputTerminalSources：bSourceID 列表
#include <jni.h>

#include <cstdint>
#include <vector>

#include "usb_uac.h"

namespace {

jintArray toIntArray(JNIEnv* env, const std::vector<jint>& values) {
    jintArray result = env->NewIntArray(static_cast<jsize>(values.size()));
    if (result != nullptr && !values.empty()) {
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    }
    return result;
}

// 借用 descriptors 字节内容调用 parse，再统一释放
template <typename Parse>
jintArray withDescriptors(JNIEnv* env, jbyteArray descriptors, Parse parse) {
    std::vector<jint> flat;
    if (descriptors != nullptr) {
        jbyte* bytes = env->GetByteArrayElements(descriptors, nullptr);
        if (bytes != nullptr) {
            parse(
                reinterpret_cast<const uint8_t*>(bytes),
                static_cast<size_t>(env->GetArrayLength(descriptors)),
                flat);
            env->ReleaseByteArrayElements(descriptors, bytes, JNI_ABORT);
        }
    }
    return toIntArray(env, flat);
}

}  // namespace

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbUacNative_parseStreamingFormats(
    JNIEnv* env,
    jobject,
    jbyteArray descriptors) {
    return withDescriptors(env, descriptors, [](const uint8_t* data, size_t size, std::vector<jint>& flat) {
        for (const auto& format : sylvakru::parseUacStreamingFormats(data, size)) {
            flat.push_back(format.interface_number);
            flat.push_back(format.alternate_setting);
            flat.push_back(format.protocol);
            flat.push_back(format.terminal_link);
            flat.push_back(format.format_type);
            flat.push_back(format.channels);
            flat.push_back(format.subslot_size);
            flat.push_back(format.bit_resolution);
            flat.push_back(static_cast<jint>(format.bm_formats));
            flat.push_back(format.has_bm_formats ? 1 : 0);
        }
    });
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbUacNative_findClockSource(
    JNIEnv* env,
    jobject,
    jbyteArray descriptors,
    jint streaming_interface_number,
    jint streaming_alternate_setting) {
    return withDescriptors(
        env,
        descriptors,
        [streaming_interface_number, streaming_alternate_setting](
            const uint8_t* data, size_t size, std::vector<jint>& flat) {
            const auto info = sylvakru::findUac2ClockSource(
                data, size, streaming_interface_number, streaming_alternate_setting);
            flat.push_back(info.has_clock_source ? 1 : 0);
            flat.push_back(info.terminal_link);
            flat.push_back(info.clock_source_id);
        });
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbUacNative_parseVolumeFeatures(
    JNIEnv* env,
    jobject,
    jbyteArray descriptors) {
    return withDescriptors(env, descriptors, [](const uint8_t* data, size_t size, std::vector<jint>& flat) {
        for (const auto& feature : sylvakru::parseUacVolumeFeatures(data, size)) {
            flat.push_back(feature.uac2 ? 1 : 0);
            flat.push_back(feature.control_interface);
            flat.push_back(feature.unit_id);
            flat.push_back(feature.source_id);
            flat.push_back(feature.channel);
            flat.push_back(feature.writable ? 1 : 0);
        }
    });
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbUacNative_parseOutputTerminalSources(
    JNIEnv* env,
    jobject,
    jbyteArray descriptors) {
    return withDescriptors(env, descriptors, [](const uint8_t* data, size_t size, std::vector<jint>& flat) {
        for (const int source : sylvakru::parseUacOutputTerminalSources(data, size)) {
            flat.push_back(source);
        }
    });
}
