import 'dart:convert';
import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:material_ui/material_ui.dart';
import 'package:http/http.dart' as http;
import 'package:sylvakru/base/audio_handler.dart';
import 'package:sylvakru/base/data/config.dart';
import 'package:sylvakru/base/data/playlist.dart';
import 'package:sylvakru/base/services/color_manager.dart';
import 'package:sylvakru/base/app.dart';
import 'package:sylvakru/base/asset_images.dart';
import 'package:sylvakru/base/services/emby_client.dart';
import 'package:sylvakru/base/services/feiniu_client.dart';
import 'package:sylvakru/base/services/interaction.dart';
import 'package:sylvakru/base/services/logger.dart';
import 'package:sylvakru/base/services/navidrome_client.dart';
import 'package:sylvakru/base/services/stream_client.dart';
import 'package:sylvakru/base/services/system_ui_service.dart';
import 'package:sylvakru/base/utils/common_utils.dart';
import 'package:sylvakru/base/utils/metadata_utils.dart';
import 'package:sylvakru/base/utils/media_query.dart';
import 'package:sylvakru/base/utils/source_type.dart';
import 'package:sylvakru/base/widgets/server_manager.dart';
import 'package:sylvakru/base/widgets/equalizer.dart';
import 'package:sylvakru/base/services/download_manager.dart';
import 'package:sylvakru/base/widgets/downloads_page.dart';
import 'package:sylvakru/base/widgets/home_layout_settings.dart';
import 'package:sylvakru/base/widgets/my_divider.dart';
import 'package:sylvakru/base/widgets/apple_form.dart';
import 'package:sylvakru/base/widgets/stats_page.dart';
import 'package:sylvakru/base/data/setting.dart';
import 'package:sylvakru/base/data/stream_quality.dart';
import 'package:sylvakru/layer/layers_manager.dart';
import 'package:sylvakru/base/widgets/manage_music_folders.dart';
import 'package:sylvakru/base/data/library.dart';
import 'package:sylvakru/base/data/loader.dart';
import 'package:sylvakru/layer/premium_layer.dart';
import 'package:sylvakru/portrait_view/portrait_view.dart';
import 'package:sylvakru/portrait_view/sleep_timer.dart';
import 'package:sylvakru/l10n/generated/app_localizations.dart';
import 'package:sylvakru/base/widgets/my_switch.dart';
import 'package:smooth_corner/smooth_corner.dart';
import 'package:url_launcher/url_launcher.dart';

class SettingsList extends StatefulWidget {
  final double? iconSize;
  const SettingsList({super.key, this.iconSize});

  @override
  State<StatefulWidget> createState() => _SettingsListState();
}

class _SettingsListState extends State<SettingsList> {
  double? iconSize;

