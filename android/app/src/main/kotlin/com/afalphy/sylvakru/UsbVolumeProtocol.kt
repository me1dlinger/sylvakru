package com.afalphy.sylvakru

/**
 * usb_volume_protocol.cpp 的 JNI 入口：iBasso HID 音量协议纯逻辑
 * （增益换算/音量表映射/HID 报文构造与解析/写后验证与 reader 恢复决策）。
 * 全部为无状态函数；可空 Int 以 hasX + value 成对参数传递，枚举按序号传递
 * （两侧枚举顺序不得重排）。对拍测试见 cpp/tests/usb_volume_protocol_test.cpp。
 *
 * ⚠️ 音量安全红线：数值行为的唯一实现在 native 侧，Kotlin 只留会话策略胶水；
 * 任何数值改动必须先过 native 对拍测试。
 */
internal object UsbVolumeNative {
    init {
        System.loadLibrary("sylvakru_usb_exclusive")
    }

    external fun preferredAutoPcmBitDepth(
        hasSourceBitDepth: Boolean,
        sourceBitDepth: Int,
        availableBitDepths: IntArray,
    ): Int

    external fun effectiveVolumeGainQ16(userGainQ16: Int, replayGainMilliDb: Int): Int

    external fun effectiveHardwareVolumeGainQ16(
        userGainQ16: Int,
        replayGainMilliDb: Int,
        dsdCompensationDb: Int,
        isDsd: Boolean,
    ): Int

    external fun ibassoVolumeIndex(gainQ16: Int): Int

    external fun ibassoDeviceVolume(index: Int): Int

    external fun ibassoDsdVolume(baseVolume: Int, compensationDb: Int): Int

    external fun ibassoAppGainToRaw(
        gainQ16: Int,
        replayGainMilliDb: Int,
        dsdCompensationDb: Int,
    ): IntArray

    external fun ibassoRawToLinearGainQ16(raw: Int): Int

    external fun ibassoDecodeEvent(packet: ByteArray): IntArray

    external fun ibassoRoutePacket(packet: ByteArray, pendingCommands: IntArray): IntArray

    external fun ibassoI2cWritePacket(
        command: Int,
        slave: Int,
        offset: Int,
        byteOffset: Int,
        value: Int,
    ): ByteArray

    external fun ibassoRoomWritePacket(command: Int, register: Int, value: Int): ByteArray

    external fun ibassoVolumeReadPacket(): ByteArray

    external fun ibassoVolumePackets(baseRaw: Int, dsdRaw: Int): ByteArray

    external fun ibassoVolumeVerificationAction(
        targetRaw: Int,
        hasPreviousRaw: Boolean,
        previousRaw: Int,
        hasReadbackRaw: Boolean,
        readbackRaw: Int,
        failureCount: Int,
        isDsd: Boolean,
        hasPendingRequest: Boolean,
        hasTargetDsdRaw: Boolean,
        targetDsdRaw: Int,
        hasPreviousDsdRaw: Boolean,
        previousDsdRaw: Int,
    ): Int

    external fun ibassoReaderRecoveryAction(
        isDsd: Boolean,
        healthWriteOnly: Boolean,
        healthRestartRequested: Boolean,
        readerRunning: Boolean,
        generationMatches: Boolean,
        waitExpired: Boolean,
    ): Int

    external fun ibassoVolumePendingDelayMs(
        lastCompletedAtMs: Long,
        hasPendingUpdatedAt: Boolean,
        pendingUpdatedAtMs: Long,
        nowMs: Long,
    ): Long

    external fun ibassoActualEventGainQ16(
        baseRaw: Int,
        isDsd: Boolean,
        dsdCompensationDb: Int,
    ): IntArray

    external fun ibassoTargetFromEvent(baseRaw: Int, dsdCompensationDb: Int): IntArray
}

internal fun preferredAutoPcmBitDepth(
    sourceBitDepth: Int?,
    availableBitDepths: List<Int>,
): Int? = UsbVolumeNative.preferredAutoPcmBitDepth(
    hasSourceBitDepth = sourceBitDepth != null,
    sourceBitDepth = sourceBitDepth ?: 0,
    availableBitDepths = availableBitDepths.toIntArray(),
).takeIf { it > 0 }

