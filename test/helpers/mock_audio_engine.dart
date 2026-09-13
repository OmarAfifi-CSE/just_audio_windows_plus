import 'dart:async';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

/// Simulated state of an individual native audio player instance.
class MockPlayerInstance {
  final String id;
  bool isDisposed = false;
  bool sourceSet = false;
  bool isPlaying = false;
  double volume = 1.0;
  double speed = 1.0;
  int loopMode = 0;
  int shuffleMode = 0;
  int positionUs = 0;
  int durationUs = 0;
  int currentIndex = 0;
  final List<Map<String, dynamic>> playlist = [];
  final List<MethodCall> methodCalls = [];
  final StreamController<dynamic> eventController = StreamController<dynamic>.broadcast();
  final StreamController<dynamic> dataController = StreamController<dynamic>.broadcast();

  MockPlayerInstance(this.id);

  void dispose() {
    isDisposed = true;
    eventController.close();
    dataController.close();
  }
}

/// A comprehensive, high-fidelity mock engine replicating the native C++ WinRT
/// plugin architecture of just_audio_windows_plus.
class MockAudioEngine {
  static const String rootChannelName = 'com.ryanheise.just_audio.methods';
  static const MethodChannel rootChannel = MethodChannel(rootChannelName);

  static final Map<String, MockPlayerInstance> _activePlayers = {};
  static final List<MethodCall> _rootMethodCalls = [];

  static Map<String, MockPlayerInstance> get activePlayers => _activePlayers;
  static List<MethodCall> get rootMethodCalls => _rootMethodCalls;

  static MockPlayerInstance? getPlayer(String id) => _activePlayers[id];

  /// Initializes the mock engine and registers binary messenger handlers.
  static void install() {
    _activePlayers.clear();
    _rootMethodCalls.clear();

    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(rootChannel, (MethodCall call) async {
      _rootMethodCalls.add(call);
      final args = call.arguments as Map<dynamic, dynamic>?;

      if (call.method == 'init') {
        if (args == null || !args.containsKey('id') || args['id'] == null) {
          throw PlatformException(
            code: 'argument_error',
            message: 'id argument missing',
          );
        }
        final id = args['id'] as String;
        final existing = _activePlayers.remove(id);
        existing?.dispose();
        _unregisterPlayerChannels(id);

        final player = MockPlayerInstance(id);
        _activePlayers[id] = player;
        _registerPlayerChannels(player);
        return <String, dynamic>{};
      } else if (call.method == 'disposePlayer') {
        if (args == null || !args.containsKey('id') || args['id'] == null) {
          throw PlatformException(
            code: 'argument_error',
            message: 'id argument missing',
          );
        }
        final id = args['id'] as String;
        final player = _activePlayers.remove(id);
        player?.dispose();
        _unregisterPlayerChannels(id);
        return <String, dynamic>{};
      } else if (call.method == 'disposeAllPlayers') {
        for (final player in _activePlayers.values) {
          player.dispose();
          _unregisterPlayerChannels(player.id);
        }
        _activePlayers.clear();
        return <String, dynamic>{};
      }

      throw MissingPluginException('Not implemented: ${call.method}');
    });
  }

  /// Removes all handlers and disposes of all active players.
  static void uninstall() {
    for (final player in _activePlayers.values) {
      player.dispose();
      _unregisterPlayerChannels(player.id);
    }
    _activePlayers.clear();
    _rootMethodCalls.clear();

    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(rootChannel, null);
  }

