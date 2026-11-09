#include "usb_uac.h"

#include <map>

namespace sylvakru {

namespace {

// USB 音频类（bInterfaceClass），对应 Android UsbConstants.USB_CLASS_AUDIO
constexpr int kUsbClassAudio = 1;

int byteAt(const uint8_t* descriptors, size_t offset) {
    return descriptors[offset];
}

}  // namespace

std::vector<UacStreamingFormatInfo> parseUacStreamingFormats(
    const uint8_t* descriptors,
    size_t size) {
    std::vector<UacStreamingFormatInfo> formats;
    if (descriptors == nullptr) {
        return formats;
    }

    auto entryFor = [&formats](int interface_number, int alternate_setting, int protocol)
        -> UacStreamingFormatInfo& {
        for (auto& format : formats) {
            if (format.interface_number == interface_number &&
                format.alternate_setting == alternate_setting) {
                return format;
            }
        }
        UacStreamingFormatInfo created;
        created.interface_number = interface_number;
        created.alternate_setting = alternate_setting;
        created.protocol = protocol;
        formats.push_back(created);
        return formats.back();
    };

    size_t offset = 0;
    int current_interface_number = -1;
    int current_alternate_setting = -1;
    int current_interface_subclass = -1;
    int current_interface_protocol = -1;

    while (offset + 1 < size) {
        const size_t length = byteAt(descriptors, offset);
        const int descriptor_type = byteAt(descriptors, offset + 1);
        if (length < 2 || offset + length > size) {
            break;
        }

        if (descriptor_type == 0x04 && length >= 9) {
            current_interface_number = byteAt(descriptors, offset + 2);
            current_alternate_setting = byteAt(descriptors, offset + 3);
            current_interface_subclass = byteAt(descriptors, offset + 6);
            // 与 Kotlin 原实现一致读 +8（仅日志用途，见头文件注释）
            current_interface_protocol = byteAt(descriptors, offset + 8);
        } else if (
            descriptor_type == 0x24 &&
            current_interface_subclass == 2 &&
            length >= 3) {
            UacStreamingFormatInfo& existing = entryFor(
                current_interface_number,
                current_alternate_setting,
                current_interface_protocol);
            const int subtype = byteAt(descriptors, offset + 2);
            switch (subtype) {
                case 0x01: {
                    if (length >= 4) {
                        existing.terminal_link = byteAt(descriptors, offset + 3);
                    }
                    if (length >= 6) {
                        existing.format_type = byteAt(descriptors, offset + 5);
                    }
                    // UAC2 AS_GENERAL（16 字节）的 bmFormats：D31=RAW_DATA 即 native DSD alt；
                    // UAC1 该描述符只有 7 字节，天然不会进这个分支
                    if (length >= 10) {
                        existing.has_bm_formats = true;
                        existing.bm_formats =
                            static_cast<uint32_t>(byteAt(descriptors, offset + 6)) |
                            (static_cast<uint32_t>(byteAt(descriptors, offset + 7)) << 8) |
                            (static_cast<uint32_t>(byteAt(descriptors, offset + 8)) << 16) |
                            (static_cast<uint32_t>(byteAt(descriptors, offset + 9)) << 24);
                    }
                    if (length >= 11) {
                        existing.channels = byteAt(descriptors, offset + 10);
                    }
                    break;
                }
                case 0x02: {
                    // UAC1 Type-I 格式描述符比 UAC2 多一个 bNrChannels 字段、且带采样率表，
                    // 描述符更长（length>=7）；UAC2 Type-I 固定 length=6。两个分支判据
                    // 顺序不能反：先判 length>=6 会让 UAC1 描述符错误命中 UAC2 布局，把
                    // bSubframeSize(2/3/4) 当成位深，16-bit 被当 2/3/4-bit 严重右移打成静音。
                    if (length >= 7) {
                        // UAC1: bFormatType, bNrChannels, bSubframeSize, bBitResolution, …
                        existing.format_type = byteAt(descriptors, offset + 3);
                        existing.channels = byteAt(descriptors, offset + 4);
                        existing.subslot_size = byteAt(descriptors, offset + 5);
                        existing.bit_resolution = byteAt(descriptors, offset + 6);
                    } else if (length >= 6) {
                        // UAC2: bFormatType, bSubslotSize, bBitResolution
                        existing.format_type = byteAt(descriptors, offset + 3);
                        existing.subslot_size = byteAt(descriptors, offset + 4);
                        existing.bit_resolution = byteAt(descriptors, offset + 5);
                    }
                    break;
                }
            }
        }

        offset += length;
    }
    return formats;
}

UacClockSourceInfo findUac2ClockSource(
    const uint8_t* descriptors,
    size_t size,
    int streaming_interface_number,
    int streaming_alternate_setting) {
    UacClockSourceInfo result;
    if (descriptors == nullptr) {
        return result;
    }

    size_t offset = 0;
    int current_interface_number = -1;
    int current_alternate_setting = -1;
    int current_interface_subclass = -1;
    int first_clock_source_id = -1;
    std::map<int, int> input_terminal_clock_ids;
    std::map<int, int> output_terminal_clock_ids;

    while (offset + 1 < size) {
        const size_t length = byteAt(descriptors, offset);
        const int descriptor_type = byteAt(descriptors, offset + 1);
        if (length < 2 || offset + length > size) {
            break;
        }

        if (descriptor_type == 0x04 && length >= 9) {
            current_interface_number = byteAt(descriptors, offset + 2);
            current_alternate_setting = byteAt(descriptors, offset + 3);
            current_interface_subclass = byteAt(descriptors, offset + 6);
        } else if (descriptor_type == 0x24 && length >= 3) {
            const int subtype = byteAt(descriptors, offset + 2);
            switch (subtype) {
                case 0x0a:
                    result.has_clock_source = true;
                    if (length >= 4 && first_clock_source_id == -1) {
                        first_clock_source_id = byteAt(descriptors, offset + 3);
                    }
                    break;
                case 0x02:
                    if (length >= 8) {
                        const int terminal_id = byteAt(descriptors, offset + 3);
                        input_terminal_clock_ids[terminal_id] = byteAt(descriptors, offset + 7);
                    }
                    break;
                case 0x03:
                    if (length >= 9) {
                        const int terminal_id = byteAt(descriptors, offset + 3);
                        output_terminal_clock_ids[terminal_id] = byteAt(descriptors, offset + 8);
                    }
                    break;
                case 0x01:
                    if (current_interface_number == streaming_interface_number &&
                        current_alternate_setting == streaming_alternate_setting &&
                        current_interface_subclass == 2 &&
                        length >= 4) {
                        result.terminal_link = byteAt(descriptors, offset + 3);
                    }
                    break;
            }
        }

        offset += length;
    }

    // UAC1 设备没有 clock source 实体：terminal→clock 映射按 UAC2 布局解析对
    // UAC1 会误读（把 INPUT_TERMINAL 的 bNrChannels 当成 clockSourceId），
    // 调用方看到 has_clock_source=false 时应改走端点 SET_CUR，不使用本结果
    if (!result.has_clock_source) {
        return result;
    }

    if (result.terminal_link != -1) {
        const auto input = input_terminal_clock_ids.find(result.terminal_link);
        if (input != input_terminal_clock_ids.end()) {
            result.clock_source_id = input->second;
            return result;
        }
        const auto output = output_terminal_clock_ids.find(result.terminal_link);
        if (output != output_terminal_clock_ids.end()) {
            result.clock_source_id = output->second;
            return result;
        }
    }
    result.clock_source_id = first_clock_source_id;
    return result;
}

std::vector<UacVolumeFeature> parseUacVolumeFeatures(
    const uint8_t* descriptors,
    size_t size) {
    std::vector<UacVolumeFeature> features;
    if (descriptors == nullptr) {
        return features;
    }

    size_t offset = 0;
    int interface_number = -1;
    int interface_class = -1;
    int interface_subclass = -1;
    int interface_protocol = -1;

    while (offset + 1 < size) {
        const size_t length = byteAt(descriptors, offset);
        const int descriptor_type = byteAt(descriptors, offset + 1);
        if (length < 2 || offset + length > size) {
            break;
        }
        if (descriptor_type == 0x04 && length >= 9) {
            interface_number = byteAt(descriptors, offset + 2);
            interface_class = byteAt(descriptors, offset + 5);
            interface_subclass = byteAt(descriptors, offset + 6);
            interface_protocol = byteAt(descriptors, offset + 7);
        } else if (
            descriptor_type == 0x24 &&
            interface_class == kUsbClassAudio &&
            interface_subclass == 1 &&
            length >= 7 &&
            byteAt(descriptors, offset + 2) == 0x06) {
            if (interface_protocol == 0x20) {
                // UAC2：每通道固定 4 字节 bmaControls，volume 控制在 bits 2-3
                const int control_count = static_cast<int>(length - 6) / 4;
                for (int channel = 0; channel < control_count; ++channel) {
                    const size_t control_offset = offset + 5 + channel * 4;
                    const uint32_t controls =
                        static_cast<uint32_t>(byteAt(descriptors, control_offset)) |
                        (static_cast<uint32_t>(byteAt(descriptors, control_offset + 1)) << 8) |
                        (static_cast<uint32_t>(byteAt(descriptors, control_offset + 2)) << 16) |
                        (static_cast<uint32_t>(byteAt(descriptors, control_offset + 3)) << 24);
                    const uint32_t volume_control = (controls >> 2) & 0x03;
                    if (volume_control != 0x01 && volume_control != 0x03) {
                        continue;
                    }
                    UacVolumeFeature feature;
                    feature.uac2 = true;
                    feature.control_interface = interface_number;
                    feature.unit_id = byteAt(descriptors, offset + 3);
                    feature.source_id = byteAt(descriptors, offset + 4);
                    feature.channel = channel;
                    feature.writable = volume_control == 0x03;
                    features.push_back(feature);
                }
            } else {
                // UAC1：bControlSize 决定每通道 bmaControls 宽度，volume 在 bit 1
                const int control_size = byteAt(descriptors, offset + 5);
                if (control_size >= 1 && control_size <= 4) {
                    const int control_count = static_cast<int>(length - 7) / control_size;
                    for (int channel = 0; channel < control_count; ++channel) {
                        uint32_t controls = 0;
                        for (int byte_index = 0; byte_index < control_size; ++byte_index) {
                            controls |= static_cast<uint32_t>(byteAt(
                                descriptors,
                                offset + 6 + channel * control_size + byte_index)) << (byte_index * 8);
                        }
                        if ((controls & 0x02) == 0) {
                            continue;
                        }
                        UacVolumeFeature feature;
                        feature.uac2 = false;
                        feature.control_interface = interface_number;
                        feature.unit_id = byteAt(descriptors, offset + 3);
                        feature.source_id = byteAt(descriptors, offset + 4);
                        feature.channel = channel;
                        feature.writable = true;
                        features.push_back(feature);
                    }
                }
            }
        }
        offset += length;
    }
    return features;
}

std::vector<int> parseUacOutputTerminalSources(
    const uint8_t* descriptors,
    size_t size) {
    std::vector<int> sources;
    if (descriptors == nullptr) {
        return sources;
    }

    size_t offset = 0;
    int interface_class = -1;
    int interface_subclass = -1;

    while (offset + 1 < size) {
        const size_t length = byteAt(descriptors, offset);
        const int descriptor_type = byteAt(descriptors, offset + 1);
        if (length < 2 || offset + length > size) {
            break;
        }
        if (descriptor_type == 0x04 && length >= 9) {
            interface_class = byteAt(descriptors, offset + 5);
            interface_subclass = byteAt(descriptors, offset + 6);
        } else if (
            descriptor_type == 0x24 &&
            interface_class == kUsbClassAudio &&
            interface_subclass == 1 &&
            length >= 8 &&
            byteAt(descriptors, offset + 2) == 0x03) {
            const int source = byteAt(descriptors, offset + 7);
            bool seen = false;
            for (const int existing : sources) {
                if (existing == source) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                sources.push_back(source);
            }
        }
        offset += length;
    }
    return sources;
}

}  // namespace sylvakru