  @override
  void initState() {
    super.initState();
    iconSize = widget.iconSize;
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context);
    bool isLandscape = !isTooNarrow(context);
    return CustomScrollView(
      slivers: [
        if (viewModeNotifier.value == .bigPicture)
          sliverBox(const SizedBox(height: 10)),

        if (isLandscape && viewModeNotifier.value != .bigPicture)
          sliverBox(
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20),

              child: Focus(
                child: ListTile(
                  leading: ImageIcon(settingImage, size: 50),
                  title: Text(
                    l10n.settings,
                    style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold),
                  ),
                  subtitle: Text(
                    l10n.settingCount(
                      (Platform.isAndroid
                              ? 16
                              : Platform.isIOS
                              ? 15
                              : 14) +
                          (isNotStreamSource ? 1 : 0),
                    ),
                    style: TextStyle(fontSize: 12),
                  ),
                ),
              ),
            ),
          ),

        if (isLandscape && viewModeNotifier.value != .bigPicture)
          sliverBox(
            MyDivider(
              thickness: 0.5,
              height: 0.5,
              indent: 20,
              endIndent: 20,
              color: dividerColor,
            ),
          ),

        if (isLandscape && viewModeNotifier.value != .bigPicture)
          sliverBox(const SizedBox(height: 10)),

        if (Platform.isIOS && viewModeNotifier.value != .bigPicture)
          sliverBox(
            paddingIfNeed(isLandscape, premiumFeaturesListTile(context, l10n)),
          ),

        sliverBox(
          paddingIfNeed(isLandscape, serversListTile(context, l10n)),
        ),

        if (isNotStreamSource)
          sliverBox(
            paddingIfNeed(
              isLandscape,
              selectMusicFoldersListTile(context, l10n),
            ),
          ),

        sliverBox(paddingIfNeed(isLandscape, syncListTile(context, l10n))),

        sliverBox(
          paddingIfNeed(isLandscape, cleanCacheListTile(context, l10n)),
        ),

        // The USB exclusive output is an Android feature: it claims a USB audio
        // device, which the other platforms do not have.
        if (Platform.isAndroid)
          sliverBox(
            paddingIfNeed(isLandscape, audioOutputListTile(context, l10n)),
          ),

        sliverBox(
          paddingIfNeed(isLandscape, downloadsListTile(context, l10n)),
        ),

        sliverBox(
          paddingIfNeed(isLandscape, listeningReportListTile(context, l10n)),
        ),

        sliverBox(
          paddingIfNeed(isLandscape, homeLayoutListTile(context, l10n)),
        ),

        if (isStreamSource) ...[
          sliverBox(
            paddingIfNeed(isLandscape, streamQualityListTile(context, l10n)),
          ),
          sliverBox(
            paddingIfNeed(isLandscape, streamFormatListTile(context, l10n)),
          ),
        ],

        sliverBox(paddingIfNeed(isLandscape, themeListTile(context, l10n))),

        sliverBox(paddingIfNeed(isLandscape, languageListTile(context, l10n))),

        if (viewModeNotifier.value != .bigPicture)
          sliverBox(paddingIfNeed(isLandscape, fontListTile(context, l10n))),

        sliverBox(
          paddingIfNeed(isLandscape, controlCenterLyricsListTile(l10n)),
        ),

        if (Platform.isIOS &&
            !isLandscape &&
            viewModeNotifier.value != .bigPicture)

        if (isMobile && !isTV)
          sliverBox(paddingIfNeed(isLandscape, vibrationListTile(l10n))),

        if (isMobile)
          sliverBox(
            paddingIfNeed(
              isLandscape,
              sleepTimerListTile(context, l10n, iconSize: iconSize),
            ),
          ),

        sliverBox(paddingIfNeed(isLandscape, equalizerListTile(context, l10n))),

        if (Platform.isAndroid && !isTV) ...[
          sliverBox(
            paddingIfNeed(isLandscape, immersiveWideLayoutListTile(l10n)),
          ),
          sliverBox(
            paddingIfNeed(isLandscape, wideSystemBarListTile(l10n, top: true)),
          ),
          sliverBox(
            paddingIfNeed(isLandscape, wideSystemBarListTile(l10n, top: false)),
          ),
        ],

        sliverBox(paddingIfNeed(isLandscape, autoPlayOnStartupListTile(l10n))),

        if (!isMobile)
          sliverBox(
            paddingForLandscape(exitOnClose(l10n)),
          ), // always landscape style

        if (!Platform.isIOS)
          sliverBox(paddingIfNeed(isLandscape, checkUpdate(context, l10n))),

        sliverBox(paddingIfNeed(isLandscape, viewLogListTile(context, l10n))),

        if (viewModeNotifier.value != .bigPicture)
          sliverBox(
            paddingIfNeed(
              isLandscape,
              ListTile(
                leading: ImageIcon(infoImage, size: iconSize),
                title: Text(l10n.about),
                onTap: () {
                  layersManager.pushDetail('settings', 'about');
                },
              ),
            ),
          ),

        if (!isLandscape) sliverBox(const SizedBox(height: 100)),

        if (viewModeNotifier.value == .bigPicture)
          sliverBox(const SizedBox(height: 75)),
      ],
    );
  }

  Widget paddingIfNeed(bool isLandscape, Widget child) {
    return isLandscape ? paddingForLandscape(child) : child;
  }

  Widget sliverBox(Widget child) => SliverToBoxAdapter(child: child);

  Widget paddingForLandscape(Widget child) {
    return Padding(
      padding: EdgeInsets.symmetric(
        horizontal: viewModeNotifier.value == .bigPicture ? 50 : 30,
      ),
      child: SmoothClipRRect(
        smoothness: 1,
        borderRadius: BorderRadius.circular(10),
        child: Material(color: Colors.transparent, child: child),
      ),
    );
  }

  Widget syncListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(reloadImage, size: iconSize),
      title: Text(l10n.syncLibrary),
      onTap: () async {
        if (await showConfirmDialog(context, l10n.syncLibrary)) {
          if (Loader.busy) {
            if (context.mounted) {
              showCenterMessage(l10n.syncingTryLater);
            }
            return;
          }
          await Loader.sync();
        }
      },
    );
  }

  Widget selectMusicFoldersListTile(
    BuildContext context,
    AppLocalizations l10n,
  ) {
    return ListTile(
      leading: ImageIcon(folderImage, size: iconSize),
      title: Text(l10n.manageMusicFolder),
      onTap: () {
        showAnimationDialog(context: context, child: ManageMusicFolders());
      },
    );
  }

  Widget premiumFeaturesListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(premiumImage, size: iconSize),
      title: Text(l10n.premiumFeatures),
      onTap: () {
        layersManager.pushDetail('settings', 'premium');
      },
      trailing: ValueListenableBuilder(
        valueListenable: trialRemainingMinNotifier,
        builder: (context, value, child) {
          if (value <= 0) {
            return SizedBox.shrink();
          }
          return Row(
            mainAxisSize: .min,
            children: [
              Text(
                "${l10n.trialRemaining}:${formatDuration(Duration(minutes: value), ms: false)}",
              ),
            ],
          );
        },
      ),
    );
  }

  Widget cleanCacheListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(cacheImage, size: iconSize),
      title: Text(l10n.viewCache),
      onTap: () => showCacheDetailDialog(context, l10n),
      trailing: ValueListenableBuilder(
        valueListenable: cacheSizeNotifier,
        builder: (context, value, child) {
          // use blank as placeholders
          return Text("${value.toStringAsFixed(1)}MB  ");
        },
      ),
    );
  }

  Widget audioOutputListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: Icon(Icons.usb_rounded, size: iconSize),
      title: Text(l10n.audioOutput),
      subtitle: Text(l10n.audioOutputSubtitle),
      trailing: const Icon(Icons.chevron_right_rounded),
      onTap: () => layersManager.pushDetail('settings', 'audio_output'),
    );
  }

  Widget streamQualityListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(equalizerImage, size: iconSize),
      title: Text(l10n.streamQuality),
      onTap: () {
        showAnimationDialog(
          context: context,
          child: SizedBox(
            width: 300,
            child: Padding(
              padding: const EdgeInsets.all(10),
              child: Builder(
                builder: (context) {
                  return Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      SizedBox(
                        height: 35,
                        child: Text(
                          l10n.streamQuality,
                          style: TextStyle(
                            fontSize: 18,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                      ),
                      for (final quality in StreamQuality.values)
                        ListTile(
                          title: Text(streamQualityLabel(l10n, quality)),
                          trailing: streamQualityNotifier.value == quality
                              ? Icon(Icons.check)
                              : null,
                          onTap: () {
                            Navigator.pop(context);
                            streamQualityNotifier.value = quality;
                            setting.save();
                          },
                        ),
                    ],
                  );
                },
              ),
            ),
          ),
        );
      },
      trailing: ValueListenableBuilder(
        valueListenable: streamQualityNotifier,
        builder: (context, value, child) {
          return Text('${streamQualityLabel(l10n, value)}  ');
        },
      ),
    );
  }

  Widget streamFormatListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(optionImage, size: iconSize),
      title: Text(l10n.streamFormat),
      onTap: () {
        showAnimationDialog(
          context: context,
          child: SizedBox(
            width: 300,
            child: Padding(
              padding: const EdgeInsets.all(10),
              child: Builder(
                builder: (context) {
                  return Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      SizedBox(
                        height: 35,
                        child: Text(
                          l10n.streamFormat,
                          style: TextStyle(
                            fontSize: 18,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                      ),
                      for (final format in StreamFormat.values)
                        ListTile(
                          title: Text(format.display),
                          trailing: streamFormatNotifier.value == format
                              ? Icon(Icons.check)
                              : null,
                          onTap: () {
                            Navigator.pop(context);
                            streamFormatNotifier.value = format;
                            setting.save();
                          },
                        ),
                    ],
                  );
                },
              ),
            ),
          ),
        );
      },
      trailing: ValueListenableBuilder(
        valueListenable: streamFormatNotifier,
        builder: (context, value, child) {
          return Text('${value.display}  ');
        },
      ),
    );
  }

  Widget downloadsListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: Icon(Icons.download_rounded, size: iconSize),
      title: Text(l10n.downloads),
      trailing: ValueListenableBuilder(
        valueListenable: downloadsNotifier,
        builder: (context, items, child) => Text('${items.length}  '),
      ),
      onTap: () {
        globalNavigatorKey.currentState?.push(
          MaterialPageRoute(builder: (context) => const DownloadsPage()),
        );
      },
    );
  }

  Widget listeningReportListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(frequentlyImage, size: iconSize),
      title: Text(l10n.listeningReport),
      trailing: const Icon(Icons.arrow_forward_ios_rounded, size: 16),
      onTap: () {
        globalNavigatorKey.currentState?.push(
          MaterialPageRoute(builder: (context) => const StatsPage()),
        );
      },
    );
  }

  Widget homeLayoutListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(optionImage, size: iconSize),
      title: Text(l10n.homeLayout),
      trailing: const Icon(Icons.arrow_forward_ios_rounded, size: 16),
      onTap: () {
        globalNavigatorKey.currentState?.push(
          MaterialPageRoute(builder: (context) => const HomeLayoutSettings()),
        );
      },
    );
  }

  Widget serversListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(optionImage, size: iconSize),
      title: Text(l10n.servers),
      trailing: const Icon(Icons.arrow_forward_ios_rounded, size: 16),
      onTap: () {
        globalNavigatorKey.currentState?.push(
          applePageRoute<void>(() => const ServerListPage()),
        );
      },
    );
  }

  Widget languageListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(languageImage, size: iconSize),
      title: Text(l10n.language),
      onTap: () {
        showAnimationDialog(
          context: context,

          child: SizedBox(
            width: 300,
            height: isMobile ? 200 : 180,
            child: Padding(
              padding: const EdgeInsets.all(15.0),
              child: ValueListenableBuilder(
                valueListenable: localeNotifier,
                builder: (context, value, child) {
                  final l10n = AppLocalizations.of(context);

                  return ListView(
                    children: [
                      ListTile(
                        title: Text(l10n.followSystem),
                        onTap: () {
                          localeNotifier.value = null;
                          setting.save();
                        },
                        trailing: value == null ? Icon(Icons.check) : null,
                      ),
                      ListTile(
                        title: Text('English'),
                        onTap: () {
                          localeNotifier.value = Locale('en');
                          setting.save();
                        },
                        trailing: value == Locale('en')
                            ? Icon(Icons.check)
                            : null,
                      ),
                      ListTile(
                        title: Text('中文'),
                        onTap: () {
                          localeNotifier.value = Locale('zh');
                          setting.save();
                        },
                        trailing: value == Locale('zh')
                            ? Icon(Icons.check)
                            : null,
                      ),
                    ],
                  );
                },
              ),
            ),
          ),
        );
      },
    );
  }

  Widget controlCenterLyricsListTile(AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(desktopLyricsImage, size: iconSize),
      title: Text(l10n.controlCenterLyrics),
      trailing: SizedBox(
        width: 50,
        child: MySwitch(
          valueNotifier: controlCenterLyricsNotifier,
          onToggleCallBack: () {
            setting.save();
            audioHandler.refreshServiceMediaItem();
          },
        ),
      ),
    );
  }

  Widget vibrationListTile(AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(vibrationImage, size: iconSize),
      title: Text(l10n.vibration),
      trailing: SizedBox(
        width: 50,
        child: MySwitch(
          valueNotifier: vibrationOnNoitifier,
          onToggleCallBack: () {
            setting.save();
          },
        ),
      ),
    );
  }

  void _updateMainPageTheme() {
    setting.save();
    colorManager.updateMainPageColors();
  }

  void _updateLyricsPageTheme() {
    setting.save();
    colorManager.updateLyricsPageColors();
  }

  Widget fontListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(fontImage, size: iconSize),

      title: Text(l10n.fonts),
      onTap: () {
        if (!isPremiumNotifier.value) {
          showPremiumDialog(context);
          return;
        }
        layersManager.pushDetail('settings', 'font_picker');
      },
      trailing: ValueListenableBuilder(
        valueListenable: isPremiumNotifier,
        builder: (context, value, child) {
          if (value) {
            return SizedBox.shrink();
          }
          return Icon(Icons.lock);
        },
      ),
    );
  }

  Widget themeListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(themeImage, size: iconSize),
      title: Text(l10n.theme),
      onTap: () async {
        mainPageThemeNotifier.addListener(_updateMainPageTheme);
        lyricsPageThemeNotifier.addListener(_updateLyricsPageTheme);
        await showAnimationDialog(
          context: context,

          child: OrientationBuilder(
            builder: (context, orientation) {
              final size = MediaQuery.of(context).size;
              final shortSide = size.shortestSide;

              bool isPhone = shortSide < 600;

              return SizedBox(
                width: 300,
                height: isPhone && orientation == .landscape
                    ? 350
                    : isMobile
                    ? 420
                    : 370,
                child: Padding(
                  padding: const EdgeInsets.all(15.0),
                  child: CustomScrollView(
                    scrollBehavior: ScrollBehavior().copyWith(
                      scrollbars: false,
                    ),
                    slivers: [
                      sliverBox(
                        ValueListenableBuilder(
                          valueListenable: mainPageThemeNotifier,
                          builder: (context, value, child) {
                            final l10n = AppLocalizations.of(context);
                            return Column(
                              children: [
                                Text(
                                  l10n.mainPageTheme,
                                  style: .new(fontSize: 18, fontWeight: .bold),
                                ),
                                ListTile(
                                  title: Text(l10n.vividMode),
                                  onTap: () {
                                    if (!isPremiumNotifier.value) {
                                      showPremiumDialog(context);
                                      return;
                                    }
                                    mainPageThemeNotifier.value = .vivid;
                                    updateHoverFocusColor();
                                  },
                                  trailing: ValueListenableBuilder(
                                    valueListenable: isPremiumNotifier,
                                    builder: (context, isPremium, child) {
                                      if (!isPremium) {
                                        return Icon(Icons.lock);
                                      }
                                      return value == .vivid
                                          ? Icon(Icons.check)
                                          : SizedBox.shrink();
                                    },
                                  ),
                                ),
                                ListTile(
                                  title: Text(l10n.lightMode),
                                  onTap: () {
                                    mainPageThemeNotifier.value = .light;
                                    updateHoverFocusColor();
                                  },
                                  trailing: value == .light
                                      ? Icon(Icons.check)
                                      : null,
                                ),
                                ListTile(
                                  title: Text(l10n.darkMode),
                                  onTap: () {
                                    mainPageThemeNotifier.value = .dark;
                                    updateHoverFocusColor();
                                  },
                                  trailing: value == .dark
                                      ? Icon(Icons.check)
                                      : null,
                                ),
                              ],
                            );
                          },
                        ),
                      ),
                      sliverBox(
                        ValueListenableBuilder(
                          valueListenable: lyricsPageThemeNotifier,
                          builder: (context, value, child) {
                            final l10n = AppLocalizations.of(context);
                            return Column(
                              children: [
                                Text(
                                  l10n.lyricsPageTheme,
                                  style: .new(fontSize: 18, fontWeight: .bold),
                                ),
                                ListTile(
                                  title: Text(l10n.vividMode),
                                  onTap: () {
                                    lyricsPageThemeNotifier.value = .vivid;
                                  },
                                  trailing: value == .vivid
                                      ? Icon(Icons.check)
                                      : null,
                                ),
                                ListTile(
                                  title: Text(l10n.lightMode),
                                  onTap: () {
                                    lyricsPageThemeNotifier.value = .light;
                                  },
                                  trailing: value == .light
                                      ? Icon(Icons.check)
                                      : null,
                                ),
                                ListTile(
                                  title: Text(l10n.darkMode),
                                  onTap: () {
                                    lyricsPageThemeNotifier.value = .dark;
                                  },
                                  trailing: value == .dark
                                      ? Icon(Icons.check)
                                      : null,
                                ),
                              ],
                            );
                          },
                        ),
                      ),
                    ],
                  ),
                ),
              );
            },
          ),
        );
        mainPageThemeNotifier.removeListener(_updateMainPageTheme);
        lyricsPageThemeNotifier.removeListener(_updateLyricsPageTheme);
      },
    );
  }

  Widget equalizerListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(equalizerImage, size: iconSize),
      title: Text(l10n.equalizer),
      onTap: () {
        if (!isPremiumNotifier.value) {
          showPremiumDialog(context);
          return;
        }
        showAnimationDialog(
          context: context,
          child: OrientationBuilder(
            builder: (context, orientation) {
              final size = MediaQuery.of(context).size;
              final shortSide = size.shortestSide;

              bool isPhone = shortSide < 600;
              if (isMobile && orientation == .portrait) {
                return SizedBox(
                  height: 500,
                  width: isPhone ? 300 : 400,
                  child: EqualizerWidget(),
                );
              } else {
                return SizedBox(
                  height: isPhone ? 350 : 400,
                  width: 540,
                  child: EqualizerWidget(),
                );
              }
            },
          ),
        );
      },
      trailing: ValueListenableBuilder(
        valueListenable: isPremiumNotifier,
        builder: (context, value, child) {
          if (value) {
            return SizedBox.shrink();
          }
          return Icon(Icons.lock);
        },
      ),
    );
  }

  Widget immersiveWideLayoutListTile(AppLocalizations l10n) {
    return ListTile(
      leading: Transform.scale(
        scale: 0.9,
        child: ImageIcon(fullscreenImage, size: iconSize),
      ),

      title: Text(l10n.immersiveWideLayout),
      trailing: SizedBox(
        width: 50,
        child: Builder(
          builder: (context) {
            return MySwitch(
              valueNotifier: immersiveWideLayoutNotifier,
              onToggleCallBack: () {
                if (!isTooNarrow(context)) {
                  applyWideLayoutSystemUiMode();
                }
                setting.save();
              },
            );
          },
        ),
      ),
    );
  }

  // 宽布局关闭沉浸模式时才显示，分别控制顶/底系统条
  Widget wideSystemBarListTile(AppLocalizations l10n, {required bool top}) {
    return ValueListenableBuilder(
      valueListenable: immersiveWideLayoutNotifier,
      builder: (context, immersive, child) {
        if (immersive) {
          return const SizedBox.shrink();
        }
        return child!;
      },
      child: ListTile(
        leading: Padding(
          padding: const EdgeInsets.only(left: 30),
          child: Icon(
            top ? Icons.border_top : Icons.border_bottom,
            size: iconSize,
            color: iconColor.value,
          ),
        ),
        title: Text(
          top ? l10n.wideShowTopSystemBar : l10n.wideShowBottomSystemBar,
        ),
        trailing: SizedBox(
          width: 50,
          child: Builder(
            builder: (context) {
              return MySwitch(
                valueNotifier: top
                    ? wideShowTopSystemBarNotifier
                    : wideShowBottomSystemBarNotifier,
                onToggleCallBack: () {
                  if (!isTooNarrow(context)) {
                    applyWideLayoutSystemUiMode();
                  }
                  setting.save();
                },
              );
            },
          ),
        ),
      ),
    );
  }

  Widget autoPlayOnStartupListTile(AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(playOutlinedImage, size: iconSize),

      title: Text(l10n.autoPlayOnStartup),
      trailing: SizedBox(
        width: 50,
        child: MySwitch(
          valueNotifier: autoPlayOnStartupNotifier,
          onToggleCallBack: () {
            setting.save();
          },
        ),
      ),
    );
  }

  Widget exitOnClose(AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(powerOffImage),

      title: Text(l10n.closeAction),
      trailing: SizedBox(
        width: 150,
        child: Row(
          children: [
            Spacer(),
            MySwitch(
              trueText: l10n.exit,
              falseText: l10n.hide,
              valueNotifier: exitOnCloseNotifier,
              onToggleCallBack: () {
                setting.save();
              },
            ),
          ],
        ),
      ),
    );
  }

  Widget checkUpdate(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(checkUpdateImage, size: iconSize),
      title: Text(l10n.checkUpdate),
      onTap: () async {
        final url = Uri.parse(
          'https://api.github.com/repos/AfalpHy/sylvakru/releases/latest',
        );

        try {
          final response = await http
              .get(url)
              .timeout(const Duration(seconds: 3));
          if (response.statusCode != 200) {
            if (context.mounted) {
              showCenterMessage(
                'Failed to fetch GitHub release:${response.statusCode}',
              );
            }
            return;
          }
          final data = jsonDecode(response.body);
          String latestVersion = (data['tag_name'] as String).replaceFirst(
            'v',
            '',
          );
          if (compareVersion(latestVersion, versionNumber) > 0) {
            if (context.mounted) {
              showAnimationDialog(
                context: context,

                child: SizedBox(
                  height: isMobile ? 350 : 400,
                  width: isMobile ? 320 : 400,
                  child: Padding(
                    padding: const EdgeInsets.all(20),
                    child: Column(
                      children: [
                        Expanded(
                          child: Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 30),
                            child: ListView(
                              children: [
                                Center(
                                  child: Text(
                                    data['tag_name'] as String,
                                    style: TextStyle(
                                      fontSize: 20,
                                      fontWeight: .bold,
                                    ),
                                  ),
                                ),
                                SizedBox(height: 10),

                                Text(data['body'] as String),
                              ],
                            ),
                          ),
                        ),
                        SizedBox(height: 20),
                        ValueListenableBuilder(
                          valueListenable: buttonColor.valueNotifier,
                          builder: (context, value, child) {
                            return Row(
                              children: [
                                Spacer(),
                                ElevatedButton(
                                  onPressed: () => Navigator.pop(context),
                                  style: ElevatedButton.styleFrom(
                                    backgroundColor: value,
                                  ),
                                  child: Text(l10n.cancel),
                                ),
                                SizedBox(width: 20),
                                ElevatedButton(
                                  onPressed: () => launchUrl(
                                    Uri.parse(
                                      "https://github.com/AfalpHy/sylvakru/releases/latest",
                                    ),
                                  ),
                                  style: ElevatedButton.styleFrom(
                                    backgroundColor: value,
                                  ),
                                  child: Text(l10n.go2Download),
                                ),
                                Spacer(),
                              ],
                            );
                          },
                        ),
                      ],
                    ),
                  ),
                ),
              );
            }
          } else {
            if (context.mounted) {
              showCenterMessage(l10n.alreadyLatest);
            }
          }
        } catch (e) {
          if (context.mounted) {
            showCenterMessage(
              'Failed to fetch GitHub release:$e',
              duration: 5000,
            );
          }
        }
      },
    );
  }

  Widget viewLogListTile(BuildContext context, AppLocalizations l10n) {
    return ListTile(
      leading: ImageIcon(exportLogImage, size: iconSize),

      title: Text(l10n.viewLog),
      onTap: () async {
        showAnimationDialog(
          context: context,
          child: Builder(
            builder: (context) {
              return ConstrainedBox(
                constraints: BoxConstraints(
                  maxHeight: MediaQuery.heightOf(context) * 0.75,
                  maxWidth: isTooNarrow(context) ? 300 : 400,
                ),
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    children: [
                      Expanded(
                        child: SingleChildScrollView(
                          padding: const EdgeInsets.all(16),
                          child: SelectableText(logger.logContent),
                        ),
                      ),
                      SizedBox(height: 20),
                      if (isMobile)
                        ValueListenableBuilder(
                          valueListenable: buttonColor.valueNotifier,
                          builder: (context, value, child) {
                            return ElevatedButton(
                              style: ElevatedButton.styleFrom(
                                backgroundColor: buttonColor.value,
                                shape: RoundedRectangleBorder(
                                  borderRadius: BorderRadius.circular(10),
                                ),
                                padding: EdgeInsets.all(10),
                              ),
                              onPressed: () async {
                                String? result;
                                if (Platform.isAndroid) {
                                  result = await FilePicker.getDirectoryPath();
                                  if (result == null) {
                                    return;
                                  }
                                  logger.export2Directory(result);
                                  if (context.mounted) {
                                    showCenterMessage('Export to $result');
                                  }
                                } else {
                                  result = '${appDocsDir.path}/logs';
                                  logger.export2Directory(result);
                                  showCenterMessage('Export to Sylvakru/logs');
                                }
                              },
                              child: Text(l10n.exportLog),
                            );
                          },
                        ),
                    ],
                  ),
                ),
              );
            },
          ),
        );
      },
    );
  }
}

