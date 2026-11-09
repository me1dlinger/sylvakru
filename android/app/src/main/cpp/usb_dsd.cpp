#include "usb_dsd.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sylvakru {

namespace {

// Windows 的 fopen 按 ANSI 代码页解释路径，UTF-8 路径（中日文文件名）会
// 打不开；转宽字符走 _wfopen。其余平台的 fopen 本身就吃 UTF-8。
FILE* openBinaryForRead(const std::string& path) {
#if defined(_WIN32)
    const int wide_length =
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide_length <= 0) {
        return nullptr;
    }
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wide_length);
    return _wfopen(wide.c_str(), L"rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

// 64 位安全的 stdio 定位（DSD 专辑单文件可超 4GB）
bool seekAbsolute(FILE* file, int64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

int64_t currentLength(FILE* file) {
#if defined(_WIN32)
    const int64_t saved = _ftelli64(file);
    _fseeki64(file, 0, SEEK_END);
    const int64_t length = _ftelli64(file);
    _fseeki64(file, saved, SEEK_SET);
#else
    const off_t saved = ftello(file);
    fseeko(file, 0, SEEK_END);
    const int64_t length = static_cast<int64_t>(ftello(file));
    fseeko(file, saved, SEEK_SET);
#endif
    return length;
}

// 对应 Kotlin readFully：读不满即失败（头部截断按无效文件处理）
bool readFully(FILE* file, uint8_t* out, size_t count) {
    // 流式下载的文件此前可能读到过 EOF，清掉粘滞标志才能看到新增数据
    clearerr(file);
    return fread(out, 1, count, file) == count;
}

bool readIntLe(FILE* file, int32_t* value) {
    uint8_t bytes[4];
    if (!readFully(file, bytes, 4)) {
        return false;
    }
    *value = static_cast<int32_t>(
        static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24));
    return true;
}

bool readLongLe(FILE* file, int64_t* value) {
    uint8_t bytes[8];
    if (!readFully(file, bytes, 8)) {
        return false;
    }
    uint64_t result = 0;
    for (int index = 7; index >= 0; --index) {
        result = (result << 8) | bytes[index];
    }
    *value = static_cast<int64_t>(result);
    return true;
}

bool readIntBe(FILE* file, int32_t* value) {
    uint8_t bytes[4];
    if (!readFully(file, bytes, 4)) {
        return false;
    }
    *value = static_cast<int32_t>(
        (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) |
        static_cast<uint32_t>(bytes[3]));
    return true;
}

bool readShortBe(FILE* file, int32_t* value) {
    uint8_t bytes[2];
    if (!readFully(file, bytes, 2)) {
        return false;
    }
    *value = (static_cast<int32_t>(bytes[0]) << 8) | static_cast<int32_t>(bytes[1]);
    return true;
}

bool readLongBe(FILE* file, int64_t* value) {
    uint8_t bytes[8];
    if (!readFully(file, bytes, 8)) {
        return false;
    }
    uint64_t result = 0;
    for (int index = 0; index < 8; ++index) {
        result = (result << 8) | bytes[index];
    }
    *value = static_cast<int64_t>(result);
    return true;
}

bool readMagic(FILE* file, char out[5]) {
    if (!readFully(file, reinterpret_cast<uint8_t*>(out), 4)) {
        return false;
    }
    out[4] = '\0';
    return true;
}

// LSB-first → MSB-first 的每字节位反转查表
const uint8_t* bitReverseTable() {
    static uint8_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (int index = 0; index < 256; ++index) {
            int value = index;
            int reversed = 0;
            for (int bit = 0; bit < 8; ++bit) {
                reversed = (reversed << 1) | (value & 1);
                value >>= 1;
            }
            table[index] = static_cast<uint8_t>(reversed);
        }
        initialized = true;
    }
    return table;
}

DsdResult invalid(std::string message) {
    return {DsdError::kInvalidFile, std::move(message)};
}

DsdResult unsupported(std::string message) {
    return {DsdError::kUnsupportedFormat, std::move(message)};
}

}  // namespace

DsdFileReader::~DsdFileReader() {
    close();
}

void DsdFileReader::close() {
    if (input_ != nullptr) {
        fclose(input_);
        input_ = nullptr;
    }
}

