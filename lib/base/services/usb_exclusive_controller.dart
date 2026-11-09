import 'dart:async';
import 'dart:io';

import 'package:audio_service/audio_service.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:sylvakru/base/app.dart';
import 'package:sylvakru/base/audio_handler.dart';
import 'package:sylvakru/base/data/library.dart';
import 'package:sylvakru/base/data/setting.dart';
import 'package:sylvakru/base/my_audio_metadata.dart';
import 'package:sylvakru/base/services/logger.dart';
import 'package:sylvakru/base/services/replay_gain.dart';
import 'package:sylvakru/base/services/usb_audio_preferences.dart';
import 'package:sylvakru/base/services/usb_audio_service.dart';
import 'package:sylvakru/base/utils/metadata_utils.dart';
import 'package:sylvakru/base/utils/path.dart';

/// Owns the USB exclusive takeover: it decides whether a track can be handed to
/// the output engine, starts/stops that session, mirrors the engine's transport
/// state, and hands control back to the shared mpv path when it must.
///
/// It deliberately knows nothing about media_kit. Everything that touches the
/// shared player - opening it from a position, advancing the queue - is done by
/// [MyAudioHandler] through the callbacks wired in [attach], so the two output
/// paths stay separable and the handler's media-session publishing is untouched.
final usbExclusiveController = UsbExclusiveController();

/// How much louder a single phone volume-key step may make the output. The key
/// is a discrete action, so it is allowed to move faster than the 1 dB that a
/// slider drag or a ramp step may add.
const _phoneUsbVolumeStepDb = 2.5;

/// How much louder a single ramp step may make the output.
const _safeUsbVolumeIncreaseDb = 1.0;

class UsbExclusiveController {
  // Callbacks into the handler. [onFallbackToMpv] is exposed so the takeover
  // can hand the shared player a position to resume from without knowing how
  // the handler opens it.
  void Function(Duration position)? onFallbackToMpv;
  void Function(Duration position)? _handOffToPlayer;
  void Function()? _onSongFinished;
  void Function(Duration position)? _onPosition;
  void Function(bool playing)? _onPlayingChanged;

  /// Called when the output may be able to take the DAC over now - a device was
  /// plugged in, or the master switch was just turned on. The handler owns the
  /// shared player, so it is the one that decides whether to re-take the current
  /// song and stop mpv; the controller only raises the request.
  void Function()? _onDeviceInserted;

  bool _active = false;

  /// Set once the platform channel turns out to be missing, so a build without
  /// the engine does not retry (and log) on every track.
  bool _engineUnavailable = false;
  Duration _position = Duration.zero;
  Duration? _duration;
  bool _playing = false;
  String? _activeSongId;

  // Bumped on every start/stop so an in-flight open is discarded when the user
  // has already moved on (permission dialog, streaming watermark wait).
  int _generation = 0;
  bool _intentionalStop = false;
  bool _handoffInProgress = false;

  ReplayGainResult _replayGain = const ReplayGainResult(0, null, null);
  double _appliedUserVolume = UsbAudioPreferences.defaultExclusiveVolume;
  double _volumeRampTarget = UsbAudioPreferences.defaultExclusiveVolume;
  Timer? _volumeRampTimer;
  double? _appliedOutputGain;
  double _outputGainRampStepDb = _safeUsbVolumeIncreaseDb;
  Timer? _outputGainRampTimer;
  int _replayGainApplyGeneration = 0;
  int _lastVolumeKeyValue = 0;

  String? _usbVolumeDeviceKey;
  int? _usbAudioDeviceId;
  bool _disposed = false;

  UsbExclusiveController() {
    usbExclusivePlaybackStateNotifier.addListener(_handleExclusiveState);
    usbAudioStatusNotifier.addListener(_handleAudioStatus);
    usbAudioEventNotifier.addListener(_handleAudioEvent);
    usbExclusiveVolumeKeyNotifier.addListener(_handleVolumeKey);
    usbHardwareVolumeNotifier.addListener(_handleHardwareVolume);
    // Re-send the gain whenever the strategy behind it changes.
    usbAudioPreferences.volumeControlModeNotifier.addListener(_reapplyVolume);
    usbAudioPreferences.dsdGainCompensationNotifier.addListener(_reapplyVolume);
    usbAudioPreferences.volumeSmoothHandoffNotifier.addListener(_reapplyVolume);
    usbAudioPreferences.replayGainModeNotifier.addListener(
      _handleReplayGainModeChanged,
    );
    usbAudioPreferences.replayGainFallbackDbNotifier.addListener(
      _handleReplayGainModeChanged,
    );
    _handleAudioStatus();
  }

