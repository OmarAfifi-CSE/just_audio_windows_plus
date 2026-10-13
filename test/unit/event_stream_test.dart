import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../helpers/mock_audio_engine.dart';

/// Pins the two event channels the native engine publishes: the playback
/// state map from `Broadcast()` and the data map carrying the playing, volume,
/// speed, loop and shuffle values.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const playerId = 'event_player';
  final eventChannelName = MockPlayerInstance.eventChannelName(playerId);
  final dataChannelName = MockPlayerInstance.dataChannelName(playerId);

  late MockPlayerInstance player;
  late MethodChannel channel;

  setUp(() async {
    MockAudioEngine.install();
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': playerId});
    player = MockAudioEngine.getPlayer(playerId)!;
    channel = player.methodChannel;
    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist(),
    });
  });

  tearDown(MockAudioEngine.uninstall);

  test('the state event carries every field just_audio reads', () async {
    final received = <Map<dynamic, dynamic>>[];
    final subscription = EventChannel(eventChannelName)
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));

    await channel.invokeMethod<void>('seek', <String, dynamic>{
      'position': 1234567,
      'index': 1,
    });
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    expect(received, isNotEmpty);
    final event = received.last;
    expect(
      event.keys,
      containsAll(<String>[
        'processingState',
        'updatePosition',
        'updateTime',
        'bufferedPosition',
        'duration',
        'currentIndex',
      ]),
    );
    expect(event['updatePosition'], 1234567);
    expect(event['currentIndex'], 1);
    expect(event['processingState'], 3);
    expect(event['duration'], isA<int>());
    expect(event['updateTime'], isA<int>());
  });

  test('processingState follows the source lifecycle', () async {
    // idle before any source
    expect(player.processingState, 3,
        reason: 'a loaded playlist reports ready');

    final freshId = 'lifecycle_player';
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': freshId});
    final fresh = MockAudioEngine.getPlayer(freshId)!;
    expect(fresh.processingState, 0, reason: 'idle before a source is set');

    fresh.load(<String, dynamic>{
      'audioSource': threeTrackPlaylist('lifecycle'),
    });
    expect(fresh.processingState, 3);

    fresh.completed = true;
    expect(fresh.processingState, 4);

    fresh.failed = true;
    expect(fresh.processingState, 0,
        reason: 'a failure rolls the source back to idle');
  });

  test('bufferedPosition never exceeds the duration', () async {
    final received = <Map<dynamic, dynamic>>[];
    final subscription = EventChannel(eventChannelName)
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));

    await channel.invokeMethod<void>('seek', <String, dynamic>{'position': 0});
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    for (final event in received) {
      final buffered = event['bufferedPosition'];
      final duration = event['duration'];
      if (buffered is int && duration is int) {
        expect(buffered, lessThanOrEqualTo(duration));
        expect(buffered, greaterThanOrEqualTo(0));
      }
    }
  });

  test('a playback failure is published on the state event with its message',
      () async {
    final received = <Map<dynamic, dynamic>>[];
    final subscription = EventChannel(eventChannelName)
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));
    await Future<void>.delayed(const Duration(milliseconds: 20));

    player.fail('WinRT: the media pipeline could not decode the stream');
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    final failure = received
        .where((event) => event['errorCode'] != null)
        .toList(growable: false);
    expect(failure, isNotEmpty);
    expect(failure.last['errorCode'], 1);
    expect(failure.last['errorMessage'], contains('decode'));
  });

  test('a stream error is delivered to the subscriber as a PlatformException',
      () async {
    final errors = <PlatformException>[];
    final subscription = EventChannel(eventChannelName)
        .receiveBroadcastStream()
        .listen((dynamic _) {}, onError: (Object error) {
      if (error is PlatformException) errors.add(error);
    });
    await Future<void>.delayed(const Duration(milliseconds: 20));

    player.eventController.addError(
      PlatformException(
        code: 'networkError',
        message: 'The network connection to the host dropped',
      ),
    );
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    expect(errors, hasLength(1));
    expect(errors.single.code, 'networkError');
    expect(errors.single.message, contains('dropped'));
  });

  test('unchanged player data is not republished', () async {
    // just_audio applies every value it receives, so a redundant copy of an
    // unchanged value can overwrite a newer decision it already made. Only a
    // real change is published; a fresh subscriber is primed so it is never left
    // without a snapshot.
    final received = <Map<dynamic, dynamic>>[];
    final subscription = player.eventChannel
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));
    await Future<void>.delayed(const Duration(milliseconds: 20));

    // Two seeks move the position but change no player data.
    await channel.invokeMethod<void>('seek', <String, dynamic>{
      'position': 0,
      'index': 0,
    });
    await channel.invokeMethod<void>('seek', <String, dynamic>{
      'position': 5000000,
      'index': 1,
    });
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    expect(received.length, greaterThanOrEqualTo(2),
        reason: 'the state map is a sample and is always published');
    expect(player.deliveredData, hasLength(lessThanOrEqualTo(2)),
        reason: 'a seek that changes no intent must not republish the data map');
    expect(player.dataWouldChange, isFalse);
  });

    test('the data channel reports volume, speed, loop and shuffle changes',
      () async {
    final received = <Map<dynamic, dynamic>>[];
    final subscription = EventChannel(dataChannelName)
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));
    await Future<void>.delayed(const Duration(milliseconds: 20));

    await channel.invokeMethod<void>('setVolume', <String, dynamic>{'volume': 0.65});
    await channel.invokeMethod<void>('setSpeed', <String, dynamic>{'speed': 1.25});
    await channel.invokeMethod<void>('setLoopMode', <String, dynamic>{'loopMode': 1});
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    expect(received, isNotEmpty);
    final last = received.last;
    expect(last['volume'], 0.65);
    expect(last['speed'], 1.25);
    expect(last['loopMode'], 1);
    expect(last['shuffleMode'], 0);
    expect(last['playing'], isFalse);
  });

  test('a disposed player stops publishing events', () async {
    final received = <Map<dynamic, dynamic>>[];
    final subscription = EventChannel(eventChannelName)
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));
    await Future<void>.delayed(const Duration(milliseconds: 20));

    await channel.invokeMethod<void>('dispose', <String, dynamic>{});
    player.broadcastState();
    await Future<void>.delayed(const Duration(milliseconds: 20));
    await subscription.cancel();

    expect(received, isEmpty,
        reason: 'Broadcast() returns early once the player is disposed');
  });

  test('forced data broadcast re-delivers current intent even when unchanged',
      () async {
    final received = <Map<dynamic, dynamic>>[];
    final subscription = EventChannel(dataChannelName)
        .receiveBroadcastStream()
        .listen((dynamic event) => received.add(event as Map<dynamic, dynamic>));
    await Future<void>.delayed(const Duration(milliseconds: 20));

    final countBefore = received.length;
    // Normal broadcast with no change does not republish
    player.broadcastState(forceData: false);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(received.length, countBefore);

    // Recovery / forced broadcast always re-delivers to guarantee convergence
    player.broadcastState(forceData: true);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(received.length, countBefore + 1);
    await subscription.cancel();
  });
}