internal data class UsbVolumeCapabilities(
    val readable: Boolean,
    val unsolicitedEvents: Boolean,
    val dsdGain: Boolean,
)

internal data class UsbVolumeEvent(
    val leftRaw: Int,
    val rightRaw: Int,
)

internal data class UsbVolumeTarget(
    val baseRaw: Int,
    val dsdRaw: Int,
)

internal data class UsbVolumeRequest(
    val gainQ16: Int,
    val replayGainMilliDb: Int,
    val mode: String,
    val dsdCompensationDb: Int,
    val smoothHandoff: Boolean,
    val sessionGeneration: Long,
)

// 顺序与 native 侧 IbassoVolumeVerificationAction 一致（JNI 按序号传递，不得重排）
internal enum class IbassoVolumeVerificationAction {
    ACCEPT_TARGET,
    KEEP_PREVIOUS,
    RETRY_READBACK,
    YIELD_TO_PENDING,
    FREEZE_PCM,
    FREEZE_DSD,
    PAUSE_DSD,
}

// 顺序与 native 侧 IbassoReaderRecoveryAction 一致（JNI 按序号传递，不得重排）
internal enum class IbassoReaderRecoveryAction {
    VERIFY_NOW,
    WAIT,
    FREEZE_PCM,
    CANCEL,
}

internal fun ibassoReaderRecoveryAction(
    isDsd: Boolean,
    health: IbassoReaderHealth,
    readerRunning: Boolean,
    generationMatches: Boolean,
    waitExpired: Boolean,
): IbassoReaderRecoveryAction = IbassoReaderRecoveryAction.values()[
    UsbVolumeNative.ibassoReaderRecoveryAction(
        isDsd = isDsd,
        healthWriteOnly = health.writeOnly,
        healthRestartRequested = health.restartRequested,
        readerRunning = readerRunning,
        generationMatches = generationMatches,
        waitExpired = waitExpired,
    ),
]

internal fun coalescedUsbVolumeRequest(
    running: UsbVolumeRequest,
    pending: UsbVolumeRequest?,
    incoming: UsbVolumeRequest,
    isDsd: Boolean,
): UsbVolumeRequest = incoming

internal fun usbVolumeProtocolForRequest(
    mode: String,
    configuredProtocol: String?,
    hardwareVolumeEnabled: Boolean,
    streamSupported: Boolean,
): String? = configuredProtocol.takeIf {
    (mode == "auto" || mode == "dac") && hardwareVolumeEnabled && streamSupported
}

internal fun usbVolumePendingDelayMs(
    protocol: String?,
    lastCompletedAtMs: Long?,
    pendingUpdatedAtMs: Long?,
    nowMs: Long,
): Long {
    if (protocol != IbassoHidVolumeProtocol.id || lastCompletedAtMs == null) return 0L
    // 150ms 事务落定 / 300ms 挂起静默窗口的取大逻辑在 native 侧
    return UsbVolumeNative.ibassoVolumePendingDelayMs(
        lastCompletedAtMs = lastCompletedAtMs,
        hasPendingUpdatedAt = pendingUpdatedAtMs != null,
        pendingUpdatedAtMs = pendingUpdatedAtMs ?: 0L,
        nowMs = nowMs,
    )
}

internal fun ibassoVolumeVerificationAction(
    targetRaw: Int,
    previousRaw: Int?,
    readbackRaw: Int?,
    failureCount: Int,
    isDsd: Boolean,
    hasPendingRequest: Boolean = false,
    targetDsdRaw: Int? = null,
    previousDsdRaw: Int? = null,
): IbassoVolumeVerificationAction = IbassoVolumeVerificationAction.values()[
    UsbVolumeNative.ibassoVolumeVerificationAction(
        targetRaw = targetRaw,
        hasPreviousRaw = previousRaw != null,
        previousRaw = previousRaw ?: 0,
        hasReadbackRaw = readbackRaw != null,
        readbackRaw = readbackRaw ?: 0,
        failureCount = failureCount,
        isDsd = isDsd,
        hasPendingRequest = hasPendingRequest,
        hasTargetDsdRaw = targetDsdRaw != null,
        targetDsdRaw = targetDsdRaw ?: 0,
        hasPreviousDsdRaw = previousDsdRaw != null,
        previousDsdRaw = previousDsdRaw ?: 0,
    ),
]

