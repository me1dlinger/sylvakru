import 'package:audio_service/audio_service.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sylvakru/base/audio_handler.dart';
import 'package:sylvakru/base/services/usb_audio_preferences.dart';

void main() {
  test('降低音量立即使用请求值', () {
    expect(nextSafeUsbVolume(0.8, 0.2), 0.2);
    expect(nextSafeUsbVolume(0.8, 0), 0);
  });

  test('提高音量单次最多增加一分贝', () {
    expect(
      nextSafeUsbVolume(0.1, 0.9),
      closeTo(0.1 * dbToUsbVolumeRatio(1), 0.000001),
    );
    expect(
      nextSafeUsbVolume(0.1, 0.11),
      closeTo(0.1 * dbToUsbVolumeRatio(1), 0.000001),
    );
  });

  test('音量安全步进限制输入和输出范围', () {
    expect(nextSafeUsbVolume(-1, 2), closeTo(0.01, 0.000001));
    expect(nextSafeUsbVolume(0.99, 2), 1);
    expect(nextSafeUsbVolume(2, -1), 0);
  });

  test('0% 是静音、100% 是满幅，且不会超过满幅', () {
    expect(usbExclusiveDigitalVolumeGain(0), 0);
    expect(usbExclusiveDigitalVolumeGain(1), 1);
    expect(usbExclusiveDigitalVolumeGain(2), 1);
    expect(usbExclusiveDigitalVolumeGain(-1), 0);
    // Monotonic in between, never above full scale.
    for (final volume in [0.1, 0.3, 0.5, 0.75, 0.99]) {
      final gain = usbExclusiveDigitalVolumeGain(volume);
      expect(gain, inInclusiveRange(0, 1));
      expect(gain, lessThanOrEqualTo(volume));
    }
  });

  test('物理音量键按方向加减一步并夹在 0..1', () {
    expect(
      adjustedRemoteVolume(0.5, AndroidVolumeDirection.raise),
      closeTo(0.52, 0.000001),
    );
    expect(
      adjustedRemoteVolume(0.5, AndroidVolumeDirection.lower),
      closeTo(0.48, 0.000001),
    );
    expect(adjustedRemoteVolume(1, AndroidVolumeDirection.raise), 1);
    expect(adjustedRemoteVolume(0, AndroidVolumeDirection.lower), 0);
  });

  test('音量键方向仅在独占接管时有效', () {
    expect(
      usbExclusiveVolumeKeyDirection(delta: 1, active: true),
      AndroidVolumeDirection.raise,
    );
    expect(
      usbExclusiveVolumeKeyDirection(delta: -1, active: true),
      AndroidVolumeDirection.lower,
    );
    expect(usbExclusiveVolumeKeyDirection(delta: 0, active: true), isNull);
    expect(usbExclusiveVolumeKeyDirection(delta: 1, active: false), isNull);
  });

  test('共享输出始终满幅，独占输出跟随请求值', () {
    expect(outputUserVolume(active: false, requested: 0), 1);
    expect(outputUserVolume(active: true, requested: 0), 0);
    expect(outputUserVolume(active: true, requested: 2), 1);
  });
}
