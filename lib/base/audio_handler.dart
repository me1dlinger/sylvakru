import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'package:audio_service/audio_service.dart';
import 'package:audio_session/audio_session.dart';
import 'package:flutter/services.dart';
import 'package:material_ui/material_ui.dart';
import 'package:media_kit/media_kit.dart';
import 'package:sylvakru/base/data/loader.dart';
import 'package:sylvakru/base/data/playlist.dart';
import 'package:sylvakru/base/services/background_task_service.dart';
import 'package:sylvakru/base/services/home_widget_service.dart';
import 'package:sylvakru/base/services/interaction.dart';
import 'package:sylvakru/base/services/my_window_listener.dart';
import 'package:sylvakru/base/services/picture_service.dart';
import 'package:sylvakru/base/services/play_queue_logic.dart';
import 'package:sylvakru/base/services/stream_relay.dart';
import 'package:sylvakru/base/services/stream_client.dart';
import 'package:sylvakru/base/services/replay_gain.dart';
import 'package:sylvakru/base/services/usb_audio_preferences.dart';
import 'package:sylvakru/base/services/usb_exclusive_controller.dart';
import 'package:sylvakru/base/services/taskbar_service.dart';
import 'package:sylvakru/base/services/webdav_client.dart';
import 'package:sylvakru/base/services/color_manager.dart';
import 'package:sylvakru/base/app.dart';
import 'package:sylvakru/base/services/logger.dart';
import 'package:sylvakru/base/services/lyric.dart';
import 'package:sylvakru/base/services/media_session_item.dart';
import 'package:sylvakru/base/services/media_art.dart';
import 'package:sylvakru/base/utils/dynamic_lyrics_page_route.dart';
import 'package:sylvakru/base/utils/common_utils.dart';
import 'package:sylvakru/base/utils/path.dart';
import 'package:sylvakru/base/widgets/equalizer.dart';
import 'package:sylvakru/base/widgets/lyric_list_view.dart';
import 'package:sylvakru/base/data/history.dart';
import 'package:sylvakru/base/data/stats.dart';
import 'package:sylvakru/layer/layers_manager.dart';
import 'package:sylvakru/base/utils/contrast_color_generator.dart';
import 'package:sylvakru/base/data/library.dart';
import 'package:sylvakru/base/my_audio_metadata.dart';
import 'package:sylvakru/base/utils/metadata_utils.dart';
import 'package:sylvakru/layer/lyrics_page_layer.dart';
import 'dart:async';

import 'package:sylvakru/portrait_view/sleep_timer.dart';

late AudioSession _session;

late MyAudioHandler audioHandler;

List<MyAudioMetadata> playQueue = [];
String? playQueueForStreamId;

final ValueNotifier<MyAudioMetadata?> currentSongNotifier = ValueNotifier(null);
final isPlayingNotifier = ValueNotifier(false);
final playModeNotifier = ValueNotifier(0);
final volumeNotifier = ValueNotifier(0.3);

/// The volume the USB exclusive output applies while it owns the DAC (0..1),
/// kept beside the media volume so the settings page, the overlay and the
/// output engine all read the same value.
final usbExclusiveVolumeNotifier = ValueNotifier(
  UsbAudioPreferences.defaultExclusiveVolume,
);

/// How much louder a single step may make the output, and how big a step the
/// phone's volume keys are for the USB exclusive output.
const _safeUsbVolumeIncreaseDb = 1.0;
const _phoneUsbVolumeStep = 0.02;

/// The gain a user volume of [db] corresponds to.
double dbToUsbVolumeRatio(double db) => math.pow(10, db / 30).toDouble();

/// The next volume to apply when moving from [applied] towards [requested],
/// limited so a jump cannot clip: raising is spread over [limitedOutputGainIncrease].
double nextSafeUsbVolume(double applied, double requested) {
  final current = applied.clamp(0.0, 1.0).toDouble();
  final target = requested.clamp(0.0, 1.0).toDouble();
  if (target <= current) {
    return target;
  }
  final nextGain = limitedOutputGainIncrease(
    appliedGain: usbExclusiveDigitalVolumeGain(current),
    targetGain: usbExclusiveDigitalVolumeGain(target),
    maxIncreaseDb: _safeUsbVolumeIncreaseDb,
  );
  return math.pow(nextGain, 2 / 3).toDouble().clamp(0.0, 1.0);
}

/// One volume-key step for the USB exclusive output.
double adjustedRemoteVolume(double current) {
  final applied = current.clamp(0.0, 1.0).toDouble();
  return (applied + _phoneUsbVolumeStep).clamp(0.0, 1.0).toDouble();
}

final autoPlayOnStartupNotifier = ValueNotifier(false);

final currentLyricsIndexNotifier = ValueNotifier(-1);

final controlCenterLyricsNotifier = ValueNotifier(false);

bool loadingSong = false;

// Android side helper that turns an app-private cover file into a
// content:// URI (see MainActivity.kt / the media_provider FileProvider), so
// the media session can expose artwork that other processes can read.
const _mediaChannel = MethodChannel('com.afalphy.media');

// cover file path -> a URI that other processes can fetch, so a track's media
// item can be published synchronously once it has been resolved once.
final Map<String, String> _coverUriCache = {};

// Covers that could not be exposed, with the modification time of the file
// that failed: the same file is not retried, but a cover written since (which
// is what happens the first time a local song's artwork is extracted) is.
final Map<String, DateTime> _coverUriFailed = {};

Future<void> initAudioService() async {
  MediaKit.ensureInitialized();
  audioHandler = await AudioService.init(
    builder: () => MyAudioHandler(),

    config: const AudioServiceConfig(
      androidNotificationChannelId: 'com.afalphy.sylvakru',
      androidNotificationChannelName: 'Sylvakru',
      androidNotificationOngoing: true,
    ),
  );
  _session = await AudioSession.instance;
  await _session.configure(AudioSessionConfiguration.music());

  await _session.setActive(true);

  // Start the LAN cover relay now, so the first track can be published with a
  // car-fetchable artwork URI straight away instead of a beat later.
  unawaited(streamRelay.warmUp());

  _session.becomingNoisyEventStream.listen((_) {
    audioHandler.pause();
  });

  _session.interruptionEventStream.listen((event) {
    if (event.begin) {
      audioHandler.pause();
    } else {
      audioHandler.play();
    }
  });

  if (isMobile) {
    const channel = MethodChannel('com.afalphy.audio_control');

    channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'toggleFavorite':
          if (currentSongNotifier.value != null) {
            // iOS only: hold background runtime so the command is not cut off
            // when the widget tap woke the app in the background.
            await audioHandler.beginWidgetCommandGrace();
            toggleFavoriteState(currentSongNotifier.value!);
          }
        case 'skipToPrevious':
          audioHandler.skipToPrevious();
        case 'togglePlay':
          await audioHandler.beginWidgetCommandGrace();
          audioHandler.togglePlay();
        case 'skipToNext':
          audioHandler.skipToNext();
        default:
      }
    });
  }
}