  bool get enabled =>
      usbAudioPreferences.usbExclusiveEnabledNotifier.value;

  /// Whether the output engine currently owns the DAC.
  bool get active => _active;

  String? get activeSongId => _activeSongId;

  Duration get position => _position;

  Duration? get duration => _duration;

  bool get playing => _playing;

  void attach({
    required void Function(Duration position) handOffToPlayer,
    required void Function() onSongFinished,
    required void Function() onDeviceInserted,
    void Function(Duration position)? onPosition,
    void Function(bool playing)? onPlayingChanged,
  }) {
    _handOffToPlayer = handOffToPlayer;
    _onSongFinished = onSongFinished;
    _onDeviceInserted = onDeviceInserted;
    _onPosition = onPosition;
    _onPlayingChanged = onPlayingChanged;
    onFallbackToMpv ??= handOffToPlayer;
  }

  /// Persists and applies the master switch. Turning it off while a session is
  /// live hands the shared player the current position so listening continues.
  /// Turning it on with a song already loaded asks the handler to take the
  /// current song over at once, so a switch flipped mid-playback is honoured.
  Future<void> setEnabled(bool value) async {
    final notifier = usbAudioPreferences.usbExclusiveEnabledNotifier;
    if (notifier.value != value) {
      notifier.value = value;
    }
    setting.save();
    if (value) {
      if (!_active) {
        _onDeviceInserted?.call();
      }
      return;
    }
    if (!_active) {
      return;
    }
    final position = _position;
    await stop(intentional: true);
    await _handoffToMpv(position);
  }

