package com.afalphy.sylvakru

import java.io.Closeable
import java.nio.ByteBuffer

/**
 * 完整本地文件的原生 PCM 解码器公共约定（libFLAC/libwavpack 实现）：
 * 输出交错 S32LE 容器、低位对齐、保留源文件真实有效位深，
 * 供 USB 独占解码线程单线程使用。
 */
interface UsbNativePcmDecoder : Closeable {
    val sampleRate: Int
    val channels: Int
    val validBitsPerSample: Int
    val totalFrames: Long

    val durationMs: Long
        get() = if (sampleRate > 0) totalFrames * 1000L / sampleRate else 0L

    /**
     * 解码最多 [capacityFrames] 帧到 [buffer]（必须是容量足够的 direct buffer，
     * 每帧 channels × 4 字节）。返回写入帧数；流已结束且无数据可读时返回 -1；
     * 解码失败抛 [java.io.IOException]，调用方不得静默回退从头播放。
     */
    fun readFrames(buffer: ByteBuffer, capacityFrames: Int): Int

    /** 定位到绝对帧位置；失败抛 [java.io.IOException]，解码器状态保持可继续使用。 */
    fun seekToFrame(frame: Long)

    companion object {
        /** 交错输出容器固定为 S32LE，每样本 4 字节。 */
        const val BYTES_PER_SAMPLE = Int.SIZE_BYTES
    }
}
