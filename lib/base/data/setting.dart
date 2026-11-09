import 'dart:convert';
import 'dart:io';

import 'package:material_ui/material_ui.dart';
import 'package:sylvakru/base/audio_handler.dart';
import 'package:sylvakru/base/services/color_manager.dart';
import 'package:sylvakru/base/services/usb_audio_preferences.dart';
import 'package:sylvakru/base/services/interaction.dart';
import 'package:sylvakru/base/widgets/lyric_list_view.dart';
import 'package:sylvakru/base/app.dart';
import 'package:sylvakru/base/data/home_layout.dart';
import 'package:sylvakru/base/data/stream_quality.dart';
import 'package:sylvakru/base/utils/path.dart';
import 'package:sylvakru/base/widgets/manage_music_folders.dart';
import 'package:sylvakru/portrait_view/portrait_view.dart';

final artistsIsListViewNotifier = ValueNotifier(true);
final artistsIsAscendingNotifier = ValueNotifier(true);
final artistsUseLargePictureNotifier = ValueNotifier(false);
final artistsRandomizeNotifier = ValueNotifier(false);

final albumsIsAscendingNotifier = ValueNotifier(true);
final albumsUseLargePictureNotifier = ValueNotifier(false);
final albumsRandomizeNotifier = ValueNotifier(false);

final playlistsUseLargePictureNotifier = ValueNotifier(true);

final exitOnCloseNotifier = ValueNotifier(false);

final setting = Setting();

/// Sort-order key of the whole library: a local library offers orders that a
/// streamed one does not, so the two are remembered apart.
String get librarySortKey =>
    isNotStreamSource ? 'library:local' : 'library:stream';

/// Sort order of each song list, keyed by the kind of list it belongs to.
///
/// The library, artist and album pages used to build a fresh notifier whenever
/// they were opened, so the chosen order went back to the default on every
/// visit - and lost for good when the source was switched.
final songSortTypes = <String, ValueNotifier<int>>{};

/// The remembered sort order for [kind], created on first use. Every change is
/// written to the settings file straight away.
ValueNotifier<int> songSortTypeNotifier(String kind, {int initial = 0}) {
  return songSortTypes.putIfAbsent(kind, () {
    final notifier = ValueNotifier<int>(initial);
    notifier.addListener(setting.save);
    return notifier;
  });
}

class Setting {
  File? file;

  Future<void> load() async {
    final target = File("${appSupportDir.path}/setting.json");
    file = target;
    initFile(target, false);

    final json = await readJsonMapFile(target);

    // USB exclusive output preferences (output format, buffers, volume mode,
    // DSD strategy, DAC quirks overrides).
    usbAudioPreferences.load(json);

    artistsIsListViewNotifier.value =
        json['artistsIsList'] as bool? ?? artistsIsListViewNotifier.value;

    artistsIsAscendingNotifier.value =
        json['artistsIsAscend'] as bool? ?? artistsIsAscendingNotifier.value;

    artistsUseLargePictureNotifier.value =
        json['artistsUseLargePicture'] as bool? ??
        artistsUseLargePictureNotifier.value;

    artistsRandomizeNotifier.value =
        json['artistsRandomize'] as bool? ?? artistsRandomizeNotifier.value;

    albumsIsAscendingNotifier.value =
        json['albumsIsAscend'] as bool? ?? albumsIsAscendingNotifier.value;

    albumsRandomizeNotifier.value =
        json['albumsRandomize'] as bool? ?? albumsRandomizeNotifier.value;

    albumsUseLargePictureNotifier.value =
        json['albumsUseLargePicture'] as bool? ??
        albumsUseLargePictureNotifier.value;

    playlistsUseLargePictureNotifier.value =
        json['playlistsUseLargePicture'] as bool? ??
        playlistsUseLargePictureNotifier.value;

    endDrawerNotifier.value = json['endDrawer'] as bool? ?? false;

    vibrationOnNoitifier.value =
        json['vibrationOn'] as bool? ?? vibrationOnNoitifier.value;

    final languageCode = json['language'] as String? ?? '';

    if (languageCode.isNotEmpty) {
      localeNotifier.value = Locale(languageCode);
    }

    immersiveWideLayoutNotifier.value =
        json['immersiveWideLayout'] as bool? ?? true;

    wideShowTopSystemBarNotifier.value =
        json['wideShowTopSystemBar'] as bool? ??
        wideShowTopSystemBarNotifier.value;

    wideShowBottomSystemBarNotifier.value =
        json['wideShowBottomSystemBar'] as bool? ??
        wideShowBottomSystemBarNotifier.value;

    autoPlayOnStartupNotifier.value =
        json['autoPlayOnStartup'] as bool? ?? false;

    if (isPremiumNotifier.value) {
      fontFamilyNotifier.value = json['fontFamily'] as String?;
    }

    mainPageThemeNotifier.value = ThemeType.values.firstWhere(
      (e) => e.name == json['mainPageTheme'],
      orElse: () => ThemeType.vivid,
    );

    if (!isPremiumNotifier.value && mainPageThemeNotifier.value == .vivid) {
      mainPageThemeNotifier.value = .light;
    }

    updateHoverFocusColor();

    lyricsPageThemeNotifier.value = ThemeType.values.firstWhere(
      (e) => e.name == json['lyricsPageTheme'],
      orElse: () => ThemeType.vivid,
    );

    lyricsFontSizeOffsetNotifier.value =
        json['lyricsFontSizeOffset'] as double? ??
        lyricsFontSizeOffsetNotifier.value;

    exitOnCloseNotifier.value =
        json['exitOnClose'] as bool? ?? exitOnCloseNotifier.value;

    recursiveScanNotifier.value = json['recursiveScan'] as bool? ?? false;

    controlCenterLyricsNotifier.value =
        json['controlCenterLyrics'] as bool? ?? false;

    streamQualityNotifier.value = StreamQuality.fromName(json['streamQuality']);
    streamFormatNotifier.value = StreamFormat.fromName(json['streamFormat']);

    homeButtonOrder.value = restoredHomeOrder(
      json['homeButtons'],
      HomeButton.values,
    );
    homeButtonHidden.value = restoredHomeHidden(
      json['homeButtonsHidden'],
      HomeButton.values,
    );
    homeShelfOrder.value = restoredHomeOrder(
      json['homeShelves'],
      HomeShelf.values,
    );
    homeShelfHidden.value = restoredHomeHidden(
      json['homeShelvesHidden'],
      HomeShelf.values,
    );

    final sortTypes = json['songSortTypes'];
    if (sortTypes is Map) {
      for (final entry in sortTypes.entries) {
        final type = entry.value;
        if (type is int) {
          // Built with the stored value rather than assigned to it, so that
          // reading the settings does not immediately write them back.
          songSortTypeNotifier('${entry.key}', initial: type);
        }
      }
    }
  }