String streamQualityLabel(AppLocalizations l10n, StreamQuality quality) {
  switch (quality) {
    case StreamQuality.standard:
      return l10n.streamQualityStandard;
    case StreamQuality.high:
      return l10n.streamQualityHigh;
    case StreamQuality.hq:
      return l10n.streamQualityHq;
    case StreamQuality.sq:
      return l10n.streamQualitySq;
    case StreamQuality.original:
      return l10n.streamQualityOriginal;
  }
}

String _cacheMb(Iterable<CacheEntry> entries) {
  var bytes = 0;
  for (final entry in entries) {
    bytes += entry.size;
  }
  return '${(bytes / (1024 * 1024)).toStringAsFixed(1)}MB';
}

String _cacheKindLabel(AppLocalizations l10n, CacheKind kind) {
  switch (kind) {
    case CacheKind.song:
      return l10n.cacheSongs;
    case CacheKind.cover:
      return l10n.cacheCovers;
    case CacheKind.lyric:
      return l10n.cacheLyrics;
    case CacheKind.list:
      return l10n.cacheLists;
  }
}

/// Per-source cache contents: one row per song, showing which parts are on
/// disk, with a detail view (cover and lyrics) and a way to drop a single
/// song's files or the whole cache.
Future<void> showCacheDetailDialog(
  BuildContext context,
  AppLocalizations l10n,
) async {
  if (Loader.busy) {
    showCenterMessage(l10n.syncLibrary);
    return;
  }
  await library.refreshCache();
  if (!context.mounted) {
    return;
  }

  showAnimationDialog(
    context: context,
    child: SizedBox(
      width: 340,
      height: 480,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          children: [
            SizedBox(
              height: 32,
              child: Text(
                l10n.cacheDetails,
                style: TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
              ),
            ),
            Row(
              children: [
                Expanded(
                  child: ValueListenableBuilder(
                    valueListenable: cacheEntriesNotifier,
                    builder: (context, entries, child) {
                      return Text(
                        '${entries.length} · ${_cacheMb(entries)}',
                        style: TextStyle(fontSize: 12),
                      );
                    },
                  ),
                ),
                ValueListenableBuilder(
                  valueListenable: precacheProgressNotifier,
                  builder: (context, progress, child) {
                    if (progress == null) {
                      return const SizedBox.shrink();
                    }
                    return Text(
                      '${progress.$1}/${progress.$2}',
                      style: TextStyle(fontSize: 12),
                    );
                  },
                ),
              ],
            ),
            ValueListenableBuilder(
              valueListenable: precacheProgressNotifier,
              builder: (context, progress, child) {
                if (progress == null) {
                  return const SizedBox.shrink();
                }
                return Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: LinearProgressIndicator(
                    value: progress.$2 == 0 ? null : progress.$1 / progress.$2,
                  ),
                );
              },
            ),
            const SizedBox(height: 6),
            Expanded(
              child: ValueListenableBuilder(
                valueListenable: cacheEntriesNotifier,
                builder: (context, entries, child) {
                  final songs = groupCacheBySong(entries);
                  final lists = entries
                      .where((entry) => entry.kind == CacheKind.list)
                      .toList();
                  if (songs.isEmpty && lists.isEmpty) {
                    return Center(child: Text(l10n.cacheEmpty));
                  }
                  return ListView(
                    children: [
                      for (final cached in songs)
                        _cachedSongTile(context, l10n, cached),
                      if (lists.isNotEmpty) ...[
                        const Divider(height: 12),
                        ListTile(
                          dense: true,
                          visualDensity: VisualDensity.compact,
                          title: Text(_cacheKindLabel(l10n, CacheKind.list)),
                          subtitle: Text('${lists.length} · ${_cacheMb(lists)}'),
                        ),
                      ],
                    ],
                  );
                },
              ),
            ),
            const Divider(height: 12),
            Row(
              children: [
                TextButton(
                  onPressed: () async {
                    if (!await showConfirmDialog(context, l10n.precacheAll)) {
                      return;
                    }
                    showCenterLoading();
                    await library.precacheSongs(library.songList);
                    removeCenterLoading();
                  },
                  child: Text(l10n.precacheAll),
                ),
                const Spacer(),
                TextButton(
                  onPressed: () async {
                    if (!await showConfirmDialog(context, l10n.clear)) {
                      return;
                    }
                    showCenterLoading();
                    layersManager.clearDataLayers();
                    audioHandler.clear();
                    await library.clearCache();
                    await library.clearPicture();
                    await library.clearLrcCache();
                    playlistManager.updateNotifier.value++;
                    removeCenterLoading();
                  },
                  child: Text(l10n.clear),
                ),
              ],
            ),
          ],
        ),
      ),
    ),
  );
}

