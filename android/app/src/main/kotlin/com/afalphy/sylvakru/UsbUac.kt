package com.afalphy.sylvakru

/**
 * usb_uac.cpp 的 JNI 入口：UAC 原始配置描述符的四个无状态解析函数。
 * 返回扁平 IntArray（槽位布局见 usb_uac_jni.cpp 顶部注释），结构化解码
 * 留在引擎调用处完成；对拍测试见 cpp/tests/usb_uac_test.cpp。
 */
internal object UsbUacNative {
    init {
        System.loadLibrary("sylvakru_usb_exclusive")
    }

    external fun parseStreamingFormats(descriptors: ByteArray): IntArray

    external fun findClockSource(
        descriptors: ByteArray,
        streamingInterfaceNumber: Int,
        streamingAlternateSetting: Int,
    ): IntArray

    external fun parseVolumeFeatures(descriptors: ByteArray): IntArray

    external fun parseOutputTerminalSources(descriptors: ByteArray): IntArray
}