DsdResult DsdFileReader::open(const std::string& path, bool streaming) {
    close();
    input_ = openBinaryForRead(path);
    if (input_ == nullptr) {
        return {DsdError::kOpenFailed, "Failed to open DSD file."};
    }
    length_at_open_ = currentLength(input_);
    char magic[5];
    DsdResult result;
    if (!readMagic(input_, magic)) {
        result = invalid("Truncated DSF/DFF header.");
    } else if (std::strcmp(magic, "DSD ") == 0) {
        result = openDsf();
    } else if (std::strcmp(magic, "FRM8") == 0) {
        result = openDff(streaming);
    } else {
        result = unsupported("Not a DSF/DFF file.");
    }
    if (!result.ok()) {
        close();
        return result;
    }
    position_bytes_per_channel_ = 0;
    chunk_length_ = 0;
    chunk_offset_ = 0;
    loaded_bytes_per_channel_ = 0;
    if (block_size_per_channel_ > 0) {
        chunk_.assign(static_cast<size_t>(block_size_per_channel_) * channels_, 0);
    } else {
        chunk_.clear();
    }
    return result;
}

DsdResult DsdFileReader::openDsf() {
    int64_t dsd_chunk_size = 0;
    if (!readLongLe(input_, &dsd_chunk_size)) {
        return invalid("Truncated DSF header.");
    }
    if (dsd_chunk_size < 28) {
        return invalid("Invalid DSF 'DSD ' chunk size: " + std::to_string(dsd_chunk_size));
    }
    if (!seekAbsolute(input_, dsd_chunk_size)) {
        return invalid("Truncated DSF header.");
    }

    char fmt_magic[5];
    if (!readMagic(input_, fmt_magic) || std::strcmp(fmt_magic, "fmt ") != 0) {
        return invalid("DSF 'fmt ' chunk is missing.");
    }
    int64_t fmt_chunk_size = 0;
    int32_t format_version = 0;
    int32_t format_id = 0;
    int32_t channel_type = 0;
    int32_t channels = 0;
    int32_t sample_rate = 0;
    int32_t bits_per_sample = 0;
    int64_t sample_count = 0;
    int32_t block_size = 0;
    if (!readLongLe(input_, &fmt_chunk_size) ||
        !readIntLe(input_, &format_version) ||
        !readIntLe(input_, &format_id) ||
        !readIntLe(input_, &channel_type) ||
        !readIntLe(input_, &channels) ||
        !readIntLe(input_, &sample_rate) ||
        !readIntLe(input_, &bits_per_sample) ||
        !readLongLe(input_, &sample_count) ||
        !readIntLe(input_, &block_size)) {
        return invalid("Truncated DSF header.");
    }
    if (format_id != 0) {
        return unsupported("Unsupported DSF format id: " + std::to_string(format_id));
    }
    // 规范允许 blockSizePerChannel 不是 4096，按 header 实际值处理；
    // 上限防御异常头（Kotlin 侧等价于块缓冲分配失败抛错）
    if (channels < 1 || channels > 6 || sample_rate <= 0 ||
        block_size <= 0 || block_size > (1 << 24)) {
        return invalid(
            "Invalid DSF fmt: channels=" + std::to_string(channels) +
            ", sampleRate=" + std::to_string(sample_rate) +
            ", blockSize=" + std::to_string(block_size));
    }
    if (bits_per_sample != 1 && bits_per_sample != 8) {
        return unsupported("Unsupported DSF bitsPerSample: " + std::to_string(bits_per_sample));
    }

    if (!seekAbsolute(input_, dsd_chunk_size + fmt_chunk_size)) {
        return invalid("Truncated DSF header.");
    }
    char data_magic[5];
    if (!readMagic(input_, data_magic) || std::strcmp(data_magic, "data") != 0) {
        return invalid("DSF 'data' chunk is missing.");
    }
    int64_t data_chunk_size = 0;
    if (!readLongLe(input_, &data_chunk_size)) {
        return invalid("Truncated DSF header.");
    }
    const int64_t data_start = dsd_chunk_size + fmt_chunk_size + 12;
    const int64_t audio_bytes =
        data_chunk_size >= 12 ? data_chunk_size - 12 : length_at_open_ - data_start;
    if (!seekAbsolute(input_, data_start)) {
        return invalid("Truncated DSF header.");
    }

    format_name_ = "dsf";
    sample_rate_ = sample_rate;
    channels_ = channels;
    data_start_ = data_start;
    bytes_per_channel_ = std::min(sample_count / 8, audio_bytes / channels);
    block_size_per_channel_ = block_size;
    lsb_first_ = bits_per_sample == 1;
    return {};
}