/// One cached song: what is on disk (song / cover / lyrics), how big it is, a
/// detail view, and a delete that asks first.
Widget _cachedSongTile(
  BuildContext context,
  AppLocalizations l10n,
  CachedSong cached,
) {
  final cover = cached.cover;
  return ListTile(
    dense: true,
    visualDensity: VisualDensity.compact,
    leading: cover == null
        ? const Icon(Icons.music_note, size: 20)
        : ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: Image.file(
              File(cover.path),
              width: 36,
              height: 36,
              fit: BoxFit.cover,
              errorBuilder: (context, error, stack) =>
                  const Icon(Icons.music_note, size: 20),
            ),
          ),
    title: Text(
      getTitle(cached.song),
      maxLines: 1,
      overflow: TextOverflow.ellipsis,
    ),
    subtitle: Text(
      '${_cachedTags(l10n, cached)} · '
      '${(cached.size / (1024 * 1024)).toStringAsFixed(1)}MB',
    ),
    onTap: () => showCachedSongDialog(context, l10n, cached),
    trailing: IconButton(
      tooltip: l10n.removeCache,
      icon: const Icon(Icons.delete_outline),
      onPressed: () async {
        if (!await showConfirmDialog(context, l10n.removeCache)) {
          return;
        }
        await library.deleteCachedSong(cached);
      },
    ),
  );
}

