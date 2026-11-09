package com.afalphy.sylvakru

import java.io.Closeable
import java.io.File
import java.io.IOException

// DSD 静音字节是 0x69（01101001，均值为半幅的交替位型），不是 0x00。
// 暂停/垫包必须发 0x69，发零会让 DAC 掉出 DSD 模式并可能爆音。
const val DSD_SILENCE_BYTE = 0x69

/**
 * usb_dsd.cpp 的 JNI 入口；句柄由各包装类独占持有。
 * 核心逻辑（容器解析/位序/DoP 打包/Native 重排）已下沉平台无关 C++，
 * 对拍测试见 cpp/tests/usb_dsd_test.cpp。
 */
internal object UsbDsdNative {
    init {
        System.loadLibrary("sylvakru_usb_exclusive")
    }

    external fun readerCreate(): Long

    external fun readerOpen(handle: Long, path: String, streaming: Boolean): String?

    external fun readerInfo(handle: Long): LongArray

    external fun readerFormatName(handle: Long): String

    external fun readerRead(handle: Long, out: ByteArray): Int

    external fun readerCanReadAt(handle: Long, fileLength: Long): Boolean

    external fun readerSeekTo(handle: Long, positionMs: Long): Long

    external fun readerPositionMs(handle: Long): Long

    external fun readerDestroy(handle: Long)

    external fun dopCreate(channels: Int): Long

    external fun nativeDsdCreate(channels: Int, bytesPerSample: Int, bigEndian: Boolean): Long

    external fun encoderEncode(handle: Long, data: ByteArray, length: Int): ByteArray

    external fun encoderEncodeSilence(handle: Long, frames: Int): ByteArray

    external fun encoderDrain(handle: Long): ByteArray

    external fun dopReset(handle: Long)

    external fun encoderDestroy(handle: Long)

    external fun nativeDsdBytesPerSample(format: String): Int
}

/**
 * 统一读取 DSF/DFF 文件，屏蔽两种容器差异，输出统一约定的 DSD 字节流：
 * MSB-first、逐字节声道交错（L R L R…）。解析与交错实现在 usb_dsd.cpp，
 * 本类只是 JNI 薄包装，仅供 USB 独占解码线程单线程使用。
 *
 * - DSF（Sony，小端）：采样按每通道 blockSizePerChannel 字节的块 planar 存放，读取时转交错；
 *   bitsPerSample=1 表示每字节 LSB-first，需查表位反转；=8 表示 MSB-first，直接透传。
 * - DFF（Philips，大端 IFF）：数据本身就是 MSB-first 逐字节交错，直接透传；
 *   DST 压缩的 DFF 不支持，open 时抛错。
 */
class DsdFileReader private constructor(
    private var handle: Long,
    val formatName: String,
    val sampleRate: Int,
    val channels: Int,
    val durationMs: Long,
) : Closeable {
    /** 当前播放位置（已交付字节换算），毫秒。 */
    val positionMs: Long
        get() {
            check(handle != 0L) { "DsdFileReader 已关闭" }
            return UsbDsdNative.readerPositionMs(handle)
        }

    /** DSD 倍率（64/128/256/512），速率不在 44.1k 族时为 null。 */
    val dsdMultiple: Int? get() = if (sampleRate % 44100 == 0) sampleRate / 44100 else null

    /** DoP 输出的 PCM 帧率 = DSD 速率 ÷ 16（每帧每声道装 2 个 DSD 字节）。 */
    val dopFrameRate: Int get() = sampleRate / 16

    /**
     * 读取交错 DSD 字节流。返回写入 [out] 的字节数（总是 channels 的整数倍），文件结束返回 -1。
     * 一次调用最多交付一个内部块的余量，调用方循环读取即可。
     */
    fun read(out: ByteArray): Int {
        check(handle != 0L) { "DsdFileReader 已关闭" }
        return UsbDsdNative.readerRead(handle, out)
    }

    /**
     * 流式下载中调用：按当前已下载的文件长度 [fileLength] 判断下一次 [read]
     * 能否交付数据而不把"数据还没下到"误判成文件结尾。
     * DSF 要求下一个块组所需字节齐全（避免半块重复交付），DFF 只要求至少一帧。
     * 已到真实结尾时返回 true，让 [read] 正常返回 -1。
     */
    fun canReadAt(fileLength: Long): Boolean {
        check(handle != 0L) { "DsdFileReader 已关闭" }
        return UsbDsdNative.readerCanReadAt(handle, fileLength)
    }

    /**
     * 定位到 [positionMs] 附近：DSF 对齐到块边界，DFF 对齐到 DoP 双字节边界。
     * 返回对齐后的实际位置（毫秒），用于进度上报。
     */
    fun seekTo(positionMs: Long): Long {
        check(handle != 0L) { "DsdFileReader 已关闭" }
        return UsbDsdNative.readerSeekTo(handle, positionMs)
    }

    override fun close() {
        if (handle != 0L) {
            UsbDsdNative.readerDestroy(handle)
            handle = 0L
        }
    }

    companion object {
        /**
         * [streaming] 为 true 表示文件还在下载增长中：DFF 的数据大小按 chunk 头
         * 声明值取（此刻文件长度不足以作截断依据），可读进度由调用方配合
         * [canReadAt] 控制。文件缺失、非 DSF/DFF 或格式不支持时抛 [IOException]。
         */
        fun open(file: File, streaming: Boolean = false): DsdFileReader {
            val handle = UsbDsdNative.readerCreate()
            val error = UsbDsdNative.readerOpen(handle, file.path, streaming)
            if (error != null) {
                UsbDsdNative.readerDestroy(handle)
                throw IOException(error)
            }
            val info = UsbDsdNative.readerInfo(handle)
            return DsdFileReader(
                handle = handle,
                formatName = UsbDsdNative.readerFormatName(handle),
                sampleRate = info[0].toInt(),
                channels = info[1].toInt(),
                durationMs = info[2],
            )
        }
    }
}