  /// Tries to take the DAC over for [song]. Returns false when the takeover is
  /// not possible, so the caller keeps (or falls back to) the shared path.
  Future<bool> start(
    MyAudioMetadata song, {
    Duration? start,
    bool paused = false,
  }) async {
    if (!enabled || _engineUnavailable || !_shouldTry(song)) {
      return false;
    }
    final generation = ++_generation;
    final isDsd = song.isDsd;
    if (isDsd && usbAudioPreferences.dsdModeNotifier.value == UsbDsdMode.pcm) {
      // DSD in PCM mode is decoded to PCM by the shared path; the engine only
      // takes DoP/native streams.
      logger.output('usb exclusive skipped: dsd pcm mode -> shared output');
      return false;
    }

    late UsbExclusiveCapability capability;
    try {
      capability = await usbAudioService.getExclusiveCapabilities();
      if (capability.available && !capability.permissionGranted) {
        await usbAudioService.probeExclusiveAccess();
        capability = await usbAudioService.getExclusiveCapabilities();
      }
    } on MissingPluginException {
      // No native side at all (a build without the engine): stop trying, and
      // never let the attempt stand between a song and the player.
      _engineUnavailable = true;
      logger.output('usb exclusive: no native engine, using the shared output');
      return false;
    } catch (error) {
      logger.output('usb exclusive probe failed:$error');
      return false;
    }
    if (!capability.available || !capability.permissionGranted) {
      logger.output('usb exclusive unavailable:${capability.message}');
      return false;
    }

    // DoP frame rate is derived by the engine from the file header (DSD rate
    // ÷ 16), so no sample-rate whitelist applies to DSD.
    final sampleRate = isDsd ? null : _preferredSampleRate(song);
    if (!isDsd && sampleRate == null) {
      logger.output(
        'usb exclusive fallback: unsupported source samplerate=${song.samplerate}',
      );
      return false;
    }

    final filePath = await _playablePath(song, generation: generation);
    if (filePath == null) {
      return false;
    }
    if (generation != _generation) {
      return false;
    }
    // A .part file is still downloading; the engine reads it as it grows.
    final streaming = filePath.endsWith('.part');

    _replayGain = replayGainFor(
      song,
      usbAudioPreferences.replayGainModeNotifier.value,
      fallbackDb: usbAudioPreferences.replayGainFallbackDbNotifier.value
          .toDouble(),
    );
    _prepareExclusiveVolume();

    final digitalGain = usbExclusiveDigitalVolumeGain(volumeNotifier.value);
    final dsdCompensationDb = isDsd
        ? usbAudioPreferences.dsdGainCompensationNotifier.value
        : 0;
    final transition = safeOutputGainTransition(
      appliedGain: _appliedOutputGain,
      userGain: digitalGain,
      adjustmentDb: _effectiveReplayGainDb(digitalGain) + dsdCompensationDb,
    );
    final previousOutputGain = _appliedOutputGain;
    _appliedOutputGain = transition.appliedGain;

    final request = UsbExclusivePlaybackRequest(
      playbackId: 'load-$generation',
      filePath: filePath,
      title: getTitle(song),
      sourceFormat: _normalizedFormat(song),
      sampleRate: sampleRate,
      bitDepth: isDsd ? null : preferredUsbExclusiveBitDepth(),
      dsdMode: isDsd ? usbAudioPreferences.dsdModeNotifier.value.name : null,
      volumeGain: digitalGain,
      replayGainDb: transition.adjustmentDb - dsdCompensationDb,
      volumeMode: usbAudioPreferences.volumeControlModeNotifier.value.name,
      dsdGainCompensationDb: dsdCompensationDb,
      smoothVolumeHandoff:
          usbAudioPreferences.volumeSmoothHandoffNotifier.value,
      targetBufferMs: _targetBufferMsForLifecycle(
        WidgetsBinding.instance.lifecycleState,
      ),
      startPaused: paused,
      streaming: streaming,
      totalBytes: streaming ? _estimateStreamTotalBytes(song) : null,
      replaceActive: _active,
    );

    late final UsbExclusivePlaybackState state;
    final previousIntentionalStop = _intentionalStop;
    _intentionalStop = true;
    try {
      state = await usbAudioService.startExclusivePlayback(request);
    } catch (_) {
      _appliedOutputGain = previousOutputGain;
      _restoreExclusiveVolume();
      rethrow;
    } finally {
      _intentionalStop = previousIntentionalStop;
    }

    if (!state.active) {
      _appliedOutputGain = previousOutputGain;
      _restoreExclusiveVolume();
      logger.output('usb exclusive fallback:${state.message}');
      return false;
    }

    _active = true;
    _activeSongId = song.id;
    _position = state.position;
    _duration = state.duration;
    _playing = state.playing;
    _publishReplayGainState(state);
    if (transition.needsRamp) {
      _scheduleOutputGainRamp();
    }
    // A restored position has to be seeked in after the session is live.
    if (start != null && start > Duration.zero) {
      await seek(start);
    }
    _onPosition?.call(_position);
    _onPlayingChanged?.call(_playing);
    logger.output(
      'usb exclusive opened: active=${state.active}, playing=${state.playing}, '
      'position=${state.position.inMilliseconds}',
    );
    return true;
  }

  Future<void> pause() async {
    if (!_active) {
      return;
    }
    await usbAudioService.pauseExclusivePlayback();
  }

  Future<void> resume() async {
    if (!_active) {
      return;
    }
    await usbAudioService.resumeExclusivePlayback();
  }

  Future<void> seek(Duration position) async {
    if (!_active) {
      return;
    }
    await usbAudioService.seekExclusivePlayback(position);
  }

  /// Stops the session. [intentional] marks a stop we asked for (track change,
  /// explicit stop, switch off) so the state listener does not treat the loss of
  /// activity as an unexpected interruption and resume the track elsewhere.
  Future<void> stop({bool intentional = true}) async {
    _generation++;
    _cancelVolumeRamp();
    _cancelOutputGainRamp();
    final previousIntentionalStop = _intentionalStop;
    _intentionalStop = intentional;
    try {
      await usbAudioService.stopExclusivePlayback();
    } finally {
      _intentionalStop = previousIntentionalStop;
    }
    _active = false;
    _activeSongId = null;
    _position = Duration.zero;
    _duration = null;
    _playing = false;
  }