  void save() {
    final target = file;

    // Until the settings have been read there is nowhere to write to.
    if (target == null) {
      return;
    }

    target.writeAsStringSync(
      jsonEncode({
        ...usbAudioPreferences.toMap(),
        'artistsIsList': artistsIsListViewNotifier.value,
        'artistsIsAscend': artistsIsAscendingNotifier.value,
        'artistsUseLargePicture': artistsUseLargePictureNotifier.value,
        'artistsRandomize': artistsRandomizeNotifier.value,

        'albumsIsAscend': albumsIsAscendingNotifier.value,
        'albumsRandomize': albumsRandomizeNotifier.value,
        'albumsUseLargePicture': albumsUseLargePictureNotifier.value,

        'playlistsUseLargePicture': playlistsUseLargePictureNotifier.value,

        'endDrawer': endDrawerNotifier.value,

        'vibrationOn': vibrationOnNoitifier.value,
        'language': localeNotifier.value?.languageCode,

        'immersiveWideLayout': immersiveWideLayoutNotifier.value,
        'wideShowTopSystemBar': wideShowTopSystemBarNotifier.value,
        'wideShowBottomSystemBar': wideShowBottomSystemBarNotifier.value,
        'autoPlayOnStartup': autoPlayOnStartupNotifier.value,

        'fontFamily': fontFamilyNotifier.value,

        'mainPageTheme': mainPageThemeNotifier.value.name,
        'lyricsPageTheme': lyricsPageThemeNotifier.value.name,

        'lyricsFontSizeOffset': lyricsFontSizeOffsetNotifier.value,
        'exitOnClose': exitOnCloseNotifier.value,

        'recursiveScan': recursiveScanNotifier.value,

        'controlCenterLyrics': controlCenterLyricsNotifier.value,

        'streamQuality': streamQualityNotifier.value.name,
        'streamFormat': streamFormatNotifier.value.name,

        'homeButtons': [for (final button in homeButtonOrder.value) button.name],
        'homeButtonsHidden': [
          for (final button in homeButtonHidden.value) button.name,
        ],
        'homeShelves': [for (final shelf in homeShelfOrder.value) shelf.name],
        'homeShelvesHidden': [
          for (final shelf in homeShelfHidden.value) shelf.name,
        ],

        'songSortTypes': {
          for (final entry in songSortTypes.entries)
            entry.key: entry.value.value,
        },
      }),
    );
  }
}