/**
 * DSD 字节流编码器的统一口径（DoP / Native DSD 共用）：输入都是 DsdFileReader 的
 * MSB-first 逐字节交错流，输出都交给 PcmIsoPacketizer 当普通 PCM 帧打包。
 * 会话级静音填充线程与写线程只依赖这三个方法，两种模式行为一致：
 * 静音一律 0x69，流一旦中断 DAC 就会掉出 DSD 模式（爆音/电流声）。
 * 实现持有 native 句柄，会话结束时必须 close。
 */
interface DsdStreamEncoder : Closeable {
    /** 编码 [data] 的前 [length] 字节，不足一帧的余量留到下次。 */
    fun encode(data: ByteArray, length: Int = data.size): ByteArray

    /** 生成 [frames] 帧 DSD 静音（0x69）。 */
    fun encodeSilence(frames: Int): ByteArray

    /** 把不足一帧的余量用 0x69 补齐成完整帧输出（无余量时返回空数组）。 */
    fun drain(): ByteArray
}

/**
 * 把 DsdFileReader 输出的交错 DSD 字节流封装成 DoP 24-bit PCM 采样流（小端，每采样 3 字节）。
 * 每帧每声道取 2 个连续 DSD 字节：sample24 = (marker shl 16) or (先到字节 shl 8) or 后到字节；
 * 标记逐帧在 0x05/0xFA 间交替，同一帧内各声道用相同标记。
 * 输出交给 PcmIsoPacketizer 当作普通 24-bit PCM（sampleRate = DSD 速率 ÷ 16），
 * 24→32 slot 的高位对齐移位恰好得到规范要求的"DoP 24 位放高位、低 8 位补零"。
 * 注意：DoP 数据被任何后续 DSP（音量、抖动、重采样）修改都会破坏标记、输出全幅噪声。
 */
class DopPacketizer(channels: Int) : DsdStreamEncoder {
    private var handle = UsbDsdNative.dopCreate(channels)

    override fun encode(data: ByteArray, length: Int): ByteArray {
        check(handle != 0L) { "DopPacketizer 已关闭" }
        return UsbDsdNative.encoderEncode(handle, data, length)
    }

    /** DoP 静音：payload 0x69 0x69，标记正常交替。 */
    override fun encodeSilence(frames: Int): ByteArray {
        check(handle != 0L) { "DopPacketizer 已关闭" }
        return UsbDsdNative.encoderEncodeSilence(handle, frames)
    }

    override fun drain(): ByteArray {
        check(handle != 0L) { "DopPacketizer 已关闭" }
        return UsbDsdNative.encoderDrain(handle)
    }

    /** seek 后重置：标记回到起始相位，丢弃余量。 */
    fun reset() {
        check(handle != 0L) { "DopPacketizer 已关闭" }
        UsbDsdNative.dopReset(handle)
    }

    override fun close() {
        if (handle != 0L) {
            UsbDsdNative.encoderDestroy(handle)
            handle = 0L
        }
    }
}

/** quirk `nativeDsd.format` → 每声道每帧字节数；未知格式返回 null。 */
fun nativeDsdBytesPerSample(format: String?): Int? =
    format?.let { UsbDsdNative.nativeDsdBytesPerSample(it) }?.takeIf { it > 0 }

/**
 * Native DSD 打包：把交错 DSD 字节流按设备期望的 subslot 排列重组，直接当
 * "bitDepth = subslot 位宽的 PCM" 交给 PcmIsoPacketizer（帧率 = DSD 速率 ÷ 8 ÷ 每采样字节数），
 * 无 DoP 标记、无任何数据修改，纯字节重排（对应 ALSA DSD_U8/U16_LE/U32_LE/U32_BE 语义）：
 * - LE：subslot 内存序 = 时间序（最早的字节在前）；
 * - BE：LSB（最早字节）在 word 高地址，subslot 组内字节倒序。
 * 静音同样是 0x69（重排后仍是 0x69），流中断一样会让 DAC 掉出 DSD 模式。
 */
class NativeDsdPacketizer(
    channels: Int,
    bytesPerSample: Int,
    bigEndian: Boolean,
) : DsdStreamEncoder {
    private var handle = UsbDsdNative.nativeDsdCreate(channels, bytesPerSample, bigEndian)

    override fun encode(data: ByteArray, length: Int): ByteArray {
        check(handle != 0L) { "NativeDsdPacketizer 已关闭" }
        return UsbDsdNative.encoderEncode(handle, data, length)
    }

    override fun encodeSilence(frames: Int): ByteArray {
        check(handle != 0L) { "NativeDsdPacketizer 已关闭" }
        return UsbDsdNative.encoderEncodeSilence(handle, frames)
    }

    override fun drain(): ByteArray {
        check(handle != 0L) { "NativeDsdPacketizer 已关闭" }
        return UsbDsdNative.encoderDrain(handle)
    }

    override fun close() {
        if (handle != 0L) {
            UsbDsdNative.encoderDestroy(handle)
            handle = 0L
        }
    }
}
