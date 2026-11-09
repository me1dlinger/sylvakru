#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "SylvakruUsbExclusive";
constexpr int kMaxIsoPacketsPerUrb = 16;
constexpr int kDefaultMaxPendingUrbs = 8;
constexpr int kAbsoluteMaxPendingUrbs = 512;

struct PendingUrb {
    usbdevfs_urb* urb;
    uint8_t* buffer;
    int length;
    int packets;
    bool feedback;
};

// usbdevfs 传输实例：原全局单例状态收拢为句柄（Kotlin UsbExclusiveNative
// 进程内持有单实例；实例化后核心逻辑可被未来多实例/桌面后端复用）。
// 各字段语义与原 g_* 全局逐一对应，行为不变。
struct Transport {
    std::mutex mutex;
    int fd = -1;
    int interface_number = -1;
    int endpoint_address = -1;
    int max_packet_size = 0;
    int iso_packet_size = 0;
    int feedback_endpoint_address = 0;
    int feedback_packet_size = 0;
    int feedback_frames_per_packet_q16 = 0;
    int feedback_log_count = 0;
    int write_log_count = 0;
    long long total_bytes = 0;
    long long total_urbs = 0;
    long long total_iso_packets = 0;
    long long last_stats_ms = 0;
    long long iso_error_count = 0;
    int max_pending_urbs = kDefaultMaxPendingUrbs;
    std::vector<PendingUrb> pending_urbs;
};

Transport* fromHandle(jlong handle) {
    return reinterpret_cast<Transport*>(handle);
}

long long monotonicMillis() {
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000LL + now.tv_nsec / 1000000LL;
}

std::string errorMessage(const char* action) {
    return std::string(action) + " failed: " + strerror(errno);
}

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

jstring nullableError(JNIEnv* env, const std::string& error) {
    if (error.empty()) {
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_WARN, kTag, "%s", error.c_str());
    return toJString(env, error);
}

void freePendingUrb(PendingUrb pending) {
    free(pending.buffer);
    free(pending.urb);
}

std::string submitFeedbackLocked(Transport& transport);

void logCompletedUrb(Transport& transport, PendingUrb pending) {
    if (pending.feedback) {
        return;
    }
    if (pending.urb->status != 0) {
        ++transport.iso_error_count;
        __android_log_print(
            ANDROID_LOG_WARN,
            kTag,
            "URB completed with status=%d length=%d packets=%d",
            pending.urb->status,
            pending.length,
            pending.packets);
    }
    for (int i = 0; i < pending.packets; ++i) {
        if (pending.urb->iso_frame_desc[i].status != 0) {
            ++transport.iso_error_count;
            __android_log_print(
                ANDROID_LOG_WARN,
                kTag,
                "iso frame status=%d actual=%u requested=%u index=%d/%d",
                pending.urb->iso_frame_desc[i].status,
                pending.urb->iso_frame_desc[i].actual_length,
                pending.urb->iso_frame_desc[i].length,
                i,
                pending.packets);
        }
    }
}

void handleFeedbackUrb(Transport& transport, PendingUrb pending) {
    if (pending.urb->status != 0 || pending.packets <= 0) {
        if (pending.urb->status != 0) {
            __android_log_print(
                ANDROID_LOG_WARN,
                kTag,
                "feedback URB status=%d length=%d packets=%d",
                pending.urb->status,
                pending.length,
                pending.packets);
        }
        return;
    }

    const auto& frame = pending.urb->iso_frame_desc[0];
    if (frame.status != 0 || frame.actual_length < 3) {
        if (frame.status != 0 && transport.feedback_log_count < 8) {
            __android_log_print(
                ANDROID_LOG_WARN,
                kTag,
                "feedback frame status=%d actual=%u requested=%u",
                frame.status,
                frame.actual_length,
                frame.length);
        }
        return;
    }

    const int actual = std::min<int>(frame.actual_length, pending.length);
    int raw = 0;
    for (int i = 0; i < std::min(actual, 4); ++i) {
        raw |= static_cast<int>(pending.buffer[i]) << (i * 8);
    }

    int q16 = 0;
    if (actual >= 4) {
        q16 = raw;
    } else {
        q16 = raw << 2;
    }

    if (q16 > 0) {
        transport.feedback_frames_per_packet_q16 = q16;
    }
    if (transport.feedback_log_count < 12) {
        ++transport.feedback_log_count;
        __android_log_print(
            ANDROID_LOG_INFO,
            kTag,
            "USB feedback actual=%d raw=0x%x framesPerPacketQ16=%d approxFrames=%.6f",
            actual,
            raw,
            q16,
            static_cast<double>(q16) / 65536.0);
    }
}

