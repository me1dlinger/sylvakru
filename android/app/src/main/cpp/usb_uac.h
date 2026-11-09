#pragma once

#include <cstdint>
#include <vector>

namespace sylvakru {

// UAC 原始配置描述符解析：输入是整段 raw descriptors（Android rawDescriptors /
// libusb 配置描述符同一格式），纯字节遍历，与引擎内原 Kotlin 实现逐行为对齐
// （tests/usb_uac_test.cpp 以手工构造的 UAC1/UAC2 描述符对拍）。

// AudioStreaming 各 (interface, alt) 的格式信息（AS_GENERAL + Type-I 格式描述符合并）。
// 数值字段 -1 表示描述符未出现该字段（对应 Kotlin 的 null）。
struct UacStreamingFormatInfo {
    int interface_number = -1;
    int alternate_setting = -1;
    // 注意：与原 Kotlin 实现保持一致，从接口描述符 offset+8 读取（标准布局中
    // +7 才是 bInterfaceProtocol，+8 是 iInterface）；该字段仅进诊断日志，无人读取
    int protocol = -1;
    int terminal_link = -1;
    int format_type = -1;
    int channels = -1;
    int subslot_size = -1;
    int bit_resolution = -1;
    bool has_bm_formats = false;
    uint32_t bm_formats = 0;

    // UAC2 bmFormats 的 D31 = RAW_DATA，即 native DSD alt
    bool isRawData() const { return has_bm_formats && (bm_formats & 0x80000000u) != 0; }
};

// 解析所有 AS 接口的格式描述符；结果按 (interface, alt) 首次出现顺序排列，
// 同一键的后续描述符合并进已有条目（与 Kotlin LinkedHashMap 语义一致）。
std::vector<UacStreamingFormatInfo> parseUacStreamingFormats(
    const uint8_t* descriptors,
    size_t size);

struct UacClockSourceInfo {
    // false = 描述符里没有 CLOCK_SOURCE 实体（UAC1 设备），采样率须走端点 SET_CUR
    bool has_clock_source = false;
    // 指定 AS (interface, alt) 的 AS_GENERAL bTerminalLink；-1 = 未找到
    int terminal_link = -1;
    // terminal→clock 映射结果，找不到时回退第一个 clock source；-1 = null
    int clock_source_id = -1;
};

UacClockSourceInfo findUac2ClockSource(
    const uint8_t* descriptors,
    size_t size,
    int streaming_interface_number,
    int streaming_alternate_setting);

// Feature Unit（子类型 0x06）里带音量控制的通道；协议按接口描述符
// bInterfaceProtocol==0x20 区分 UAC2/UAC1，bmaControls 布局随之不同。
struct UacVolumeFeature {
    bool uac2 = false;
    int control_interface = -1;
    int unit_id = -1;
    int source_id = -1;
    int channel = -1;
    // UAC2：volume 控制位 0x01=只读、0x03=读写（其余通道不收录）；UAC1 恒可写
    bool writable = false;
};

std::vector<UacVolumeFeature> parseUacVolumeFeatures(
    const uint8_t* descriptors,
    size_t size);

// AudioControl OUTPUT_TERMINAL（子类型 0x03）的 bSourceID 集合（按首次出现去重），
// 用于判断 Feature Unit 是否直连输出端子。
std::vector<int> parseUacOutputTerminalSources(
    const uint8_t* descriptors,
    size_t size);

}  // namespace sylvakru
