import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('EventChannel & DataChannel Reactive Stream Tests', () {
    const playerId = 'event_player';
    const eventChannelName = 'com.ryanheise.just_audio.events.$playerId';
    const dataChannelName = 'com.ryanheise.just_audio.data.$playerId';

    setUp(() async {
      MockAudioEngine.install();
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': playerId,
      });
    });

    tearDown(() {
      MockAudioEngine.uninstall();
    });

    test('PlaybackEvent stream receives all sequential processing states', () async {
      final player = MockAudioEngine.getPlayer(playerId)!;
      const channel = EventChannel(eventChannelName);
      final receivedEvents = <Map<dynamic, dynamic>>[];

      final subscription = channel.receiveBroadcastStream().listen((dynamic event) {
        receivedEvents.add(event as Map<dynamic, dynamic>);
      });

      // 1. Loading
      player.eventController.add(<String, dynamic>{
        'processingState': 1,
        'updatePosition': 0,
        'updateTime': 1000,
        'bufferedPosition': 0,
        'duration': 180000000,
        'currentIndex': 0,
      });

      // 2. Buffering
      player.eventController.add(<String, dynamic>{
        'processingState': 2,
        'updatePosition': 0,
        'updateTime': 1100,
        'bufferedPosition': 45000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      // 3. Ready
      player.eventController.add(<String, dynamic>{
        'processingState': 3,
        'updatePosition': 1000000,
        'updateTime': 1200,
        'bufferedPosition': 180000000,
        'duration': 180000000,
        'currentIndex': 0,
      });

      // 4. Completed
      player.eventController.add(<String, dynamic>{
        'processingState': 4,
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

      await subscription.cancel();
    });

    test('PlaybackEvent stream propagates native networkError and decodingError', () async {
      final player = MockAudioEngine.getPlayer(playerId)!;
      const channel = EventChannel(eventChannelName);
      final errors = <PlatformException>[];

      final subscription = channel.receiveBroadcastStream().listen(
        (dynamic event) {},
        onError: (dynamic err) {
          errors.add(err as PlatformException);
        },
      );

      player.eventController.addError(PlatformException(
        code: 'networkError',
        message: 'The network connection to server10.mp3quran.net dropped',
      ));

      player.eventController.addError(PlatformException(
        code: 'decodingError',
        message: 'WinRT: Media pipeline failed to decode MP3 stream frames',
      ));

      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(errors.length, 2);
      expect(errors[0].code, 'networkError');
      expect(errors[1].code, 'decodingError');

      await subscription.cancel();
    });

    test('DataChannel broadcasts player state updates (playing, volume, speed, loop, shuffle)', () async {
      const channel = EventChannel(dataChannelName);
      final playerMethodChannel = MethodChannel('com.ryanheise.just_audio.methods.$playerId');
      final receivedData = <Map<dynamic, dynamic>>[];

      final subscription = channel.receiveBroadcastStream().listen((dynamic event) {
        receivedData.add(event as Map<dynamic, dynamic>);
      });

      // Play
      await playerMethodChannel.invokeMethod('play', <String, dynamic>{});
      // Volume
      await playerMethodChannel.invokeMethod('setVolume', <String, dynamic>{'volume': 0.65});
      // Speed
      await playerMethodChannel.invokeMethod('setSpeed', <String, dynamic>{'speed': 1.25});
      // Loop
      await playerMethodChannel.invokeMethod('setLoopMode', <String, dynamic>{'loopMode': 1});

      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(receivedData.isNotEmpty, isTrue);
      expect(receivedData.last['volume'], 0.65);
      expect(receivedData.last['speed'], 1.25);
      expect(receivedData.last['loopMode'], 1);

      await subscription.cancel();
    });
  });
}