class MyAudioHandler extends BaseAudioHandler {
  final _player = Player();
  bool _started = false;
  int currentIndex = -1;
  List<MyAudioMetadata> _playQueueTmp = [];
  int _tmpPlayMode = 0;
  DateTime? _playLastSyncTime;
  Duration _playedDuration = Duration.zero;

  File? _playQueueState;
  late File _playState;
  late File _equalizerState;
  late File _positionState;

  Timer? _positionTimer;

  // Progress is fed from whichever output owns the sound: the shared player
  // when it is playing, the exclusive controller otherwise. A single stream
  // keeps the seekbar and media session independent of which path is live.
  final _positionController = StreamController<Duration>.broadcast();
  final _durationController = StreamController<Duration>.broadcast();

  // Set while the shared player is stopped on purpose as the DAC is handed to
  // the exclusive engine, so the stop's completed event is not read as the
  // track finishing and advancing the queue.
  bool _suppressPlayerCompleted = false;

  // Last lyric line pushed to the system media session, so the notification is
  // only refreshed when the displayed line actually changes.
  String? _publishedLyric;

  // Artwork announced for the song currently in the media session. Kept per
  // song so a lyric-line update never drops the artwork again (that is what made
  // a cover appear and then vanish), and so a track that is switched back to
  // needs no work.
  String? _pubSongId;
  String? _pubFileArtUri;
  String? _pubCoverPath;
  String? _pubSmallArtPath;
  String? _pubFetchArtUri;

  // Work already started for a song, so a burst of publishes does not queue the
  // same download or URI resolution twice.
  final Set<String> _coverLoadInFlight = {};
  final Set<String> _fetchUriInFlight = {};
  final Set<String> _smallArtInFlight = {};

  DateTime? _songSwitchedAt;

  // iOS background-execution grace for the current load (see _beginLoadGrace).
  StreamSubscription<bool>? _loadGraceSub;
  int? _loadGraceToken;
  bool _loadGraceSawBuffering = false;

  // iOS background-execution grace for widget-button commands.
  int? _widgetCommandToken;
  Timer? _widgetCommandTimer;
  StreamSubscription<Duration>? _widgetCommandSub;

  // iOS background-execution grace held while mpv is buffering, including
  // mid-song stalls on weak network.
  int? _stallGraceToken;
  bool _stallGraceArming = false;

  MyAudioHandler() {
    // avoid reading .lrc files
    (_player.platform as NativePlayer).setProperty('sub-auto', 'no');

    if (Platform.isIOS) {
      (_player.platform as NativePlayer).setProperty('audio-exclusive', 'yes');
    }

    _player.stream.error.listen((onData) {
      logger.output("player error:$onData");
    });

    _player.stream.completed.listen((completed) async {
      if (_suppressPlayerCompleted) {
        return;
      }
      if (completed) {
        final position = _player.state.position;
        final duration = _player.state.duration;

        // fake completed
        if ((duration - position).inSeconds > 2) {
          await pause();
          return;
        }

        if (Loader.busy) {
          await pause();
          return;
        }
        if (playModeNotifier.value == 2) {
          // repeat
          await load();
        } else {
          await skipToNext(); // automatically go to next song
        }
      }
    });

    currentSongNotifier.addListener(() {
      if (viewModeNotifier.value == .bigPicture) {
        if (useCurrentSongForBg) {
          colorManager.updateBigPictureRelatedColors(
            currentSongNotifier.value?.picture,
          );
        }
        return;
      }
      layersManager.updateBackground();
    });

    _player.stream.position.listen(_updateCurrentLyricIndex);

    // While the exclusive engine owns the output the shared player is stopped,
    // so its streams would go silent; the controller feeds progress instead.
    _player.stream.position.listen((position) {
      if (!usbExclusiveController.active) {
        _positionController.add(position);
      }
    });
    _player.stream.duration.listen((duration) {
      if (!usbExclusiveController.active) {
        _durationController.add(duration);
      }
    });

    usbExclusiveController.attach(
      handOffToPlayer: _openMpvAtPosition,
      onSongFinished: _handleExclusiveSongFinished,
      onPosition: (position) {
        _positionController.add(position);
        updatePlaybackState(postion: position);
        _updateCurrentLyricIndex(position);
      },
      onPlayingChanged: (playing) {
        updateIsPlaying(playing);
        updatePlaybackState();
      },
    );

    // iOS: hold a background task for the whole buffering phase (including
    // mid-song stalls on weak network) so the process is not suspended before
    // buffering completes. core-idle also reports true on stop/pause, so only
    // arm while a song is actually playing or loading.
    _player.stream.buffering.listen((buffering) {
      if (buffering) {
        _beginStallGrace();
      } else {
        _endStallGrace();
      }
    });
  }

  void _updateCurrentLyricIndex(Duration position) {
    final currentSong = currentSongNotifier.value;
    if (currentSong == null ||
        loadingSong ||
        position < Duration.zero ||
        currentSongNotifier.value!.parsedLyrics == null) {
      return;
    }
    int tmp = currentLyricsIndexNotifier.value;
    final lines = currentSong.parsedLyrics!.lines;

    position += Duration(milliseconds: currentSong.lyricsTimeOffset);

    if (tmp >= 0 &&
        tmp + 1 < lines.length &&
        position >= lines[tmp].start &&
        position <= lines[tmp + 1].start) {
      return;
    }

    int current = -1;
    if (position >= lines.last.start) {
      current = lines.length - 1;
    } else {
      for (int i = 0; i < lines.length; i++) {
        final line = lines[i];
        if (position < line.start) {
          break;
        }
        if (current == -1 || line.start > lines[current].start) {
          current = i;
        }
      }
    }

    if (current != currentLyricsIndexNotifier.value) {
      currentLyricsIndexNotifier.value = current;

      // Publish the current lyric line to the system media session: the Android
      // media notification and Bluetooth (AVRCP) head units read the now
      // playing metadata from there, so the lyric has to be written into the
      // media item itself.
      final lineText = (current >= 0 && current < lines.length)
          ? lines[current].text
          : null;
      if (lineText != _publishedLyric) {
        updateServiceMediaItem(currentSong, lyric: lineText);
        updatePlaybackState();
      }

      if (isMobile) {
        HomeWidgetService.updateLyricsIndex();
      }
    }
  }

  Future<void> updateIsPlaying(bool isPlaying) async {
    if (isPlaying) {
      _playLastSyncTime = DateTime.now();
    } else if (_playLastSyncTime != null) {
      _playedDuration += DateTime.now().difference(_playLastSyncTime!);
      _playLastSyncTime = null;
    }
    needPause = false;
    isPlayingNotifier.value = isPlaying;
    if (Platform.isWindows) {
      if (!windowIsClosed) {
        setupTaskbar();
      }
    } else if (isMobile) {
      await HomeWidgetService.updateIsPlaying();
    }
  }

