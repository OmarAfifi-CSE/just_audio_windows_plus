import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('Per-Player MethodChannel Contract Tests', () {
    const playerId = 'player_unit_test';
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

    test('load progressive URI with remote Holy Quran recitation returns valid duration', () async {
      final res = await channel.invokeMapMethod<String, dynamic>('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
      });
      expect(res, isNotNull);
      expect(res!['duration'], 180000000);
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.sourceSet, isTrue);
      expect(player?.durationUs, 180000000);
    });

    test('load progressive URI with Arabic Unicode and spaces in file path', () async {
      const arabicUri = 'file:///C:/تلاوات قرآنية/الشيخ محمد صديق المنشاوي/001 - الفاتحة.mp3';
      final res = await channel.invokeMapMethod<String, dynamic>('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'progressive',
          'uri': arabicUri,
        },
        'initialPosition': 10000000, // 10s
        'initialIndex': 0,
      });
      expect(res, isNotNull);
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.sourceSet, isTrue);
    });

    test('load clipping audio source correctly calculates duration from bounds', () async {
      final res = await channel.invokeMapMethod<String, dynamic>('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'clipping',
          'start': 10000000,
          'end': 45000000,
          'child': <String, dynamic>{
            'type': 'progressive',
            'uri': 'https://server10.mp3quran.net/minsh/002.mp3',
          },
        },
      });
      expect(res, isNotNull);
      expect(res!['duration'], 35000000); // 45s - 10s = 35s
    });

    test('load clipping source without child throws PlatformException load_error', () async {
      expect(
        () => channel.invokeMethod('load', <String, dynamic>{
          'audioSource': <String, dynamic>{
            'type': 'clipping',
            'start': 0,
          },
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'load_error',
        )),
      );
    });

    test('load with missing source type throws PlatformException load_error', () async {
      expect(
        () => channel.invokeMethod('load', <String, dynamic>{
          'audioSource': <String, dynamic>{
            'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
          },
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'load_error',
        )),
      );
    });

    test('load with unsupported source type throws PlatformException load_error', () async {
      expect(
        () => channel.invokeMethod('load', <String, dynamic>{
          'audioSource': <String, dynamic>{
            'type': 'unsupported_engine_format',
          },
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'load_error',
        )),
      );
    });

    test('load HLS and DASH streaming types succeed without exception', () async {
      for (final type in ['hls', 'dash']) {
        final res = await channel.invokeMapMethod<String, dynamic>('load', <String, dynamic>{
          'audioSource': <String, dynamic>{
            'type': type,
            'uri': 'https://example.com/audio/stream.m3u8',
          },
        });
        expect(res, isNotNull);
      }
    });

    test('play, pause, and stop toggle player state accurately', () async {
      final player = MockAudioEngine.getPlayer(playerId);

      await channel.invokeMethod('play', <String, dynamic>{});
      expect(player?.isPlaying, isTrue);

      await channel.invokeMethod('pause', <String, dynamic>{});
      expect(player?.isPlaying, isFalse);

      player?.positionUs = 5000000;
      await channel.invokeMethod('stop', <String, dynamic>{});
      expect(player?.isPlaying, isFalse);
      expect(player?.positionUs, 0);
    });

    test('seek handles extreme bounds (0ms to 1 hour and negative values safely)', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      final positions = [0, 500000, 30000000, 3600000000, -1000];

      for (final pos in positions) {
        await channel.invokeMethod('seek', <String, dynamic>{
          'position': pos,
          'index': 0,
        });
        expect(player?.positionUs, pos);
      }
    });

    test('setVolume accepts volume levels and updates internal state', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      for (final vol in [0.0, 0.25, 0.5, 0.75, 1.0]) {
        await channel.invokeMethod('setVolume', <String, dynamic>{
          'volume': vol,
        });
        expect(player?.volume, vol);
      }
    });

    test('setSpeed supports variable playback rates', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      for (final spd in [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]) {
        await channel.invokeMethod('setSpeed', <String, dynamic>{
          'speed': spd,
        });
        expect(player?.speed, spd);
      }
    });

    test('setLoopMode updates loop mode flags (0=off, 1=one, 2=all)', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      for (final mode in [0, 1, 2]) {
        await channel.invokeMethod('setLoopMode', <String, dynamic>{
          'loopMode': mode,
        });
        expect(player?.loopMode, mode);
      }
    });

    test('setShuffleMode updates shuffle mode flags (0=none, 1=all)', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      for (final mode in [0, 1]) {
        await channel.invokeMethod('setShuffleMode', <String, dynamic>{
          'shuffleMode': mode,
        });
        expect(player?.shuffleMode, mode);
      }
    });

    test('audio effect stubs return empty success maps without crashing', () async {
      final stubs = [
        'setAndroidAudioAttributes',
        'audioEffectSetEnabled',
        'androidLoudnessEnhancerSetTargetGain',
        'androidEqualizerGetParameters',
        'androidEqualizerBandSetGain',
        'setPitch',
        'setSkipSilence',
      ];

      for (final stub in stubs) {
        final res = await channel.invokeMapMethod<String, dynamic>(stub, <String, dynamic>{});
        expect(res, isNotNull);
      }
    });

    test('dispose is idempotent and marks player disposed', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.isDisposed, isFalse);

      await channel.invokeMethod('dispose', <String, dynamic>{});
      expect(player?.isDisposed, isTrue);

      // Redundant dispose call should succeed safely
      await channel.invokeMethod('dispose', <String, dynamic>{});
      expect(player?.isDisposed, isTrue);
    });
  });
}