DsdResult DsdFileReader::openDff(bool streaming) {
    int64_t form_size = 0;
    if (!readLongBe(input_, &form_size)) {
        return invalid("Truncated DFF header.");
    }
    char form_type[5];
    if (!readMagic(input_, form_type) || std::strcmp(form_type, "DSD ") != 0) {
        return unsupported("Unsupported DFF form type.");
    }

    int32_t sample_rate = 0;
    int32_t channels = 0;
    int64_t data_start = -1;
    int64_t data_size = 0;
    int64_t offset = 16;
    const int64_t form_end = std::min<int64_t>(12 + form_size, length_at_open_);
    char id[5];
    while (offset + 12 <= form_end) {
        if (!seekAbsolute(input_, offset) || !readMagic(input_, id)) {
            return invalid("Truncated DFF header.");
        }
        int64_t chunk_size = 0;
        if (!readLongBe(input_, &chunk_size)) {
            return invalid("Truncated DFF header.");
        }
        if (std::strcmp(id, "PROP") == 0) {
            char prop_type[5];
            if (!readMagic(input_, prop_type)) {
                return invalid("Truncated DFF header.");
            }
            if (std::strcmp(prop_type, "SND ") == 0) {
                int64_t prop_offset = offset + 16;
                const int64_t prop_end = std::min<int64_t>(offset + 12 + chunk_size, form_end);
                while (prop_offset + 12 <= prop_end) {
                    if (!seekAbsolute(input_, prop_offset) || !readMagic(input_, id)) {
                        return invalid("Truncated DFF header.");
                    }
                    int64_t sub_size = 0;
                    if (!readLongBe(input_, &sub_size)) {
                        return invalid("Truncated DFF header.");
                    }
                    if (std::strcmp(id, "FS  ") == 0) {
                        if (!readIntBe(input_, &sample_rate)) {
                            return invalid("Truncated DFF header.");
                        }
                    } else if (std::strcmp(id, "CHNL") == 0) {
                        if (!readShortBe(input_, &channels)) {
                            return invalid("Truncated DFF header.");
                        }
                    } else if (std::strcmp(id, "CMPR") == 0) {
                        char compression[5];
                        if (!readMagic(input_, compression)) {
                            return invalid("Truncated DFF header.");
                        }
                        if (std::strcmp(compression, "DST ") == 0) {
                            return unsupported("DST-compressed DFF is not supported.");
                        }
                    }
                    // IFF chunk 按偶数字节对齐
                    prop_offset += 12 + sub_size + (sub_size & 1);
                }
            }
        } else if (std::strcmp(id, "DSD ") == 0) {
            data_start = offset + 12;
            data_size = chunk_size;
        } else if (std::strcmp(id, "DST ") == 0) {
            return unsupported("DST-compressed DFF is not supported.");
        }
        offset += 12 + chunk_size + (chunk_size & 1);
    }

    if (sample_rate <= 0 || channels < 1 || channels > 6 || data_start < 0) {
        return invalid(
            "Invalid DFF: sampleRate=" + std::to_string(sample_rate) +
            ", channels=" + std::to_string(channels) +
            ", hasData=" + (data_start >= 0 ? "true" : "false"));
    }
    // 流式下载中文件长度还在增长，数据大小只信 chunk 头声明值
    const int64_t audio_bytes =
        streaming ? data_size : std::min(data_size, length_at_open_ - data_start);
    if (!seekAbsolute(input_, data_start)) {
        return invalid("Truncated DFF header.");
    }

    format_name_ = "dff";
    sample_rate_ = sample_rate;
    channels_ = channels;
    data_start_ = data_start;
    bytes_per_channel_ = audio_bytes / channels;
    block_size_per_channel_ = 0;
    lsb_first_ = false;
    return {};
}

int DsdFileReader::read(uint8_t* out, int capacity) {
    if (block_size_per_channel_ > 0) {
        return readDsf(out, capacity);
    }
    const int64_t remaining = (bytes_per_channel_ - position_bytes_per_channel_) * channels_;
    if (remaining <= 0) {
        return -1;
    }
    const int wanted = static_cast<int>(
        std::min<int64_t>(capacity / channels_ * channels_, remaining));
    if (wanted <= 0) {
        return -1;
    }
    clearerr(input_);
    int read_total = 0;
    while (read_total < wanted) {
        const size_t count = fread(out + read_total, 1, wanted - read_total, input_);
        if (count == 0) {
            break;
        }
        read_total += static_cast<int>(count);
    }
    // 文件比头部声明的短：把不完整的尾部对齐丢弃
    const int delivered = read_total / channels_ * channels_;
    if (delivered <= 0) {
        position_bytes_per_channel_ = bytes_per_channel_;
        return -1;
    }
    position_bytes_per_channel_ += delivered / channels_;
    return delivered;
}

