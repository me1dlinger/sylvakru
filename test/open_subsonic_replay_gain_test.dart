import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:sylvakru/base/app.dart' as app;
import 'package:sylvakru/base/data/library.dart' as library_data;
import 'package:sylvakru/base/my_audio_metadata.dart';

/// OpenSubsonic reports ReplayGain per track and per album, and the USB
/// exclusive output uses it to normalise loudness. It arrives either as a
/// number or as a string with a "dB" suffix, and a peak of zero (or less) is
/// meaningless - all of which the parser has to cope with.
void main() {
  late Directory appSupportDirectory;

  setUpAll(() {
    appSupportDirectory = Directory.systemTemp.createTempSync(
      'open_subsonic_replay_gain_test',
    );
    app.appSupportDir = appSupportDirectory;
    app.sourceType = app.SourceType.navidrome;
    app.isStreamSource = true;
    app.isNotStreamSource = false;
  });

  setUp(() {
    library_data.library.id2Song.clear();
  });

  tearDownAll(() {
    appSupportDirectory.deleteSync(recursive: true);
  });

  test('numbers and "dB" strings are both understood', () {
    final song = MyAudioMetadata.fromMap({
      'id': 'song-id',
      'title': 'Song',
      'replayGain': {
        'trackGain': ' -6.75 dB ',
        'trackPeak': 0.95,
        'albumGain': 1.25,
        'albumPeak': '1.10',
      },
    }, app.SourceType.navidrome);

    expect(song.replayGainTrackGainDb, -6.75);
    expect(song.replayGainTrackPeak, 0.95);
    expect(song.replayGainAlbumGainDb, 1.25);
    expect(song.replayGainAlbumPeak, 1.1);
  });

  test('invalid values, non-finite values and non-positive peaks are ignored',
      () {
    final song = MyAudioMetadata.fromMap({
      'id': 'song-id',
      'title': 'Song',
      'replayGain': {
        'trackGain': 'not a number',
        'trackPeak': 0,
        'albumGain': double.infinity,
        'albumPeak': -1,
      },
    }, app.SourceType.navidrome);

    expect(song.replayGainTrackGainDb, isNull);
    expect(song.replayGainTrackPeak, isNull);
    expect(song.replayGainAlbumGainDb, isNull);
    expect(song.replayGainAlbumPeak, isNull);
  });

  test('a song without replayGain simply has none', () {
    final song = MyAudioMetadata.fromMap({
      'id': 'song-id',
      'title': 'Song',
    }, app.SourceType.navidrome);

    expect(song.replayGainTrackGainDb, isNull);
    expect(song.replayGainAlbumGainDb, isNull);
  });
}