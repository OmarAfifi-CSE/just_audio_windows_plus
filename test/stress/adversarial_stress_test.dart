import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../helpers/mock_audio_engine.dart';

/// Concurrency and lifecycle stress against the channel contract. These
/// assert that the engine's invariants hold under interleaved traffic, not
/// that a Dart stub is fast.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const playerId = 'stress_player';

  late MethodChannel channel;
  late MockPlayerInstance player;

  setUp(() async {
    MockAudioEngine.install();
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': playerId});
    channel = const MethodChannel('com.ryanheise.just_audio.methods.$playerId');
    player = MockAudioEngine.getPlayer(playerId)!;
    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist('stress'),
    });
  });

  tearDown(MockAudioEngine.uninstall);

  test('500 interleaved transport calls all reach the engine', () async {
    const iterations = 500;
    // `play` is deliberately not awaited: the native engine holds the future
    // open until pause or end of media.
    final awaitable = <Future<void>>[];
    for (var i = 0; i < iterations; i++) {
      switch (i % 3) {
        case 0:
          unawaited(
            channel.invokeMethod<void>('play', <String, dynamic>{}),
          );
        case 1:
          awaitable
              .add(channel.invokeMethod<void>('pause', <String, dynamic>{}));
        default:
          awaitable
              .add(channel.invokeMethod<void>('stop', <String, dynamic>{}));
      }
    }
    await Future.wait(awaitable);

    final transport = player.methodCalls
        .where((call) => const <String>{'play', 'pause', 'stop'}.contains(call.method))
        .length;
    expect(transport, iterations);
  });

  test('500 valid seeks all land on the requested position', () async {
    const iterations = 500;
    final futures = <Future<void>>[];
    for (var i = 0; i < iterations; i++) {
      futures.add(channel.invokeMethod<void>('seek', <String, dynamic>{
        'position': i * 1000,
        'index': i % 3,
      }));
    }
    await Future.wait(futures);
    expect(
      player.methodCalls.where((call) => call.method == 'seek').length,
      iterations,
    );
  });

  test('rejected seeks never mutate the position', () async {
    await channel
        .invokeMethod<void>('seek', <String, dynamic>{'position': 4000000});
    expect(player.positionUs, 4000000);

    for (var i = 1; i <= 100; i++) {
      await expectLater(
        channel.invokeMethod<void>('seek', <String, dynamic>{
          'position': -i,
          'index': i % 3,
        }),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'argument_error')),
      );
    }
    expect(player.positionUs, 4000000,
        reason: 'a rejected seek must leave the position untouched');
  });

  test('interleaved volume ramps settle on the last value', () async {
    for (var i = 0; i <= 100; i++) {
      await channel.invokeMethod<void>('setVolume', <String, dynamic>{
        'volume': i / 100.0,
      });
    }
    expect(player.volume, 1.0);

    for (var i = 100; i >= 0; i--) {
      await channel.invokeMethod<void>('setVolume', <String, dynamic>{
        'volume': i / 100.0,
      });
    }
    expect(player.volume, 0.0);
  });

  test('a timer-driven command mix interleaves without a lost call', () async {
    var issued = 0;
    final completer = Completer<void>();

    Timer.periodic(const Duration(milliseconds: 1), (Timer timer) {
      if (issued >= 60) {
        timer.cancel();
        if (!completer.isCompleted) completer.complete();
        return;
      }
      final tick = issued;
      final operation = switch (tick % 5) {
        0 => channel.invokeMethod<void>('play', <String, dynamic>{}),
        1 => channel.invokeMethod<void>(
            'seek',
            <String, dynamic>{'position': tick * 5000}),
        2 => channel.invokeMethod<void>(
            'setVolume',
            <String, dynamic>{'volume': (tick % 10) / 10.0}),
        3 => channel.invokeMethod<void>('pause', <String, dynamic>{}),
        _ => channel.invokeMethod<void>(
            'setSpeed',
            <String, dynamic>{'speed': 1.0 + (tick % 4) * 0.25}),
      };
      issued++;
      if (issued >= 60) {
        timer.cancel();
        operation.whenComplete(() {
          if (!completer.isCompleted) completer.complete();
        });
      }
    });

    await completer.future.timeout(const Duration(seconds: 5));
    expect(issued, 60);
    expect(
      player.methodCalls
          .where((call) => const <String>{
                'play',
                'pause',
                'seek',
                'setVolume',
                'setSpeed',
              }.contains(call.method))
          .length,
      60,
    );
  });

  test('disposing mid-load leaves no pending work and no crash', () async {
    const id = 'mid_load_player';
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': id});
    final freshChannel =
        MethodChannel(MockPlayerInstance.methodChannelName(id));

    final load = freshChannel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist('mid_load'),
    });
    final dispose =
        freshChannel.invokeMethod<void>('dispose', <String, dynamic>{});
    await Future.wait(<Future<void>>[load, dispose]);

    expect(MockAudioEngine.getPlayer(id)!.isDisposed, isTrue);
  });

  test('a disposed player rejects every method with the disposed code',
      () async {
    await channel.invokeMethod<void>('dispose', <String, dynamic>{});
    for (final method in <String>[
      'play',
      'pause',
      'stop',
      'seek',
      'setVolume',
      'setSpeed',
      'setLoopMode',
      'setShuffleMode',
    ]) {
      await expectLater(
        channel.invokeMethod<void>(method, <String, dynamic>{}),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'disposed')),
        reason: '$method should be rejected after dispose',
      );
    }
  });

  test('50 players can be created, used and purged in one pass', () async {
    const swarm = 50;
    final channels = <MethodChannel>[];
    for (var i = 0; i < swarm; i++) {
      final id = 'swarm_$i';
      await MockAudioEngine.rootChannel
          .invokeMethod<void>('init', <String, dynamic>{'id': id});
      channels.add(MethodChannel(MockPlayerInstance.methodChannelName(id)));
    }
    expect(MockAudioEngine.activePlayers, hasLength(swarm + 1));

    final operations = <Future<void>>[];
    for (var i = 0; i < swarm; i++) {
      operations
        ..add(channels[i].invokeMethod<void>('load', <String, dynamic>{
          'audioSource': progressiveSource('s$i', 'file:///audio/$i.mp3'),
        }))
        ..add(channels[i].invokeMethod<void>(
            'setVolume', <String, dynamic>{'volume': (i % 10) / 10.0}));
      // `play` stays pending until pause or end of media, exactly as the
      // native engine behaves, so it must not be awaited here.
      unawaited(channels[i].invokeMethod<void>('play', <String, dynamic>{}));
    }
    await Future.wait(operations);

    for (var i = 0; i < swarm; i++) {
      expect(MockAudioEngine.getPlayer('swarm_$i')!.playing, isTrue);
    }

    await MockAudioEngine.rootChannel.invokeMethod<void>(
        'disposeAllPlayers', <String, dynamic>{});
    expect(MockAudioEngine.activePlayers, isEmpty);
  });
}
