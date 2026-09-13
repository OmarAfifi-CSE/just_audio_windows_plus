import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('Root MethodChannel (com.ryanheise.just_audio.methods) Protocol Tests', () {
    setUp(() {
      MockAudioEngine.install();
    });

    tearDown(() {
      MockAudioEngine.uninstall();
    });

    test('init creates player with valid id', () async {
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': 'player_1',
      });
      expect(MockAudioEngine.activePlayers.containsKey('player_1'), isTrue);
      expect(MockAudioEngine.getPlayer('player_1')?.isDisposed, isFalse);
    });

    test('init throws PlatformException argument_error on missing id', () async {
      expect(
        () => MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{}),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'argument_error',
        )),
      );
    });

    test('init throws PlatformException argument_error on null id', () async {
      expect(
        () => MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': null}),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'argument_error',
        )),
      );
    });

    test('init with pre-existing id cleanly re-creates player without leaking', () async {
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': 'duplicate_id_player',
      });
      final firstInstance = MockAudioEngine.getPlayer('duplicate_id_player');
      expect(firstInstance, isNotNull);
      expect(firstInstance!.isDisposed, isFalse);

      // Re-initialize with same ID
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': 'duplicate_id_player',
      });
      final secondInstance = MockAudioEngine.getPlayer('duplicate_id_player');
      expect(secondInstance, isNotNull);
      expect(identical(firstInstance, secondInstance), isFalse);
      expect(firstInstance.isDisposed, isTrue);
      expect(secondInstance!.isDisposed, isFalse);
    });

    test('disposePlayer removes and disposes player instance', () async {
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': 'player_to_dispose',
      });
      final player = MockAudioEngine.getPlayer('player_to_dispose');
      expect(player, isNotNull);

      await MockAudioEngine.rootChannel.invokeMethod('disposePlayer', <String, dynamic>{
        'id': 'player_to_dispose',
      });
      expect(MockAudioEngine.activePlayers.containsKey('player_to_dispose'), isFalse);
      expect(player!.isDisposed, isTrue);
    });

    test('disposePlayer throws PlatformException argument_error on missing id', () async {
      expect(
        () => MockAudioEngine.rootChannel.invokeMethod('disposePlayer', <String, dynamic>{}),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'argument_error',
        )),
      );
    });

    test('disposeAllPlayers purges 30 active players in a single atomic pass', () async {
      for (var i = 0; i < 30; i++) {
        await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
          'id': 'player_$i',
        });
      }
      expect(MockAudioEngine.activePlayers.length, 30);

      await MockAudioEngine.rootChannel.invokeMethod('disposeAllPlayers', <String, dynamic>{});
      expect(MockAudioEngine.activePlayers.isEmpty, isTrue);
    });

    test('unrecognized root channel method throws MissingPluginException', () async {
      expect(
        () => MockAudioEngine.rootChannel.invokeMethod('invalidRootMethod', <String, dynamic>{}),
        throwsA(isA<MissingPluginException>()),
      );
    });
  });
}