internal enum class HardwareVolumeHandoffSource { DEVICE, APP }

internal data class HardwareVolumeHandoffTarget(
    val gainQ16: Int,
    val source: HardwareVolumeHandoffSource,
)

internal data class UsbActualVolume(
    val raw: Int,
    val gainQ16: Int,
)

internal data class HardwareVolumeWriteResult(
    val error: String? = null,
    val actual: UsbActualVolume? = null,
)

internal fun actualHardwareVolume(
    valuesQ8_8: List<Int>,
    muteQ8_8: Int,
): UsbActualVolume? = valuesQ8_8
    .map { raw -> UsbActualVolume(raw, hardwareVolumeGainQ16(raw, muteQ8_8)) }
    .minWithOrNull(compareBy<UsbActualVolume> { it.gainQ16 }.thenBy { it.raw })

internal sealed interface UsbVolumeProtocolSelection

internal data object StandardUsbVolumeProtocol : UsbVolumeProtocolSelection

internal data class VendorUsbVolumeProtocol(
    val protocol: UsbVolumeProtocol,
) : UsbVolumeProtocolSelection

internal data class UnsupportedUsbVolumeProtocol(
    val id: String,
) : UsbVolumeProtocolSelection

internal sealed interface IbassoVolumePacketRoute {
    data class CommandResponse(
        val command: Int,
        val packet: ByteArray,
    ) : IbassoVolumePacketRoute

    data class Event(
        val event: UsbVolumeEvent,
        val isWriteConfirmation: Boolean,
    ) : IbassoVolumePacketRoute

    data object Unknown : IbassoVolumePacketRoute
}

internal data class IbassoReaderHealth(
    val failureCount: Int = 0,
    val pendingReadFailureCount: Int = 0,
    val restartRequested: Boolean = false,
    val writeOnly: Boolean = false,
    val readbackVerified: Boolean = false,
) {
    val readable: Boolean
        get() = !writeOnly

    fun afterFailure(): IbassoReaderHealth = if (failureCount == 0) {
        copy(
            failureCount = 1,
            pendingReadFailureCount = 0,
            restartRequested = true,
            writeOnly = false,
            readbackVerified = false,
        )
    } else {
        copy(
            failureCount = failureCount + 1,
            pendingReadFailureCount = 0,
            restartRequested = false,
            writeOnly = true,
            readbackVerified = false,
        )
    }

    fun afterReadResult(readLength: Int, hasPendingResponse: Boolean): IbassoReaderHealth =
        if (readLength > 0 || !hasPendingResponse) {
            copy(pendingReadFailureCount = 0)
        } else {
            copy(pendingReadFailureCount = pendingReadFailureCount + 1)
        }

    fun hasPersistentPendingFailure(limit: Int): Boolean =
        pendingReadFailureCount >= limit.coerceAtLeast(1)

    fun afterRestart(): IbassoReaderHealth = copy(
        pendingReadFailureCount = 0,
        restartRequested = false,
    )

    fun afterVerifiedReadback(): IbassoReaderHealth = copy(
        failureCount = 0,
        pendingReadFailureCount = 0,
        restartRequested = false,
        writeOnly = false,
        readbackVerified = true,
    )
}

internal fun shouldResumeIbassoReaderHealth(
    health: IbassoReaderHealth,
    healthDeviceId: Int?,
    deviceId: Int,
): Boolean = health.failureCount > 0 && healthDeviceId == deviceId

internal fun isCurrentIbassoReaderGeneration(
    readerGeneration: Long,
    currentGeneration: Long,
    running: Boolean,
    threadMatches: Boolean,
    connectionMatches: Boolean,
    endpointMatches: Boolean,
): Boolean = readerGeneration == currentGeneration &&
    running &&
    threadMatches &&
    connectionMatches &&
    endpointMatches

internal fun shouldRestartIbassoReaderGeneration(
    readerGeneration: Long,
    currentGeneration: Long,
    running: Boolean,
    readerThreadExited: Boolean,
    connectionMatches: Boolean,
    endpointMatches: Boolean,
    volumeConnectionMatches: Boolean,
    restartRequested: Boolean,
): Boolean = isFailedIbassoReaderGenerationCurrent(
    readerGeneration,
    currentGeneration,
    running,
    failedThreadNotReplaced = readerThreadExited,
    connectionMatches,
    endpointMatches,
    volumeConnectionMatches,
) && restartRequested

