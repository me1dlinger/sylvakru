package com.afalphy.sylvakru

import android.media.AudioFormat
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

// 增益换算/音量表/HID 报文/验证与恢复决策等数值用例已随实现下沉，
// 由 cpp/tests/usb_volume_protocol_test.cpp 真机对拍接管；
// 本文件只保留仍在 Kotlin 层的会话策略与状态机用例。
class UsbVolumeProtocolTest {
    private val protocol = IbassoHidVolumeProtocol

    @Test
    fun mapsAndroidPcmEncodingsToBitDepths() {
        assertEquals(16, bitDepthFromPcmEncoding(AudioFormat.ENCODING_PCM_16BIT))
        assertEquals(24, bitDepthFromPcmEncoding(AudioFormat.ENCODING_PCM_24BIT_PACKED))
        assertEquals(32, bitDepthFromPcmEncoding(AudioFormat.ENCODING_PCM_32BIT))
    }

    @Test
    fun allowsPcmDigitalFallbackWithoutVerifiedHardwareVolume() {
        assertNull(
            unsafeDsdVolumeReason(
                isDsd = false,
                hardwareVolumeActive = false,
                readbackVerified = false,
                writeOnly = true,
            ),
        )
    }

    @Test
    fun usesDigitalFallbackWhenPcmHardwareVolumeLosesVerification() {
        assertTrue(
            shouldUsePcmDigitalVolumeFallback(
                isDsd = false,
                volumeMode = "auto",
                hardwareVolumeActive = true,
                readbackVerified = false,
                writeOnly = true,
            ),
        )
        assertFalse(
            shouldUsePcmDigitalVolumeFallback(
                isDsd = true,
                volumeMode = "auto",
                hardwareVolumeActive = true,
                readbackVerified = false,
                writeOnly = true,
            ),
        )
        assertFalse(
            shouldUsePcmDigitalVolumeFallback(
                isDsd = false,
                volumeMode = "raw",
                hardwareVolumeActive = false,
                readbackVerified = false,
                writeOnly = true,
            ),
        )
    }

    @Test
    fun attenuatesImmediatelyWhenPcmFallsBackFromHardwareVolume() {
        assertFalse(
            shouldSmoothPcmVolumeHandoff(
                smoothHandoff = true,
                isDsd = false,
                wasHardwareActive = true,
                hardwareVolumeActive = false,
            ),
        )
        assertTrue(
            shouldSmoothPcmVolumeHandoff(
                smoothHandoff = true,
                isDsd = false,
                wasHardwareActive = false,
                hardwareVolumeActive = true,
            ),
        )
        assertFalse(
            shouldSmoothPcmVolumeHandoff(
                smoothHandoff = false,
                isDsd = false,
                wasHardwareActive = false,
                hardwareVolumeActive = true,
            ),
        )
    }

