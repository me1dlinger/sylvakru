package com.afalphy.sylvakru

// 选择逻辑下沉 usb_dac_quirks.cpp：-1 表示"未指定/无结果"（对应 null）；
// 请求速率必须恰好命中（不回退到其它位完美速率），未指定时取最大支持速率。
internal fun chooseBitPerfectMixerSampleRate(
    requestedSampleRate: Int?,
    supportedSampleRates: List<Int>,
): Int? = UsbDacQuirksNative
    .chooseBitPerfectMixerSampleRate(requestedSampleRate ?: -1, supportedSampleRates.toIntArray())
    .takeIf { it > 0 }