  void updatePlaybackState({Duration? postion, bool stop = false}) {
    playbackState.add(
      PlaybackState(
        controls: [
          MediaControl.skipToPrevious,
          isPlayingNotifier.value ? MediaControl.pause : MediaControl.play,
          MediaControl.skipToNext,
        ],
        systemActions: {MediaAction.seek},
        playing: isPlayingNotifier.value,
        processingState: stop ? .idle : .ready,
        speed: usbExclusiveController.active ? 1.0 : _player.state.rate,
        updatePosition: postion ?? getPosition(),
      ),
    );
  }

  void _prepare() {
    _playQueueState = File(
      "${appSupportDir.path}/${sourceType.name}/play_queue_state.json",
    );
    if (!(_playQueueState!.existsSync())) {
      _playQueueState!.createSync(recursive: true);
      _savePlayQueueState();
    }

    _playState = File(
      "${appSupportDir.path}/${sourceType.name}/play_state.json",
    );
    if (!(_playState.existsSync())) {
      _playState.createSync(recursive: true);
      savePlayState();
    }
    _equalizerState = File("${appSupportDir.path}/equalizer_state.json");
    if (!(_equalizerState.existsSync())) {
      saveEqualizerState();
    }

    _positionState = File(
      "${appSupportDir.path}/${sourceType.name}/position_state.json",
    );
    if (!(_positionState.existsSync())) {
      _positionState.createSync(recursive: true);
      _positionState.writeAsString(Duration.zero.inMilliseconds.toString());
    }
  }

  List<MyAudioMetadata> _restoreQueue(List<dynamic>? rawList) {
    final result = <MyAudioMetadata>[];

    for (final id in rawList ?? []) {
      final song = library.id2Song[id];
      if (song != null) result.add(song);
    }

    return result;
  }

  Future<void> loadStates() async {
    _prepare();
    await _loadPlayState();
    await _loadPlayQueueState();
    await _loadEqualizerState();
    await _tryPlay();
  }

  Future<void> _loadPlayQueueState() async {
    final content = await _playQueueState!.readAsString();

    final json = jsonDecode(content) as Map<String, dynamic>;

    _playQueueTmp.addAll(_restoreQueue(json['playQueueTmp']));
    playQueue.addAll(_restoreQueue(json['playQueue']));
  }

  Future<void> _savePlayQueueState() async {
    _playQueueState!.writeAsStringSync(
      jsonEncode({
        'playQueueTmp': _playQueueTmp.map((e) => e.id).toList(),
        'playQueue': playQueue.map((e) => e.id).toList(),
      }),
    );
  }

