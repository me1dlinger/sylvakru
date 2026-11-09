// UsbVolumeNative 的 JNI 边界：iBasso 音量协议纯逻辑的无状态函数集，
// 无句柄、无生命周期。可空 Int 以 has*/value 成对参数传递；枚举按
// Kotlin/C++ 双侧一致的序号传递（两侧枚举顺序不得重排）。
#include <jni.h>

#include <cstdint>
#include <vector>

#include "usb_volume_protocol.h"

namespace {

jintArray toIntArray(JNIEnv* env, const std::vector<jint>& values) {
    jintArray result = env->NewIntArray(static_cast<jsize>(values.size()));
    if (result != nullptr && !values.empty()) {
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    }
    return result;
}

jbyteArray toByteArray(JNIEnv* env, const uint8_t* bytes, size_t size) {
    jbyteArray result = env->NewByteArray(static_cast<jsize>(size));
    if (result != nullptr && size > 0) {
        env->SetByteArrayRegion(
            result, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(bytes));
    }
    return result;
}

std::vector<int> toIntVector(JNIEnv* env, jintArray values) {
    std::vector<int> result;
    if (values == nullptr) {
        return result;
    }
    const jsize length = env->GetArrayLength(values);
    result.resize(static_cast<size_t>(length));
    if (length > 0) {
        env->GetIntArrayRegion(values, 0, length, reinterpret_cast<jint*>(result.data()));
    }
    return result;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_preferredAutoPcmBitDepth(
    JNIEnv* env,
    jobject,
    jboolean has_source_bit_depth,
    jint source_bit_depth,
    jintArray available_bit_depths) {
    const std::vector<int> available = toIntVector(env, available_bit_depths);
    const int source = source_bit_depth;
    return sylvakru::preferredAutoPcmBitDepth(
        has_source_bit_depth == JNI_TRUE ? &source : nullptr, available);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_effectiveVolumeGainQ16(
    JNIEnv*,
    jobject,
    jint user_gain_q16,
    jint replay_gain_milli_db) {
    return sylvakru::effectiveVolumeGainQ16(user_gain_q16, replay_gain_milli_db);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_effectiveHardwareVolumeGainQ16(
    JNIEnv*,
    jobject,
    jint user_gain_q16,
    jint replay_gain_milli_db,
    jint dsd_compensation_db,
    jboolean is_dsd) {
    return sylvakru::effectiveHardwareVolumeGainQ16(
        user_gain_q16, replay_gain_milli_db, dsd_compensation_db, is_dsd == JNI_TRUE);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoVolumeIndex(
    JNIEnv*,
    jobject,
    jint gain_q16) {
    return sylvakru::ibassoVolumeIndex(gain_q16);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoDeviceVolume(
    JNIEnv*,
    jobject,
    jint index) {
    return sylvakru::ibassoDeviceVolume(index);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoDsdVolume(
    JNIEnv*,
    jobject,
    jint base_volume,
    jint compensation_db) {
    return sylvakru::ibassoDsdVolume(base_volume, compensation_db);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoAppGainToRaw(
    JNIEnv* env,
    jobject,
    jint gain_q16,
    jint replay_gain_milli_db,
    jint dsd_compensation_db) {
    const auto target =
        sylvakru::ibassoAppGainToRaw(gain_q16, replay_gain_milli_db, dsd_compensation_db);
    return toIntArray(env, {target.base_raw, target.dsd_raw});
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoRawToLinearGainQ16(
    JNIEnv*,
    jobject,
    jint raw) {
    return sylvakru::ibassoRawToLinearGainQ16(raw);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoDecodeEvent(
    JNIEnv* env,
    jobject,
    jbyteArray packet) {
    if (packet == nullptr) {
        return toIntArray(env, {});
    }
    jbyte* bytes = env->GetByteArrayElements(packet, nullptr);
    if (bytes == nullptr) {
        return toIntArray(env, {});
    }
    const auto event = sylvakru::ibassoDecodeEvent(
        reinterpret_cast<const uint8_t*>(bytes),
        static_cast<size_t>(env->GetArrayLength(packet)));
    env->ReleaseByteArrayElements(packet, bytes, JNI_ABORT);
    if (!event.valid) {
        return toIntArray(env, {});
    }
    return toIntArray(env, {event.left_raw, event.right_raw});
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoRoutePacket(
    JNIEnv* env,
    jobject,
    jbyteArray packet,
    jintArray pending_commands) {
    if (packet == nullptr) {
        return toIntArray(env, {0});
    }
    jbyte* bytes = env->GetByteArrayElements(packet, nullptr);
    if (bytes == nullptr) {
        return toIntArray(env, {0});
    }
    const auto route = sylvakru::routeIbassoVolumePacket(
        reinterpret_cast<const uint8_t*>(bytes),
        static_cast<size_t>(env->GetArrayLength(packet)),
        toIntVector(env, pending_commands));
    env->ReleaseByteArrayElements(packet, bytes, JNI_ABORT);
    switch (route.kind) {
        case sylvakru::IbassoPacketRouteKind::kEvent:
            return toIntArray(env, {1, route.left_raw, route.right_raw});
        case sylvakru::IbassoPacketRouteKind::kCommandResponse:
            return toIntArray(env, {2, route.command});
        case sylvakru::IbassoPacketRouteKind::kUnknown:
        default:
            return toIntArray(env, {0});
    }
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoI2cWritePacket(
    JNIEnv* env,
    jobject,
    jint command,
    jint slave,
    jint offset,
    jint byte_offset,
    jint value) {
    uint8_t packet[16];
    sylvakru::ibassoI2cWritePacket(command, slave, offset, byte_offset, value, packet);
    return toByteArray(env, packet, sizeof(packet));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoRoomWritePacket(
    JNIEnv* env,
    jobject,
    jint command,
    jint register_id,
    jint value) {
    uint8_t packet[16];
    sylvakru::ibassoRoomWritePacket(command, register_id, value, packet);
    return toByteArray(env, packet, sizeof(packet));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoVolumeReadPacket(
    JNIEnv* env,
    jobject) {
    uint8_t packet[16];
    sylvakru::ibassoVolumeReadPacket(packet);
    return toByteArray(env, packet, sizeof(packet));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoVolumePackets(
    JNIEnv* env,
    jobject,
    jint base_raw,
    jint dsd_raw) {
    const auto packets = sylvakru::ibassoVolumePackets({base_raw, dsd_raw});
    // 10 个 16 字节包按顺序平铺成 160 字节，Kotlin 侧按 16 切分
    std::vector<uint8_t> flat;
    flat.reserve(packets.size() * 16);
    for (const auto& packet : packets) {
        flat.insert(flat.end(), packet.begin(), packet.end());
    }
    return toByteArray(env, flat.data(), flat.size());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoVolumeVerificationAction(
    JNIEnv*,
    jobject,
    jint target_raw,
    jboolean has_previous_raw,
    jint previous_raw,
    jboolean has_readback_raw,
    jint readback_raw,
    jint failure_count,
    jboolean is_dsd,
    jboolean has_pending_request,
    jboolean has_target_dsd_raw,
    jint target_dsd_raw,
    jboolean has_previous_dsd_raw,
    jint previous_dsd_raw) {
    const int previous = previous_raw;
    const int readback = readback_raw;
    const int target_dsd = target_dsd_raw;
    const int previous_dsd = previous_dsd_raw;
    return static_cast<jint>(sylvakru::ibassoVolumeVerificationAction(
        target_raw,
        has_previous_raw == JNI_TRUE ? &previous : nullptr,
        has_readback_raw == JNI_TRUE ? &readback : nullptr,
        failure_count,
        is_dsd == JNI_TRUE,
        has_pending_request == JNI_TRUE,
        has_target_dsd_raw == JNI_TRUE ? &target_dsd : nullptr,
        has_previous_dsd_raw == JNI_TRUE ? &previous_dsd : nullptr));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoReaderRecoveryAction(
    JNIEnv*,
    jobject,
    jboolean is_dsd,
    jboolean health_write_only,
    jboolean health_restart_requested,
    jboolean reader_running,
    jboolean generation_matches,
    jboolean wait_expired) {
    return static_cast<jint>(sylvakru::ibassoReaderRecoveryAction(
        is_dsd == JNI_TRUE,
        health_write_only == JNI_TRUE,
        health_restart_requested == JNI_TRUE,
        reader_running == JNI_TRUE,
        generation_matches == JNI_TRUE,
        wait_expired == JNI_TRUE));
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoVolumePendingDelayMs(
    JNIEnv*,
    jobject,
    jlong last_completed_at_ms,
    jboolean has_pending_updated_at,
    jlong pending_updated_at_ms,
    jlong now_ms) {
    return sylvakru::ibassoVolumePendingDelayMs(
        last_completed_at_ms,
        has_pending_updated_at == JNI_TRUE,
        pending_updated_at_ms,
        now_ms);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoActualEventGainQ16(
    JNIEnv* env,
    jobject,
    jint base_raw,
    jboolean is_dsd,
    jint dsd_compensation_db) {
    const auto actual =
        sylvakru::ibassoActualEventGainQ16(base_raw, is_dsd == JNI_TRUE, dsd_compensation_db);
    return toIntArray(env, {actual.raw, actual.gain_q16});
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_afalphy_sylvakru_UsbVolumeNative_ibassoTargetFromEvent(
    JNIEnv* env,
    jobject,
    jint base_raw,
    jint dsd_compensation_db) {
    const auto target = sylvakru::ibassoTargetFromEvent(base_raw, dsd_compensation_db);
    return toIntArray(env, {target.base_raw, target.dsd_raw});
}