  /// Applies a user volume (0..1) to the live exclusive output, ramping up so a
  /// jump cannot clip. When no session is live the value is only remembered for
  /// the device, to be used the next time it takes over.
  Future<void> applyVolume(double userVolume) async {
    final target = userVolume.clamp(0.0, 1.0).toDouble();
    if (!_active) {
      _rememberVolume(target);
      setting.save();
      return;
    }
    _volumeRampTarget = target;
    if (target <= _appliedUserVolume) {
      _cancelVolumeRamp();
      await _applyUserVolume(target);
      return;
    }
    await _applyUserVolume(nextSafeUsbVolume(_appliedUserVolume, target));
    if (_appliedUserVolume + 0.000001 >= _volumeRampTarget) {
      return;
    }
    _volumeRampTimer ??= Timer.periodic(const Duration(milliseconds: 100), (_) {
      unawaited(
        _applyUserVolume(nextSafeUsbVolume(_appliedUserVolume, _volumeRampTarget)),
      );
      if (_appliedUserVolume + 0.000001 >= _volumeRampTarget) {
        _cancelVolumeRamp();
        setting.save();
      }
    });
  }

  /// The engine-side volume the current device is remembered with, so a
  /// reconnect restores what the user set rather than the default.
  void _rememberVolume(double volume) {
    final next = volume.clamp(0.0, 1.0).toDouble();
    usbExclusiveVolumeNotifier.value = next;
    usbAudioPreferences.setVolumeForDevice(_usbVolumeDeviceKey, next);
  }

  /// Remembers [volume] for the connected device without a live session, so the
  /// next takeover on that DAC starts where the user left it.
  void rememberVolume(double volume) {
    _handleAudioStatus();
    _rememberVolume(volume.clamp(0.0, 1.0).toDouble());
    setting.save();
  }

  Future<void> _applyUserVolume(
    double volume, {
    double maxIncreaseDb = _safeUsbVolumeIncreaseDb,
  }) async {
    final next = volume.clamp(0.0, 1.0).toDouble();
    _appliedUserVolume = next;
    _rememberVolume(next);
    if ((volumeNotifier.value - next).abs() > 0.000001) {
      volumeNotifier.value = next;
    }
    await _applyExclusiveVolume(
      usbExclusiveDigitalVolumeGain(next),
      maxIncreaseDb: maxIncreaseDb,
    );
  }

  Future<void> _applyExclusiveVolume(
    double digitalGain, {
    double maxIncreaseDb = _safeUsbVolumeIncreaseDb,
  }) async {
    if (!_active) {
      return;
    }
    final isDsd = currentSongNotifier.value?.isDsd == true;
    final dsdCompensationDb = isDsd
        ? usbAudioPreferences.dsdGainCompensationNotifier.value
        : 0;
    final transition = safeOutputGainTransition(
      appliedGain: _appliedOutputGain,
      userGain: digitalGain,
      adjustmentDb: _effectiveReplayGainDb(digitalGain) + dsdCompensationDb,
      maxIncreaseDb: maxIncreaseDb,
    );
    _appliedOutputGain = transition.appliedGain;
    final mode = usbAudioPreferences.volumeControlModeNotifier.value.name;
    // Logged because whether a DAC applies the gain depends on the mode it was
    // asked for: `auto` may pick the device's own volume, which a dongle
    // without a volume protocol cannot honour - the log is what tells the two
    // apart when the slider appears to do nothing.
    logger.output(
      'usb exclusive volume: user=${digitalGain.toStringAsFixed(4)} '
      'gain=${transition.appliedGain.toStringAsFixed(4)} mode=$mode '
      'replayGainDb=${(transition.adjustmentDb - dsdCompensationDb).toStringAsFixed(2)}',
    );
    await usbAudioService.setExclusiveVolume(
      gain: digitalGain,
      replayGainDb: transition.adjustmentDb - dsdCompensationDb,
      mode: mode,
      dsdGainCompensationDb: dsdCompensationDb,
      smoothHandoff: usbAudioPreferences.volumeSmoothHandoffNotifier.value,
    );
    if (_active) {
      _publishReplayGainState(usbExclusivePlaybackStateNotifier.value);
    }
    if (transition.needsRamp) {
      _scheduleOutputGainRamp(maxIncreaseDb);
    } else {
      _cancelOutputGainRamp();
    }
  }