internal fun isFailedIbassoReaderGenerationCurrent(
    readerGeneration: Long,
    currentGeneration: Long,
    running: Boolean,
    failedThreadNotReplaced: Boolean,
    connectionMatches: Boolean,
    endpointMatches: Boolean,
    volumeConnectionMatches: Boolean,
): Boolean = readerGeneration == currentGeneration &&
    !running &&
    failedThreadNotReplaced &&
    connectionMatches &&
    endpointMatches &&
    volumeConnectionMatches

internal fun hardwareVolumeWriteOnlyForState(
    protocol: String?,
    ibassoHealth: IbassoReaderHealth,
): Boolean = protocol == "ibassoHid" && ibassoHealth.writeOnly

internal fun hardwareVolumeReadbackVerifiedForState(
    protocol: String?,
    standardReadbackVerified: Boolean,
    ibassoHealth: IbassoReaderHealth,
): Boolean = when (protocol) {
    null -> false
    "ibassoHid" -> ibassoHealth.readbackVerified && !ibassoHealth.writeOnly
    else -> standardReadbackVerified
}

internal fun shouldUseDirectIbassoSetReport(
    writeOnly: Boolean,
    readerAvailable: Boolean,
    allowWhenReaderUnavailable: Boolean,
): Boolean = writeOnly || (!readerAvailable && allowWhenReaderUnavailable)

internal class IbassoVolumeEventDebouncer {
    private val lock = Any()
    private var token = 0L
    private var event: UsbVolumeEvent? = null

    fun submit(value: UsbVolumeEvent): Long = synchronized(lock) {
        event = value
        ++token
    }

    fun consume(expectedToken: Long): UsbVolumeEvent? = synchronized(lock) {
        if (expectedToken != token) {
            null
        } else {
            event.also { event = null }
        }
    }

    fun clear() = synchronized(lock) {
        event = null
        token += 1
    }
}

internal interface UsbVolumeProtocol {
    val id: String
    val capabilities: UsbVolumeCapabilities

    fun appGainToRaw(
        gainQ16: Int,
        replayGainMilliDb: Int,
        dsdCompensationDb: Int,
    ): UsbVolumeTarget

    fun rawToLinearGainQ16(raw: Int): Int

    fun decodeEvent(packet: ByteArray): UsbVolumeEvent?

    fun isWriteConfirmation(event: UsbVolumeEvent, lastWrittenRaw: Int?): Boolean =
        lastWrittenRaw != null &&
            event.leftRaw == lastWrittenRaw &&
            event.rightRaw == lastWrittenRaw
}

internal object IbassoHidVolumeProtocol : UsbVolumeProtocol {
    override val id = "ibassoHid"
    override val capabilities = UsbVolumeCapabilities(
        readable = true,
        unsolicitedEvents = true,
        dsdGain = true,
    )

    override fun appGainToRaw(
        gainQ16: Int,
        replayGainMilliDb: Int,
        dsdCompensationDb: Int,
    ): UsbVolumeTarget {
        val target =
            UsbVolumeNative.ibassoAppGainToRaw(gainQ16, replayGainMilliDb, dsdCompensationDb)
        return UsbVolumeTarget(baseRaw = target[0], dsdRaw = target[1])
    }

    override fun rawToLinearGainQ16(raw: Int): Int =
        UsbVolumeNative.ibassoRawToLinearGainQ16(raw)

    override fun decodeEvent(packet: ByteArray): UsbVolumeEvent? {
        // native 返回空数组表示不是音量事件
        val event = UsbVolumeNative.ibassoDecodeEvent(packet)
        if (event.size < 2) {
            return null
        }
        return UsbVolumeEvent(leftRaw = event[0], rightRaw = event[1])
    }
}

internal fun usbVolumeProtocolFor(id: String?): UsbVolumeProtocol? =
    when (id?.trim()) {
        "ibassoHid" -> IbassoHidVolumeProtocol
        else -> null
    }

internal fun usbVolumeProtocolSelection(id: String?): UsbVolumeProtocolSelection {
    val normalized = id?.trim()?.takeIf { it.isNotEmpty() }
    return when (normalized) {
        null, "uac1", "uac2" -> StandardUsbVolumeProtocol
        "ibassoHid" -> VendorUsbVolumeProtocol(IbassoHidVolumeProtocol)
        else -> UnsupportedUsbVolumeProtocol(normalized)
    }
}

