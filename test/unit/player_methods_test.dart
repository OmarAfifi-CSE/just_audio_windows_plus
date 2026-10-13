import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../helpers/mock_audio_engine.dart';

/// Pins the per-player channel contract in `windows/player.hpp`: which
/// arguments each method requires, which values it accepts, and which error
/// codes it returns.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const playerId = 'player_unit_test';
  const channelName = 'com.ryanheise.just_audio.methods.$playerId';

  late MethodChannel channel;
  late MockPlayerInstance player;

  setUp(() async {
    MockAudioEngine.install();
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': playerId});
    channel = const MethodChannel(channelName);
    player = MockAudioEngine.getPlayer(playerId)!;
  });

  tearDown(MockAudioEngine.uninstall);

  final Matcher argumentError = throwsA(isA<PlatformException>()
      .having((e) => e.code, 'code', 'argument_error'));

  group('load', () {
    test('resolves the leaf duration for a progressive source', () async {
      final res = await channel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': progressiveSource('a', 'file:///audio/0.mp3'),
      });
      expect(res, isNotNull);
      expect(res!['duration'], 180000000);
      expect(player.sourceSet, isTrue);
      expect(player.leaves, hasLength(1));
    });

    test('resolves a null duration for a streaming source it cannot decode',
        () async {
      final res = await channel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': streamingSource('a', 'hls', 'https://host/s.m3u8'),
      });
      expect(res, isNotNull);
    });

    test('reports a clipping duration from the bounds, not the child', () async {
      final res = await channel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': clippingSource(
          'clip',
          'a',
          'file:///audio/0.mp3',
          start: 10000000,
          end: 45000000,
        ),
      });
      expect(res!['duration'], 35000000);
    });

    test('honours the initial index and position', () async {
      await channel.invokeMethod<void>('load', {
        'audioSource': threeTrackPlaylist(),
        'initialIndex': 2,
        'initialPosition': 5000000,
      });
      expect(player.currentIndex, 2);
      expect(player.positionUs, 5000000);
    });

    test('rejects an out-of-range initial index', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': threeTrackPlaylist(),
          'initialIndex': 3,
        }),
        argumentError,
      );
    });

    test('a rejected load leaves the loaded source usable', () async {
      // The rejection must be an argument error answered from the load request
      // itself, not routed through the failure path, which would tear the
      // engine down and destroy a perfectly good loaded source.
      await channel.invokeMethod<void>('load', {
        'audioSource': threeTrackPlaylist(),
        'initialIndex': 2,
        'initialPosition': 1000000,
      });
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': threeTrackPlaylist(),
          'initialIndex': 99,
        }),
        argumentError,
      );
      expect(player.sourceSet, isTrue);
      expect(player.failed, isFalse);
      expect(player.currentIndex, 2);
      expect(player.positionUs, 1000000);
    });

    test('rejects a negative initial position', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': threeTrackPlaylist(),
          'initialPosition': -1,
        }),
        argumentError,
      );
    });

    test('rejects a source with no id', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': <String, dynamic>{
            'type': 'progressive',
            'uri': 'file:///audio/0.mp3',
          },
        }),
        argumentError,
      );
    });

    test('rejects a source with no type', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': <String, dynamic>{
            'id': 'a',
            'uri': 'file:///audio/0.mp3',
          },
        }),
        argumentError,
      );
    });

    test('rejects an empty uri', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': progressiveSource('a', ''),
        }),
        argumentError,
      );
    });

    test('rejects an unsupported source type', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': <String, dynamic>{
            'id': 'a',
            'type': 'unsupported_engine_format',
          },
        }),
        argumentError,
      );
    });

    test('rejects a clipping source with no child', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': <String, dynamic>{
            'id': 'clip',
            'type': 'clipping',
            'start': 0,
          },
        }),
        argumentError,
      );
    });

    test('rejects an inverted clipping range', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': clippingSource(
            'clip',
            'a',
            'file:///audio/0.mp3',
            start: 5000000,
            end: 1000000,
          ),
        }),
        argumentError,
      );
    });

    test('rejects direct request headers and points at the proxy', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': <String, dynamic>{
            'id': 'a',
            'type': 'progressive',
            'uri': 'https://host/a.mp3',
            'headers': <String, dynamic>{'Authorization': 'Bearer x'},
          },
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.message,
          'message',
          contains('proxy'),
        )),
      );
    });

    test('rejects a concatenating source whose shuffleOrder is not a permutation',
        () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': concatenatingSource(
            'root',
            <Map<String, dynamic>>[
              progressiveSource('a', 'file:///audio/0.mp3'),
              progressiveSource('b', 'file:///audio/1.mp3'),
            ],
            <int>[0, 0],
          ),
        }),
        argumentError,
      );
    });

    test('rejects a concatenating source with a missing shuffleOrder', () async {
      await expectLater(
        channel.invokeMethod<void>('load', {
          'audioSource': <String, dynamic>{
            'id': 'root',
            'type': 'concatenating',
            'children': <Map<String, dynamic>>[
              progressiveSource('a', 'file:///audio/0.mp3'),
            ],
          },
        }),
        argumentError,
      );
    });

    test('resolves a null duration for an empty playlist and completes play',
        () async {
      final res = await channel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': concatenatingSource('root', <Map<String, dynamic>>[], <int>[]),
      });
      expect(res!['duration'], isNull);
      expect(player.sourceSet, isTrue);
      expect(player.leaves, isEmpty);
    });
  });

  group('transport', () {
    setUp(() async {
      await channel.invokeMethod<void>('load', {
        'audioSource': threeTrackPlaylist(),
      });
    });

    test('play stays pending until pause, then clears the playing intent',
        () async {
      var completed = false;
      final play = channel
          .invokeMethod<void>('play', <String, dynamic>{})
          .then((_) => completed = true);

      expect(player.playing, isTrue);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      expect(completed, isFalse,
          reason: 'play() must not resolve before pause or end of media');

      await channel.invokeMethod<void>('pause', <String, dynamic>{});
      await play;
      expect(completed, isTrue);
      expect(player.playing, isFalse);
    });

    test('play with no source stays pending, as just_audio expects', () async {
      // just_audio's platform activation sends `play` before `load`, so a play
      // that arrives with nothing attached must stay pending. Settling it would
      // resolve `await player.play()` before the track starts.
      const freshId = 'no_source_player';
      await MockAudioEngine.rootChannel
          .invokeMethod<void>('init', <String, dynamic>{'id': freshId});
      final freshChannel =
          MethodChannel('com.ryanheise.just_audio.methods.$freshId');

      var completed = false;
      final play = freshChannel
          .invokeMethod<void>('play', <String, dynamic>{})
          .then((_) => completed = true);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      expect(completed, isFalse,
          reason: 'a source-less play must not resolve before a load');

      // An explicit pause is what settles it, because nothing can ever reach
      // the end of media on its own.
      await freshChannel.invokeMethod<void>('pause', <String, dynamic>{});
      await play.timeout(const Duration(seconds: 1));
      expect(completed, isTrue);
    });

    test('play against an empty playlist resolves immediately', () async {
      // A loaded but empty playlist is a *stable* unplayable state: no item can
      // ever open, so the play is settled at once rather than hanging forever.
      const emptyId = 'empty_playlist_player';
      await MockAudioEngine.rootChannel
          .invokeMethod<void>('init', <String, dynamic>{'id': emptyId});
      final emptyChannel =
          MethodChannel('com.ryanheise.just_audio.methods.$emptyId');
      await emptyChannel.invokeMethod<void>('load', <String, dynamic>{
        'audioSource':
            concatenatingSource('root', <Map<String, dynamic>>[], <int>[]),
      });
      await emptyChannel
          .invokeMethod<void>('play', <String, dynamic>{})
          .timeout(const Duration(seconds: 1));
    });

    test('stop clears the playing intent and rewinds', () async {
      await channel.invokeMethod<void>('seek', <String, dynamic>{
        'position': 5000000,
      });
      expect(player.positionUs, 5000000);

      await channel.invokeMethod<void>('stop', <String, dynamic>{});
      expect(player.playing, isFalse);
      expect(player.positionUs, 0);
    });

    test('stop drops the deferred seek as well as the in-flight one', () async {
      // `seek_pending_` is applied by Opened() when the item opens. Leaving it
      // set after a stop would undo the rewind a moment later.
      await channel.invokeMethod<void>('seek', <String, dynamic>{
        'position': 5000000,
        'index': 2,
      });
      expect(player.seekPending, isTrue);

      await channel.invokeMethod<void>('stop', <String, dynamic>{});
      expect(player.positionUs, 0);
      expect(player.seekPending, isFalse);
      expect(player.seekInProgress, isFalse);
    });
  });

  group('seek', () {
    setUp(() async {
      await channel.invokeMethod<void>('load', {
        'audioSource': threeTrackPlaylist(),
      });
    });

    test('accepts positions across the whole 64-bit range', () async {
      for (final position in <int>[0, 500000, 30000000, 3600000000]) {
        await channel.invokeMethod<void>('seek', <String, dynamic>{
          'position': position,
          'index': 0,
        });
        expect(player.positionUs, position);
      }
    });

    test('rejects a negative position', () async {
      await expectLater(
        channel.invokeMethod<void>('seek', <String, dynamic>{'position': -1000}),
        argumentError,
      );
    });

    test('rejects an index beyond the loaded playlist', () async {
      await expectLater(
        channel.invokeMethod<void>('seek', <String, dynamic>{
          'index': 3,
          'position': 0,
        }),
        argumentError,
      );
    });

    test('rejects an index below -1', () async {
      await expectLater(
        channel.invokeMethod<void>('seek', <String, dynamic>{
          'index': -2,
          'position': 0,
        }),
        argumentError,
      );
    });

    test('an unindexed seek is allowed even with no source', () async {
      final emptyId = 'empty_seek_player';
      await MockAudioEngine.rootChannel
          .invokeMethod<void>('init', <String, dynamic>{'id': emptyId});
      final emptyChannel =
          MethodChannel('com.ryanheise.just_audio.methods.$emptyId');
      await emptyChannel.invokeMethod<void>('seek', <String, dynamic>{
        'position': 500000,
      });
      await expectLater(
        emptyChannel.invokeMethod<void>('seek', <String, dynamic>{
          'index': 0,
          'position': 0,
        }),
        argumentError,
      );
    });
  });

  group('volume, speed and pitch', () {
    test('setVolume accepts the full 0.0 to 1.0 range', () async {
      for (final volume in <double>[0.0, 0.25, 0.5, 0.75, 1.0]) {
        await channel.invokeMethod<void>('setVolume', <String, dynamic>{
          'volume': volume,
        });
        expect(player.volume, volume);
      }
    });

    test('setVolume rejects out-of-range, non-finite and non-numeric values',
        () async {
      for (final volume in <Object>[-0.1, 1.1, double.nan, double.infinity, 'loud']) {
        await expectLater(
          channel.invokeMethod<void>('setVolume', <String, dynamic>{'volume': volume}),
          argumentError,
        );
      }
    });

    test('setSpeed accepts positive rates', () async {
      for (final speed in <double>[0.5, 0.75, 1.0, 1.25, 1.5, 2.0]) {
        await channel.invokeMethod<void>('setSpeed', <String, dynamic>{'speed': speed});
        expect(player.speed, speed);
      }
    });

    test('setSpeed rejects zero, negative and non-finite rates', () async {
      for (final speed in <Object>[0.0, -1.0, double.nan, 'fast']) {
        await expectLater(
          channel.invokeMethod<void>('setSpeed', <String, dynamic>{'speed': speed}),
          argumentError,
        );
      }
    });

    test('setPitch succeeds only for the neutral value', () async {
      await channel.invokeMethod<void>('setPitch', <String, dynamic>{'pitch': 1.0});
      await expectLater(
        channel.invokeMethod<void>('setPitch', <String, dynamic>{'pitch': 1.5}),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'unsupported')),
      );
    });

    test('setPitch without a pitch argument is an argument error', () async {
      await expectLater(
        channel.invokeMethod<void>('setPitch', <String, dynamic>{}),
        argumentError,
      );
    });

    test('setSkipSilence succeeds only when disabled', () async {
      await channel.invokeMethod<void>(
          'setSkipSilence', <String, dynamic>{'enabled': false});
      await expectLater(
        channel.invokeMethod<void>(
            'setSkipSilence', <String, dynamic>{'enabled': true}),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'unsupported')),
      );
    });
  });

  group('loop and shuffle modes', () {
    test('setLoopMode accepts off, one and all', () async {
      for (final mode in <int>[0, 1, 2]) {
        await channel.invokeMethod<void>('setLoopMode', <String, dynamic>{'loopMode': mode});
        expect(player.loopMode, mode);
      }
    });

    test('setLoopMode rejects any other value', () async {
      for (final mode in <int>[-1, 3, 7]) {
        await expectLater(
          channel.invokeMethod<void>('setLoopMode', <String, dynamic>{'loopMode': mode}),
          argumentError,
        );
      }
    });

    test('setShuffleMode accepts off and all', () async {
      for (final mode in <int>[0, 1]) {
        await channel
            .invokeMethod<void>('setShuffleMode', <String, dynamic>{'shuffleMode': mode});
        expect(player.shuffleMode, mode);
      }
    });

    test('setShuffleMode rejects any other value', () async {
      for (final mode in <int>[-1, 2]) {
        await expectLater(
          channel.invokeMethod<void>(
              'setShuffleMode', <String, dynamic>{'shuffleMode': mode}),
          argumentError,
        );
      }
    });

    test('setShuffleOrder applies a new permutation for the loaded tree',
        () async {
      await channel.invokeMethod<void>('load', {
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

      await channel.invokeMethod<void>('setShuffleOrder', {
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

    test('the shuffle order is only applied to playback while shuffling is on',
        () async {
      // just_audio sends a permuted shuffleOrder with every load, shuffle mode
      // notwithstanding. The playlist advances in leaf order while shuffling is
      // off, and in the declared permutation once it is enabled.
      await channel.invokeMethod<void>('load', {
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
      expect(player.playbackOrder, <int>[0, 1, 2],
          reason: 'shuffle off must play the natural leaf order');

      await channel.invokeMethod<void>('setShuffleMode', {'shuffleMode': 1});
      expect(player.playbackOrder, <int>[0, 2, 1]);

      await channel.invokeMethod<void>('setShuffleMode', {'shuffleMode': 0});
      expect(player.playbackOrder, <int>[0, 1, 2],
          reason: 'turning shuffle off restores the natural order');
    });

    test('setShuffleOrder rejects a tree that does not match the loaded one',
        () async {
      await channel.invokeMethod<void>('load', {
        'audioSource': threeTrackPlaylist(),
      });
      await expectLater(
        channel.invokeMethod<void>('setShuffleOrder', {
          'audioSource': concatenatingSource(
            'different',
            <Map<String, dynamic>>[
              progressiveSource('a', 'file:///audio/0.mp3'),
            ],
            <int>[0],
          ),
        }),
        argumentError,
      );
    });
  });

  group('unsupported Android methods', () {
    test('setAndroidAudioAttributes is accepted as a no-op', () async {
      await channel
          .invokeMethod<void>('setAndroidAudioAttributes', <String, dynamic>{});
    });

    test('audio effect methods surface as MissingPluginException', () async {
      for (final method in <String>[
        'audioEffectSetEnabled',
        'androidLoudnessEnhancerSetTargetGain',
        'androidEqualizerGetParameters',
        'androidEqualizerBandSetGain',
      ]) {
        await expectLater(
          channel.invokeMethod<void>(method, <String, dynamic>{}),
          throwsA(isA<MissingPluginException>()),
        );
      }
    });
  });

  group('dispose', () {
    test('succeeds once, then rejects with the disposed code', () async {
      expect(player.isDisposed, isFalse);
      await channel.invokeMethod<void>('dispose', <String, dynamic>{});
      expect(player.isDisposed, isTrue);

      // The native handler checks `disposed_` before dispatching, so a repeat
      // call is reported as disposed rather than silently succeeding.
      await expectLater(
        channel.invokeMethod<void>('dispose', <String, dynamic>{}),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'disposed')),
      );
      expect(player.isDisposed, isTrue);
    });

    test('a disposed player rejects further calls with the disposed code',
        () async {
      await channel.invokeMethod<void>('dispose', <String, dynamic>{});
      await expectLater(
        channel.invokeMethod<void>('play', <String, dynamic>{}),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'disposed')),
      );
    });
  });
}