  void _reapplyVolume() {
    unawaited(
      _applyExclusiveVolume(usbExclusiveDigitalVolumeGain(volumeNotifier.value)),
    );
  }

  void _handleReplayGainModeChanged() {
    final song = currentSongNotifier.value;
    _replayGain = song == null
        ? const ReplayGainResult(0, null, null)
        : replayGainFor(
            song,
            usbAudioPreferences.replayGainModeNotifier.value,
            fallbackDb: usbAudioPreferences.replayGainFallbackDbNotifier.value
                .toDouble(),
          );
    if (_active) {
      _reapplyVolume();
    }
  }

  double _effectiveReplayGainDb(double userLinearGain) {
    return replayGainWithinOutputHeadroom(_replayGain.gainDb, userLinearGain);
  }

  void _handleExclusiveState() {
    final state = usbExclusivePlaybackStateNotifier.value;
    final wasActive = _active;
    final interruptedPosition = trustedUsbExclusivePosition(
      current: _position,
      reported: state.position,
      stateActive: state.active,
    );
    _position = interruptedPosition;
    _duration = state.duration ?? _duration;
    _active = state.active;
    _playing = state.playing;

    if (wasActive && !state.active) {
      _cancelVolumeRamp();
      _cancelOutputGainRamp();
    }

    if (state.active) {
      if (!wasActive &&
          state.hardwareVolumeActive &&
          state.hardwareVolumeReadbackVerified &&
          state.hardwareVolumeGainQ16 != null) {
        _syncHardwareVolume(
          gainQ16: state.hardwareVolumeGainQ16!,
          replayGainDb: state.replayGainMilliDb / 1000,
          isDsd: currentSongNotifier.value?.isDsd == true,
        );
      }
      // The start request already carries a safe starting gain; keep ramping it
      // up on a fixed cadence once the session is live.
      if (!wasActive) {
        _scheduleOutputGainRamp();
      }
      _onPosition?.call(state.position);
      if (isPlayingNotifier.value != state.playing) {
        _onPlayingChanged?.call(state.playing);
      }
      return;
    }

    if (!wasActive) {
      return;
    }

    _activeSongId = null;
    if (state.message?.contains('completed') == true) {
      logger.output('usb exclusive completed -> auto advance');
      _onSongFinished?.call();
      return;
    }
    if (shouldStartUsbOutputHandoff(
      wasActive: wasActive,
      intentionalStop: _intentionalStop,
      handoffInProgress: _handoffInProgress,
      completed: false,
    )) {
      logger.output(
        'usb exclusive interrupted -> fallback to shared output:${state.message}',
      );
      unawaited(_handoffToMpv(interruptedPosition));
    }
  }

  Future<void> _handoffToMpv(Duration position) async {
    if (_handoffInProgress) {
      return;
    }
    if (currentSongNotifier.value == null) {
      return;
    }
    _handoffInProgress = true;
    try {
      final handoff = onFallbackToMpv ?? _handOffToPlayer;
      handoff?.call(position);
    } catch (error) {
      logger.output('usb exclusive handoff failed:$error');
    } finally {
      _handoffInProgress = false;
    }
  }

  void _handleAudioStatus() {
    final status = usbAudioStatusNotifier.value;
    if (status.activeDeviceId != null) {
      _usbAudioDeviceId = status.activeDeviceId;
    }
    final deviceKey = usbExclusiveVolumeDeviceKey(
      status.vendorId,
      status.productId,
    );
    if (deviceKey != null) {
      _usbVolumeDeviceKey = deviceKey;
    }
    if (!_active &&
        status.hasConnectedUsbAudioDevice &&
        _usbVolumeDeviceKey != null) {
      usbExclusiveVolumeNotifier.value = usbAudioPreferences.volumeForDevice(
        _usbVolumeDeviceKey,
      );
    }
  }

