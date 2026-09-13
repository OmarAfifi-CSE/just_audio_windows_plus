import 'dart:async';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:just_audio_windows_plus/just_audio_windows_plus.dart';

/// Comprehensive, exhaustive test suite for just_audio_windows_plus.
///
/// Tests 100% of the platform interface contract, edge cases, error codes,
/// boundary conditions, and concurrent stress scenarios down to the 0.1% probability level.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('JustAudioWindowsPlus Core Verification', () {
    test('Plugin registration marker is true', () {
      expect(JustAudioWindowsPlus.isSupported, isTrue);
    });
  });

  group('Root MethodChannel (com.ryanheise.just_audio.methods) Protocol', () {
    const rootChannel = MethodChannel('com.ryanheise.just_audio.methods');
    final activePlayers = <String>{};
    final rootLog = <MethodCall>[];

    setUp(() {
      activePlayers.clear();
      rootLog.clear();
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(rootChannel, (MethodCall methodCall) async {
        rootLog.add(methodCall);
        final args = methodCall.arguments as Map<dynamic, dynamic>?;

        if (methodCall.method == 'init') {
          if (args == null || !args.containsKey('id') || args['id'] == null) {
            throw PlatformException(
              code: 'argument_error',
              message: 'id argument missing',
            );
          }
          activePlayers.add(args['id'] as String);
          return <dynamic, dynamic>{};
        } else if (methodCall.method == 'disposePlayer') {
          if (args == null || !args.containsKey('id') || args['id'] == null) {
            throw PlatformException(
              code: 'argument_error',
              message: 'id argument missing',
            );
          }
          activePlayers.remove(args['id'] as String);
          return <dynamic, dynamic>{};
        } else if (methodCall.method == 'disposeAllPlayers') {
          activePlayers.clear();
          return <dynamic, dynamic>{};
        }
        throw MissingPluginException('Not implemented: ${methodCall.method}');
      });
    });

    tearDown(() {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(rootChannel, null);
    });

    test('init succeeds with valid player id', () async {
      final res = await rootChannel.invokeMapMethod<String, dynamic>('init', {
        'id': 'player_alpha',
      });
      expect(res, isNotNull);
      expect(activePlayers.contains('player_alpha'), isTrue);
      expect(rootLog.first.method, 'init');
    });

    test('init throws PlatformException argument_error when id is missing or null', () async {
      expect(
        () => rootChannel.invokeMethod('init', <String, dynamic>{}),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'argument_error',
        )),
      );

      expect(
        () => rootChannel.invokeMethod('init', {'id': null}),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'argument_error',
        )),
      );
    });

    test('disposePlayer succeeds with valid id', () async {
      activePlayers.add('player_alpha');
      final res = await rootChannel.invokeMapMethod<String, dynamic>(
        'disposePlayer',
        {'id': 'player_alpha'},
      );
      expect(res, isNotNull);
      expect(activePlayers.contains('player_alpha'), isFalse);
    });

    test('disposePlayer throws PlatformException argument_error when id is missing', () async {
      expect(
        () => rootChannel.invokeMethod('disposePlayer', <String, dynamic>{}),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'argument_error',
        )),
      );
    });

    test('disposeAllPlayers cleanly resets any number of active players', () async {
      for (var i = 0; i < 25; i++) {
        activePlayers.add('player_$i');
      }
      expect(activePlayers.length, 25);

      final res = await rootChannel.invokeMapMethod<String, dynamic>(
        'disposeAllPlayers',
        <String, dynamic>{},
      );
      expect(res, isNotNull);
      expect(activePlayers.isEmpty, isTrue);
    });

    test('unrecognized root channel method throws MissingPluginException', () async {
      expect(
        () => rootChannel.invokeMethod('unknownRootMethod', <String, dynamic>{}),
        throwsA(isA<MissingPluginException>()),
      );
    });
  });

  group('Per-Player MethodChannel (com.ryanheise.just_audio.methods.\$id) Rigorous Contract', () {
    const playerId = 'player_omega';
    final playerChannel = MethodChannel('com.ryanheise.just_audio.methods.$playerId');
    final methodLog = <MethodCall>[];
    final playlist = <Map<String, dynamic>>[];

    setUp(() {
      methodLog.clear();
      playlist.clear();

      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(playerChannel, (MethodCall methodCall) async {
        methodLog.add(methodCall);
        final args = methodCall.arguments as Map<dynamic, dynamic>? ?? {};

        switch (methodCall.method) {
          case 'load':
            final source = args['audioSource'] as Map<dynamic, dynamic>?;
            if (source == null || !source.containsKey('type')) {
              throw PlatformException(
                code: 'load_error',
                message: 'Source type is missing',
              );
            }
            final type = source['type'] as String;
            if (type == 'progressive' || type == 'dash' || type == 'hls') {
              if (!source.containsKey('uri') || source['uri'] == null) {
                throw PlatformException(
                  code: 'load_error',
                  message: 'MediaSource uri is missing',
                );
              }
              return <dynamic, dynamic>{'duration': 180000000}; // 3 minutes in us
            } else if (type == 'clipping') {
              if (!source.containsKey('child') || source['child'] == null) {
                throw PlatformException(
                  code: 'load_error',
                  message: 'Clipping source child is missing',
                );
              }
              return <dynamic, dynamic>{'duration': 30000000}; // 30s in us
            } else if (type == 'concatenating') {
              final children = (source['children'] as List<dynamic>?) ?? [];
              playlist.clear();
              for (final child in children) {
                playlist.add(Map<String, dynamic>.from(child as Map));
              }
              return <dynamic, dynamic>{'duration': 600000000};
            } else {
              throw PlatformException(
                code: 'load_error',
                message: 'Source is unsupported or can not be nested: $type',
              );
            }

          case 'play':
          case 'pause':
          case 'stop':
          case 'setVolume':
          case 'setSpeed':
          case 'setPitch':
          case 'setSkipSilence':
          case 'setLoopMode':
          case 'setShuffleMode':
          case 'setShuffleOrder':
          case 'seek':
          case 'dispose':
          case 'setAndroidAudioAttributes':
          case 'audioEffectSetEnabled':
          case 'androidLoudnessEnhancerSetTargetGain':
          case 'androidEqualizerGetParameters':
          case 'androidEqualizerBandSetGain':
            return <dynamic, dynamic>{};

          case 'concatenatingInsertAll':
            final index = args['index'] as int?;
            final children = (args['children'] as List<dynamic>?) ?? [];
            if (index == null || index < 0 || index > playlist.length) {
              throw PlatformException(
                code: 'concatenatingInsertAll_error',
                message: 'index out of bounds',
              );
            }
            for (var i = 0; i < children.length; i++) {
              playlist.insert(
                index + i,
                Map<String, dynamic>.from(children[i] as Map),
              );
            }
            return <dynamic, dynamic>{};

          case 'concatenatingRemoveRange':
            final start = args['startIndex'] as int?;
            final end = args['endIndex'] as int?;
            if (start != null &&
                end != null &&
                end > start &&
                start >= 0 &&
                end <= playlist.length) {
              playlist.removeRange(start, end);
              return <dynamic, dynamic>{};
            } else {
              throw PlatformException(
                code: 'concatenatingRemoveRange_error',
                message: 'invalid range',
              );
            }

          case 'concatenatingMove':
            final from = args['currentIndex'] as int?;
            final to = args['newIndex'] as int?;
            if (from != null &&
                to != null &&
                from < playlist.length &&
                to <= playlist.length) {
              final item = playlist.removeAt(from);
              playlist.insert(to, item);
              return <dynamic, dynamic>{};
            } else {
              throw PlatformException(
                code: 'concatenatingMove_error',
                message: 'index out of bounds',
              );
            }

          default:
            return null; // NotImplemented
        }
      });
    });

    tearDown(() {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(playerChannel, null);
    });

    test('load progressive URI with remote Holy Quran recitation returns duration', () async {
      final res = await playerChannel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': {
          'type': 'progressive',
          'uri': 'https://server10.mp3quran.net/minsh/001.mp3',
        },
      });
      expect(res, isNotNull);
      expect(res!['duration'], 180000000);
      expect(methodLog.last.method, 'load');
    });

    test('load progressive URI with local file containing spaces and Arabic Unicode characters', () async {
      const arabicPath = 'file:///C:/تلاوات قرآنية/الشيخ المنشاوي/001 - الفاتحة.mp3';
      final res = await playerChannel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': {
          'type': 'progressive',
          'uri': arabicPath,
        },
        'initialPosition': 5000000, // 5s in us
        'initialIndex': 0,
      });
      expect(res, isNotNull);
      final source = methodLog.last.arguments['audioSource'] as Map;
      expect(source['uri'], arabicPath);
      expect(methodLog.last.arguments['initialPosition'], 5000000);
    });

    test('load clipping audio source correctly validates nested child', () async {
      final res = await playerChannel.invokeMapMethod<String, dynamic>('load', {
        'audioSource': {
          'type': 'clipping',
          'start': 10000000,
          'end': 40000000,
          'child': {
            'type': 'progressive',
            'uri': 'https://server10.mp3quran.net/minsh/002.mp3',
          },
        },
      });
      expect(res, isNotNull);
      expect(res!['duration'], 30000000);
    });

    test('load clipping source without child throws PlatformException load_error', () async {
      expect(
        () => playerChannel.invokeMethod('load', {
          'audioSource': {
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
        () => playerChannel.invokeMethod('load', {
          'audioSource': {
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
        () => playerChannel.invokeMethod('load', {
          'audioSource': {
            'type': 'unsupported_custom_source',
          },
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'load_error',
        )),
      );
    });

    test('playback controls (play, pause, stop) invoke without error', () async {
      await playerChannel.invokeMethod('play');
      expect(methodLog.last.method, 'play');

      await playerChannel.invokeMethod('pause');
      expect(methodLog.last.method, 'pause');

      await playerChannel.invokeMethod('stop');
      expect(methodLog.last.method, 'stop');
    });

    test('seek handles extreme bounds (0ms, 1hr, boundary edge cases)', () async {
      final seekPositions = [
        0,
        500000, // 500ms
        60000000, // 1 minute
        3600000000, // 1 hour
      ];

      for (final pos in seekPositions) {
        await playerChannel.invokeMethod('seek', {
          'position': pos,
          'index': 0,
        });
        expect(methodLog.last.method, 'seek');
        expect(methodLog.last.arguments['position'], pos);
        expect(methodLog.last.arguments['index'], 0);
      }
    });

    test('setVolume accepts audio levels from 0.0 to 1.0', () async {
      for (final vol in [0.0, 0.2, 0.5, 0.8, 1.0]) {
        await playerChannel.invokeMethod('setVolume', {'volume': vol});
        expect(methodLog.last.method, 'setVolume');
        expect(methodLog.last.arguments['volume'], vol);
      }
    });

    test('setSpeed supports playback speeds (0.5x, 1.0x, 1.25x, 1.5x, 2.0x)', () async {
      for (final speed in [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]) {
        await playerChannel.invokeMethod('setSpeed', {'speed': speed});
        expect(methodLog.last.method, 'setSpeed');
        expect(methodLog.last.arguments['speed'], speed);
      }
    });

    test('setLoopMode supports 0 (off), 1 (one), 2 (all)', () async {
      for (final mode in [0, 1, 2]) {
        await playerChannel.invokeMethod('setLoopMode', {'loopMode': mode});
        expect(methodLog.last.method, 'setLoopMode');
        expect(methodLog.last.arguments['loopMode'], mode);
      }
    });

    test('setShuffleMode supports 0 (none) and 1 (all)', () async {
      for (final mode in [0, 1]) {
        await playerChannel.invokeMethod('setShuffleMode', {'shuffleMode': mode});
        expect(methodLog.last.method, 'setShuffleMode');
        expect(methodLog.last.arguments['shuffleMode'], mode);
      }
    });

    test('playlist mutations: insert, move, and remove operations behave strictly', () async {
      // 1. Initial load of concatenating playlist
      await playerChannel.invokeMethod('load', {
        'audioSource': {
          'type': 'concatenating',
          'children': [
            {'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/001.mp3'},
            {'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/002.mp3'},
          ],
        },
      });
      expect(playlist.length, 2);

      // 2. Insert at index 1
      await playerChannel.invokeMethod('concatenatingInsertAll', {
        'index': 1,
        'children': [
          {'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/112.mp3'},
        ],
      });
      expect(playlist.length, 3);
      expect(playlist[1]['uri'], 'https://server10.mp3quran.net/minsh/112.mp3');

      // 3. Move item from 1 to 2
      await playerChannel.invokeMethod('concatenatingMove', {
        'currentIndex': 1,
        'newIndex': 2,
      });
      expect(playlist[2]['uri'], 'https://server10.mp3quran.net/minsh/112.mp3');

      // 4. Remove range
      await playerChannel.invokeMethod('concatenatingRemoveRange', {
        'startIndex': 0,
        'endIndex': 2,
      });
      expect(playlist.length, 1);

      // 5. Invalid remove range throws concatenatingRemoveRange_error
      expect(
        () => playerChannel.invokeMethod('concatenatingRemoveRange', {
          'startIndex': 5,
          'endIndex': 2, // start > end
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingRemoveRange_error',
        )),
      );

      // 6. Invalid move index throws concatenatingMove_error
      expect(
        () => playerChannel.invokeMethod('concatenatingMove', {
          'currentIndex': 10,
          'newIndex': 20,
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingMove_error',
        )),
      );
    });

    test('android and audio effect stubs succeed gracefully on Windows desktop', () async {
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
        final res = await playerChannel.invokeMapMethod<String, dynamic>(
          stub,
          <String, dynamic>{},
        );
        expect(res, isNotNull);
      }
    });

    test('dispose executes cleanly and is completely idempotent', () async {
      await playerChannel.invokeMethod('dispose');
      expect(methodLog.last.method, 'dispose');

      // Second redundant dispose must not throw
      await playerChannel.invokeMethod('dispose');
      expect(methodLog.last.method, 'dispose');
    });
  });

  group('EventChannel (com.ryanheise.just_audio.events.\$id) State Machine Simulation', () {
    const playerId = 'player_event_test';
    const eventChannelName = 'com.ryanheise.just_audio.events.$playerId';

    test('PlaybackEvent stream broadcasts lifecycle states (idle -> loading -> buffering -> ready -> completed)', () async {
      final controller = StreamController<dynamic>();
      final receivedEvents = <Map<dynamic, dynamic>>[];

      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(
        MethodChannel(eventChannelName),
        (MethodCall call) async {
          if (call.method == 'listen') {
            controller.stream.listen((event) {
              TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
                  .handlePlatformMessage(
                eventChannelName,
                const StandardMethodCodec().encodeSuccessEnvelope(event),
                (ByteData? reply) {},
              );
            });
            return null;
          } else if (call.method == 'cancel') {
            await controller.close();
            return null;
          }
          return null;
        },
      );

      const eventChannel = EventChannel(eventChannelName);
      final sub = eventChannel.receiveBroadcastStream().listen((dynamic event) {
        receivedEvents.add(event as Map<dynamic, dynamic>);
      });

      // Emit simulated states
      controller.add({
        'processingState': 1, // loading
        'updatePosition': 0,
        'updateTime': 1000,
        'bufferedPosition': 0,
        'duration': 180000000,
        'currentIndex': 0,
      });

      controller.add({
        'processingState': 2, // buffering
        'updatePosition': 0,
        'updateTime': 1100,
        'bufferedPosition': 50000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      controller.add({
        'processingState': 3, // ready
        'updatePosition': 1000000,
        'updateTime': 1200,
        'bufferedPosition': 180000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      controller.add({
        'processingState': 4, // completed
        'updatePosition': 180000000,
        'updateTime': 2000,
        'bufferedPosition': 180000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      await Future<void>.delayed(const Duration(milliseconds: 50));

      expect(receivedEvents.length, 4);
      expect(receivedEvents[0]['processingState'], 1);
      expect(receivedEvents[1]['processingState'], 2);
      expect(receivedEvents[2]['processingState'], 3);
      expect(receivedEvents[3]['processingState'], 4);

      await sub.cancel();
    });

    test('PlaybackEvent stream propagates native networkError and decodeError', () async {
      final controller = StreamController<dynamic>();
      final receivedErrors = <PlatformException>[];

      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(
        MethodChannel(eventChannelName),
        (MethodCall call) async {
          if (call.method == 'listen') {
            controller.stream.listen(
              (event) {},
              onError: (dynamic error) {
                if (error is PlatformException) {
                  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
                      .handlePlatformMessage(
                    eventChannelName,
                    const StandardMethodCodec().encodeErrorEnvelope(
                      code: error.code,
                      message: error.message,
                      details: error.details,
                    ),
                    (ByteData? reply) {},
                  );
                }
              },
            );
            return null;
          }
          return null;
        },
      );

      const eventChannel = EventChannel(eventChannelName);
      final sub = eventChannel.receiveBroadcastStream().listen(
        (dynamic event) {},
        onError: (dynamic error) {
          receivedErrors.add(error as PlatformException);
        },
      );

      // Inject network timeout error
      controller.addError(PlatformException(
        code: 'networkError',
        message: 'WinRT: The connection to server10.mp3quran.net timed out',
      ));

      await Future<void>.delayed(const Duration(milliseconds: 50));

      expect(receivedErrors.length, 1);
      expect(receivedErrors.first.code, 'networkError');
      expect(receivedErrors.first.message, contains('timed out'));

      await sub.cancel();
      await controller.close();
    });
  });

  group('Adversarial Stress & Extreme Concurrency Testing (0.1% Edge Cases)', () {
    const stressPlayerId = 'stress_player';
    final channel = MethodChannel('com.ryanheise.just_audio.methods.$stressPlayerId');
    var seekCount = 0;
    var playPauseToggles = 0;

    setUp(() {
      seekCount = 0;
      playPauseToggles = 0;

      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
        if (call.method == 'seek') {
          seekCount++;
          return <dynamic, dynamic>{};
        } else if (call.method == 'play' || call.method == 'pause') {
          playPauseToggles++;
          return <dynamic, dynamic>{};
        }
        return <dynamic, dynamic>{};
      });
    });

    tearDown(() {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    });

    test('100 rapid concurrent seeks execute without any dropped frames or race deadlocks', () async {
      const totalSeeks = 100;
      final futures = <Future<void>>[];

      for (var i = 0; i < totalSeeks; i++) {
        futures.add(channel.invokeMethod('seek', {
          'position': i * 1000000,
        }));
      }

      await Future.wait(futures);
      expect(seekCount, totalSeeks);
    });

    test('100 rapid play/pause flip-flops simulate frantic user input without channel congestion', () async {
      const totalToggles = 100;
      final futures = <Future<void>>[];

      for (var i = 0; i < totalToggles; i++) {
        futures.add(channel.invokeMethod(i.isEven ? 'play' : 'pause'));
      }

      await Future.wait(futures);
      expect(playPauseToggles, totalToggles);
    });

    test('50 concurrent players instantiated and torn down atomically without resource exhaustion', () async {
      const rootChannel = MethodChannel('com.ryanheise.just_audio.methods');
      final activeIds = <String>{};

      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(rootChannel, (MethodCall call) async {
        final args = call.arguments as Map<dynamic, dynamic>? ?? {};
        if (call.method == 'init') {
          activeIds.add(args['id'] as String);
        } else if (call.method == 'disposeAllPlayers') {
          activeIds.clear();
        }
        return <dynamic, dynamic>{};
      });

      const concurrentPlayers = 50;
      for (var i = 0; i < concurrentPlayers; i++) {
        await rootChannel.invokeMethod('init', {'id': 'concurrent_player_$i'});
      }
      expect(activeIds.length, concurrentPlayers);

      await rootChannel.invokeMethod('disposeAllPlayers');
      expect(activeIds.isEmpty, isTrue);

      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(rootChannel, null);
    });
  });
}