    @Test
    fun allowsDsdOnlyWithVerifiedReadableHardwareVolume() {
        assertNull(
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = true,
                readbackVerified = true,
                writeOnly = false,
            ),
        )
    }

    @Test
    fun rejectsDsdWithoutActiveHardwareVolume() {
        assertEquals(
            "DSD playback requires active hardware volume.",
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = false,
                readbackVerified = true,
                writeOnly = false,
            ),
        )
    }

    @Test
    fun rejectsDsdWriteOnlyOrUnverifiedHardwareVolume() {
        assertEquals(
            "DSD playback requires readable hardware volume confirmation.",
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = true,
                readbackVerified = false,
                writeOnly = false,
            ),
        )
        assertEquals(
            "DSD playback requires readable hardware volume confirmation.",
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = true,
                readbackVerified = true,
                writeOnly = true,
            ),
        )
    }

    @Test
    fun allowsFrozenDsdPlaybackOnlyAtATrustedTarget() {
        assertNull(
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = true,
                readbackVerified = false,
                writeOnly = false,
                frozenAtTrustedTarget = true,
            ),
        )
        // reader 中途失联转 write-only 也一样：可信值只可能来自已验证回读或
        // 设备事件，冻结在可信值上实际音量只可能 ≤ 可信值，继续播放
        assertNull(
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = true,
                readbackVerified = false,
                writeOnly = true,
                frozenAtTrustedTarget = true,
            ),
        )
        assertEquals(
            "DSD playback requires active hardware volume.",
            unsafeDsdVolumeReason(
                isDsd = true,
                hardwareVolumeActive = false,
                readbackVerified = false,
                writeOnly = false,
                frozenAtTrustedTarget = true,
            ),
        )
    }

    @Test
    fun exposesIbassoProtocolCapabilities() {
        assertEquals("ibassoHid", protocol.id)
        assertEquals(
            UsbVolumeCapabilities(
                readable = true,
                unsolicitedEvents = true,
                dsdGain = true,
            ),
            protocol.capabilities,
        )
    }

    @Test
    fun derivesReadbackStateFromTheActiveProtocol() {
        val contradictoryIbassoHealth = IbassoReaderHealth(
            writeOnly = true,
            readbackVerified = true,
        )

        assertTrue(hardwareVolumeWriteOnlyForState("ibassoHid", contradictoryIbassoHealth))
        assertFalse(
            hardwareVolumeReadbackVerifiedForState(
                "ibassoHid",
                standardReadbackVerified = true,
                ibassoHealth = contradictoryIbassoHealth,
            ),
        )
        assertFalse(hardwareVolumeWriteOnlyForState("uac2", contradictoryIbassoHealth))
        assertTrue(
            hardwareVolumeReadbackVerifiedForState(
                "uac2",
                standardReadbackVerified = true,
                ibassoHealth = contradictoryIbassoHealth,
            ),
        )
        assertFalse(
            hardwareVolumeReadbackVerifiedForState(
                null,
                standardReadbackVerified = true,
                ibassoHealth = contradictoryIbassoHealth,
            ),
        )
    }

    @Test
    fun selectsOnlyTheGenericIbassoHidProtocolId() {
        assertSame(IbassoHidVolumeProtocol, usbVolumeProtocolFor("ibassoHid"))
        assertNull(usbVolumeProtocolFor("ibassoDc03Pro"))
        assertEquals(
            VendorUsbVolumeProtocol(IbassoHidVolumeProtocol),
            usbVolumeProtocolSelection("ibassoHid"),
        )
        assertEquals(
            UnsupportedUsbVolumeProtocol("ibassoDc03Pro"),
            usbVolumeProtocolSelection("ibassoDc03Pro"),
        )
        assertEquals(StandardUsbVolumeProtocol, usbVolumeProtocolSelection(null))
        assertEquals(StandardUsbVolumeProtocol, usbVolumeProtocolSelection("uac1"))
        assertEquals(StandardUsbVolumeProtocol, usbVolumeProtocolSelection("uac2"))
        assertEquals(
            UnsupportedUsbVolumeProtocol("unknownProtocol"),
            usbVolumeProtocolSelection("unknownProtocol"),
        )
    }

    @Test
    fun gatesDsdHardwareGainByProtocolCapabilityAndQuirkEvidence() {
        val vendorWithDsdGain = VendorUsbVolumeProtocol(protocol)
        val vendorWithoutDsdGain = VendorUsbVolumeProtocol(
            object : UsbVolumeProtocol by protocol {
                override val capabilities = protocol.capabilities.copy(dsdGain = false)
            },
        )

        assertTrue(hardwareVolumeSupportedForStream(vendorWithDsdGain, isDsd = true, true))
        assertTrue(hardwareVolumeSupportedForStream(vendorWithDsdGain, isDsd = true, null))
        assertFalse(hardwareVolumeSupportedForStream(vendorWithDsdGain, isDsd = true, false))
        assertFalse(hardwareVolumeSupportedForStream(vendorWithoutDsdGain, isDsd = true, true))
        assertFalse(hardwareVolumeSupportedForStream(vendorWithoutDsdGain, isDsd = true, null))
        assertFalse(hardwareVolumeSupportedForStream(vendorWithoutDsdGain, isDsd = true, false))

        assertTrue(hardwareVolumeSupportedForStream(StandardUsbVolumeProtocol, isDsd = true, true))
        assertFalse(hardwareVolumeSupportedForStream(StandardUsbVolumeProtocol, isDsd = true, false))
        assertFalse(hardwareVolumeSupportedForStream(StandardUsbVolumeProtocol, isDsd = true, null))

        val unsupported = UnsupportedUsbVolumeProtocol("unknownProtocol")
        assertFalse(hardwareVolumeSupportedForStream(unsupported, isDsd = true, true))
        assertFalse(hardwareVolumeSupportedForStream(unsupported, isDsd = true, false))
        assertFalse(hardwareVolumeSupportedForStream(unsupported, isDsd = true, null))

        assertTrue(hardwareVolumeSupportedForStream(vendorWithoutDsdGain, isDsd = false, false))
        assertTrue(hardwareVolumeSupportedForStream(StandardUsbVolumeProtocol, isDsd = false, null))
        assertTrue(hardwareVolumeSupportedForStream(unsupported, isDsd = false, null))
    }

    @Test
    fun reportsPcmBitPerfectOnlyWhenEffectiveDepthAndUsbSlotMatch() {
        assertTrue(pcmBitPerfect(24, 24, 24, digitalVolumeActive = false))
        assertTrue(pcmBitPerfect(16, 16, 16, digitalVolumeActive = false))
        assertFalse(pcmBitPerfect(24, 16, 24, digitalVolumeActive = false))
        assertFalse(pcmBitPerfect(16, 16, 24, digitalVolumeActive = false))
        assertFalse(pcmBitPerfect(null, 16, 16, digitalVolumeActive = false))
        assertFalse(pcmBitPerfect(24, 24, 24, digitalVolumeActive = true))
    }

    @Test
    fun keepsLatestPendingTargetWhenPendingDoesNotLowerOutput() {
        val running = UsbVolumeRequest(1000, 0, "dac", 0, true, 7)
        val pending = UsbVolumeRequest(2000, 0, "dac", 0, true, 7)
        val incoming = UsbVolumeRequest(3000, 0, "dac", 0, true, 7)

        assertEquals(
            incoming,
            coalescedUsbVolumeRequest(running, pending, incoming, isDsd = false),
        )
    }

    @Test
    fun keepsOnlyTheLatestPendingAbsoluteTarget() {
        val running = UsbVolumeRequest(3000, 0, "dac", 0, true, 7)
        val pending = UsbVolumeRequest(2000, 0, "dac", 0, true, 7)
        val incoming = UsbVolumeRequest(2500, 0, "dac", 0, true, 7)

        assertEquals(
            incoming,
            coalescedUsbVolumeRequest(running, pending, incoming, isDsd = false),
        )
    }

    @Test
    fun replacesLatchedReductionWithAnEvenLowerTarget() {
        val running = UsbVolumeRequest(3000, 0, "dac", 0, true, 7)
        val pending = UsbVolumeRequest(2000, 0, "dac", 0, true, 7)
        val incoming = UsbVolumeRequest(1000, 0, "dac", 0, true, 7)

        assertEquals(
            incoming,
            coalescedUsbVolumeRequest(running, pending, incoming, isDsd = false),
        )
    }

    @Test
    fun acceptsAnIncreaseAfterTheLowerTargetBecomesRunning() {
        val loweredRunning = UsbVolumeRequest(2000, 0, "dac", 0, true, 7)
        val incoming = UsbVolumeRequest(2500, 0, "dac", 0, true, 7)

        assertEquals(
            incoming,
            coalescedUsbVolumeRequest(loweredRunning, null, incoming, isDsd = false),
        )
    }

    @Test
    fun acceptsLatestTargetAcrossSessionOrModeChanges() {
        val running = UsbVolumeRequest(3000, 0, "dac", 0, true, 7)
        val pending = UsbVolumeRequest(2000, 0, "dac", 0, true, 7)
        val nextSession = UsbVolumeRequest(2500, 0, "dac", 0, true, 8)
        val digital = UsbVolumeRequest(2500, 0, "digital", 0, true, 7)

        assertEquals(
            nextSession,
            coalescedUsbVolumeRequest(running, pending, nextSession, isDsd = false),
        )
        assertEquals(
            digital,
            coalescedUsbVolumeRequest(running, pending, digital, isDsd = false),
        )
    }

    @Test
    fun comparesTotalEffectiveDsdOutputWhenCoalescingTargets() {
        val running = UsbVolumeRequest(32768, 0, "dac", 0, true, 7)
        val pending = UsbVolumeRequest(32768, -1000, "dac", 0, true, 7)
        val incoming = UsbVolumeRequest(32768, -500, "dac", 6, true, 7)

        assertEquals(
            incoming,
            coalescedUsbVolumeRequest(running, pending, incoming, isDsd = true),
        )
    }

    @Test
    fun skipsPendingDebounceOutsideAnActiveIbassoSequence() {
        assertEquals(0L, usbVolumePendingDelayMs(null, null, 1000L, 1100L))
        assertEquals(
            0L,
            usbVolumePendingDelayMs("standardUsbAudioClass", 1000L, 1050L, 1100L),
        )
    }

    @Test
    fun keepsConfiguredProtocolOnlyForHardwareVolumeRequests() {
        val protocol = IbassoHidVolumeProtocol.id

        assertEquals(protocol, usbVolumeProtocolForRequest("auto", protocol, true, true))
        assertEquals(protocol, usbVolumeProtocolForRequest("dac", protocol, true, true))
        assertNull(usbVolumeProtocolForRequest("digital", protocol, true, true))
        assertNull(usbVolumeProtocolForRequest("raw", protocol, true, true))
        assertNull(usbVolumeProtocolForRequest("auto", null, true, true))
        assertNull(usbVolumeProtocolForRequest("auto", protocol, false, true))
        assertNull(usbVolumeProtocolForRequest("auto", protocol, true, false))
    }

    @Test
    fun skipsOnlyVerifiedDuplicateIbassoVolumeTargets() {
        val target = UsbVolumeTarget(baseRaw = 130, dsdRaw = 130)

        assertTrue(
            shouldSkipIbassoVolumeWrite(
                target = target,
                previousTarget = target,
                readbackVerified = true,
            ),
        )
        assertFalse(
            shouldSkipIbassoVolumeWrite(
                target = target,
                previousTarget = target,
                readbackVerified = false,
            ),
        )
        assertFalse(
            shouldSkipIbassoVolumeWrite(
                target = target,
                previousTarget = UsbVolumeTarget(baseRaw = 120, dsdRaw = 120),
                readbackVerified = true,
            ),
        )
    }

    @Test
    fun recognizesOnlyMatchingStereoWriteConfirmation() {
        assertTrue(protocol.isWriteConfirmation(UsbVolumeEvent(97, 97), 97))
        assertFalse(protocol.isWriteConfirmation(UsbVolumeEvent(97, 98), 97))
        assertFalse(protocol.isWriteConfirmation(UsbVolumeEvent(97, 97), null))
    }

    @Test
    fun transitionsReaderFromRestartToWriteOnlyAfterTwoFailures() {
        val initial = IbassoReaderHealth()
        assertTrue(initial.readable)
        assertFalse(initial.restartRequested)
        assertFalse(initial.writeOnly)

        val firstFailure = initial.afterFailure()
        assertTrue(firstFailure.readable)
        assertTrue(firstFailure.restartRequested)
        assertFalse(firstFailure.writeOnly)
        assertFalse(firstFailure.readbackVerified)

        val restarted = firstFailure.afterRestart()
        assertFalse(restarted.restartRequested)
        val secondFailure = restarted.afterFailure()
        assertFalse(secondFailure.readable)
        assertFalse(secondFailure.restartRequested)
        assertTrue(secondFailure.writeOnly)
        assertFalse(secondFailure.readbackVerified)
    }

    @Test
    fun verifiedReadbackClearsPreviousReaderFailureBeforeNextFailure() {
        val recovered = IbassoReaderHealth()
            .afterFailure()
            .afterRestart()
            .afterVerifiedReadback()

        assertEquals(0, recovered.failureCount)
        assertFalse(recovered.restartRequested)
        assertFalse(recovered.writeOnly)
        assertTrue(recovered.readbackVerified)

        val nextFailure = recovered.afterFailure()
        assertEquals(1, nextFailure.failureCount)
        assertTrue(nextFailure.restartRequested)
        assertFalse(nextFailure.writeOnly)
    }

    @Test
    fun rejectsCallbacksFromSupersededReaderGenerations() {
        assertTrue(
            isCurrentIbassoReaderGeneration(
                readerGeneration = 2,
                currentGeneration = 2,
                running = true,
                threadMatches = true,
                connectionMatches = true,
                endpointMatches = true,
            ),
        )
        assertFalse(
            isCurrentIbassoReaderGeneration(
                readerGeneration = 1,
                currentGeneration = 2,
                running = true,
                threadMatches = true,
                connectionMatches = true,
                endpointMatches = true,
            ),
        )
        assertFalse(
            isCurrentIbassoReaderGeneration(
                readerGeneration = 2,
                currentGeneration = 2,
                running = true,
                threadMatches = false,
                connectionMatches = true,
                endpointMatches = true,
            ),
        )
    }

    @Test
    fun restartsOnlyAfterTheFailedCurrentReaderThreadExits() {
        assertTrue(
            shouldRestartIbassoReaderGeneration(
                readerGeneration = 2,
                currentGeneration = 2,
                running = false,
                readerThreadExited = true,
                connectionMatches = true,
                endpointMatches = true,
                volumeConnectionMatches = true,
                restartRequested = true,
            ),
        )
        assertFalse(
            shouldRestartIbassoReaderGeneration(
                readerGeneration = 1,
                currentGeneration = 2,
                running = false,
                readerThreadExited = true,
                connectionMatches = true,
                endpointMatches = true,
                volumeConnectionMatches = true,
                restartRequested = true,
            ),
        )
        assertFalse(
            shouldRestartIbassoReaderGeneration(
                readerGeneration = 2,
                currentGeneration = 2,
                running = false,
                readerThreadExited = false,
                connectionMatches = true,
                endpointMatches = true,
                volumeConnectionMatches = true,
                restartRequested = true,
            ),
        )
    }

    @Test
    fun ignoresIdleReaderTimeoutsWithoutPendingResponse() {
        var health = IbassoReaderHealth()

        repeat(10) {
            health = health.afterReadResult(readLength = -1, hasPendingResponse = false)
        }

        assertEquals(0, health.pendingReadFailureCount)
        assertEquals(0, health.failureCount)
        assertFalse(health.restartRequested)
        assertFalse(health.writeOnly)
    }

    @Test
    fun pendingReaderFailuresRestartThenBecomeWriteOnly() {
        var health = IbassoReaderHealth()
        repeat(3) {
            health = health.afterReadResult(readLength = -1, hasPendingResponse = true)
        }
        assertTrue(health.hasPersistentPendingFailure(3))

        health = health.afterFailure()
        assertTrue(health.restartRequested)
        assertFalse(health.writeOnly)

        health = health.afterRestart()
        repeat(3) {
            health = health.afterReadResult(readLength = 0, hasPendingResponse = true)
        }
        assertTrue(health.hasPersistentPendingFailure(3))

        health = health.afterFailure()
        assertFalse(health.restartRequested)
        assertTrue(health.writeOnly)
    }

    @Test
    fun successfulReaderReadResetsPendingFailures() {
        var health = IbassoReaderHealth()
            .afterReadResult(readLength = -1, hasPendingResponse = true)
            .afterReadResult(readLength = 0, hasPendingResponse = true)
        assertEquals(2, health.pendingReadFailureCount)

        health = health.afterReadResult(readLength = 16, hasPendingResponse = true)

        assertEquals(0, health.pendingReadFailureCount)
        assertFalse(health.hasPersistentPendingFailure(3))
        assertEquals(0, health.failureCount)
    }

    @Test
    fun idleTimeoutResetsAnIncompletePendingFailureSequence() {
        var health = IbassoReaderHealth()
            .afterReadResult(readLength = -1, hasPendingResponse = true)
            .afterReadResult(readLength = -1, hasPendingResponse = true)

        health = health.afterReadResult(readLength = -1, hasPendingResponse = false)

        assertEquals(0, health.pendingReadFailureCount)
        assertFalse(health.restartRequested)
        assertFalse(health.writeOnly)
    }

    @Test
    fun resumesReaderFailureHealthOnlyForTheSameDevice() {
        val failed = IbassoReaderHealth().afterFailure()

        assertTrue(shouldResumeIbassoReaderHealth(failed, healthDeviceId = 7, deviceId = 7))
        assertFalse(shouldResumeIbassoReaderHealth(failed, healthDeviceId = 7, deviceId = 8))
        assertFalse(
            shouldResumeIbassoReaderHealth(
                IbassoReaderHealth(),
                healthDeviceId = 7,
                deviceId = 7,
            ),
        )
    }

    @Test
    fun keepsTrustedIbassoTargetOnlyForSameDevice() {
        val target = UsbVolumeTarget(baseRaw = 97, dsdRaw = 85)

        assertEquals(target, trustedIbassoTargetForDevice(target, 7, 7))
        assertNull(trustedIbassoTargetForDevice(target, 7, 8))
        assertNull(trustedIbassoTargetForDevice(null, 7, 7))
    }

    @Test
    fun selectsDirectSetReportForRollbackWhenReaderIsUnavailable() {
        assertTrue(
            shouldUseDirectIbassoSetReport(
                writeOnly = false,
                readerAvailable = false,
                allowWhenReaderUnavailable = true,
            ),
        )
        assertFalse(
            shouldUseDirectIbassoSetReport(
                writeOnly = false,
                readerAvailable = true,
                allowWhenReaderUnavailable = true,
            ),
        )
        assertFalse(
            shouldUseDirectIbassoSetReport(
                writeOnly = false,
                readerAvailable = false,
                allowWhenReaderUnavailable = false,
            ),
        )
        assertTrue(
            shouldUseDirectIbassoSetReport(
                writeOnly = true,
                readerAvailable = true,
                allowWhenReaderUnavailable = false,
            ),
        )
    }

    @Test
    fun keepsWrittenRawOnlyInsideConfirmationWindow() {
        assertEquals(97, recentIbassoWrittenRaw(97, 1000, 1001, 500))
        assertEquals(97, recentIbassoWrittenRaw(97, 1000, 1500, 500))
        assertNull(recentIbassoWrittenRaw(97, 1000, 1501, 500))
        assertNull(recentIbassoWrittenRaw(null, 1000, 1001, 500))
    }

    @Test
    fun selectsReadableDeviceGainOnlyWhenItCannotRaiseVolume() {
        assertEquals(
            HardwareVolumeHandoffTarget(16384, HardwareVolumeHandoffSource.DEVICE),
            hardwareVolumeHandoffTarget(true, 16384, 32768),
        )
        assertEquals(
            HardwareVolumeHandoffTarget(16384, HardwareVolumeHandoffSource.APP),
            hardwareVolumeHandoffTarget(true, 32768, 16384),
        )
        assertEquals(
            HardwareVolumeHandoffTarget(16384, HardwareVolumeHandoffSource.APP),
            hardwareVolumeHandoffTarget(false, 32768, 16384),
        )
        assertEquals(
            HardwareVolumeHandoffTarget(16384, HardwareVolumeHandoffSource.APP),
            hardwareVolumeHandoffTarget(true, null, 16384),
        )
        assertEquals(
            HardwareVolumeHandoffTarget(16384, HardwareVolumeHandoffSource.APP),
            hardwareVolumeHandoffTarget(true, 65537, 16384),
        )
    }

    @Test
    fun readsInitialHardwareVolumeOnlyForNewReadableControl() {
        assertTrue(shouldReadInitialHardwareVolume(isNewConnection = true, readable = true))
        assertFalse(shouldReadInitialHardwareVolume(isNewConnection = false, readable = true))
        assertFalse(shouldReadInitialHardwareVolume(isNewConnection = true, readable = false))
    }

    @Test
    fun selectsOnlyTrustedIbassoRollbackTargets() {
        val lastApplied = UsbVolumeTarget(baseRaw = 97, dsdRaw = 85)

        // 由 initialBaseRaw 推导回滚目标的数值用例（含 DSD 补偿换算）已移至
        // native 对拍测试；此处只保留不触发 native 的空值/优先级分支
        assertEquals(lastApplied, ibassoRollbackTarget(lastApplied, 109, 6))
        assertNull(ibassoRollbackTarget(null, null, 6))
    }

    @Test
    fun consumesOnlyTheLatestDebouncedVolumeEventOnce() {
        val debouncer = IbassoVolumeEventDebouncer()
        val eventA = UsbVolumeEvent(97, 97)
        val eventB = UsbVolumeEvent(98, 99)

        val token1 = debouncer.submit(eventA)
        val token2 = debouncer.submit(eventB)
        assertFalse(token1 == token2)
        assertNull(debouncer.consume(token1))
        assertEquals(eventB, debouncer.consume(token2))
        assertNull(debouncer.consume(token2))

        val staleToken = debouncer.submit(eventA)
        debouncer.clear()
        assertNull(debouncer.consume(staleToken))
    }
}