internal fun hardwareVolumeSupportedForStream(
    protocolSelection: UsbVolumeProtocolSelection,
    isDsd: Boolean,
    quirkDsdSupported: Boolean?,
): Boolean {
    if (!isDsd) return true
    return when (protocolSelection) {
        StandardUsbVolumeProtocol -> quirkDsdSupported == true
        is VendorUsbVolumeProtocol ->
            protocolSelection.protocol.capabilities.dsdGain && quirkDsdSupported != false
        is UnsupportedUsbVolumeProtocol -> false
    }
}

internal fun effectiveVolumeGainQ16(userGainQ16: Int, replayGainMilliDb: Int): Int =
    UsbVolumeNative.effectiveVolumeGainQ16(userGainQ16, replayGainMilliDb)

internal fun effectiveHardwareVolumeGainQ16(
    userGainQ16: Int,
    replayGainMilliDb: Int,
    dsdCompensationDb: Int,
    isDsd: Boolean,
): Int = UsbVolumeNative.effectiveHardwareVolumeGainQ16(
    userGainQ16,
    replayGainMilliDb,
    dsdCompensationDb,
    isDsd,
)

internal fun pcmBitPerfect(
    sourceBitDepth: Int?,
    decodedBitDepth: Int?,
    usbBitDepth: Int?,
    digitalVolumeActive: Boolean,
): Boolean = !digitalVolumeActive &&
    sourceBitDepth != null &&
    sourceBitDepth == decodedBitDepth &&
    decodedBitDepth == usbBitDepth

internal fun shouldSkipIbassoVolumeWrite(
    target: UsbVolumeTarget,
    previousTarget: UsbVolumeTarget?,
    readbackVerified: Boolean,
): Boolean = readbackVerified && target == previousTarget

internal fun unsafeDsdVolumeReason(
    isDsd: Boolean,
    hardwareVolumeActive: Boolean,
    readbackVerified: Boolean,
    writeOnly: Boolean,
    frozenAtTrustedTarget: Boolean = false,
): String? {
    if (!isDsd) return null
    if (!hardwareVolumeActive) {
        return "DSD playback requires active hardware volume."
    }
    // 回读失灵但冻结在本会话可信硬件值上：实际音量只可能 ≤ 可信值，允许
    // 继续播放；升音量请求在冻结路径里已被拒绝。可信值只可能来自已验证
    // 回读或设备事件（write-only 从始至终不会产生可信目标，也就进不了
    // 冻结且 active 的状态），因此 reader 中途失联转 write-only 时同样适用。
    if (frozenAtTrustedTarget) return null
    if (writeOnly || !readbackVerified) {
        return "DSD playback requires readable hardware volume confirmation."
    }
    return null
}

internal fun shouldUsePcmDigitalVolumeFallback(
    isDsd: Boolean,
    volumeMode: String,
    hardwareVolumeActive: Boolean,
    readbackVerified: Boolean,
    writeOnly: Boolean,
): Boolean = !isDsd &&
    volumeMode != "raw" &&
    (!hardwareVolumeActive || !readbackVerified || writeOnly)

internal fun shouldSmoothPcmVolumeHandoff(
    smoothHandoff: Boolean,
    isDsd: Boolean,
    wasHardwareActive: Boolean,
    hardwareVolumeActive: Boolean,
): Boolean = smoothHandoff &&
    !isDsd &&
    !wasHardwareActive &&
    hardwareVolumeActive

internal fun routeIbassoVolumePacket(
    packet: ByteArray,
    pendingCommands: Set<Int>,
    lastWrittenRaw: Int?,
): IbassoVolumePacketRoute {
    // native 返回 [0]=Unknown、[1,left,right]=Event、[2,command]=CommandResponse
    val parsed = UsbVolumeNative.ibassoRoutePacket(packet, pendingCommands.toIntArray())
    return when (parsed[0]) {
        1 -> {
            val event = UsbVolumeEvent(leftRaw = parsed[1], rightRaw = parsed[2])
            IbassoVolumePacketRoute.Event(
                event = event,
                isWriteConfirmation =
                    IbassoHidVolumeProtocol.isWriteConfirmation(event, lastWrittenRaw),
            )
        }
        2 -> IbassoVolumePacketRoute.CommandResponse(parsed[1], packet)
        else -> IbassoVolumePacketRoute.Unknown
    }
}