std::string reapOneLocked(Transport& transport, bool blocking, bool* reaped = nullptr) {
    if (transport.pending_urbs.empty()) {
        return {};
    }

    void* completed = nullptr;
    const int request = blocking ? USBDEVFS_REAPURB : USBDEVFS_REAPURBNDELAY;
    if (ioctl(transport.fd, request, &completed) < 0) {
        if (!blocking && errno == EAGAIN) {
            return {};
        }
        return errorMessage(blocking ? "USBDEVFS_REAPURB" : "USBDEVFS_REAPURBNDELAY");
    }
    auto found = std::find_if(
        transport.pending_urbs.begin(),
        transport.pending_urbs.end(),
        [completed](const PendingUrb& pending) { return pending.urb == completed; });
    if (found == transport.pending_urbs.end()) {
        return "USBDEVFS_REAPURB returned an unknown URB.";
    }
    if (reaped != nullptr) {
        *reaped = true;
    }

    const PendingUrb completed_pending = *found;
    if (completed_pending.feedback) {
        handleFeedbackUrb(transport, completed_pending);
    } else {
        logCompletedUrb(transport, completed_pending);
    }
    freePendingUrb(completed_pending);
    transport.pending_urbs.erase(found);
    if (completed_pending.feedback &&
        transport.fd >= 0 &&
        transport.feedback_endpoint_address != 0) {
        const auto feedback_error = submitFeedbackLocked(transport);
        if (!feedback_error.empty()) {
            return feedback_error;
        }
    }
    return {};
}

std::string reapCompletedLocked(Transport& transport) {
    std::string error;
    // 非阻塞收回全部已完成 URB。之前每次只收一个，切歌排空轮询看到的
    // pending 数远落后于实际播放进度，排空永远等不到 0 只能超时硬关，
    // DISCARDURB 掐断残留音频出小音爆。
    while (error.empty() && !transport.pending_urbs.empty()) {
        bool reaped = false;
        error = reapOneLocked(transport, false, &reaped);
        if (!reaped) {
            break;
        }
    }
    while (error.empty() &&
           static_cast<int>(transport.pending_urbs.size()) >= transport.max_pending_urbs) {
        error = reapOneLocked(transport, true);
    }
    return error;
}

void discardPendingLocked(Transport& transport) {
    for (const auto& pending : transport.pending_urbs) {
        ioctl(transport.fd, USBDEVFS_DISCARDURB, pending.urb);
    }
}

// seek/暂停时丢弃在途输出 URB：DISCARDURB 后仍要通过 REAPURB 收回并释放；
// 反馈 URB 不丢（收回后 reapOneLocked 会自动重挂）。
std::string flushOutputLocked(Transport& transport) {
    if (transport.fd < 0) {
        return {};
    }
    for (const auto& pending : transport.pending_urbs) {
        if (!pending.feedback) {
            ioctl(transport.fd, USBDEVFS_DISCARDURB, pending.urb);
        }
    }
    const auto has_output = [&transport] {
        for (const auto& pending : transport.pending_urbs) {
            if (!pending.feedback) {
                return true;
            }
        }
        return false;
    };
    std::string error;
    while (error.empty() && has_output()) {
        error = reapOneLocked(transport, true);
    }
    return error;
}

void freeAllPendingLocked(Transport& transport) {
    for (auto& pending : transport.pending_urbs) {
        freePendingUrb(pending);
    }
    transport.pending_urbs.clear();
}

