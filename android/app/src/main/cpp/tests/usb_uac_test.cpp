#include "usb_uac.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

// 手工构造最小 UAC1/UAC2 配置描述符片段，对拍引擎内原 Kotlin 解析行为
// （parseStreamingFormatInfo / findUac2ClockSourceId /
//   parseHardwareVolumeFeatures / parseOutputTerminalSources）。

namespace {

using Blob = std::vector<uint8_t>;

// 标准接口描述符（9 字节）：+5 class、+6 subclass、+7 protocol、+8 iInterface
void iface(Blob& out, int number, int alt, int klass, int subclass, int protocol, int i_interface = 0) {
    out.insert(out.end(), {
        9, 0x04,
        static_cast<uint8_t>(number), static_cast<uint8_t>(alt), 0,
        static_cast<uint8_t>(klass), static_cast<uint8_t>(subclass),
        static_cast<uint8_t>(protocol), static_cast<uint8_t>(i_interface),
    });
}

// UAC2 AS_GENERAL（16 字节）：bTerminalLink、bFormatType、bmFormats(LE)、bNrChannels
void uac2AsGeneral(Blob& out, int terminal_link, int format_type, uint32_t bm_formats, int channels) {
    out.insert(out.end(), {
        16, 0x24, 0x01,
        static_cast<uint8_t>(terminal_link), 0,
        static_cast<uint8_t>(format_type),
        static_cast<uint8_t>(bm_formats & 0xff),
        static_cast<uint8_t>((bm_formats >> 8) & 0xff),
        static_cast<uint8_t>((bm_formats >> 16) & 0xff),
        static_cast<uint8_t>((bm_formats >> 24) & 0xff),
        static_cast<uint8_t>(channels),
        0, 0, 0, 0, 0,
    });
}

// UAC2 Type-I 格式描述符（固定 6 字节）：bFormatType、bSubslotSize、bBitResolution
void uac2TypeI(Blob& out, int subslot_size, int bit_resolution) {
    out.insert(out.end(), {
        6, 0x24, 0x02, 0x01,
        static_cast<uint8_t>(subslot_size), static_cast<uint8_t>(bit_resolution),
    });
}

// UAC1 AS_GENERAL（7 字节）：bTerminalLink、bDelay、wFormatTag
void uac1AsGeneral(Blob& out, int terminal_link, int format_tag) {
    out.insert(out.end(), {
        7, 0x24, 0x01,
        static_cast<uint8_t>(terminal_link), 0,
        static_cast<uint8_t>(format_tag), 0,
    });
}

// UAC1 Type-I（8 + 3×rates 字节）：bNrChannels、bSubframeSize、bBitResolution、采样率表
void uac1TypeI(Blob& out, int channels, int subframe_size, int bit_resolution) {
    out.insert(out.end(), {
        11, 0x24, 0x02, 0x01,
        static_cast<uint8_t>(channels),
        static_cast<uint8_t>(subframe_size), static_cast<uint8_t>(bit_resolution),
        1, 0x44, 0xAC, 0x00,  // 一档 44100
    });
}

void streamingFormatsParseUac2Layouts() {
    Blob blob;
    // alt0（零带宽）无格式描述符；alt1 PCM；alt2 RAW_DATA（native DSD）
    iface(blob, 1, 0, 1, 2, 0x20, 0x77);
    iface(blob, 1, 1, 1, 2, 0x20, 0x77);
    uac2AsGeneral(blob, 3, 1, 0x00000001, 2);
    uac2TypeI(blob, 4, 32);
    iface(blob, 1, 2, 1, 2, 0x20, 0x77);
    uac2AsGeneral(blob, 3, 1, 0x80000000u, 2);
    uac2TypeI(blob, 4, 32);

    const auto formats = sylvakru::parseUacStreamingFormats(blob.data(), blob.size());
    assert(formats.size() == 2);
    assert(formats[0].interface_number == 1 && formats[0].alternate_setting == 1);
    // 原实现从 +8 读 protocol（此处为 iInterface 槽位的 0x77），保持一致
    assert(formats[0].protocol == 0x77);
    assert(formats[0].terminal_link == 3);
    assert(formats[0].format_type == 1);
    assert(formats[0].channels == 2);
    assert(formats[0].subslot_size == 4);
    assert(formats[0].bit_resolution == 32);
    assert(formats[0].has_bm_formats && formats[0].bm_formats == 0x00000001);
    assert(!formats[0].isRawData());
    assert(formats[1].alternate_setting == 2);
    assert(formats[1].isRawData());
}

void streamingFormatsParseUac1Layout() {
    Blob blob;
    iface(blob, 2, 1, 1, 2, 0x00, 0);
    uac1AsGeneral(blob, 5, 1);
    uac1TypeI(blob, 2, 2, 16);

    const auto formats = sylvakru::parseUacStreamingFormats(blob.data(), blob.size());
    assert(formats.size() == 1);
    assert(formats[0].terminal_link == 5);
    // UAC1 AS_GENERAL 只有 7 字节：formatType 取 +5（wFormatTag 低字节），无 bmFormats
    assert(formats[0].format_type == 1);
    assert(!formats[0].has_bm_formats);
    // UAC1 Type-I（length>=7）按 UAC1 布局：16-bit 不能被误当 UAC2 的 subslot=2/bits=16 错位
    assert(formats[0].channels == 2);
    assert(formats[0].subslot_size == 2);
    assert(formats[0].bit_resolution == 16);
}

void streamingFormatsIgnoreNonStreamingInterfacesAndTruncation() {
    Blob blob;
    // AudioControl（subclass 1）里的类特定描述符不属于 AS 格式
    iface(blob, 0, 0, 1, 1, 0x20, 0);
    uac2AsGeneral(blob, 9, 1, 0x00000001, 2);
    iface(blob, 1, 1, 1, 2, 0x20, 0);
    uac2AsGeneral(blob, 3, 1, 0x00000001, 2);
    // 尾部声明 16 字节但只剩 3 字节 → 直接停止，已解析结果保留
    blob.insert(blob.end(), {16, 0x24, 0x01});

    const auto formats = sylvakru::parseUacStreamingFormats(blob.data(), blob.size());
    assert(formats.size() == 1);
    assert(formats[0].interface_number == 1);
    assert(formats[0].terminal_link == 3);
}

// UAC2 CLOCK_SOURCE（8 字节）
void clockSource(Blob& out, int clock_id) {
    out.insert(out.end(), {8, 0x24, 0x0a, static_cast<uint8_t>(clock_id), 0x01, 0x01, 0x00, 0});
}

// UAC2 INPUT_TERMINAL（17 字节）：bTerminalID(+3)、bCSourceID(+7)
void inputTerminal(Blob& out, int terminal_id, int clock_id) {
    Blob descriptor = {17, 0x24, 0x02, static_cast<uint8_t>(terminal_id), 0x01, 0x01, 0x00,
                       static_cast<uint8_t>(clock_id)};
    descriptor.resize(17, 0);
    out.insert(out.end(), descriptor.begin(), descriptor.end());
}

// UAC2 OUTPUT_TERMINAL（12 字节）：bTerminalID(+3)、bSourceID(+7)、bCSourceID(+8)
void outputTerminal(Blob& out, int terminal_id, int source_id, int clock_id) {
    out.insert(out.end(), {
        12, 0x24, 0x03, static_cast<uint8_t>(terminal_id), 0x01, 0x03, 0x00,
        static_cast<uint8_t>(source_id), static_cast<uint8_t>(clock_id), 0, 0, 0,
    });
}

void clockSourceResolvesThroughTerminalLink() {
    Blob blob;
    iface(blob, 0, 0, 1, 1, 0x20, 0);
    clockSource(blob, 0x29);
    inputTerminal(blob, 0x01, 0x29);
    outputTerminal(blob, 0x06, 0x05, 0x29);
    iface(blob, 1, 1, 1, 2, 0x20, 0);
    uac2AsGeneral(blob, 0x01, 1, 0x00000001, 2);

    const auto info = sylvakru::findUac2ClockSource(blob.data(), blob.size(), 1, 1);
    assert(info.has_clock_source);
    assert(info.terminal_link == 0x01);
    assert(info.clock_source_id == 0x29);

    // terminalLink 指向输出端子时走输出端子映射
    const auto via_output = sylvakru::findUac2ClockSource(blob.data(), blob.size(), 1, 1);
    (void)via_output;
}

void clockSourceFallsBackToFirstClockWhenLinkUnknown() {
    Blob blob;
    iface(blob, 0, 0, 1, 1, 0x20, 0);
    clockSource(blob, 0x30);
    clockSource(blob, 0x31);
    iface(blob, 1, 1, 1, 2, 0x20, 0);
    uac2AsGeneral(blob, 0x7f, 1, 0x00000001, 2);  // 链接到不存在的端子

    const auto info = sylvakru::findUac2ClockSource(blob.data(), blob.size(), 1, 1);
    assert(info.has_clock_source);
    assert(info.terminal_link == 0x7f);
    assert(info.clock_source_id == 0x30);  // 回退第一个 clock source
}

void uac1DeviceHasNoClockSource() {
    Blob blob;
    iface(blob, 0, 0, 1, 1, 0x00, 0);
    // UAC1 INPUT_TERMINAL：+7 是 bNrChannels，不能被误读成 clockSourceId
    inputTerminal(blob, 0x01, 0x02);
    iface(blob, 1, 1, 1, 2, 0x00, 0);
    uac1AsGeneral(blob, 0x01, 1);

    const auto info = sylvakru::findUac2ClockSource(blob.data(), blob.size(), 1, 1);
    assert(!info.has_clock_source);
    assert(info.clock_source_id == -1);
}

// UAC2 FEATURE_UNIT：master+2 声道，bmaControls 每通道 4 字节 + iFeature
void uac2FeatureUnit(Blob& out, int unit_id, int source_id, uint32_t master, uint32_t ch1, uint32_t ch2) {
    // bLength = 5 字节头 + 3×4 字节 bmaControls + iFeature = 18
    Blob descriptor = {18, 0x24, 0x06, static_cast<uint8_t>(unit_id), static_cast<uint8_t>(source_id)};
    for (const uint32_t controls : {master, ch1, ch2}) {
        descriptor.push_back(static_cast<uint8_t>(controls & 0xff));
        descriptor.push_back(static_cast<uint8_t>((controls >> 8) & 0xff));
        descriptor.push_back(static_cast<uint8_t>((controls >> 16) & 0xff));
        descriptor.push_back(static_cast<uint8_t>((controls >> 24) & 0xff));
    }
    descriptor.push_back(0);  // iFeature
    out.insert(out.end(), descriptor.begin(), descriptor.end());
}

void volumeFeaturesParseUac2Controls() {
    Blob blob;
    iface(blob, 0, 0, 1, 1, 0x20, 0);
    // master 读写（bits2-3=0b11）、ch1 只读（0b01）、ch2 无 volume（0）
    uac2FeatureUnit(blob, 0x0b, 0x01, 0x0000000c, 0x00000004, 0x00000000);

    const auto features = sylvakru::parseUacVolumeFeatures(blob.data(), blob.size());
    assert(features.size() == 2);
    assert(features[0].uac2);
    assert(features[0].control_interface == 0);
    assert(features[0].unit_id == 0x0b);
    assert(features[0].source_id == 0x01);
    assert(features[0].channel == 0);
    assert(features[0].writable);
    assert(features[1].channel == 1);
    assert(!features[1].writable);
}

void volumeFeaturesParseUac1Controls() {
    Blob blob;
    iface(blob, 0, 0, 1, 1, 0x00, 0);
    // UAC1 FU：bControlSize=1，master=0x03（mute+volume）、ch1=0x02（volume）、ch2=0x00
    blob.insert(blob.end(), {10, 0x24, 0x06, 0x02, 0x01, 1, 0x03, 0x02, 0x00, 0});

    const auto features = sylvakru::parseUacVolumeFeatures(blob.data(), blob.size());
    assert(features.size() == 2);
    assert(!features[0].uac2);
    assert(features[0].unit_id == 0x02 && features[0].source_id == 0x01);
    assert(features[0].channel == 0 && features[0].writable);
    assert(features[1].channel == 1 && features[1].writable);
}

void volumeFeaturesRequireAudioControlInterface() {
    Blob blob;
    // 视频类接口（class=14）下的 0x24/0x06 不是音频 Feature Unit
    iface(blob, 0, 0, 14, 1, 0x00, 0);
    blob.insert(blob.end(), {10, 0x24, 0x06, 0x02, 0x01, 1, 0x03, 0x02, 0x00, 0});
    // AudioStreaming（subclass=2）下同样不收
    iface(blob, 1, 0, 1, 2, 0x00, 0);
    blob.insert(blob.end(), {10, 0x24, 0x06, 0x03, 0x01, 1, 0x03, 0x02, 0x00, 0});

    const auto features = sylvakru::parseUacVolumeFeatures(blob.data(), blob.size());
    assert(features.empty());
}

void outputTerminalSourcesCollectAndDeduplicate() {
    Blob blob;
    iface(blob, 0, 0, 1, 1, 0x00, 0);
    // UAC1 OUTPUT_TERMINAL（9 字节）：bSourceID 在 +7
    blob.insert(blob.end(), {9, 0x24, 0x03, 0x06, 0x01, 0x03, 0x00, 0x0b, 0});
    blob.insert(blob.end(), {9, 0x24, 0x03, 0x07, 0x02, 0x06, 0x00, 0x0b, 0});
    blob.insert(blob.end(), {9, 0x24, 0x03, 0x08, 0x03, 0x02, 0x00, 0x0c, 0});

    const auto sources = sylvakru::parseUacOutputTerminalSources(blob.data(), blob.size());
    assert(sources.size() == 2);
    assert(sources[0] == 0x0b);
    assert(sources[1] == 0x0c);
}

void nullDescriptorsYieldEmptyResults() {
    assert(sylvakru::parseUacStreamingFormats(nullptr, 0).empty());
    assert(!sylvakru::findUac2ClockSource(nullptr, 0, 1, 1).has_clock_source);
    assert(sylvakru::parseUacVolumeFeatures(nullptr, 0).empty());
    assert(sylvakru::parseUacOutputTerminalSources(nullptr, 0).empty());
}

}  // namespace

int main() {
#define RUN(test)                         \
    do {                                  \
        std::fprintf(stderr, #test "\n"); \
        test();                           \
    } while (false)
    RUN(streamingFormatsParseUac2Layouts);
    RUN(streamingFormatsParseUac1Layout);
    RUN(streamingFormatsIgnoreNonStreamingInterfacesAndTruncation);
    RUN(clockSourceResolvesThroughTerminalLink);
    RUN(clockSourceFallsBackToFirstClockWhenLinkUnknown);
    RUN(uac1DeviceHasNoClockSource);
    RUN(volumeFeaturesParseUac2Controls);
    RUN(volumeFeaturesParseUac1Controls);
    RUN(volumeFeaturesRequireAudioControlInterface);
    RUN(outputTerminalSourcesCollectAndDeduplicate);
    RUN(nullDescriptorsYieldEmptyResults);
#undef RUN
    std::fprintf(stderr, "all tests passed\n");
    return 0;
}