internal fun recentIbassoWrittenRaw(
    lastWrittenRaw: Int?,
    lastWrittenAtMs: Long,
    nowMs: Long,
    windowMs: Long,
): Int? = lastWrittenRaw?.takeIf {
    nowMs - lastWrittenAtMs in 0..windowMs
}

internal fun hardwareVolumeHandoffTarget(
    smooth: Boolean,
    readGainQ16: Int?,
    appTargetQ16: Int,
): HardwareVolumeHandoffTarget {
    val safeAppTarget = appTargetQ16.coerceIn(0, IBASSO_UNITY_GAIN_Q16)
    return if (
        smooth &&
        readGainQ16 != null &&
        readGainQ16 in 0..safeAppTarget
    ) {
        HardwareVolumeHandoffTarget(readGainQ16, HardwareVolumeHandoffSource.DEVICE)
    } else {
        HardwareVolumeHandoffTarget(safeAppTarget, HardwareVolumeHandoffSource.APP)
    }
}

internal fun shouldReadInitialHardwareVolume(
    isNewConnection: Boolean,
    readable: Boolean,
): Boolean = isNewConnection && readable

internal fun ibassoActualEventGainQ16(
    baseRaw: Int,
    isDsd: Boolean,
    dsdCompensationDb: Int,
): UsbActualVolume {
    val actual = UsbVolumeNative.ibassoActualEventGainQ16(baseRaw, isDsd, dsdCompensationDb)
    return UsbActualVolume(raw = actual[0], gainQ16 = actual[1])
}

private const val IBASSO_UNITY_GAIN_Q16 = 65536

internal fun ibassoVolumeIndex(gainQ16: Int): Int =
    UsbVolumeNative.ibassoVolumeIndex(gainQ16)

internal fun ibassoDeviceVolume(index: Int): Int =
    UsbVolumeNative.ibassoDeviceVolume(index)

internal fun ibassoDsdVolume(baseVolume: Int, compensationDb: Int): Int =
    UsbVolumeNative.ibassoDsdVolume(baseVolume, compensationDb)

internal fun ibassoI2cWritePacket(
    command: Int,
    slave: Int,
    offset: Int,
    byteOffset: Int,
    value: Int,
): ByteArray = UsbVolumeNative.ibassoI2cWritePacket(command, slave, offset, byteOffset, value)

internal fun ibassoRoomWritePacket(command: Int, register: Int, value: Int): ByteArray =
    UsbVolumeNative.ibassoRoomWritePacket(command, register, value)

internal fun ibassoVolumePackets(target: UsbVolumeTarget): List<ByteArray> {
    // native 把 10 个 16 字节包按固定顺序平铺成 160 字节，此处按 16 切分还原
    val flat = UsbVolumeNative.ibassoVolumePackets(target.baseRaw, target.dsdRaw)
    return (flat.indices step 16).map { flat.copyOfRange(it, it + 16) }
}

internal fun ibassoRollbackTarget(
    lastAppliedTarget: UsbVolumeTarget?,
    initialBaseRaw: Int?,
    dsdCompensationDb: Int,
): UsbVolumeTarget? = lastAppliedTarget ?: initialBaseRaw?.coerceIn(0, 255)?.let { baseRaw ->
    UsbVolumeTarget(baseRaw, ibassoDsdVolume(baseRaw, dsdCompensationDb))
}

internal fun trustedIbassoTargetForDevice(
    target: UsbVolumeTarget?,
    targetDeviceId: Int?,
    deviceId: Int,
): UsbVolumeTarget? = target.takeIf { targetDeviceId == deviceId }

internal fun ibassoTargetFromEvent(
    baseRaw: Int,
    dsdCompensationDb: Int,
): UsbVolumeTarget {
    val target = UsbVolumeNative.ibassoTargetFromEvent(baseRaw, dsdCompensationDb)
    return UsbVolumeTarget(target[0], target[1])
}

internal fun ibassoVolumeReadPacket(): ByteArray = UsbVolumeNative.ibassoVolumeReadPacket()
