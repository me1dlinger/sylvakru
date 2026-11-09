// UsbDacQuirksNative 的 JNI 边界：两个无状态函数。quirk 表不过界——
// Kotlin 传入键字符串数组，native 返回命中下标（-1 未命中）；
// 采样率选择用 -1 表示"未指定/无结果"（对应 Kotlin null）。
#include <jni.h>

#include <string>
#include <vector>

#include "usb_dac_quirks.h"

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbDacQuirksNative_matchQuirkIndex(
    JNIEnv* env,
    jobject,
    jobjectArray keys,
    jint vendor_id,
    jint product_id) {
    if (keys == nullptr) {
        return -1;
    }
    const jsize count = env->GetArrayLength(keys);
    std::vector<std::string> native_keys;
    native_keys.reserve(count);
    for (jsize index = 0; index < count; ++index) {
        auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, index));
        if (key == nullptr) {
            native_keys.emplace_back();
            continue;
        }
        const char* key_chars = env->GetStringUTFChars(key, nullptr);
        native_keys.emplace_back(key_chars != nullptr ? key_chars : "");
        if (key_chars != nullptr) {
            env->ReleaseStringUTFChars(key, key_chars);
        }
        env->DeleteLocalRef(key);
    }
    return sylvakru::matchDacQuirkIndex(native_keys, vendor_id, product_id);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbDacQuirksNative_chooseBitPerfectMixerSampleRate(
    JNIEnv* env,
    jobject,
    jint requested_sample_rate,
    jintArray supported_sample_rates) {
    std::vector<int> rates;
    if (supported_sample_rates != nullptr) {
        const jsize count = env->GetArrayLength(supported_sample_rates);
        rates.resize(count);
        if (count > 0) {
            env->GetIntArrayRegion(supported_sample_rates, 0, count, rates.data());
        }
    }
    return sylvakru::chooseBitPerfectMixerSampleRate(requested_sample_rate, rates);
}