  static void _registerPlayerChannels(MockPlayerInstance player) {
    final methodChannelName = 'com.ryanheise.just_audio.methods.${player.id}';
    final eventChannelName = 'com.ryanheise.just_audio.events.${player.id}';
    final dataChannelName = 'com.ryanheise.just_audio.data.${player.id}';

    // Player Method Channel Handler
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(MethodChannel(methodChannelName), (MethodCall call) async {
      player.methodCalls.add(call);
      if (player.isDisposed) {
        return <String, dynamic>{};
      }

      final args = call.arguments as Map<dynamic, dynamic>? ?? <String, dynamic>{};

      switch (call.method) {
        case 'load':
          player.sourceSet = true;
          final source = args['audioSource'] as Map<dynamic, dynamic>?;
          if (source == null || !source.containsKey('type')) {
            throw PlatformException(
              code: 'load_error',
              message: 'Source type is missing',
            );
          }
          final type = source['type'] as String;
          if (type == 'progressive' || type == 'dash' || type == 'hls') {
            final uri = source['uri'] as String?;
            if (uri == null || uri.isEmpty) {
              throw PlatformException(
                code: 'load_error',
                message: 'MediaSource uri is missing',
              );
            }
            player.durationUs = 180000000; // 3 minutes
            return <String, dynamic>{'duration': player.durationUs};
          } else if (type == 'clipping') {
            final child = source['child'] as Map<dynamic, dynamic>?;
            if (child == null) {
              throw PlatformException(
                code: 'load_error',
                message: 'Clipping source child is missing',
              );
            }
            final start = (source['start'] as int?) ?? 0;
            final end = source['end'] as int?;
            player.durationUs = end != null ? (end - start) : 30000000;
            return <String, dynamic>{'duration': player.durationUs};
          } else if (type == 'concatenating') {
            final children = (source['children'] as List<dynamic>?) ?? [];
            player.playlist.clear();
            for (final item in children) {
              player.playlist.add(Map<String, dynamic>.from(item as Map));
            }
            player.durationUs = 600000000; // 10 minutes
            return <String, dynamic>{'duration': player.durationUs};
          } else {
            throw PlatformException(
              code: 'load_error',
              message: 'Source is unsupported or can not be nested: $type',
            );
          }

        case 'play':
          player.isPlaying = true;
          _broadcastData(player);
          return <String, dynamic>{};

        case 'pause':
          player.isPlaying = false;
          _broadcastData(player);
          return <String, dynamic>{};

        case 'stop':
          player.isPlaying = false;
          player.positionUs = 0;
          _broadcastData(player);
          return <String, dynamic>{};

        case 'seek':
          final pos = args['position'] as int?;
          final idx = args['index'] as int?;
          if (idx != null) player.currentIndex = idx;
          if (pos != null) player.positionUs = pos;
          return <String, dynamic>{};

        case 'setVolume':
          final vol = args['volume'] as double?;
          if (vol != null) {
            player.volume = vol;
            _broadcastData(player);
          }
          return <String, dynamic>{};

        case 'setSpeed':
          final spd = args['speed'] as double?;
          if (spd != null) {
            player.speed = spd;
            _broadcastData(player);
          }
          return <String, dynamic>{};

        case 'setLoopMode':
          final loop = args['loopMode'] as int?;
          if (loop != null) {
            player.loopMode = loop;
            _broadcastData(player);
          }
          return <String, dynamic>{};

        case 'setShuffleMode':
          final shf = args['shuffleMode'] as int?;
          if (shf != null) {
            player.shuffleMode = shf;
            _broadcastData(player);
          }
          return <String, dynamic>{};

        case 'setShuffleOrder':
          return <String, dynamic>{};

        case 'concatenatingInsertAll':
          final index = args['index'] as int?;
          final children = (args['children'] as List<dynamic>?) ?? [];
          if (index == null || index < 0 || index > player.playlist.length) {
            throw PlatformException(
              code: 'concatenatingInsertAll_error',
              message: 'index out of bounds',
            );
          }
          for (var i = 0; i < children.length; i++) {
            player.playlist.insert(
              index + i,
              Map<String, dynamic>.from(children[i] as Map),
            );
          }
          return <String, dynamic>{};

        case 'concatenatingRemoveRange':
          final start = args['startIndex'] as int?;
          final end = args['endIndex'] as int?;
          if (start != null &&
              end != null &&
              end > start &&
              start >= 0 &&
              end <= player.playlist.length) {
            player.playlist.removeRange(start, end);
            return <String, dynamic>{};
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
              from < player.playlist.length &&
              to <= player.playlist.length) {
            final item = player.playlist.removeAt(from);
            player.playlist.insert(to, item);
            return <String, dynamic>{};
          } else {
            throw PlatformException(
              code: 'concatenatingMove_error',
              message: 'index out of bounds',
            );
          }

        case 'setPitch':
        case 'setSkipSilence':
        case 'setAndroidAudioAttributes':
        case 'audioEffectSetEnabled':
        case 'androidLoudnessEnhancerSetTargetGain':
        case 'androidEqualizerGetParameters':
        case 'androidEqualizerBandSetGain':
          return <String, dynamic>{};

        case 'dispose':
          player.dispose();
          return <String, dynamic>{};

        default:
          throw MissingPluginException('Not implemented: ${call.method}');
      }
    });

    // Event Channel Mock Handler
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(MethodChannel(eventChannelName), (MethodCall call) async {
      if (call.method == 'listen') {
        player.eventController.stream.listen(
          (dynamic event) {
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
                .handlePlatformMessage(
              eventChannelName,
              const StandardMethodCodec().encodeSuccessEnvelope(event),
              (ByteData? reply) {},
            );
          },
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
      } else if (call.method == 'cancel') {
        return null;
      }
      return null;
    });

    // Data Channel Mock Handler
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(MethodChannel(dataChannelName), (MethodCall call) async {
      if (call.method == 'listen') {
        player.dataController.stream.listen(
          (dynamic event) {
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
                .handlePlatformMessage(
              dataChannelName,
              const StandardMethodCodec().encodeSuccessEnvelope(event),
              (ByteData? reply) {},
            );
          },
        );
        return null;
      } else if (call.method == 'cancel') {
        return null;
      }
      return null;
    });
  }

  static void _unregisterPlayerChannels(String id) {
    final methodChannelName = 'com.ryanheise.just_audio.methods.$id';
    final eventChannelName = 'com.ryanheise.just_audio.events.$id';
    final dataChannelName = 'com.ryanheise.just_audio.data.$id';

    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(MethodChannel(methodChannelName), null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(MethodChannel(eventChannelName), null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(MethodChannel(dataChannelName), null);
  }

  static void _broadcastData(MockPlayerInstance player) {
    if (player.isDisposed) return;
    player.dataController.add(<String, dynamic>{
      'playing': player.isPlaying,
      'volume': player.volume,
      'speed': player.speed,
      'loopMode': player.loopMode,
      'shuffleMode': player.shuffleMode,
    });
  }
}