int DsdFileReader::readDsf(uint8_t* out, int capacity) {
    if (chunk_offset_ >= chunk_length_ && !loadNextDsfBlock()) {
        return -1;
    }
    const int count = std::min(capacity / channels_ * channels_, chunk_length_ - chunk_offset_);
    if (count <= 0) {
        return -1;
    }
    std::memcpy(out, chunk_.data() + chunk_offset_, count);
    chunk_offset_ += count;
    position_bytes_per_channel_ += count / channels_;
    return count;
}

bool DsdFileReader::loadNextDsfBlock() {
    const int valid = static_cast<int>(std::min<int64_t>(
        bytes_per_channel_ - loaded_bytes_per_channel_, block_size_per_channel_));
    if (valid <= 0) {
        return false;
    }
    const int64_t group_index = loaded_bytes_per_channel_ / block_size_per_channel_;
    const int64_t group_bytes = static_cast<int64_t>(block_size_per_channel_) * channels_;
    if (!seekAbsolute(input_, data_start_ + group_index * group_bytes)) {
        loaded_bytes_per_channel_ = bytes_per_channel_;
        return false;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(block_size_per_channel_) * channels_);
    clearerr(input_);
    int read_total = 0;
    while (read_total < static_cast<int>(raw.size())) {
        const size_t count = fread(raw.data() + read_total, 1, raw.size() - read_total, input_);
        if (count == 0) {
            break;
        }
        read_total += static_cast<int>(count);
    }
    // planar 块转逐字节交错；LSB-first 时同步做位反转
    const int usable = std::min(valid, read_total / channels_);
    if (usable <= 0) {
        loaded_bytes_per_channel_ = bytes_per_channel_;
        return false;
    }
    const uint8_t* reverse = bitReverseTable();
    for (int index = 0; index < usable; ++index) {
        for (int channel = 0; channel < channels_; ++channel) {
            const uint8_t byte = raw[channel * block_size_per_channel_ + index];
            chunk_[index * channels_ + channel] = lsb_first_ ? reverse[byte] : byte;
        }
    }
    chunk_length_ = usable * channels_;
    chunk_offset_ = 0;
    loaded_bytes_per_channel_ += usable;
    return true;
}

bool DsdFileReader::canReadAt(int64_t file_length) const {
    if (position_bytes_per_channel_ >= bytes_per_channel_) {
        return true;
    }
    if (block_size_per_channel_ > 0) {
        if (chunk_offset_ < chunk_length_) {
            return true;
        }
        const int64_t valid = std::min<int64_t>(
            bytes_per_channel_ - loaded_bytes_per_channel_, block_size_per_channel_);
        if (valid <= 0) {
            return true;
        }
        const int64_t group_start = data_start_ +
            loaded_bytes_per_channel_ / block_size_per_channel_ *
                block_size_per_channel_ * channels_;
        // planar 块组内最后一个声道的段也要够 valid 字节
        return file_length >=
            group_start + static_cast<int64_t>(channels_ - 1) * block_size_per_channel_ + valid;
    }
    return file_length >= data_start_ + (position_bytes_per_channel_ + 1) * channels_;
}

int64_t DsdFileReader::seekTo(int64_t position_ms) {
    const int64_t target = std::min(
        std::max<int64_t>(position_ms, 0) * sample_rate_ / 8000, bytes_per_channel_);
    const int64_t aligned = block_size_per_channel_ > 0
        ? target / block_size_per_channel_ * block_size_per_channel_
        : target / 2 * 2;
    if (block_size_per_channel_ > 0) {
        loaded_bytes_per_channel_ = aligned;
        chunk_length_ = 0;
        chunk_offset_ = 0;
    } else {
        seekAbsolute(input_, data_start_ + aligned * channels_);
    }
    position_bytes_per_channel_ = aligned;
    return aligned * 8000 / sample_rate_;
}

DopPacketizer::DopPacketizer(int channels)
    : channels_(channels),
      frame_bytes_(2 * channels),
      carry_(static_cast<size_t>(2 * channels), 0) {}

uint8_t DopPacketizer::byteAt(const uint8_t* data, int index) const {
    // 逻辑上 carry 与 data 是拼接的一段流，按拼接后的下标取字节
    return index < carry_length_ ? carry_[index] : data[index - carry_length_];
}