std::string claimInterfaceLocked(Transport& transport) {
    usbdevfs_disconnect_claim disconnect_claim = {};
    disconnect_claim.interface = static_cast<unsigned int>(transport.interface_number);

    if (ioctl(transport.fd, USBDEVFS_DISCONNECT_CLAIM, &disconnect_claim) == 0) {
        __android_log_print(
            ANDROID_LOG_INFO,
            kTag,
            "USBDEVFS_DISCONNECT_CLAIM ok interface=%d",
            transport.interface_number);
        return {};
    }

    const int disconnect_claim_errno = errno;
    __android_log_print(
        ANDROID_LOG_WARN,
        kTag,
        "USBDEVFS_DISCONNECT_CLAIM failed interface=%d: %s",
        transport.interface_number,
        strerror(disconnect_claim_errno));

    if (ioctl(transport.fd, USBDEVFS_CLAIMINTERFACE, &transport.interface_number) == 0) {
        __android_log_print(
            ANDROID_LOG_INFO,
            kTag,
            "USBDEVFS_CLAIMINTERFACE ok interface=%d",
            transport.interface_number);
        return {};
    }

    return errorMessage("USBDEVFS_CLAIMINTERFACE");
}

void closeLocked(Transport& transport) {
    if (transport.fd < 0) {
        return;
    }

    __android_log_print(
        ANDROID_LOG_INFO,
        kTag,
        "closing exclusive USB fd=%d interface=%d endpoint=0x%x pendingUrbs=%zu",
        transport.fd,
        transport.interface_number,
        transport.endpoint_address,
        transport.pending_urbs.size());
    discardPendingLocked(transport);
    if (transport.interface_number >= 0) {
        // UAC 标准停流信号：先回 alt 0 再释放接口。少了这步 Macaron 从
        // native DSD 退出时固件停在 DSD 状态，后续 PCM 会话声道错乱（单声道）
        // 且反馈端点一直报上一个会话的速率，只能拔插恢复。
        usbdevfs_setinterface set_interface = {};
        set_interface.interface = static_cast<unsigned int>(transport.interface_number);
        set_interface.altsetting = 0;
        ioctl(transport.fd, USBDEVFS_SETINTERFACE, &set_interface);
        ioctl(transport.fd, USBDEVFS_RELEASEINTERFACE, &transport.interface_number);
    }
    close(transport.fd);
    freeAllPendingLocked(transport);
    transport.fd = -1;
    transport.interface_number = -1;
    transport.endpoint_address = -1;
    transport.max_packet_size = 0;
    transport.iso_packet_size = 0;
    transport.feedback_endpoint_address = 0;
    transport.feedback_packet_size = 0;
    transport.feedback_frames_per_packet_q16 = 0;
    transport.feedback_log_count = 0;
    transport.write_log_count = 0;
    transport.total_bytes = 0;
    transport.total_urbs = 0;
    transport.total_iso_packets = 0;
    transport.last_stats_ms = 0;
    transport.iso_error_count = 0;
    transport.max_pending_urbs = kDefaultMaxPendingUrbs;
}

std::string submitIsoPacketsLocked(
    Transport& transport,
    const uint8_t* data,
    int length,
    const int* packet_lengths,
    int packet_count) {
    if (transport.fd < 0) {
        return "USB exclusive device is not open.";
    }
    if (transport.endpoint_address < 0 || transport.max_packet_size <= 0) {
        return "USB exclusive endpoint is not configured.";
    }
    if (data == nullptr || length <= 0 || packet_lengths == nullptr || packet_count <= 0) {
        return {};
    }

    const int packets = std::min(packet_count, kMaxIsoPacketsPerUrb);
    int described_length = 0;
    for (int i = 0; i < packets; ++i) {
        if (packet_lengths[i] <= 0 || packet_lengths[i] > transport.max_packet_size) {
            return "USB exclusive iso packet length is invalid.";
        }
        described_length += packet_lengths[i];
    }
    if (described_length != length) {
        return "USB exclusive iso packet lengths do not match PCM length.";
    }

    const size_t urb_size =
        sizeof(usbdevfs_urb) + sizeof(usbdevfs_iso_packet_desc) * packets;
    auto* urb = static_cast<usbdevfs_urb*>(calloc(1, urb_size));
    auto* buffer = static_cast<uint8_t*>(malloc(length));
    if (urb == nullptr || buffer == nullptr) {
        free(urb);
        free(buffer);
        return "Failed to allocate USB isochronous transfer.";
    }

    memcpy(buffer, data, length);
    urb->type = USBDEVFS_URB_TYPE_ISO;
    urb->endpoint = static_cast<unsigned char>(transport.endpoint_address);
    urb->status = 0;
    urb->flags = USBDEVFS_URB_ISO_ASAP;
    urb->buffer = buffer;
    urb->buffer_length = length;
    urb->number_of_packets = packets;

    for (int i = 0; i < packets; ++i) {
        urb->iso_frame_desc[i].length = packet_lengths[i];
    }

    if (ioctl(transport.fd, USBDEVFS_SUBMITURB, urb) < 0) {
        const auto error = errorMessage("USBDEVFS_SUBMITURB");
        free(buffer);
        free(urb);
        return error;
    }

    transport.pending_urbs.push_back(PendingUrb{urb, buffer, length, packets, false});
    transport.total_bytes += length;
    transport.total_urbs += 1;
    transport.total_iso_packets += packets;
    const long long now_ms = monotonicMillis();
    if (transport.last_stats_ms == 0) {
        transport.last_stats_ms = now_ms;
    } else if (now_ms - transport.last_stats_ms >= 1000) {
        __android_log_print(
            ANDROID_LOG_INFO,
            kTag,
            "USB write stats bytes=%lld urbs=%lld isoPackets=%lld pendingUrbs=%zu isoPacketSize=%d endpoint=0x%x",
            transport.total_bytes,
            transport.total_urbs,
            transport.total_iso_packets,
            transport.pending_urbs.size(),
            transport.iso_packet_size,
            transport.endpoint_address);
        transport.last_stats_ms = now_ms;
    }
    return reapCompletedLocked(transport);
}