/// Which parts of a song are cached, as short tags - only the ones present.
String _cachedTags(AppLocalizations l10n, CachedSong cached) {
  final tags = <String>[
    if (cached.hasCover) l10n.cacheTagCover,
    if (cached.hasLyric) l10n.cacheTagLyric,
    if (cached.hasAudio) l10n.cacheTagAudio,
  ];
  return tags.isEmpty ? l10n.cacheEmpty : tags.join(' · ');
}

/// What one cached song actually holds: the cover image and the lyrics.
Future<void> showCachedSongDialog(
  BuildContext context,
  AppLocalizations l10n,
  CachedSong cached,
) async {
  final cover = cached.cover;
  final lyric = cached.lyric;

  var lyrics = '';
  if (lyric != null) {
    final file = File(lyric.path);
    if (await file.exists()) {
      lyrics = await file.readAsString();
    }
  }
  if (!context.mounted) {
    return;
  }

  showAnimationDialog(
    context: context,
    child: SizedBox(
      width: 340,
      height: 460,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          children: [
            Text(
              getTitle(cached.song),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
            ),
            Text(
              getArtist(cached.song),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(fontSize: 12),
            ),
            const SizedBox(height: 8),
            if (cover != null)
              ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: Image.file(
                  File(cover.path),
                  height: 160,
                  fit: BoxFit.cover,
                  errorBuilder: (context, error, stack) => const SizedBox(
                    height: 160,
                    child: Icon(Icons.broken_image),
                  ),
                ),
              )
            else
              Text('${l10n.cacheTagCover}: -'),
            const SizedBox(height: 8),
            Expanded(
              child: SingleChildScrollView(
                child: Text(
                  lyrics.isEmpty ? l10n.noLyrics : lyrics,
                  style: TextStyle(fontSize: 13),
                ),
              ),
            ),
          ],
        ),
      ),
    ),
  );
}