  void _handleAudioEvent() {
    final event = usbAudioEventNotifier.value;
    if (event == null) {
      return;
    }
    if (event.type == UsbAudioDeviceEventType.added) {
      // A DAC arrived while the switch is on: ask the handler to take the
      // current song over at its current position, so plugging in mid-playback
      // does not need a manual re-play. When a session is already live (or the
      // switch is off) the handler's own guards make this a no-op.
      if (enabled && !_active) {
        _onDeviceInserted?.call();
      }
      return;
    }
    if (event.deviceId != _usbAudioDeviceId || !_active) {
      return;
    }
    // Unplugging invalidates the session; the state listener then hands the
    // shared player the last known position, keeping the play/pause state.
    usbAudioService.markExclusiveDeviceRemoved(position: _position);
  }

  void _handleVolumeKey() {
    final value = usbExclusiveVolumeKeyNotifier.value;
    final delta = value - _lastVolumeKeyValue;
    _lastVolumeKeyValue = value;
    final direction = usbExclusiveVolumeKeyDirection(
      delta: delta,
      active: _active,
    );
    if (direction == null) {
      return;
    }
    unawaited(_applyPhoneVolumeKey(direction));
  }

  /// A physical volume-key press: apply the step at once (no ramp) so the key
  /// feels direct, allowing a bigger single step than the slider's ramp.
  Future<void> _applyPhoneVolumeKey(AndroidVolumeDirection direction) async {
    try {
      await _setUserVolumeImmediately(
        adjustedRemoteVolume(volumeNotifier.value, direction),
      );
      usbVolumeOverlayNotifier.value += 1;
    } on Object catch (error) {
      logger.output('usb volume key apply failed:$error');
    }
  }

  Future<void> _setUserVolumeImmediately(double volume) {
    _cancelVolumeRamp();
    _volumeRampTarget = volume.clamp(0.0, 1.0).toDouble();
    final write = _applyUserVolume(
      _volumeRampTarget,
      maxIncreaseDb: _phoneUsbVolumeStepDb,
    );
    setting.save();
    return write;
  }

  void _handleHardwareVolume() {
    final event = usbHardwareVolumeNotifier.value;
    final state = usbExclusivePlaybackStateNotifier.value;
    if (!_active ||
        !state.active ||
        event == null ||
        event.playbackId != state.playbackId ||
        event.protocol != state.hardwareVolumeProtocol) {
      return;
    }
    _syncHardwareVolume(
      gainQ16: event.gainQ16,
      replayGainDb: event.replayGainMilliDb / 1000,
      isDsd: event.isDsd,
      dsdGainCompensationDb: event.dsdGainCompensationDb,
      showOverlay: true,
    );
  }

  void _syncHardwareVolume({
    required int gainQ16,
    required double replayGainDb,
    required bool isDsd,
    int dsdGainCompensationDb = 0,
    bool showOverlay = false,
  }) {
    final volume = usbUserVolumeFromHardwareGain(
      gainQ16,
      replayGainDb,
      isDsd ? dsdGainCompensationDb : 0,
    );
    _appliedOutputGain = (gainQ16 / 65536).clamp(0.0, 1.0).toDouble();
    _cancelVolumeRamp();
    _cancelOutputGainRamp();
    _handleAudioStatus();
    _appliedUserVolume = volume;
    _volumeRampTarget = volume;
    _rememberVolume(volume);
    if ((volumeNotifier.value - volume).abs() > 0.000001) {
      volumeNotifier.value = volume;
    }
    setting.save();
    if (showOverlay) {
      usbVolumeOverlayNotifier.value += 1;
    }
  }

  void _prepareExclusiveVolume() {
    _handleAudioStatus();
    final volume = usbAudioPreferences.volumeForDevice(_usbVolumeDeviceKey);
    _cancelVolumeRamp();
    _appliedUserVolume = volume;
    _volumeRampTarget = volume;
    usbExclusiveVolumeNotifier.value = volume;
    volumeNotifier.value = volume;
  }

  void _restoreExclusiveVolume() {
    _cancelVolumeRamp();
    _cancelOutputGainRamp();
  }

  void _scheduleOutputGainRamp([
    double maxIncreaseDb = _safeUsbVolumeIncreaseDb,
  ]) {
    _outputGainRampStepDb = maxIncreaseDb;
    _outputGainRampTimer ??= Timer.periodic(const Duration(milliseconds: 100), (
      _,
    ) {
      if (_active) {
        unawaited(
          _applyExclusiveVolume(
            usbExclusiveDigitalVolumeGain(volumeNotifier.value),
            maxIncreaseDb: _outputGainRampStepDb,
          ),
        );
      }
    });
  }

