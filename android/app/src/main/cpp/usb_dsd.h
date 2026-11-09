#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sylvakru {

// DSD 静音字节是 0x69（01101001，均值为半幅的交替位型），不是 0x00。
// 暂停/垫包必须发 0x69，发零会让 DAC 掉出 DSD 模式并可能爆音。
inline constexpr uint8_t kDsdSilenceByte = 0x69;

enum class DsdError {
    kNone,
    kOpenFailed,
    kInvalidFile,
    kUnsupportedFormat,
};

struct DsdResult {
    DsdError error = DsdError::kNone;
    std::string message;

    bool ok() const { return error == DsdError::kNone; }
};

// 统一读取 DSF/DFF 文件，屏蔽两种容器差异，输出统一约定的 DSD 字节流：
// MSB-first、逐字节声道交错（L R L R…）。纯标准 C++ 实现，不依赖平台 API，
// 与 Kotlin 版 DsdFileReader 逐行为对齐（tests/usb_dsd_test.cpp 对拍）。
//
// - DSF（Sony，小端）：采样按每通道 blockSizePerChannel 字节的块 planar 存放，读取时转交错；
//   bitsPerSample=1 表示每字节 LSB-first，需查表位反转；=8 表示 MSB-first，直接透传。
// - DFF（Philips，大端 IFF）：数据本身就是 MSB-first 逐字节交错，直接透传；
//   DST 压缩的 DFF 不支持，open 时报错。
class DsdFileReader {
public:
    DsdFileReader() = default;
    ~DsdFileReader();
    DsdFileReader(const DsdFileReader&) = delete;
    DsdFileReader& operator=(const DsdFileReader&) = delete;

    // streaming 为 true 表示文件还在下载增长中：DFF 的数据大小按 chunk 头
    // 声明值取（此刻文件长度不足以作截断依据），可读进度由调用方配合
    // canReadAt 控制。
    DsdResult open(const std::string& path, bool streaming = false);
    void close();

    // "dsf" / "dff"
    const std::string& formatName() const { return format_name_; }
    int sampleRate() const { return sample_rate_; }
    int channels() const { return channels_; }

    int64_t durationMs() const { return bytes_per_channel_ * 8000 / sample_rate_; }
    int64_t positionMs() const { return position_bytes_per_channel_ * 8000 / sample_rate_; }

    // DSD 倍率（64/128/256/512），速率不在 44.1k 族时为 0（对应 Kotlin 的 null）。
    int dsdMultiple() const {
        return sample_rate_ % 44100 == 0 ? sample_rate_ / 44100 : 0;
    }

    // DoP 输出的 PCM 帧率 = DSD 速率 ÷ 16（每帧每声道装 2 个 DSD 字节）。
    int dopFrameRate() const { return sample_rate_ / 16; }

    // 读取交错 DSD 字节流。返回写入 out 的字节数（总是 channels 的整数倍），
    // 文件结束返回 -1。一次调用最多交付一个内部块的余量，调用方循环读取即可。
    int read(uint8_t* out, int capacity);

    // 流式下载中调用：按当前已下载的文件长度 file_length 判断下一次 read
    // 能否交付数据而不把"数据还没下到"误判成文件结尾。
    // DSF 要求下一个块组所需字节齐全（避免半块重复交付），DFF 只要求至少一帧。
    // 已到真实结尾时返回 true，让 read 正常返回 -1。
    bool canReadAt(int64_t file_length) const;

    // 定位到 position_ms 附近：DSF 对齐到块边界，DFF 对齐到 DoP 双字节边界。
    // 返回对齐后的实际位置（毫秒），用于进度上报。
    int64_t seekTo(int64_t position_ms);

private:
    DsdResult openDsf();
    DsdResult openDff(bool streaming);
    int readDsf(uint8_t* out, int capacity);
    bool loadNextDsfBlock();

    FILE* input_ = nullptr;
    std::string format_name_;
    int sample_rate_ = 0;
    int channels_ = 0;
    int64_t data_start_ = 0;
    int64_t bytes_per_channel_ = 0;
    int block_size_per_channel_ = 0;
    bool lsb_first_ = false;
    // open 时刻的文件长度（Kotlin 侧 input.length() 只在 open 期间使用）
    int64_t length_at_open_ = 0;

    // 每通道已交付给调用方的字节数，即当前播放位置
    int64_t position_bytes_per_channel_ = 0;

