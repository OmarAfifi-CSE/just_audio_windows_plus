import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('End-to-End Audio Flow Simulation Tests', () {
    setUp(() {
      MockAudioEngine.install();
    });

    tearDown(() {
      MockAudioEngine.uninstall();
    });

    test('Complete Surah Recitation Lifecycle Flow', () async {
      const playerId = 'surah_player';
      const eventChannelName = 'com.ryanheise.just_audio.events.$playerId';

      // 1. Initialize Player
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': playerId});
      final playerChannel = MethodChannel('com.ryanheise.just_audio.methods.$playerId');
      final player = MockAudioEngine.getPlayer(playerId)!;

      // 2. Connect to EventChannel
      const eventChannel = EventChannel(eventChannelName);
      final receivedStates = <int>[];
      final sub = eventChannel.receiveBroadcastStream().listen((dynamic event) {
        final map = event as Map<dynamic, dynamic>;
        receivedStates.add(map['processingState'] as int);
      });

      // 3. Load Holy Quran Surah Al-Fatihah
      final loadRes = await playerChannel.invokeMapMethod<String, dynamic>('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
      });
      expect(loadRes!['duration'], 180000000);

      // 4. Emit Loading & Buffering events
      player.eventController.add(<String, dynamic>{
        'processingState': 1, // loading
        'updatePosition': 0,
        'updateTime': 100,
        'bufferedPosition': 0,
        'duration': 180000000,
        'currentIndex': 0,
      });

      player.eventController.add(<String, dynamic>{
        'processingState': 2, // buffering
        'updatePosition': 0,
        'updateTime': 200,
        'bufferedPosition': 50000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      // 5. Start Playback
      await playerChannel.invokeMethod('play', <String, dynamic>{});
      expect(player.isPlaying, isTrue);

      player.eventController.add(<String, dynamic>{
        'processingState': 3, // ready
        'updatePosition': 1000000,
        'updateTime': 300,
        'bufferedPosition': 180000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      // 6. Mid-stream Seek
      await playerChannel.invokeMethod('seek', <String, dynamic>{
        'position': 90000000, // 90s halfway
        'index': 0,
      });
      expect(player.positionUs, 90000000);

      // 7. Pause & Resume
      await playerChannel.invokeMethod('pause', <String, dynamic>{});
      expect(player.isPlaying, isFalse);

      await playerChannel.invokeMethod('play', <String, dynamic>{});
      expect(player.isPlaying, isTrue);

      // 8. Track Completion
      player.eventController.add(<String, dynamic>{
        'processingState': 4, // completed
        'updatePosition': 180000000,
        'updateTime': 1000,
        'bufferedPosition': 180000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(receivedStates, containsAllInOrder([1, 2, 3, 4]));

      // 9. Teardown
      await sub.cancel();
      await MockAudioEngine.rootChannel.invokeMethod('disposePlayer', <String, dynamic>{'id': playerId});
      expect(MockAudioEngine.activePlayers.isEmpty, isTrue);
    });

    test('Two-Player Cross-Fade Audio Flow Simulation', () async {
      const playerAId = 'crossfade_a';
      const playerBId = 'crossfade_b';

      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': playerAId});
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': playerBId});

      final channelA = MethodChannel('com.ryanheise.just_audio.methods.$playerAId');
      final channelB = MethodChannel('com.ryanheise.just_audio.methods.$playerBId');

      final playerA = MockAudioEngine.getPlayer(playerAId)!;
      final playerB = MockAudioEngine.getPlayer(playerBId)!;

      // Load both
      await channelA.invokeMethod('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
      });

      await channelB.invokeMethod('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/002.mp3',
        },
      });

      // Player A playing at volume 1.0, Player B ready at volume 0.0
      await channelA.invokeMethod('play', <String, dynamic>{});
      await channelA.invokeMethod('setVolume', <String, dynamic>{'volume': 1.0});

      await channelB.invokeMethod('play', <String, dynamic>{});
      await channelB.invokeMethod('setVolume', <String, dynamic>{'volume': 0.0});

      // Crossfade simulation over 5 steps
      for (var step = 1; step <= 5; step++) {
        final volA = (5 - step) / 5.0;
        final volB = step / 5.0;
        await channelA.invokeMethod('setVolume', <String, dynamic>{'volume': volA});
        await channelB.invokeMethod('setVolume', <String, dynamic>{'volume': volB});
      }

      expect(playerA.volume, 0.0);
      expect(playerB.volume, 1.0);

      // Stop Player A
      await channelA.invokeMethod('stop', <String, dynamic>{});
      expect(playerA.isPlaying, isFalse);
      expect(playerB.isPlaying, isTrue);
    });

    test('Network Drop and Auto-Recovery Flow Simulation', () async {
      const playerId = 'network_recovery_player';
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': playerId});
      final channel = MethodChannel('com.ryanheise.just_audio.methods.$playerId');
      final player = MockAudioEngine.getPlayer(playerId)!;

      // 1. Initial Load & Play
      await channel.invokeMethod('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
      });
      await channel.invokeMethod('play', <String, dynamic>{});

      // 2. Network drop simulation
      player.eventController.addError(PlatformException(
        code: 'networkError',
        message: 'Network connection dropped unexpectedly',
      ));

      // 3. Client handles error and reloads
      await channel.invokeMethod('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
        'initialPosition': 45000000, // Resume from 45s
      });
      await channel.invokeMethod('play', <String, dynamic>{});

      expect(player.isPlaying, isTrue);
      expect(player.sourceSet, isTrue);
    });
  });
}