  void _cancelOutputGainRamp() {
    _outputGainRampTimer?.cancel();
    _outputGainRampTimer = null;
    _outputGainRampStepDb = _safeUsbVolumeIncreaseDb;
  }

  void _cancelVolumeRamp() {
    _volumeRampTimer?.cancel();
    _volumeRampTimer = null;
    _volumeRampTarget = _appliedUserVolume;
  }

  void _publishReplayGainState(UsbExclusivePlaybackState state) {
    if (!state.active) {
      return;
    }
    final generation = ++_replayGainApplyGeneration;
    final path = state.hardwareVolumeActive
        ? ReplayGainOutputPath.usbHardware
        : state.digitalVolumeActive
        ? ReplayGainOutputPath.usbDigital
        : ReplayGainOutputPath.none;
    final pending = _pendingReplayGainState(path, generation);
    if (pending.phase != ReplayGainApplyPhase.pending) {
      replayGainPlaybackStateNotifier.value = pending;
      return;
    }
    if (path == ReplayGainOutputPath.none) {
      replayGainPlaybackStateNotifier.value = pending.failed();
      return;
    }
    replayGainPlaybackStateNotifier.value = pending.applied(
      actualDb: state.replayGainMilliDb / 1000,
    );
  }

  ReplayGainPlaybackState _pendingReplayGainState(
    ReplayGainOutputPath path,
    int generation,
  ) {
    final mode = usbAudioPreferences.replayGainModeNotifier.value;
    if (mode == ReplayGainMode.off) {
      return ReplayGainPlaybackState.off();
    }
    if (_replayGain.source == null && _replayGain.gainDb == 0) {
      return ReplayGainPlaybackState.noTag();
    }
    return ReplayGainPlaybackState.pending(
      selectedDb: _replayGain.gainDb,
      path: path,
      generation: generation,
      isFallback: _replayGain.source == null,
    );
  }

  bool _shouldTry(MyAudioMetadata song) {
    if (!Platform.isAndroid) {
      logger.output('usb exclusive skipped: not android');
      return false;
    }
    return true;
  }

  /// The output format handed to the engine, normalised from the tag or the
  /// file extension: the engine keys its decoder on a small set of names.
  String? _normalizedFormat(MyAudioMetadata song) {
    final format = song.format?.toLowerCase().trim();
    if (format != null && format.isNotEmpty) {
      if (format.contains('flac')) return 'flac';
      // WavPack has to be tested before wav, or contains('wav') eats it.
      if (format.contains('wavpack') || format == 'wv') return 'wv';
      if (format.contains('wav') || format.contains('wave')) return 'wav';
      if (format.contains('dsf')) return 'dsf';
      if (format.contains('dff')) return 'dff';
      return format;
    }

    final path = (song.path ?? song.cachePath ?? '').toLowerCase();
    if (path.endsWith('.flac')) return 'flac';
    if (path.endsWith('.wv')) return 'wv';
    if (path.endsWith('.wav') || path.endsWith('.wave')) return 'wav';
    if (path.endsWith('.dsf')) return 'dsf';
    if (path.endsWith('.dff')) return 'dff';
    return null;
  }

  /// Estimated full size of a still-downloading stream: duration × bitrate,
  /// padded so the engine's extractor can seek a growing .part correctly.
  /// Unknown duration returns null (engine falls back to its old behaviour).
  int? _estimateStreamTotalBytes(MyAudioMetadata song) {
    final durationSec = song.duration?.inSeconds ?? 0;
    if (durationSec <= 0) {
      return null;
    }
    final bytesPerSecond =
        ((song.bitrate ?? 0) > 0 ? song.bitrate! : 2000) * 1000 ~/ 8;
    return (durationSec * bytesPerSecond * 1.2).toInt();
  }