std::string submitIsoChunkLocked(Transport& transport, const uint8_t* data, int length) {
    const int iso_packet_size = transport.iso_packet_size > 0
        ? std::min(transport.iso_packet_size, transport.max_packet_size)
        : transport.max_packet_size;
    const int packets = std::max(
        1,
        std::min(kMaxIsoPacketsPerUrb, (length + iso_packet_size - 1) / iso_packet_size));
    int remaining = length;
    int packet_lengths[kMaxIsoPacketsPerUrb] = {};
    for (int i = 0; i < packets; ++i) {
        packet_lengths[i] = std::min(iso_packet_size, remaining);
        remaining -= packet_lengths[i];
    }
    return submitIsoPacketsLocked(transport, data, length, packet_lengths, packets);
}

std::string submitFeedbackLocked(Transport& transport) {
    if (transport.fd < 0 ||
        transport.feedback_endpoint_address == 0 ||
        transport.feedback_packet_size <= 0) {
        return {};
    }

    const int packets = 1;
    const int length = std::min(4, std::max(3, transport.feedback_packet_size));
    const size_t urb_size =
        sizeof(usbdevfs_urb) + sizeof(usbdevfs_iso_packet_desc) * packets;
    auto* urb = static_cast<usbdevfs_urb*>(calloc(1, urb_size));
    auto* buffer = static_cast<uint8_t*>(calloc(1, length));
    if (urb == nullptr || buffer == nullptr) {
        free(urb);
        free(buffer);
        return "Failed to allocate USB feedback transfer.";
    }

    urb->type = USBDEVFS_URB_TYPE_ISO;
    urb->endpoint = static_cast<unsigned char>(transport.feedback_endpoint_address);
    urb->status = 0;
    urb->flags = USBDEVFS_URB_ISO_ASAP;
    urb->buffer = buffer;
    urb->buffer_length = length;
    urb->number_of_packets = packets;
    urb->iso_frame_desc[0].length = length;

    if (ioctl(transport.fd, USBDEVFS_SUBMITURB, urb) < 0) {
        const auto error = errorMessage("USBDEVFS_SUBMITURB feedback");
        free(buffer);
        free(urb);
        return error;
    }

    transport.pending_urbs.push_back(PendingUrb{urb, buffer, length, packets, true});
    return {};
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeCreate(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new Transport());
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeDestroy(JNIEnv*, jobject, jlong handle) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        closeLocked(*transport);
    }
    delete transport;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeOpen(
    JNIEnv* env,
    jobject,
    jlong handle,
    jint fd,
    jint interface_number,
    jint alternate_setting,
    jint endpoint_address,
    jint max_packet_size,
    jint feedback_endpoint_address,
    jint feedback_max_packet_size,
    jboolean interface_already_claimed) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return nullableError(env, "USB exclusive transport handle is invalid.");
    }
    std::lock_guard<std::mutex> lock(transport->mutex);
    closeLocked(*transport);

    __android_log_print(
        ANDROID_LOG_INFO,
        kTag,
        "open requested fd=%d interface=%d alt=%d endpoint=0x%x maxPacket=%d",
        fd,
        interface_number,
        alternate_setting,
        endpoint_address,
        max_packet_size);

    const int duplicated = dup(fd);
    if (duplicated < 0) {
        return nullableError(env, errorMessage("dup"));
    }

    transport->fd = duplicated;
    transport->interface_number = interface_number;
    transport->endpoint_address = endpoint_address;
    transport->max_packet_size = max_packet_size;
    transport->feedback_endpoint_address = feedback_endpoint_address;
    transport->feedback_packet_size = feedback_max_packet_size;

    if (interface_already_claimed == JNI_TRUE) {
        __android_log_print(
            ANDROID_LOG_INFO,
            kTag,
            "USB interface already claimed by UsbDeviceConnection interface=%d",
            transport->interface_number);
    } else {
        const auto claim_error = claimInterfaceLocked(*transport);
        if (!claim_error.empty()) {
            closeLocked(*transport);
            return nullableError(env, claim_error);
        }
    }

    usbdevfs_setinterface set_interface = {};
    set_interface.interface = interface_number;
    set_interface.altsetting = alternate_setting;
    if (ioctl(transport->fd, USBDEVFS_SETINTERFACE, &set_interface) < 0) {
        const auto error = errorMessage("USBDEVFS_SETINTERFACE");
        closeLocked(*transport);
        return nullableError(env, error);
    }
    __android_log_print(
        ANDROID_LOG_INFO,
        kTag,
        "USBDEVFS_SETINTERFACE ok interface=%d alt=%d",
        interface_number,
        alternate_setting);

    if (transport->feedback_endpoint_address != 0 && transport->feedback_packet_size > 0) {
        const auto feedback_error = submitFeedbackLocked(*transport);
        if (!feedback_error.empty()) {
            __android_log_print(
                ANDROID_LOG_WARN,
                kTag,
                "%s",
                feedback_error.c_str());
        } else {
            __android_log_print(
                ANDROID_LOG_INFO,
                kTag,
                "USB feedback endpoint armed endpoint=0x%x maxPacket=%d",
                transport->feedback_endpoint_address,
                transport->feedback_packet_size);
        }
    }

    return nullptr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeWritePcm(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray bytes,
    jint length) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return nullableError(env, "USB exclusive transport handle is invalid.");
    }
    if (bytes == nullptr || length <= 0) {
        return nullptr;
    }

    const jsize array_length = env->GetArrayLength(bytes);
    const int safe_length = std::min<int>(length, array_length);
    auto* input = reinterpret_cast<uint8_t*>(env->GetByteArrayElements(bytes, nullptr));
    if (input == nullptr) {
        return nullableError(env, "Failed to access PCM buffer.");
    }

    std::string error;
    int offset = 0;
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        const int iso_packet_size = transport->iso_packet_size > 0
            ? std::min(transport->iso_packet_size, transport->max_packet_size)
            : transport->max_packet_size;
        const int max_chunk = std::max(1, iso_packet_size * kMaxIsoPacketsPerUrb);
        while (offset < safe_length && error.empty()) {
            const int chunk = std::min(max_chunk, safe_length - offset);
            error = submitIsoChunkLocked(*transport, input + offset, chunk);
            offset += chunk;
        }
    }

    env->ReleaseByteArrayElements(bytes, reinterpret_cast<jbyte*>(input), JNI_ABORT);
    if (error.empty() && transport->write_log_count < 5) {
        ++transport->write_log_count;
        __android_log_print(
            ANDROID_LOG_DEBUG,
            kTag,
            "writePcm submitted %d bytes to endpoint=0x%x isoPacket=%d",
            safe_length,
            transport->endpoint_address,
            transport->iso_packet_size);
    }
    return nullableError(env, error);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeWriteIsoPackets(
    JNIEnv* env,
    jobject,
    jlong handle,
    jbyteArray bytes,
    jintArray packet_lengths,
    jint packet_count) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return nullableError(env, "USB exclusive transport handle is invalid.");
    }
    if (bytes == nullptr || packet_lengths == nullptr || packet_count <= 0) {
        return nullptr;
    }

    const jsize array_length = env->GetArrayLength(bytes);
    const jsize lengths_length = env->GetArrayLength(packet_lengths);
    const int safe_packet_count = std::min<int>(
        std::min<int>(packet_count, lengths_length),
        kMaxIsoPacketsPerUrb);
    if (safe_packet_count <= 0) {
        return nullptr;
    }

    int safe_length = 0;
    jint stack_lengths[kMaxIsoPacketsPerUrb] = {};
    env->GetIntArrayRegion(packet_lengths, 0, safe_packet_count, stack_lengths);
    for (int i = 0; i < safe_packet_count; ++i) {
        if (stack_lengths[i] <= 0) {
            return nullableError(env, "USB exclusive iso packet length is invalid.");
        }
        safe_length += stack_lengths[i];
    }
    if (safe_length > array_length) {
        return nullableError(env, "USB exclusive iso packet data is shorter than packet lengths.");
    }

    auto* input = reinterpret_cast<uint8_t*>(env->GetByteArrayElements(bytes, nullptr));
    if (input == nullptr) {
        return nullableError(env, "Failed to access PCM buffer.");
    }

    std::string error;
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        error = submitIsoPacketsLocked(
            *transport, input, safe_length, stack_lengths, safe_packet_count);
    }

    env->ReleaseByteArrayElements(bytes, reinterpret_cast<jbyte*>(input), JNI_ABORT);
    if (error.empty() && transport->write_log_count < 5) {
        ++transport->write_log_count;
        __android_log_print(
            ANDROID_LOG_DEBUG,
            kTag,
            "writeIsoPackets submitted %d bytes packets=%d endpoint=0x%x",
            safe_length,
            safe_packet_count,
            transport->endpoint_address);
    }
    return nullableError(env, error);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeFeedbackFramesPerPacketQ16(
    JNIEnv*,
    jobject,
    jlong handle) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(transport->mutex);
    return transport->feedback_frames_per_packet_q16;
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeTransportTelemetry(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* transport = fromHandle(handle);
    jlong values[] = {0, 0, 0, 0};
    if (transport != nullptr) {
        std::lock_guard<std::mutex> lock(transport->mutex);
        if (transport->fd >= 0) {
            reapCompletedLocked(*transport);
        }

        long long pending_iso_packets = 0;
        long long pending_output_urbs = 0;
        for (const auto& pending : transport->pending_urbs) {
            if (!pending.feedback) {
                pending_iso_packets += pending.packets;
                ++pending_output_urbs;
            }
        }
        values[0] = static_cast<jlong>(pending_iso_packets);
        values[1] = static_cast<jlong>(transport->total_iso_packets);
        values[2] = static_cast<jlong>(pending_output_urbs);
        values[3] = static_cast<jlong>(transport->iso_error_count);
    }
    jlongArray result = env->NewLongArray(4);
    if (result != nullptr) {
        env->SetLongArrayRegion(result, 0, 4, values);
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeSetIsoPacketSize(
    JNIEnv*,
    jobject,
    jlong handle,
    jint packet_size) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(transport->mutex);
    transport->iso_packet_size =
        std::max(0, std::min(static_cast<int>(packet_size), transport->max_packet_size));
    __android_log_print(
        ANDROID_LOG_INFO,
        kTag,
        "iso packet size set to %d bytes",
        transport->iso_packet_size);
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeSetMaxPendingOutputUrbs(
    JNIEnv*,
    jobject,
    jlong handle,
    jint max_pending_urbs) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(transport->mutex);
    transport->max_pending_urbs = std::max(
        kDefaultMaxPendingUrbs,
        std::min(static_cast<int>(max_pending_urbs), kAbsoluteMaxPendingUrbs));
    __android_log_print(
        ANDROID_LOG_INFO,
        kTag,
        "max pending output URBs set to %d",
        transport->max_pending_urbs);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeFlushOutput(
    JNIEnv* env,
    jobject,
    jlong handle) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(transport->mutex);
    const std::string error = flushOutputLocked(*transport);
    if (error.empty()) {
        return nullptr;
    }
    return env->NewStringUTF(error.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_afalphy_sylvakru_UsbExclusiveNative_nativeClose(JNIEnv*, jobject, jlong handle) {
    auto* transport = fromHandle(handle);
    if (transport == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(transport->mutex);
    closeLocked(*transport);
}
