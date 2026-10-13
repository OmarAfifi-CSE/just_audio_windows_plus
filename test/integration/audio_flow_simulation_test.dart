import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../helpers/mock_audio_engine.dart';

/// End-to-end flows through the channel contract: the sequences an application
/// actually performs, asserted against the same rules the native engine
/// enforces. The equivalent flows against the real compiled plugin live in
/// `example/integration_test/native_audio_test.dart`.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(MockAudioEngine.install);
  tearDown(MockAudioEngine.uninstall);

  Future<MockPlayerInstance> init(String id) async {
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': id});
    return MockAudioEngine.getPlayer(id)!;
  }

  test('a full load, play, seek, pause and resume cycle', () async {
    final player = await init('flow_player');
    final channel = player.methodChannel;

    final states = <int>[];
    final subscription = player.eventChannel
        .receiveBroadcastStream()
        .listen((dynamic event) {
      states.add((event as Map<dynamic, dynamic>)['processingState'] as int);
    });
    await Future<void>.delayed(const Duration(milliseconds: 20));

    final load =
        await channel.invokeMapMethod<String, dynamic>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist('flow'),
    });
    expect(load!['duration'], 180000000);
    expect(player.sourceSet, isTrue);

    var playCompleted = false;
    final play = channel
        .invokeMethod<void>('play', <String, dynamic>{})
        .then((_) => playCompleted = true);
    expect(player.playing, isTrue);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(playCompleted, isFalse);

    await channel.invokeMethod<void>('seek', <String, dynamic>{
      'index': 2,
      'position': 90000000,
    });
    expect(player.currentIndex, 2);
    expect(player.positionUs, 90000000);

    await channel.invokeMethod<void>('pause', <String, dynamic>{});
    await play.timeout(const Duration(seconds: 1));
    expect(playCompleted, isTrue);
    expect(player.playing, isFalse);

    await subscription.cancel();
    expect(states, isNotEmpty);
  });

  test('playback completes at the end of media and stays replayable', () async {
    final player = await init('completion_player');
    final channel = player.methodChannel;

    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': progressiveSource('a', 'file:///audio/0.mp3'),
    });

    var playCompleted = false;
    final play = channel
        .invokeMethod<void>('play', <String, dynamic>{})
        .then((_) => playCompleted = true);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(playCompleted, isFalse,
        reason: 'play must not resolve before the end of media');

    // The native engine settles the play from MediaEnded.
    player.completed = true;
    player.broadcastState();
    player.completePlay();
    await play.timeout(const Duration(seconds: 1));

    expect(playCompleted, isTrue);
    expect(player.processingState, 4);
    expect(player.playing, isTrue,
        reason: 'the playing intent survives completion, as just_audio expects');

    // Replaying from completed re-arms the lifecycle.
    await channel.invokeMethod<void>('play', <String, dynamic>{});
    expect(player.completed, isFalse);
    expect(player.processingState, 3);
  });

  test('a load failure rolls the source back and reports the message',
      () async {
    final player = await init('failure_player');
    final channel = player.methodChannel;

    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': progressiveSource('a', 'file:///audio/0.mp3'),
    });
    expect(player.sourceSet, isTrue);

    player.fail('WinRT: 0x80070005 access denied');
    expect(player.sourceSet, isFalse);
    expect(player.failed, isTrue);
    expect(player.processingState, 0);

    // A fresh load recovers the player.
    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': progressiveSource('a', 'file:///audio/0.mp3'),
    });
    expect(player.sourceSet, isTrue);
    expect(player.failed, isFalse);
  });

  test('an invalid mutation leaves the loaded source usable', () async {
    final player = await init('resilience_player');
    final channel = player.methodChannel;

    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist('resilience'),
    });

    for (final (String name, Map<String, dynamic> args) request
        in <(String, Map<String, dynamic>)>[
      ('setVolume', <String, dynamic>{'volume': 'loud'}),
      ('setVolume', <String, dynamic>{'volume': double.nan}),
      ('setSpeed', <String, dynamic>{'speed': -1.0}),
      ('setLoopMode', <String, dynamic>{'loopMode': 7}),
      ('seek', <String, dynamic>{'index': 1 << 40}),
      (
        'concatenatingInsertAll',
        <String, dynamic>{
          'id': 'root',
          'index': 1,
          'children': <Map<String, dynamic>>[
            progressiveSource('x', 'file:///audio/x.mp3'),
          ],
          'shuffleOrder': <int>[0, 0],
        },
      ),
      (
        'setShuffleOrder',
        <String, dynamic>{'audioSource': threeTrackPlaylist('mismatch')},
      ),
    ]) {
      await expectLater(
        channel.invokeMethod<void>(request.$1, request.$2),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'argument_error')),
        reason: '${request.$1} should have been rejected',
      );
    }

    // The original source is untouched and still plays. `play` is deliberately
    // not awaited: the engine holds it open until pause or end of media.
    expect(player.leaves, hasLength(3));
    unawaited(channel.invokeMethod<void>('play', <String, dynamic>{}));
    expect(player.playing, isTrue);
  });

  test('two players stay isolated', () async {
    final a = await init('player_a');
    final b = await init('player_b');

    await a.methodChannel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist('a'),
    });
    await b.methodChannel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': progressiveSource('b', 'file:///audio/b.mp3'),
    });

    await a.methodChannel
        .invokeMethod<void>('setVolume', <String, dynamic>{'volume': 0.25});
    await a.methodChannel
        .invokeMethod<void>('setSpeed', <String, dynamic>{'speed': 1.5});
    await a.methodChannel
        .invokeMethod<void>('setLoopMode', <String, dynamic>{'loopMode': 2});
    unawaited(
      a.methodChannel.invokeMethod<void>('play', <String, dynamic>{}),
    );

    expect(a.volume, 0.25);
    expect(a.speed, 1.5);
    expect(a.loopMode, 2);
    expect(a.playing, isTrue);

    expect(b.volume, 1.0);
    expect(b.speed, 1.0);
    expect(b.loopMode, 0);
    expect(b.playing, isFalse);
    expect(b.leaves, hasLength(1));
  });

  test('disposing one player leaves the other intact', () async {
    final a = await init('dispose_a');
    final b = await init('dispose_b');
    await a.methodChannel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': progressiveSource('a', 'file:///audio/a.mp3'),
    });
    await b.methodChannel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': progressiveSource('b', 'file:///audio/b.mp3'),
    });

    await MockAudioEngine.rootChannel
        .invokeMethod<void>('disposePlayer', <String, dynamic>{'id': 'dispose_a'});
    expect(a.isDisposed, isTrue);
    expect(b.isDisposed, isFalse);

    unawaited(b.methodChannel.invokeMethod<void>('play', <String, dynamic>{}));
    expect(b.playing, isTrue);
  });

  test('replacing a playlist applies the new shuffle order', () async {
    final player = await init('shuffle_player');
    final channel = player.methodChannel;

    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': concatenatingSource(
        'root',
        <Map<String, dynamic>>[
          progressiveSource('a', 'file:///audio/0.mp3'),
          progressiveSource('b', 'file:///audio/1.mp3'),
          progressiveSource('c', 'file:///audio/2.mp3'),
        ],
        <int>[0, 1, 2],
      ),
    });
    expect(player.progressionOrder, <int>[0, 1, 2]);

    await channel.invokeMethod<void>('setShuffleOrder', <String, dynamic>{
      'audioSource': concatenatingSource(
        'root',
        <Map<String, dynamic>>[
          progressiveSource('a', 'file:///audio/0.mp3'),
          progressiveSource('b', 'file:///audio/1.mp3'),
          progressiveSource('c', 'file:///audio/2.mp3'),
        ],
        <int>[0, 2, 1],
      ),
    });
    expect(player.progressionOrder, <int>[0, 2, 1]);
  });
}