  /// Resolves the file the engine reads: a local file (through the redirect if
  /// the source needs one), an already cached file, or - for a cloud track with
  /// no cache yet - a .part file once enough of it has downloaded to stream.
  Future<String?> _playablePath(
    MyAudioMetadata song, {
    int? generation,
  }) async {
    if (sourceType == .local && song.path != null) {
      return await covertToRedirectPathIfNeed(song.path!) ?? song.path;
    }

    if (song.cacheExist && song.cachePath != null) {
      return song.cachePath;
    }

    // Not cached: start the cache download in the background and, if enough
    // arrives, stream the growing .part; otherwise fall back to shared output
    // immediately rather than leaving the user waiting silently.
    unawaited(
      library.tryAddCache(song).catchError((Object error) {
        logger.output('usb exclusive cache failed:$error');
      }),
    );

    // Formats the engine can stream while downloading; wv/ape have no system
    // decoder and so wait for the cache to finish.
    const streamableExts = {
      'flac',
      'wav',
      'wave',
      'dsf',
      'dff',
      'mp3',
      'm4a',
      'm4b',
      'mp4',
      'aac',
      'ogg',
      'oga',
      'opus',
    };
    final ext = _normalizedFormat(song)?.toLowerCase();
    if (!streamableExts.contains(ext)) {
      return null;
    }

    final partPath = '${song.cachePath!}.part';
    final bytesPerSecond =
        ((song.bitrate ?? 0) > 0 ? song.bitrate! : 2000) * 1000 ~/ 8;
    final startBytes = bytesPerSecond * 10;
    final waitDeadline = DateTime.now().add(const Duration(seconds: 15));
    var lastSize = -1;
    while (true) {
      if (generation != null && generation != _generation) {
        return null;
      }
      if (DateTime.now().isAfter(waitDeadline)) {
        logger.output('usb exclusive streaming watermark timeout');
        return null;
      }
      if (song.cacheExist && song.cachePath != null) {
        return song.cachePath;
      }
      final part = File(partPath);
      final size = part.existsSync() ? part.lengthSync() : 0;
      // Need a watermark plus a measured advance, so a stale .part left from an
      // interrupted download is not mistaken for fresh data.
      if (lastSize >= 0 &&
          size >= startBytes &&
          size - lastSize >= bytesPerSecond ~/ 5) {
        logger.output('usb exclusive streaming start: $size bytes');
        return partPath;
      }
      lastSize = size;
      await Future.delayed(const Duration(milliseconds: 200));
    }
  }

  int? _preferredSampleRate(MyAudioMetadata song) {
    // A fixed requested rate overrides the source rate; adaptive keeps the
    // source rate, which the engine packs straight to the DAC without resampling.
    final preferred = usbAudioPreferences.outputSampleRateNotifier.value;
    if (preferred != 0) {
      return UsbAudioPreferences.sampleRates.contains(preferred)
          ? preferred
          : null;
    }
    final source = song.samplerate;
    if (source == null || source <= 0) {
      return null;
    }
    return UsbAudioPreferences.sampleRates.contains(source) ? source : null;
  }

  /// Re-pushes the target buffer for the new lifecycle state while a session is
  /// live, so the engine's output window matches what foreground/background
  /// expects (a background session gets a deeper buffer to survive throttling).
  Future<void> applyLifecycleBuffer(AppLifecycleState? state) async {
    if (!_active) {
      return;
    }
    await usbAudioService.setExclusiveTargetBufferMs(
      _targetBufferMsForLifecycle(state),
    );
  }

  int _targetBufferMsForLifecycle(AppLifecycleState? state) {
    return preferredUsbExclusiveTargetBufferMs(
      background: switch (state) {
        AppLifecycleState.resumed => false,
        AppLifecycleState.inactive ||
        AppLifecycleState.hidden ||
        AppLifecycleState.paused ||
        AppLifecycleState.detached => true,
        null => false,
      },
    );
  }

  @visibleForTesting
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _cancelVolumeRamp();
    _cancelOutputGainRamp();
    usbExclusivePlaybackStateNotifier.removeListener(_handleExclusiveState);
    usbAudioStatusNotifier.removeListener(_handleAudioStatus);
    usbAudioEventNotifier.removeListener(_handleAudioEvent);
    usbExclusiveVolumeKeyNotifier.removeListener(_handleVolumeKey);
    usbHardwareVolumeNotifier.removeListener(_handleHardwareVolume);
  }
}