void DopPacketizer::encode(const uint8_t* data, int length, std::vector<uint8_t>& out) {
    const int total = carry_length_ + length;
    const int frames = total / frame_bytes_;
    out.assign(static_cast<size_t>(frames) * channels_ * 3, 0);
    int output_offset = 0;
    int consumed = 0;
    for (int frame = 0; frame < frames; ++frame) {
        // 帧内逐声道装两个字节：时间靠前的放 bits 15-8
        for (int channel = 0; channel < channels_; ++channel) {
            const uint8_t early = byteAt(data, consumed + channel);
            const uint8_t late = byteAt(data, consumed + channels_ + channel);
            out[output_offset + channel * 3] = late;
            out[output_offset + channel * 3 + 1] = early;
            out[output_offset + channel * 3 + 2] = static_cast<uint8_t>(marker_);
        }
        consumed += frame_bytes_;
        output_offset += channels_ * 3;
        marker_ = marker_ == 0x05 ? 0xFA : 0x05;
    }

    // 更新余量：把没吃完的尾巴挪到 carry 开头（先取出再写回，避免读写重叠）
    const int leftover = total - frames * frame_bytes_;
    if (leftover > 0) {
        std::vector<uint8_t> tail(leftover);
        for (int index = 0; index < leftover; ++index) {
            tail[index] = byteAt(data, frames * frame_bytes_ + index);
        }
        std::memcpy(carry_.data(), tail.data(), leftover);
    }
    carry_length_ = leftover;
}

void DopPacketizer::encodeSilence(int frames, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(frames) * channels_ * 3, 0);
    int output_offset = 0;
    for (int frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels_; ++channel) {
            out[output_offset + channel * 3] = kDsdSilenceByte;
            out[output_offset + channel * 3 + 1] = kDsdSilenceByte;
            out[output_offset + channel * 3 + 2] = static_cast<uint8_t>(marker_);
        }
        output_offset += channels_ * 3;
        marker_ = marker_ == 0x05 ? 0xFA : 0x05;
    }
}

void DopPacketizer::drain(std::vector<uint8_t>& out) {
    if (carry_length_ == 0) {
        out.clear();
        return;
    }
    const std::vector<uint8_t> padding(frame_bytes_ - carry_length_, kDsdSilenceByte);
    encode(padding.data(), static_cast<int>(padding.size()), out);
}

void DopPacketizer::reset() {
    marker_ = 0x05;
    carry_length_ = 0;
}

int nativeDsdBytesPerSample(const std::string& format) {
    if (format == "u8") {
        return 1;
    }
    if (format == "u16le") {
        return 2;
    }
    if (format == "u32le" || format == "u32be") {
        return 4;
    }
    return 0;
}

NativeDsdPacketizer::NativeDsdPacketizer(int channels, int bytes_per_sample, bool big_endian)
    : channels_(channels),
      bytes_per_sample_(bytes_per_sample),
      big_endian_(big_endian),
      frame_bytes_(bytes_per_sample * channels),
      carry_(static_cast<size_t>(bytes_per_sample) * channels, 0) {}

uint8_t NativeDsdPacketizer::byteAt(const uint8_t* data, int index) const {
    return index < carry_length_ ? carry_[index] : data[index - carry_length_];
}

void NativeDsdPacketizer::encode(const uint8_t* data, int length, std::vector<uint8_t>& out) {
    const int total = carry_length_ + length;
    const int frames = total / frame_bytes_;
    out.assign(static_cast<size_t>(frames) * frame_bytes_, 0);
    int output_offset = 0;
    int consumed = 0;
    for (int frame = 0; frame < frames; ++frame) {
        // 交错流 L0 R0 L1 R1… → 每声道连续 bytesPerSample 字节（LE 时间正序 / BE 倒序）
        for (int channel = 0; channel < channels_; ++channel) {
            for (int sample_index = 0; sample_index < bytes_per_sample_; ++sample_index) {
                const int slot_index =
                    big_endian_ ? bytes_per_sample_ - 1 - sample_index : sample_index;
                out[output_offset + channel * bytes_per_sample_ + slot_index] =
                    byteAt(data, consumed + sample_index * channels_ + channel);
            }
        }
        consumed += frame_bytes_;
        output_offset += frame_bytes_;
    }

    const int leftover = total - frames * frame_bytes_;
    if (leftover > 0) {
        std::vector<uint8_t> tail(leftover);
        for (int index = 0; index < leftover; ++index) {
            tail[index] = byteAt(data, frames * frame_bytes_ + index);
        }
        std::memcpy(carry_.data(), tail.data(), leftover);
    }
    carry_length_ = leftover;
}

void NativeDsdPacketizer::encodeSilence(int frames, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(frames) * frame_bytes_, kDsdSilenceByte);
}

void NativeDsdPacketizer::drain(std::vector<uint8_t>& out) {
    if (carry_length_ == 0) {
        out.clear();
        return;
    }
    const std::vector<uint8_t> padding(frame_bytes_ - carry_length_, kDsdSilenceByte);
    encode(padding.data(), static_cast<int>(padding.size()), out);
}

}  // namespace sylvakru