    // DSF 专用：一个块组（blockSizePerChannel × channels）转交错后的缓冲
    std::vector<uint8_t> chunk_;
    int chunk_length_ = 0;
    int chunk_offset_ = 0;
    // DSF 专用：已从文件装载进 chunk 的每通道字节数
    int64_t loaded_bytes_per_channel_ = 0;
};

// DSD 字节流编码器的统一口径（DoP / Native DSD 共用）：输入都是 DsdFileReader 的
// MSB-first 逐字节交错流，输出都交给 PcmIsoPacketizer 当普通 PCM 帧打包。
// 会话级静音填充线程与写线程只依赖这三个方法，两种模式行为一致：
// 静音一律 0x69，流一旦中断 DAC 就会掉出 DSD 模式（爆音/电流声）。
// 输出参数 out 每次调用先被清空再填充（对应 Kotlin 返回新 ByteArray）。
class DsdStreamEncoder {
public:
    virtual ~DsdStreamEncoder() = default;

    // 编码 data 的前 length 字节，不足一帧的余量留到下次。
    virtual void encode(const uint8_t* data, int length, std::vector<uint8_t>& out) = 0;

    // 生成 frames 帧 DSD 静音（0x69）。
    virtual void encodeSilence(int frames, std::vector<uint8_t>& out) = 0;

    // 把不足一帧的余量用 0x69 补齐成完整帧输出（无余量时输出空）。
    virtual void drain(std::vector<uint8_t>& out) = 0;
};

// 把 DsdFileReader 输出的交错 DSD 字节流封装成 DoP 24-bit PCM 采样流（小端，每采样 3 字节）。
// 每帧每声道取 2 个连续 DSD 字节：sample24 = (marker << 16) | (先到字节 << 8) | 后到字节；
// 标记逐帧在 0x05/0xFA 间交替，同一帧内各声道用相同标记。
// 输出交给 PcmIsoPacketizer 当作普通 24-bit PCM（sampleRate = DSD 速率 ÷ 16），
// 24→32 slot 的高位对齐移位恰好得到规范要求的"DoP 24 位放高位、低 8 位补零"。
// 注意：DoP 数据被任何后续 DSP（音量、抖动、重采样）修改都会破坏标记、输出全幅噪声。
class DopPacketizer : public DsdStreamEncoder {
public:
    explicit DopPacketizer(int channels);

    void encode(const uint8_t* data, int length, std::vector<uint8_t>& out) override;

    // DoP 静音：payload 0x69 0x69，标记正常交替。
    void encodeSilence(int frames, std::vector<uint8_t>& out) override;

    void drain(std::vector<uint8_t>& out) override;

    // seek 后重置：标记回到起始相位，丢弃余量。
    void reset();

private:
    uint8_t byteAt(const uint8_t* data, int index) const;

    const int channels_;
    const int frame_bytes_;
    int marker_ = 0x05;
    std::vector<uint8_t> carry_;
    int carry_length_ = 0;
};

// quirk `nativeDsd.format` → 每声道每帧字节数；未知格式返回 0（对应 Kotlin 的 null）。
int nativeDsdBytesPerSample(const std::string& format);

// Native DSD 打包：把交错 DSD 字节流按设备期望的 subslot 排列重组，直接当
// "bitDepth = subslot 位宽的 PCM" 交给 PcmIsoPacketizer（帧率 = DSD 速率 ÷ 8 ÷ 每采样字节数），
// 无 DoP 标记、无任何数据修改，纯字节重排（对应 ALSA DSD_U8/U16_LE/U32_LE/U32_BE 语义）：
// - LE：subslot 内存序 = 时间序（最早的字节在前）；
// - BE：LSB（最早字节）在 word 高地址，subslot 组内字节倒序。
// 静音同样是 0x69（重排后仍是 0x69），流中断一样会让 DAC 掉出 DSD 模式。
class NativeDsdPacketizer : public DsdStreamEncoder {
public:
    NativeDsdPacketizer(int channels, int bytes_per_sample, bool big_endian);

    void encode(const uint8_t* data, int length, std::vector<uint8_t>& out) override;

    void encodeSilence(int frames, std::vector<uint8_t>& out) override;

    void drain(std::vector<uint8_t>& out) override;

private:
    uint8_t byteAt(const uint8_t* data, int index) const;

    const int channels_;
    const int bytes_per_sample_;
    const bool big_endian_;
    const int frame_bytes_;
    std::vector<uint8_t> carry_;
    int carry_length_ = 0;
};

}  // namespace sylvakru
