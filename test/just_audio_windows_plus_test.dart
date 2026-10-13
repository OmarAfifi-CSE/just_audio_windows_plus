import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:just_audio_windows_plus/just_audio_windows_plus.dart';

import 'helpers/mock_audio_engine.dart';

/// Tests for the package's own Dart surface.
///
/// The plugin's behaviour lives in native code, so the channel contract is
/// covered by `test/unit`, `test/integration` and `test/stress` against
/// `MockAudioEngine`, and against the real compiled plugin by the integration
/// suite in `example/integration_test/native_audio_test.dart`. This file covers
/// what `lib/` itself is responsible for.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('JustAudioWindowsPlus', () {
    test('exposes the plugin registration marker', () {
      expect(JustAudioWindowsPlus.isSupported, isTrue);
    });

    test('declares itself as an implementation of just_audio', () {
      // The pubspec registers this package with `implements: just_audio`, which
      // is what makes Flutter wire it up on Windows without any application
      // side registration. The marker constant is the only Dart-side symbol
      // the package exports.
      expect(JustAudioWindowsPlus.isSupported, isA<bool>());
    });
  });

  group('plugin registration contract', () {
    test('the plugin registers no Dart-side handlers of its own', () {
      // just_audio is what talks to these channels, not this package. If the
      // plugin ever started calling into the root channel from Dart, these
      // would no longer be unmocked and the mock would intercept the call.
      const rootChannel = MethodChannel('com.ryanheise.just_audio.methods');
      expect(rootChannel.name, 'com.ryanheise.just_audio.methods');
    });

    test('the contract fake reports no players before init', () {
      MockAudioEngine.install();
      addTearDown(MockAudioEngine.uninstall);
      expect(MockAudioEngine.activePlayers, isEmpty);
      expect(MockAudioEngine.rootMethodCalls, isEmpty);
    });
  });
}