  Future<void> _tryPlay() async {
    if (!_started && autoPlayOnStartupNotifier.value) {
      if (playQueue.isEmpty) {
        currentIndex = 0;
        playQueue = List.from(library.songList);
      }
      if (playQueue.isNotEmpty) {
        isPlayingNotifier.value = true;
      } else {
        currentIndex = -1;
      }
    }

    if (playQueue.isNotEmpty) {
      // reload may make some songs not in the library to be removed
      if (currentIndex == -1 || currentIndex >= playQueue.length) {
        currentIndex = 0;
      }

      final positionMs = await _positionState.readAsString();

      await load(start: Duration(milliseconds: int.tryParse(positionMs) ?? 0));

      if (isPlayingNotifier.value) {
        _positionTimer ??= Timer.periodic(Duration(seconds: 1), (_) {
          _positionState.writeAsString(getPosition().inMilliseconds.toString());
        });
      }
    }

    if (!_started) {
      _started = true;
      if (autoPlayOnStartupNotifier.value) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (currentSongNotifier.value != null) {
            globalNavigatorKey.currentState?.push(
              DynamicLyricsPageRoute(
                pageBuilder: (_, _, _) => LyricsPageLayer(),
              ),
            );
          }
        });
      }
    }
  }

  Future<void> _loadPlayState() async {
    final content = await _playState.readAsString();
    final Map<String, dynamic> json =
        jsonDecode(content) as Map<String, dynamic>;

    currentIndex = json['currentIndex'] as int? ?? -1;
    playModeNotifier.value = json['playMode'] as int? ?? 0;
    _tmpPlayMode = json['tmpPlayMode'] as int? ?? 0;

    volumeNotifier.value = json['volume'] as double? ?? 0.3;

    if (!isMobile) {
      setVolume(volumeNotifier.value);
    }
  }

  void savePlayState() {
    _playState.writeAsStringSync(
      jsonEncode({
        'currentIndex': currentIndex,
        'playMode': playModeNotifier.value,
        'tmpPlayMode': _tmpPlayMode,
        'volume': volumeNotifier.value,
      }),
    );
  }

  Future<void> _loadEqualizerState() async {
    if (!isPremiumNotifier.value) {
      return;
    }
    final content = await _equalizerState.readAsString();
    gains = (jsonDecode(content) as List<dynamic>).cast();
    await applyEqualizer();
  }

  void saveEqualizerState() {
    _equalizerState.writeAsStringSync(jsonEncode(gains));
  }

  void saveAllStates() async {
    await audioHandler._savePlayQueueState();
    audioHandler.savePlayState();
  }

  bool insert2Next(MyAudioMetadata song) {
    final result = PlayQueueLogic.insert2Next(playQueue, currentIndex, song);
    if (result == null) {
      return false;
    }
    currentIndex = result.currentIndex;
    if (result.wasNewlyInserted &&
        (playModeNotifier.value == 1 ||
            (playModeNotifier.value == 2 && audioHandler._tmpPlayMode == 1))) {
      _playQueueTmp.add(song);
    }
    // The queue changed, so what is next/previous changed too.
    _prefetchNeighbours();
    return true;
  }

  bool add2Last(MyAudioMetadata song) {
    final result = PlayQueueLogic.add2Last(playQueue, currentIndex, song);
    if (result == null) {
      return false;
    }
    currentIndex = result.currentIndex;
    if (result.wasNewlyInserted &&
        (playModeNotifier.value == 1 ||
            (playModeNotifier.value == 2 && audioHandler._tmpPlayMode == 1))) {
      _playQueueTmp.add(song);
    }
    return true;
  }

  void singlePlay(MyAudioMetadata song) async {
    if (insert2Next(song)) {
      await skipToNext();
    }
    play();
  }

  Future<void> setPlayQueue(
    List<MyAudioMetadata> source,
    int playMode, {
    int? targetIndex,
  }) async {
    if (targetIndex != null) {
      currentIndex = targetIndex;
    } else {
      currentIndex = playMode == 0 ? 0 : math.Random().nextInt(source.length);
      playModeNotifier.value = playMode;
    }
    playQueue = List.from(source);
    if (playModeNotifier.value == 1 ||
        (playModeNotifier.value == 2 && audioHandler._tmpPlayMode == 1)) {
      shuffle();
    }
    await audioHandler.load();
    audioHandler.play();

    saveAllStates();
  }

  void reversePlayQueue() {
    if (playQueue.isEmpty) {
      return;
    }
    playQueue = playQueue.reversed.toList();
    currentIndex = playQueue.indexOf(currentSongNotifier.value!);
    _prefetchNeighbours();
    saveAllStates();
  }

  void shuffle() {
    if (playQueue.isEmpty) {
      return;
    }
    _playQueueTmp = List.from(playQueue);
    final others = List.of(playQueue)..removeAt(currentIndex);
    others.shuffle();
    playQueue = [playQueue[currentIndex], ...others];
    currentIndex = 0;
    _prefetchNeighbours();
  }

  void changePlayMode(int newPlayMode) async {
    if (newPlayMode == playModeNotifier.value) {
      return;
    }

    switch (newPlayMode) {
      case 0:
        if (_playQueueTmp.isNotEmpty) {
          playQueue = List.from(_playQueueTmp);
          _playQueueTmp = [];
          currentIndex = playQueue.indexOf(currentSongNotifier.value!);
        }
        break;
      case 1:
        if (_playQueueTmp.isEmpty) {
          shuffle();
        }
        break;
      default:
        break;
    }
    playModeNotifier.value = newPlayMode;
    if (newPlayMode != 2) {
      await _savePlayQueueState();
    }

    savePlayState();
  }

  void toggleRepeat() {
    if (playModeNotifier.value != 2) {
      _tmpPlayMode = playModeNotifier.value;
      playModeNotifier.value = 2;
    } else {
      playModeNotifier.value = _tmpPlayMode;
    }
    savePlayState();
  }

  void delete(int index) {
    MyAudioMetadata tmp = playQueue[index];
    if (_playQueueTmp.isNotEmpty) {
      _playQueueTmp.remove(tmp);
    }
    playQueue.removeAt(index);
    _prefetchNeighbours();
  }

  Future<void> clear() async {
    stop();
    playQueue = [];
    _playQueueTmp = [];
    currentIndex = -1;
    currentSongNotifier.value = null;
    currentCoverArtColor = Colors.grey;
    saveAllStates();

    if (isMobile) {
      HomeWidgetService.updateNowPlayingWidget();
      HomeWidgetService.reloadPlaylistsWidget();
    }
  }

  void justClear() {
    _endLoadGrace();
    _endStallGrace();
    if (usbExclusiveController.active) {
      unawaited(usbExclusiveController.stop());
    }
    _player.stop();
    updateIsPlaying(false);
    updatePlaybackState(stop: true);
    _positionTimer?.cancel();
    _positionTimer = null;

    playQueue = [];
    _playQueueTmp = [];
    currentIndex = -1;
    currentSongNotifier.value = null;
    currentCoverArtColor = Colors.grey;

    if (isMobile) {
      HomeWidgetService.updateNowPlayingWidget();
      HomeWidgetService.reloadPlaylistsWidget();
    }
  }

  List<MyAudioMetadata> getNewQueue(List<MyAudioMetadata> oldQueue) {
    final List<MyAudioMetadata> newPlayQueue = [];
    for (final song in oldQueue) {
      final newSong = library.id2Song[song.id];
      if (newSong != null) {
        newPlayQueue.add(newSong);
      }
    }
    return newPlayQueue;
  }

  Future<void> sync() async {
    playQueue = getNewQueue(playQueue);
    _playQueueTmp = getNewQueue(_playQueueTmp);
    final currentSong = currentSongNotifier.value;
    if (currentSong != null) {
      final tmpCurrentSong = library.id2Song[currentSong.id];
      if (tmpCurrentSong != null) {
        await _setLyricsAndUpdateColors(tmpCurrentSong);
        currentSongNotifier.value = tmpCurrentSong;
        currentIndex = playQueue.indexOf(tmpCurrentSong);
        updateServiceMediaItem(tmpCurrentSong);
      } else {
        currentSongNotifier.value = null;
        currentIndex = -1;
        if (playQueue.isNotEmpty) {
          await skipToNext();
        } else {
          await stop();
        }
      }
    }
    saveAllStates();
  }

  Future<void> _setLyricsAndUpdateColors(MyAudioMetadata song) async {
    await setParsedLyrics(song);
    await _updateColorsFor(song);
  }

  /// The cover's colour, which drives the app's theme. Kept apart from the
  /// lyrics so the stream never waits for the artwork to be downloaded first:
  /// the colour is cosmetic and can land whenever it is ready.
  Future<void> _updateColorsFor(MyAudioMetadata song) async {
    final color = await computeColor(song.picture);
    if (currentSongNotifier.value?.id != song.id) {
      // The song moved on while the colour was being computed.
      return;
    }
    currentCoverArtColor = color;
    updateHoverFocusColor();
    contrastColorTheme = ContrastColorGenerator.generate(currentCoverArtColor);
    if (lyricsPageThemeNotifier.value == .vivid) {
      colorManager.updateLyricsPageColors();
    }

    if (viewModeNotifier.value == .mini) {
      colorManager.updateMiniViewColors();
    }
  }

  /// Warms the artwork and lyrics of the tracks either side of the current one,
  /// so going next or previous shows both at once instead of after a download.
  ///
  /// Only worth doing on a phone, where something other than this app displays
  /// them (a head unit); on a desktop it would just do work for nothing.
  void _prefetchNeighbours() {
    if (!isMobile) {
      return;
    }
    for (final delta in const [1, -1]) {
      final neighbour = currentIndex + delta;
      if (neighbour < 0 || neighbour >= playQueue.length) {
        continue;
      }
      unawaited(_prefetch(playQueue[neighbour]));
    }
  }

  Future<void> _prefetch(MyAudioMetadata song) async {
    try {
      if (!song.picture.isLoaded) {
        await loadPictureSafe(song.picture);
      }
      await setParsedLyrics(song);
    } catch (e) {
      _mediaLog('prefetch failed for ${song.id}: $e');
    }
  }

  /// How long the artwork and lyrics are waited for before giving up on a song.
  static const _prepareTimeout = Duration(seconds: 8);

  /// Fetches what the media session needs - the artwork and the lyrics - so a
  /// track does not start before there is anything to show on the head unit.
  ///
  /// The neighbouring tracks were preheated, so this normally returns at once.
  /// A song that has neither artwork nor lyrics still counts as ready (the
  /// attempt finished); only a fetch that hangs past [_prepareTimeout] fails.
  Future<bool> _prepareForPlayback(MyAudioMetadata song) async {
    final started = DateTime.now();
    try {
      await Future.wait([
        if (!song.picture.isLoaded)
          loadPictureSafe(song.picture, priority: true),
        setParsedLyrics(song),
      ]).timeout(_prepareTimeout);
    } on TimeoutException {
      _mediaLog(
        'artwork/lyrics not ready after ${_prepareTimeout.inSeconds}s '
        'for ${song.id}',
      );
      return false;
    } catch (e) {
      // A failure is not a reason to refuse to play.
      _mediaLog('artwork/lyrics failed for ${song.id}: $e');
      return true;
    }

    // The car is handed a downscaled copy, so making it is part of being ready.
    final coverPath = song.picture.path;
    if (coverPath.isNotEmpty && File(coverPath).existsSync()) {
      try {
        await mediaArt.smallFor(coverPath).timeout(_prepareTimeout);
      } catch (e) {
        _mediaLog('art downscale timed out for ${song.id}: $e');
      }
    }

    _mediaLog(
      'ready for ${song.id} in '
      '${DateTime.now().difference(started).inMilliseconds}ms',
    );
    return true;
  }

  // iOS: keeps the app running while a song loads in the background. The
  // grace ends when buffering settles after MPV_EVENT_START_FILE; the player
  // reset before that event also emits buffering=false, so only a false
  // following a true counts as loaded.
  Future<void> _beginLoadGrace() async {
    await _endLoadGrace();
    _loadGraceToken = await BackgroundTaskService.begin();
    if (_loadGraceToken == null) {
      return;
    }
    _loadGraceSawBuffering = false;
    _loadGraceSub = _player.stream.buffering.listen((buffering) async {
      if (buffering) {
        _loadGraceSawBuffering = true;
      } else if (_loadGraceSawBuffering) {
        if (needPause) {
          // ensure widget is updated
          await Future.delayed(Duration(milliseconds: 500));
          await pause();
        }
        _endLoadGrace();
      }
    });
  }

  Future<void> _endLoadGrace() async {
    _loadGraceSub?.cancel();
    _loadGraceSub = null;
    final token = _loadGraceToken;
    _loadGraceToken = null;
    await BackgroundTaskService.end(token);
  }

  // iOS: a widget tap wakes the app in the background with only a few seconds
  // of runtime, which can suspend it before playback becomes audible (Dart
  // ran, but no sound). Holds a background task until the first position tick
  // (audio flowing) or the timeout, whichever comes first.
  Future<void> beginWidgetCommandGrace() async {
    await endWidgetCommandGrace();
    _widgetCommandToken = await BackgroundTaskService.begin();
    if (_widgetCommandToken == null) {
      return;
    }

    _widgetCommandTimer = Timer(Duration(seconds: 15), endWidgetCommandGrace);
  }

  Future<void> endWidgetCommandGrace() async {
    _widgetCommandSub?.cancel();
    _widgetCommandSub = null;
    _widgetCommandTimer?.cancel();
    _widgetCommandTimer = null;
    final token = _widgetCommandToken;
    _widgetCommandToken = null;
    await BackgroundTaskService.end(token);
  }

  Future<void> _beginStallGrace() async {
    if (!Platform.isIOS || _stallGraceArming || _stallGraceToken != null) {
      return;
    }
    if (!loadingSong && !isPlayingNotifier.value) {
      return;
    }
    _stallGraceArming = true;
    _stallGraceToken = await BackgroundTaskService.begin();
    _stallGraceArming = false;
  }

  Future<void> _endStallGrace() async {
    final token = _stallGraceToken;
    _stallGraceToken = null;
    await BackgroundTaskService.end(token);
  }

  Future<void> load({Duration? start}) async {
    // iOS only: request background execution time for the lyrics fetch,
    // stream ping, player open and buffering below.
    await _beginLoadGrace();

    if (currentSongNotifier.value != null) {
      if (_playLastSyncTime != null) {
        _playedDuration += DateTime.now().difference(_playLastSyncTime!);
      }
      if (_player.state.duration.inSeconds > 0) {
        // The share listened to is measured in *played* time, not by how far
        // the progress bar moved: dragging it to the end adds nothing, and a
        // pause does not count either.
        double times =
            _playedDuration.inSeconds / _player.state.duration.inSeconds;
        if (times > 0.5) {
          library.tryAddCache(currentSongNotifier.value!);
        }
        // Past two thirds of the track it is a finished listen: the play count,
        // the server's scrobble and the listening report all take it from here,
        // without having to run to the very end.
        if (times > 0.67) {
          history.addSongTimes(currentSongNotifier.value!, times.round());
          // Local listening report: one entry per mostly-finished listen,
          // aggregated across every source (see ListeningStats).
          unawaited(
            stats.recordPlay(currentSongNotifier.value!, _playedDuration),
          );
        }
      }
    }
    _playLastSyncTime = null;
    _playedDuration = Duration.zero;

    // save currentIndex
    savePlayState();

    final currentSong = playQueue[currentIndex];

    loadingSong = true;

    // Announce the track before anything else. What a car shows comes from the
    // media session, so it has to switch in the same instant as the song - not
    // once the artwork has been fetched and the stream opened.
    currentSongNotifier.value = currentSong;

    currentLyricsIndexNotifier.value = -1;
    _publishedLyric = null;
    updateServiceMediaItem(currentSong);

    // Lyrics and the artwork are what a head unit shows, so on a phone both are
    // fetched before the stream starts: a track that plays for fifteen seconds
    // with nothing on screen is worse than a short wait. The neighbouring tracks
    // were preheated, so this normally returns at once. On a desktop there is no
    // head unit to keep waiting for, so playback is not held back at all.
    final prepared = isMobile ? await _prepareForPlayback(currentSong) : true;

    // The artwork's colour is cosmetic, so it is computed on its own and never
    // holds the stream back.
    unawaited(_updateColorsFor(currentSong));

    if (isMobile) {
      await HomeWidgetService.updateNowPlayingWidget();
      await HomeWidgetService.reloadPlaylistsWidget();
    }

    // Warm the tracks either side, so next/previous are instant.
    _prefetchNeighbours();

    if (!prepared) {
      _mediaLog('not playing ${currentSong.id}: nothing to show yet');
      showCenterMessage(getAppLocalizations().playFailed, duration: 3000);
      loadingSong = false;
      stop();
      return;
    }

    // With the takeover switch on, a playable track is handed to the output
    // engine first; only when that is refused does the shared player open it.
    var openedExclusive = false;
    if (usbExclusiveController.enabled) {
      try {
        openedExclusive = await usbExclusiveController.start(
          currentSong,
          start: start,
          paused: !isPlayingNotifier.value,
        );
      } catch (error) {
        logger.output("[${currentSong.title}] usb exclusive start failed: $error");
        openedExclusive = false;
      }
    }

    try {
      if (openedExclusive) {
        // The engine owns the DAC now; stop the shared player so both paths
        // cannot drive the output at once.
        await _stopPlayerForExclusive();
        if (isPlayingNotifier.value) {
          _playLastSyncTime = DateTime.now();
        }
      } else {
        if (usbExclusiveController.active) {
          await usbExclusiveController.stop();
        }
        await _openMpvForCurrentSong(currentSong, start: start);
      }
    } catch (error) {
      stop();
      logger.output("[${currentSong.title}] $error");
    }

    updatePlaybackState(postion: getPosition());

    if (start == null) {
      _positionState.writeAsString(getPosition().inMilliseconds.toString());
    }

    loadingSong = false;

    // Publish the line the song opens on, instead of waiting for the first
    // position tick to notice it.
    _updateCurrentLyricIndex(getPosition());
    updateServiceMediaItem(currentSong, lyric: _currentLyricText());
  }

  /// Opens [currentSong] with the shared player at [start], keeping the current
  /// play/pause state. Extracted from [load] so the exclusive handoff can reuse
  /// the exact same open path - artwork/lyrics publishing stays in [load].
  Future<void> _openMpvForCurrentSong(
    MyAudioMetadata currentSong, {
    Duration? start,
  }) async {
    if (currentSong.cacheExist) {
      await _player.open(
        Media(currentSong.cachePath!, start: start),
        play: isPlayingNotifier.value,
      );
    } else {
      String? resource;
      Map<String, String>? headers;

      switch (sourceType) {
        case .webdav:
          final tmpPath = await covertToRedirectPathIfNeed(currentSong.path!);
          if (tmpPath == null) {
            headers = webdavClient?.headers;
          } else {
            resource = tmpPath;
          }
        case .navidrome:
        case .emby:
        case .feiniu:
          await streamClient?.ping();
          resource = streamClient?.getStreamUrl(currentSong.id);
          headers = streamClient?.headers;
        default:
          break;
      }
      resource ??= currentSong.path!;

      // libmpv's bundled ffmpeg does TLS with macOS Secure Transport, which
      // cannot negotiate TLS 1.3 - servers requiring it kill the handshake
      // regardless of any proxy setting. Dart's HttpClient handles TLS 1.3 and
      // the system proxy, so remote streams are re-served to mpv over plain
      // localhost HTTP (see StreamRelay).
      final uri = Uri.tryParse(resource);
      final isRemote =
          uri != null &&
          (uri.scheme == 'https' || uri.scheme == 'http') &&
          uri.host != '127.0.0.1' &&
          uri.host != 'localhost' &&
          uri.host != '::1';

      if (isRemote) {
        final relayUrl = await streamRelay.register(resource, headers);
        resource = relayUrl;
        headers = null;
      }

      await _player.open(
        Media(resource, httpHeaders: headers, start: start),
        play: isPlayingNotifier.value,
      );
    }

    if (isPlayingNotifier.value) {
      _playLastSyncTime = DateTime.now();
    }
  }

  /// Stops the shared player without letting its completed event advance the
  /// queue: used when the exclusive engine takes the output over mid-load.
  Future<void> _stopPlayerForExclusive() async {
    _suppressPlayerCompleted = true;
    try {
      await _player.stop();
    } finally {
      scheduleMicrotask(() {
        _suppressPlayerCompleted = false;
      });
    }
  }

  /// Hand-off entry used by the exclusive controller: reopens the current song
  /// with the shared player at [position] and restores the play/pause state.
  Future<void> _openMpvAtPosition(Duration position) async {
    final currentSong = currentSongNotifier.value;
    if (currentSong == null) {
      return;
    }
    try {
      await _openMpvForCurrentSong(currentSong, start: position);
      // Streaming media drops a seek issued before it is ready, so wait for a
      // duration (bounded) before locating, rather than leaving it at zero.
      if (position > Duration.zero && _player.state.duration <= Duration.zero) {
        await _player.stream.duration
            .firstWhere((duration) => duration > Duration.zero)
            .timeout(
              const Duration(seconds: 5),
              onTimeout: () => Duration.zero,
            );
        if (_player.state.duration > Duration.zero) {
          await _player.seek(position);
        }
      }
      _positionController.add(position);
      updatePlaybackState(postion: position);
    } catch (error) {
      logger.output("usb exclusive handoff failed:$error");
    }
  }

  /// Natural end of an exclusive track: advance the queue the same way the
  /// shared player's completed event does, honouring repeat and sleep-timer.
  Future<void> _handleExclusiveSongFinished() async {
    final needPauseTmp = needPause;
    while (Loader.busy) {
      await Future.delayed(const Duration(milliseconds: 50));
    }
    if (playModeNotifier.value == 2) {
      await load();
    } else {
      await skipToNext();
    }
    if (needPauseTmp) {
      await pause();
    }
  }

  /// Current lyric line, or null when the song has no lyrics, the position is
  /// outside the lyrics timeline, or the line is the parser's placeholder.
  ///
  /// The parser stores a localised "no lyrics"/"parsing failed" note as a line
  /// of its own; that is not a lyric, and publishing it would replace the head
  /// unit's artist line with it.
  String? _currentLyricText() {
    final lines = currentSongNotifier.value?.parsedLyrics?.lines;
    final index = currentLyricsIndexNotifier.value;
    if (lines == null || index < 0 || index >= lines.length) {
      return null;
    }
    final line = lines[index].text;
    final l10n = getAppLocalizations();
    if (line == l10n.noLyrics || line == l10n.lyricsParseFailed) {
      return null;
    }
    return line;
  }

  /// Diagnostics for the media session. Written to the app's log (exportable
  /// from the settings) and to stdout, so `adb logcat` shows the same lines
  /// while the phone - rather than a car - is under test.
  void _mediaLog(String message) {
    logger.output('[media] $message');
    debugPrint('[media] $message');
  }

  /// Milliseconds since the song currently in the session was switched to.
  String _sinceSwitch() {
    final started = _songSwitchedAt;
    if (started == null) {
      return '';
    }
    return ' t=+${DateTime.now().difference(started).inMilliseconds}ms';
  }

  /// Loads the current artwork ahead of any other one and re-publishes once it
  /// is on disk, so a cover that has to be fetched cannot queue behind list
  /// thumbnails and does not leave the session without artwork.
  Future<void> _whenCoverReady(MyAudioMetadata song) async {
    final started = DateTime.now();
    try {
      await loadPictureSafe(song.picture, priority: true);
    } catch (e) {
      _mediaLog('cover load failed for ${song.id}: $e');
      return;
    } finally {
      _coverLoadInFlight.remove(song.id);
    }

    final ms = DateTime.now().difference(started).inMilliseconds;
    if (currentSongNotifier.value?.id != song.id) {
      _mediaLog('cover ready for ${song.id} in ${ms}ms, song already left');
      return;
    }
    _mediaLog(
      'cover ready for ${song.id} in ${ms}ms'
      '${_sinceSwitch()} (on disk: ${song.picture.isExist})',
    );
    refreshServiceMediaItem();
    if (isMobile) {
      unawaited(HomeWidgetService.updateNowPlayingWidget());
    }
  }

  /// Refreshes the system media session with the current song and lyric line.
  void refreshServiceMediaItem() {
    final song = currentSongNotifier.value;
    if (song != null) {
      updateServiceMediaItem(song, lyric: _currentLyricText());
    }
  }

  /// Resolves the URI a head unit fetches for the current artwork, then
  /// re-publishes so it reaches the session.
  ///
  /// Preference: a server URL the car can reach on its own, then this phone on
  /// the LAN through the relay, and only then a `content://` URI - which another
  /// process can read only if it was granted access, but beats nothing.
  Future<void> _resolveFetchArtUri(MyAudioMetadata song) async {
    final path = song.picture.path;
    if (path.isEmpty) {
      return;
    }

    final failedAt = _coverUriFailed[path];
    if (failedAt != null) {
      final file = File(path);
      if (!file.existsSync() || file.statSync().modified == failedAt) {
        return;
      }
      // The cover was written since the failure: try again.
      _coverUriFailed.remove(path);
    }

    if (!_fetchUriInFlight.add(path)) {
      return;
    }

    String? uri;
    try {
      uri = streamRelay.coverUrlSync(filePath: path);
      if (uri == null) {
        await streamRelay.warmUp();
        uri = streamRelay.coverUrlSync(filePath: path);
      }
      uri ??= await _contentUriFor(path);
    } catch (e) {
      _mediaLog('cover URI failed for $path: $e');
    } finally {
      _fetchUriInFlight.remove(path);
    }

    if (uri == null) {
      final file = File(path);
      _coverUriFailed[path] = file.existsSync()
          ? file.statSync().modified
          : DateTime.fromMillisecondsSinceEpoch(0);
      _mediaLog('no fetchable cover URI for $path');
      return;
    }

    _coverUriCache[path] = uri;
    _mediaLog('cover URI${_sinceSwitch()}: $uri');
    if (currentSongNotifier.value?.id == song.id) {
      _pubFetchArtUri = uri;
      refreshServiceMediaItem();
    }
  }

  /// A `content://` URI for [path], or null when the platform cannot expose it.
  Future<String?> _contentUriFor(String path) async {
    if (!Platform.isAndroid) {
      return null;
    }
    try {
      final uri = await _mediaChannel.invokeMethod<String>(
        'getContentUri',
        {'path': path},
      );
      if (uri != null && uri.isNotEmpty) {
        return uri;
      }
    } catch (e) {
      _mediaLog('content URI failed: $e');
    }
    return null;
  }

  /// The artwork to announce for [song]: a local URI for the notification's
  /// bitmap, plus one the head unit can fetch by itself.
  ///
  /// Both are remembered per song, so a publish that follows (the next lyric
  /// line, say) carries the same artwork instead of dropping it - that dropping
  /// is what made a cover appear and then disappear.
  ({String? fileUri, String? coverPath, String? smallArtPath, String? fetchUri})
      _artworkFor(MyAudioMetadata song) {
    if (_pubSongId != song.id) {
      _pubSongId = song.id;
      _pubFileArtUri = null;
      _pubCoverPath = null;
      _pubSmallArtPath = null;
      _pubFetchArtUri = null;
      _songSwitchedAt = DateTime.now();
    }

    final path = song.picture.path;
    // Ask the file, not `picture.isExist`: that flag is only set once the
    // artwork has been loaded, and it stays false for a picture whose load
    // failed once even though the image is right there on disk.
    final onDisk = path.isNotEmpty && File(path).existsSync();
    if (onDisk) {
      _pubCoverPath = path;
      _pubFileArtUri = File(path).uri.toString();

      // A downscaled copy is what goes to the car; the original can be several
      // megabytes and a head unit's screen is small.
      _pubSmallArtPath ??= mediaArt.readyPath(path);
      if (_pubSmallArtPath == null && _smallArtInFlight.add(path)) {
        unawaited(_resolveSmallArt(song));
      }
    }

    // A server that authenticates through the query string can hand the car a
    // plain URL. That is known without any I/O, so it must not wait for the
    // artwork to be downloaded here first.
    final remote = streamClient?.getPictureUrl(song.picture.id);
    if (remote != null && remote.isNotEmpty) {
      _pubFetchArtUri ??= remote;
    } else if (onDisk) {
      // Everything else is served by the local relay, warm since startup, so
      // this normally answers synchronously.
      _pubFetchArtUri ??=
          _coverUriCache[path] ?? streamRelay.coverUrlSync(filePath: path);
      if (_pubFetchArtUri != null) {
        _coverUriCache[path] = _pubFetchArtUri!;
      } else {
        unawaited(_resolveFetchArtUri(song));
      }
    }

    // Not on disk yet - a cover still to be fetched from a server, or one that
    // is only extracted from a local file the first time it plays: load it now,
    // ahead of the list thumbnails.
    if (!onDisk && !song.picture.isLoaded && _coverLoadInFlight.add(song.id)) {
      unawaited(_whenCoverReady(song));
    }

    return (
      fileUri: _pubFileArtUri,
      coverPath: _pubCoverPath,
      smallArtPath: _pubSmallArtPath,
      fetchUri: _pubFetchArtUri,
    );
  }

  /// Makes (or reuses) the downscaled copy of the current cover, then
  /// re-publishes so the car gets the small one instead of the original.
  Future<void> _resolveSmallArt(MyAudioMetadata song) async {
    final path = song.picture.path;
    String? small;
    try {
      small = await mediaArt.smallFor(path);
    } catch (e) {
      _mediaLog('art downscale failed for $path: $e');
    } finally {
      _smallArtInFlight.remove(path);
    }
    // Cannot be downscaled (an unusual format): hand the original over rather
    // than nothing at all.
    small ??= path;
    if (_pubSongId != song.id) {
      return;
    }
    _pubSmallArtPath = small;
    _mediaLog('small art${_sinceSwitch()}: $small');
    refreshServiceMediaItem();
  }

  /// Sets the volume the USB exclusive output should apply.
  ///
  /// While the exclusive session owns the DAC this is routed to it (see the
  /// takeover); otherwise the value is simply remembered for the next time it
  /// takes over.
  void setUsbExclusiveVolume(double volume) {
    final next = volume.clamp(0.0, 1.0).toDouble();
    if (usbExclusiveController.active) {
      unawaited(
        usbExclusiveController.applyVolume(next).then((_) => savePlayState()),
      );
      return;
    }
    usbExclusiveVolumeNotifier.value = next;
    savePlayState();
  }

  /// What the shared player is doing and what it is feeding the output, for the
  /// diagnostics report the USB settings page exports.
  ///
  /// The USB exclusive session adds its own keys when it is the one playing;
  /// this is the media_kit side of the picture.
  Future<Map<String, Object?>> collectSharedAudioDiagnostics() async {
    final snapshot = <String, Object?>{
      'capturedAtMs': DateTime.now().millisecondsSinceEpoch,
      'playing': _player.state.playing,
      'loading': loadingSong,
    };

    final player = _player.platform as NativePlayer;
    try {
      final path = await player.getProperty('path');
      snapshot['input'] = path.isEmpty ? 'unavailable' : path;
      for (final property in const [
        'current-ao',
        'audio-codec-name',
        'audio-params/format',
        'audio-params/samplerate',
        'audio-params/channel-count',
        'audio-out-params/format',
        'audio-out-params/samplerate',
        'audio-out-params/channel-count',
      ]) {
        final value = await player.getProperty(property);
        snapshot[property] = value.isEmpty ? 'unavailable' : value;
      }
    } catch (error) {
      snapshot['error'] = error.toString();
    }

    return snapshot;
  }

  /// Publishes the current track to the system media session.
  ///
  /// The Android media notification, Bluetooth/AVRCP and a car head unit all
  /// read the now-playing state from here; see [buildServiceMediaItem] for how
  /// the fields map. Called as soon as the song changes - before the stream is
  /// opened - so a head unit never keeps showing the previous track while the
  /// phone fetches artwork or lyrics. Anything not known yet is re-published as
  /// soon as it is (artwork when it lands on disk, the lyric line as playback
  /// passes it).
  void updateServiceMediaItem(MyAudioMetadata currentSong, {String? lyric}) {
    final art = _artworkFor(currentSong);
    final line = (lyric != null && lyric.trim().isNotEmpty) ? lyric : null;
    _publishedLyric = line;

    mediaItem.add(
      buildServiceMediaItem(
        id: currentSong.id,
        songTitle: getTitle(currentSong),
        artist: getArtist(currentSong),
        album: getAlbum(currentSong),
        duration: currentSong.duration,
        lyric: line,
        lyricInTitle: controlCenterLyricsNotifier.value,
        fileArtUri: art.fileUri,
        artCacheFile: art.smallArtPath,
        fetchableArtUri: art.fetchUri,
      ),
    );

    _mediaLog(
      'publish id=${currentSong.id}'
      ' art=${art.fileUri == null ? 'none' : 'file'}'
      ' bitmap=${art.smallArtPath == null ? 'none' : 'yes'}'
      ' fetch=${art.fetchUri ?? 'none'}'
      ' lyric=${line == null ? '-' : '"$line"'}'
      '${_sinceSwitch()}',
    );
  }

  @override
  Future<void> play() async {
    if (playQueue.isEmpty) return;
    if (usbExclusiveController.active) {
      await usbExclusiveController.resume();
    } else {
      _player.play();
    }

    await updateIsPlaying(true);
    updatePlaybackState();

    _positionTimer ??= Timer.periodic(Duration(seconds: 1), (_) {
      _positionState.writeAsString(getPosition().inMilliseconds.toString());
    });
  }

  @override
  Future<void> pause() async {
    if (usbExclusiveController.active) {
      await usbExclusiveController.pause();
    } else {
      _player.pause();
    }
    await updateIsPlaying(false);
    updatePlaybackState(postion: getPosition());
    _positionTimer?.cancel();
    _positionTimer = null;
    _positionState.writeAsString(getPosition().inMilliseconds.toString());
  }

  @override
  Future<void> stop() async {
    await _endLoadGrace();
    await _endStallGrace();
    if (usbExclusiveController.active) {
      await usbExclusiveController.stop();
    }
    _player.stop();
    await updateIsPlaying(false);
    updatePlaybackState(stop: true);
    _positionTimer?.cancel();
    _positionTimer = null;
    _positionState.writeAsString(Duration.zero.inMilliseconds.toString());
  }

  @override
  Future<void> seek(Duration position) async {
    updatePlaybackState(postion: position);
    if (usbExclusiveController.active) {
      await usbExclusiveController.seek(position);
      // ensure position is updated
      await Future.delayed(Duration(milliseconds: 50));
      updateLyricsNotifier.value++;
      _positionState.writeAsString(position.inMilliseconds.toString());
      return;
    }
    await _player.seek(position);
    // ensure position is updated
    await Future.delayed(Duration(milliseconds: 50));
    updateLyricsNotifier.value++;
    _positionState.writeAsString(getPosition().inMilliseconds.toString());
  }

  @override
  Future<void> skipToNext() async {
    if (playQueue.isEmpty) return;

    currentIndex = (currentIndex + 1) % playQueue.length;
    await load();
  }

  @override
  Future<void> skipToPrevious() async {
    if (playQueue.isEmpty) return;

    currentIndex = (currentIndex + playQueue.length - 1) % playQueue.length;
    await load();
  }

  void togglePlay() {
    if (playQueue.isEmpty) return;

    if (isPlayingNotifier.value) {
      pause();
    } else {
      play();
    }
  }

  Stream<Duration> getPositionStream() {
    return _positionController.stream;
  }

  Stream<Duration> getDurationStream() {
    return _durationController.stream;
  }

  Duration getPosition() {
    return usbExclusiveController.active
        ? usbExclusiveController.position
        : _player.state.position;
  }

  Duration getCurrentDuration() {
    return usbExclusiveController.active
        ? usbExclusiveController.duration ?? getDuration(currentSongNotifier.value)
        : _player.state.duration;
  }

  void setVolume(double volume) {
    if (usbExclusiveController.active) {
      unawaited(
        usbExclusiveController.applyVolume(volume).then((_) => savePlayState()),
      );
      return;
    }
    double adjustedVolume = (math.log(volume * 9 + 1) / math.log(10)) * 100;
    _player.setVolume(adjustedVolume);
  }

  Future<void> applyEqualizer() async {
    bool isAllZero = gains.every((g) => g.abs() < 0.01);
    String af = '';

    if (!isAllZero) {
      double g1 = gains[0]; // 31Hz
      double g2 = gains[1]; // 62Hz
      double g3 = gains[2]; // 125Hz
      double g4 = gains[3]; // 250Hz
      double g5 = gains[4]; // 500Hz
      double g6 = gains[5]; // 1kHz
      double g7 = gains[6]; // 2kHz
      double g8 = gains[7]; // 4kHz
      double g9 = gains[8]; // 8kHz
      double g10 = gains[9]; // 16kHz

      double b1 = g1; // 65Hz
      double b2 = 0.0;
      double b3 = g2; // 131Hz
      double b4 = g3; // 185Hz
      double b5 = 0.0; // 263Hz
      double b6 = g4; // 371Hz
      double b7 = g5; // 525Hz
      double b8 = 0.0; // 742Hz
      double b9 = g6; // 1050Hz
      double b10 = g7; // 1480Hz
      double b11 = 0.0; // 2090Hz
      double b12 = g8; // 2960Hz
      double b13 = 0.0;
      double b14 = g9; // 5920Hz
      double b15 = 0.0; // 8370Hz
      double b16 = g10; // 11800Hz
      double b17 = g10; // 16700Hz
      double b18 = g10; // 20000Hz

      List<double> bValues = [
        b1,
        b2,
        b3,
        b4,
        b5,
        b6,
        b7,
        b8,
        b9,
        b10,
        b11,
        b12,
        b13,
        b14,
        b15,
        b16,
        b17,
        b18,
      ];

      final List<String> activeParams = [];
      for (int i = 0; i < bValues.length; i++) {
        double multiplier = math.pow(10, bValues[i] / 20).toDouble();

        activeParams.add('${i + 1}b=${multiplier.toStringAsFixed(3)}');
      }
      af = 'superequalizer=${activeParams.join(":")}';
    }

    await (_player.platform as NativePlayer).setProperty('af', af);
    saveEqualizerState();
  }
}
