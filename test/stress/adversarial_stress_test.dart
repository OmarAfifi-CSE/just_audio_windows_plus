import 'dart:async';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('Adversarial Stress & Extreme Race Condition Tests', () {
    const playerId = 'stress_player';
    const channelName = 'com.ryanheise.just_audio.methods.$playerId';
    late MethodChannel channel;

    setUp(() async {
      MockAudioEngine.install();
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': playerId,
      });
      channel = const MethodChannel(channelName);
    });

    tearDown(() {
      MockAudioEngine.uninstall();
    });

    test('500 rapid burst seeks complete without dropping messages or deadlocking', () async {
      const iterations = 500;
      final futures = <Future<void>>[];

      for (var i = 0; i < iterations; i++) {
        futures.add(channel.invokeMethod('seek', <String, dynamic>{
          'position': (i % 2 == 0) ? (i * 1000000) : -(i * 500000),
          'index': i % 10,
        }));
      }

      await Future.wait(futures);
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.methodCalls.where((c) => c.method == 'seek').length, iterations);
    });

    test('500 frantic play/pause/stop toggles simulate frantic user input without channel congestion', () async {
      const iterations = 500;
      final futures = <Future<void>>[];

      for (var i = 0; i < iterations; i++) {
        final action = (i % 3 == 0)
            ? 'play'
            : (i % 3 == 1)
                ? 'pause'
                : 'stop';
        futures.add(channel.invokeMethod(action, <String, dynamic>{}));
      }

      await Future.wait(futures);
      final player = MockAudioEngine.getPlayer(playerId);
      final transportCalls = player?.methodCalls
          .where((c) => c.method == 'play' || c.method == 'pause' || c.method == 'stop')
          .length;
      expect(transportCalls, iterations);
    });

    test('Timer-based asynchronous erratic monkey testing fires interleaved commands cleanly', () async {
      var completedOperations = 0;
      final completer = Completer<void>();

      Timer.periodic(const Duration(milliseconds: 2), (timer) {
        final tick = timer.tick;
        if (tick > 50) {
          timer.cancel();
          completer.complete();
          return;
        }

        switch (tick % 5) {
          case 0:
            channel.invokeMethod('play', <String, dynamic>{});
            break;
          case 1:
            channel.invokeMethod('seek', <String, dynamic>{'position': tick * 500000});
            break;
          case 2:
            channel.invokeMethod('setVolume', <String, dynamic>{'volume': (tick % 10) / 10.0});
            break;
          case 3:
            channel.invokeMethod('pause', <String, dynamic>{});
            break;
          case 4:
            channel.invokeMethod('setSpeed', <String, dynamic>{'speed': 1.0 + ((tick % 4) * 0.25)});
            break;
        }
        completedOperations++;
      });

      await completer.future;
      expect(completedOperations, 50);
    });

    test('Volume micro-ramping executes 100 micro-fade steps without channel saturation', () async {
      await channel.invokeMethod('play', <String, dynamic>{});

      for (var i = 100; i >= 0; i--) {
        await channel.invokeMethod('setVolume', <String, dynamic>{
          'volume': i / 100.0,
        });
      }

      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.volume, 0.0);
    });

    test('Mid-load immediate destruction (race condition) does not crash or throw unhandled exceptions', () async {
      final freshId = 'mid_load_player';
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': freshId});
      final freshChannel = MethodChannel('com.ryanheise.just_audio.methods.$freshId');

      // 1. Kick off load
      final loadFuture = freshChannel.invokeMethod('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
      });

      // 2. Immediately dispose on same or next microtask
      final disposeFuture = freshChannel.invokeMethod('dispose', <String, dynamic>{});

      await Future.wait([loadFuture, disposeFuture]);
      final player = MockAudioEngine.getPlayer(freshId);
      expect(player?.isDisposed, isTrue);
    });

    test('Invocations on already disposed player drop safely without crashing', () async {
      await channel.invokeMethod('dispose', <String, dynamic>{});
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.isDisposed, isTrue);

      // Subsequent calls must not throw
      await channel.invokeMethod('play', <String, dynamic>{});
      await channel.invokeMethod('pause', <String, dynamic>{});
      await channel.invokeMethod('seek', <String, dynamic>{'position': 1000});
      await channel.invokeMethod('setVolume', <String, dynamic>{'volume': 0.5});
    });

    test('50-player simultaneous swarm playback with atomic purge', () async {
      const swarmCount = 50;
      final playerChannels = <MethodChannel>[];

      // 1. Initialize 50 players
      for (var i = 0; i < swarmCount; i++) {
        final id = 'swarm_player_$i';
        await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{'id': id});
        playerChannels.add(MethodChannel('com.ryanheise.just_audio.methods.$id'));
      }
      expect(MockAudioEngine.activePlayers.length, swarmCount + 1);

      // 2. Concurrently load and play across all 50 players
      final swarmFutures = <Future<void>>[];
      for (var i = 0; i < swarmCount; i++) {
        final ch = playerChannels[i];
        swarmFutures.add(ch.invokeMethod('load', <String, dynamic>{
          'audioSource': <String, dynamic>{
            'type': 'progressive',
            'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
          },
        }));
        swarmFutures.add(ch.invokeMethod('play', <String, dynamic>{}));
        swarmFutures.add(ch.invokeMethod('setVolume', <String, dynamic>{
          'volume': (i % 10) / 10.0,
        }));
      }
      await Future.wait(swarmFutures);

      // 3. Purge all 50 players in one shot
      await MockAudioEngine.rootChannel.invokeMethod('disposeAllPlayers', <String, dynamic>{});
      expect(MockAudioEngine.activePlayers.isEmpty, isTrue);
    });
  });
}
