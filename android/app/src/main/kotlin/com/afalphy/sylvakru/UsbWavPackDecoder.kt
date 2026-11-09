package com.afalphy.sylvakru

import java.io.IOException
import java.nio.ByteBuffer

/** libwavpack 原生解码器的 JNI 入口；句柄由 [UsbWavPackDecoder] 独占持有。 */
internal object UsbWavPackNative {
    init {
        System.loadLibrary("sylvakru_usb_exclusive")
    }

    external fun create(): Long

    external fun open(handle: Long, path: String): String?

    external fun streamInfo(handle: Long): LongArray

    external fun readFrames(handle: Long, buffer: ByteBuffer, capacityFrames: Int): Int

    external fun endOfStream(handle: Long): Boolean

    external fun lastError(handle: Long): String?

    external fun seekToFrame(handle: Long, frame: Long): String?

    external fun destroy(handle: Long)
}

/**
 * 完整本地 WavPack (.wv) 文件的原生解码器：系统 MediaCodec 没有 WavPack
 * 解码器，用 libwavpack 输出交错 S32LE 容器并保留源文件真实有效位深，
 * 让 .wv 也能走 USB 独占直驱。浮点与 DSD 封装的 .wv 不支持（打开报错，
 * 调用方回退共享输出）；旁有 .wvc 校正文件的 hybrid 文件自动无损还原。
 *
 * 仅供 USB 独占解码线程单线程使用。open 失败抛 [IOException]；
 * 读到流结尾后 [readFrames] 返回 -1；close 可重复调用。
 */
class UsbWavPackDecoder private constructor(
    private var handle: Long,
    override val sampleRate: Int,
    override val channels: Int,
    override val validBitsPerSample: Int,
    override val totalFrames: Long,
) : UsbNativePcmDecoder {
    override fun readFrames(buffer: ByteBuffer, capacityFrames: Int): Int {
        check(handle != 0L) { "UsbWavPackDecoder 已关闭" }
        val frames = UsbWavPackNative.readFrames(handle, buffer, capacityFrames)
        if (frames < 0) {
            throw IOException(UsbWavPackNative.lastError(handle) ?: "WavPack 解码失败")
        }
        if (frames == 0 && UsbWavPackNative.endOfStream(handle)) {
            return -1
        }
        return frames
    }

    override fun seekToFrame(frame: Long) {
        check(handle != 0L) { "UsbWavPackDecoder 已关闭" }
        val error = UsbWavPackNative.seekToFrame(handle, frame)
        if (error != null) {
            throw IOException(error)
        }
    }

    override fun close() {
        if (handle != 0L) {
            UsbWavPackNative.destroy(handle)
            handle = 0L
        }
    }

    companion object {
        /** 打开完整本地 WavPack 文件；文件缺失、非 WavPack 或格式不支持时抛 [IOException]。 */
        fun open(path: String): UsbWavPackDecoder {
            val handle = UsbWavPackNative.create()
            val error = UsbWavPackNative.open(handle, path)
            if (error != null) {
                UsbWavPackNative.destroy(handle)
                throw IOException(error)
            }
            val info = UsbWavPackNative.streamInfo(handle)
            val decoder = UsbWavPackDecoder(
                handle = handle,
                sampleRate = info[0].toInt(),
                channels = info[1].toInt(),
                validBitsPerSample = info[2].toInt(),
                totalFrames = info[3],
            )
            if (decoder.sampleRate <= 0 || decoder.channels <= 0) {
                decoder.close()
                throw IOException("WavPack 流信息无效")
            }
            return decoder
        }

        /** 探测文件能否被 libwavpack 解成独占可用的 PCM（用于 start 前的支持性判定）。 */
        fun canDecode(path: String): Boolean {
            return try {
                open(path).close()
                true
            } catch (_: IOException) {
                false
            }
        }
    }
}
