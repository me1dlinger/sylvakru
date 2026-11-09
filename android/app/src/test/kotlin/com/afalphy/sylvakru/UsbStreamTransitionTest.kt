package com.afalphy.sylvakru

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class UsbStreamTransitionTest {
    private val pcm441 = UsbStreamSignature(7, 44100, 2, 24, null, null)

    @Test
    fun reusesOnlyAnExactReplacementSignature() {
        assertEquals(
            UsbStreamTransitionAction.REUSE,
            usbStreamTransitionAction(pcm441, pcm441, replaceActive = true),
        )
        assertEquals(
            UsbStreamTransitionAction.SILENT_RECONFIGURE,
            usbStreamTransitionAction(
                pcm441,
                pcm441.copy(sampleRate = 96000),
                replaceActive = true,
            ),
        )
        assertEquals(
            UsbStreamTransitionAction.SILENT_RECONFIGURE,
            usbStreamTransitionAction(
                pcm441,
                pcm441.copy(dsdKind = "dop", bitDepth = null),
                replaceActive = true,
            ),
        )
    }

    @Test
    fun reusesAnExactIdleSessionAndOpensFreshForOtherRegularStarts() {
        assertEquals(
            UsbStreamTransitionAction.REUSE,
            usbStreamTransitionAction(pcm441, pcm441, replaceActive = false),
        )
        assertEquals(
            UsbStreamTransitionAction.OPEN_FRESH,
            usbStreamTransitionAction(
                pcm441,
                pcm441.copy(sampleRate = 96000),
                replaceActive = false,
            ),
        )
        assertEquals(
            UsbStreamTransitionAction.OPEN_FRESH,
            usbStreamTransitionAction(null, pcm441, replaceActive = true),
        )
    }

    @Test
    fun padsPreRollForEveryNewlyOpenedOrReconfiguredStream() {
        assertEquals(
            UsbTransitionSilencePlan(0, 0, 0),
            usbTransitionSilencePlan(UsbStreamTransitionAction.REUSE),
        )
        assertEquals(
            UsbTransitionSilencePlan(16, 24, 100),
            usbTransitionSilencePlan(UsbStreamTransitionAction.SILENT_RECONFIGURE),
        )
        assertEquals(
            UsbTransitionSilencePlan(0, 0, 100),
            usbTransitionSilencePlan(UsbStreamTransitionAction.OPEN_FRESH),
        )
        // quirk 覆盖预滚时长时按覆盖值垫；REUSE 不受影响
        assertEquals(
            UsbTransitionSilencePlan(16, 24, 300),
            usbTransitionSilencePlan(
                UsbStreamTransitionAction.SILENT_RECONFIGURE,
                preRollMs = 300,
            ),
        )
        assertEquals(
            UsbTransitionSilencePlan(0, 0, 0),
            usbTransitionSilencePlan(UsbStreamTransitionAction.REUSE, preRollMs = 300),
        )
    }

    @Test
    fun limitsVolumeRiseSpeedButKeepsReductionFast() {
        assertEquals(6, pcmVolumeRampSteps(65536, 0))
        assertEquals(6, pcmVolumeRampSteps(65536, 32768))
        assertEquals(6, pcmVolumeRampSteps(32768, 32768 + 6554))
        assertEquals(15, pcmVolumeRampSteps(0, 32768))
        assertEquals(30, pcmVolumeRampSteps(0, 65536))
    }

    @Test
    fun doesNotPublishInactiveStateForAnUncommittedReplacementFailure() {
        assertFalse(shouldPublishUsbStartFailure(true, false, true))
        assertTrue(shouldPublishUsbStartFailure(true, true, true))
        assertTrue(shouldPublishUsbStartFailure(false, false, true))
        assertTrue(shouldPublishUsbStartFailure(true, false, false))
    }

    @Test
    fun computesBoundedSilenceFramesForEveryPcmWidth() {
        assertEquals(706, usbSilenceFrames(44100, 16))
        assertEquals(1536, usbSilenceFrames(96000, 16))
        assertEquals(9600, usbSilenceFrames(96000, 100))
    }

    @Test
    fun makesOutputDrainBounded() {
        assertEquals(OutputDrainAction.DRAINED, outputDrainAction(0, 0, 220))
        assertEquals(OutputDrainAction.WAIT, outputDrainAction(4, 219, 220))
        assertEquals(OutputDrainAction.TIMED_OUT, outputDrainAction(4, 220, 220))
    }

    @Test
    fun scalesDrainTimeoutToTheOldSessionBufferLevel() {
        assertEquals(410L, usbTransitionDrainTimeoutMs(150))
        assertEquals(1260L, usbTransitionDrainTimeoutMs(1000))
    }

    @Test
    fun preservesTrustedHardwareTargetOnlyForTheSameDeviceAndProtocol() {
        assertTrue(
            shouldPreserveTrustedHardwareVolume(
                currentDeviceId = 7,
                nextDeviceId = 7,
                currentProtocol = "ibassoHid",
                nextProtocol = "ibassoHid",
                readbackVerified = true,
                writeOnly = false,
            ),
        )
        assertFalse(
            shouldPreserveTrustedHardwareVolume(7, 8, "ibassoHid", "ibassoHid", true, false),
        )
        assertFalse(
            shouldPreserveTrustedHardwareVolume(7, 7, "ibassoHid", "uac2", true, false),
        )
        assertFalse(
            shouldPreserveTrustedHardwareVolume(7, 7, "ibassoHid", "ibassoHid", false, false),
        )
        assertFalse(
            shouldPreserveTrustedHardwareVolume(7, 7, "ibassoHid", "ibassoHid", true, true),
        )
    }

    @Test
    fun frozenPcmCompensationCanOnlyReduceTotalOutput() {
        assertEquals(32768, frozenPcmCompensationGainQ16(65536, 32768))
        assertEquals(65536, frozenPcmCompensationGainQ16(32768, 65536))
        assertEquals(65536, frozenPcmCompensationGainQ16(32768, 32768))
        assertEquals(0, frozenPcmCompensationGainQ16(65536, 0))
        assertEquals(0, frozenPcmCompensationGainQ16(0, 32768))
    }

    @Test
    fun staleOrDsdVerificationCannotMutateTheNewPcmSession() {
        assertEquals(
            PreservedVolumeVerificationAction.IGNORE,
            preservedVolumeVerificationAction(false, false, 20, 20),
        )
        assertEquals(
            PreservedVolumeVerificationAction.KEEP_FROZEN,
            preservedVolumeVerificationAction(true, true, 20, 20),
        )
        assertEquals(
            PreservedVolumeVerificationAction.ACCEPT,
            preservedVolumeVerificationAction(true, false, 20, 20),
        )
        assertEquals(
            PreservedVolumeVerificationAction.KEEP_FROZEN,
            preservedVolumeVerificationAction(true, false, null, 20),
        )
    }

    @Test
    fun keepsUsbOutputQueuedWhenPlaybackStops() {
        assertEquals(false, shouldFlushOutputOnStop(null))
        assertEquals(false, shouldFlushOutputOnStop("dop"))
        assertEquals(false, shouldFlushOutputOnStop("native"))
    }
}
