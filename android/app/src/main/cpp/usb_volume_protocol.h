#pragma once

#include <cstdint>
#include <vector>

namespace sylvakru {

// iBasso HID 音量协议的纯逻辑核心：增益换算、音量表映射、HID 报文构造/解析/
// 路由、写后验证与 reader 恢复决策。从 Kotlin UsbVolumeProtocol.kt 逐行为对齐
// 平移（tests/usb_volume_protocol_test.cpp 对拍）。
//
// ⚠️ 音量安全红线：本文件任何数值行为改动都必须先过逐字节/逐数值对拍测试；
// "回读失灵时只允许只降不升、无可信值宁可暂停"的决策分支不得放宽。
//
// 会话策略胶水（协议选择、DSD 安全门文案、reader 世代判定等）留在 Kotlin。

inline constexpr int kIbassoUnityGainQ16 = 65536;

// 可空 Int 的约定：指针为 nullptr 表示 Kotlin 侧 null。

// quirk 自动位深选择：优先与源一致，否则向上取最近，再否则向下取最近，
// 全无可用位深才返回 0（Kotlin 侧视为 null）。
// source_bit_depth 为 nullptr 表示未知源位深：按 24/32/16 顺序取第一个可用，
// 都没有则取最小可用位深。
int preferredAutoPcmBitDepth(
    const int* source_bit_depth,
    const std::vector<int>& available_bit_depths);

// ReplayGain(千分之一 dB) 合成进用户线性增益（Q16），NaN/溢出安全钳位。
int effectiveVolumeGainQ16(int user_gain_q16, int replay_gain_milli_db);

// 硬件音量的合成增益：DSD 时把补偿 dB 并入 ReplayGain 后再换算。
int effectiveHardwareVolumeGainQ16(
    int user_gain_q16,
    int replay_gain_milli_db,
    int dsd_compensation_db,
    bool is_dsd);

// 线性增益（Q16）→ 音量表下标（0..100，感知曲线 ^(2/3)）。
int ibassoVolumeIndex(int gain_q16);

// 音量表下标 → 设备寄存器值（255=静音，0=最大）。
int ibassoDeviceVolume(int index);

// DSD 寄存器值 = 基础值 - 补偿dB×2（半 dB 步进），补偿钳位 [-12, 6]。
int ibassoDsdVolume(int base_volume, int compensation_db);

struct IbassoVolumeTarget {
    int base_raw = 0;
    int dsd_raw = 0;
};

// 应用增益 → 双寄存器目标；增益≤0 时两寄存器都取 255（静音）。
IbassoVolumeTarget ibassoAppGainToRaw(
    int gain_q16,
    int replay_gain_milli_db,
    int dsd_compensation_db);

// 设备寄存器值 → 线性增益（Q16）：取表中最接近值的下标（并列取靠前者）。
int ibassoRawToLinearGainQ16(int raw);

struct IbassoVolumeEvent {
    bool valid = false;
    int left_raw = 0;
    int right_raw = 0;
};

// 解析主动上报的音量事件（端点前缀 0xfe 0x01 在 [4][5]，或旧版在 [0][1]）。
IbassoVolumeEvent ibassoDecodeEvent(const uint8_t* packet, size_t size);

enum class IbassoPacketRouteKind {
    kUnknown,
    kEvent,
    kCommandResponse,
};

struct IbassoPacketRoute {
    IbassoPacketRouteKind kind = IbassoPacketRouteKind::kUnknown;
    // kEvent 时有效
    int left_raw = 0;
    int right_raw = 0;
    // kCommandResponse 时有效
    int command = 0;
};

// HID 读包分流：事件优先；否则按挂起命令或自识别响应头归类；都不是则 Unknown。
IbassoPacketRoute routeIbassoVolumePacket(
    const uint8_t* packet,
    size_t size,
    const std::vector<int>& pending_commands);

// 16 字节 I2C 写包 / ROOM 写包 / 音量读包（布局与设备固件约定一致，勿动）。
void ibassoI2cWritePacket(
    int command, int slave, int offset, int byte_offset, int value, uint8_t out[16]);
void ibassoRoomWritePacket(int command, int register_id, int value, uint8_t out[16]);
void ibassoVolumeReadPacket(uint8_t out[16]);

// 一次完整音量写事务的 10 个包（4×基础 I2C + 4×DSD I2C + 2×ROOM，顺序固定）。
std::vector<std::vector<uint8_t>> ibassoVolumePackets(const IbassoVolumeTarget& target);

// 顺序与 Kotlin enum 一致（JNI 按序号传递，不得重排）。
enum class IbassoVolumeVerificationAction {
    kAcceptTarget,
    kKeepPrevious,
    kRetryReadback,
    kYieldToPending,
    kFreezePcm,
    kFreezeDsd,
    kPauseDsd,
};

// 写后回读验证决策：读回匹配目标/上一值 → 接受/保持；3 次内重试；有挂起
// 请求让位；DSD 仅当两个寄存器目标都只降不升时才允许冻结在可信值，否则暂停。
IbassoVolumeVerificationAction ibassoVolumeVerificationAction(
    int target_raw,
    const int* previous_raw,
    const int* readback_raw,
    int failure_count,
    bool is_dsd,
    bool has_pending_request,
    const int* target_dsd_raw,
    const int* previous_dsd_raw);

// 顺序与 Kotlin enum 一致（JNI 按序号传递，不得重排）。
enum class IbassoReaderRecoveryAction {
    kVerifyNow,
    kWait,
    kFreezePcm,
    kCancel,
};

// reader 掉线恢复决策：DSD 有界等待重启后仍走严格验证（绝不落 FREEZE_PCM），
// PCM 写死/超时冻结数字兜底。
IbassoReaderRecoveryAction ibassoReaderRecoveryAction(
    bool is_dsd,
    bool health_write_only,
    bool health_restart_requested,
    bool reader_running,
    bool generation_matches,
    bool wait_expired);

// iBasso 写事务节流：距上次完成 150ms 内或最新挂起更新 300ms 内继续等待。
// 协议判定（仅 ibassoHid 生效）留在 Kotlin 守卫。
int64_t ibassoVolumePendingDelayMs(
    int64_t last_completed_at_ms,
    bool has_pending_updated_at,
    int64_t pending_updated_at_ms,
    int64_t now_ms);

struct IbassoActualVolume {
    int raw = 0;
    int gain_q16 = 0;
};

// 主动事件的基础寄存器值 → 当前生效（PCM/DSD）寄存器值与线性增益。
IbassoActualVolume ibassoActualEventGainQ16(
    int base_raw,
    bool is_dsd,
    int dsd_compensation_db);

// 主动事件 → 可信双寄存器目标（DSD 寄存器按当前补偿推导）。
IbassoVolumeTarget ibassoTargetFromEvent(int base_raw, int dsd_compensation_db);

}  // namespace sylvakru
