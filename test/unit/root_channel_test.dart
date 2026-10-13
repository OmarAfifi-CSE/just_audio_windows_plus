import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('Root MethodChannel (com.ryanheise.just_audio.methods) Protocol', () {
    const rootChannel = MethodChannel('com.ryanheise.just_audio.methods');
    final activeIds = <String>{};
    final rootLog = <MethodCall>[];

    setUp(() {
      activeIds.clear();
      rootLog.clear();
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(rootChannel, (MethodCall methodCall) async {
        rootLog.add(methodCall);
        // The native root handler requires an EncodableMap and reports
        // anything else as argument_error rather than crashing.
        if (methodCall.arguments is! Map) {
          throw ChannelContractError('argument_error', 'arguments must be a map');
        }
        final args = methodCall.arguments as Map<dynamic, dynamic>;

        if (methodCall.method == 'init') {
          if (args['id'] == null) {
            throw ChannelContractError('argument_error', 'id argument missing');
          }
          activeIds.add(args['id'] as String);
          return <dynamic, dynamic>{};
        } else if (methodCall.method == 'disposePlayer') {
          if (args['id'] == null) {
            throw ChannelContractError('argument_error', 'id argument missing');
          }
          activeIds.remove(args['id'] as String);
          return <dynamic, dynamic>{};
        } else if (methodCall.method == 'disposeAllPlayers') {
          activeIds.clear();
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
      final res = await rootChannel
          .invokeMapMethod<String, dynamic>('init', {'id': 'player_alpha'});
      expect(res, isNotNull);
      expect(activeIds, contains('player_alpha'));
      expect(rootLog.first.method, 'init');
    });

    test('init rejects a missing or null id with argument_error', () async {
      for (final args in <Map<String, dynamic>>[
        <String, dynamic>{},
        <String, dynamic>{'id': null},
      ]) {
        await expectLater(
          rootChannel.invokeMethod<void>('init', args),
          throwsA(isA<PlatformException>()
              .having((e) => e.code, 'code', 'argument_error')),
        );
      }
    });

    test('init rejects non-map arguments with argument_error', () async {
      await expectLater(
        rootChannel.invokeMethod<void>('init', 'not-a-map'),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'argument_error')),
      );
    });

    test('disposePlayer removes a known id and is a no-op otherwise', () async {
      activeIds.add('player_alpha');
      final res = await rootChannel.invokeMapMethod<String, dynamic>(
        'disposePlayer',
        {'id': 'player_alpha'},
      );
      expect(res, isNotNull);
      expect(activeIds, isNot(contains('player_alpha')));

      // Disposing an id that was never created must still succeed, matching
      // the native DisposePlayerByPlayerId lookup.
      final again = await rootChannel.invokeMapMethod<String, dynamic>(
        'disposePlayer',
        {'id': 'never_created'},
      );
      expect(again, isNotNull);
    });

    test('disposePlayer rejects a missing id with argument_error', () async {
      await expectLater(
        rootChannel.invokeMethod<void>('disposePlayer', <String, dynamic>{}),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'argument_error')),
      );
    });

    test('disposeAllPlayers clears any number of active players', () async {
      for (var i = 0; i < 25; i++) {
        activeIds.add('player_$i');
      }
      expect(activeIds, hasLength(25));

      final res = await rootChannel.invokeMapMethod<String, dynamic>(
        'disposeAllPlayers',
        <String, dynamic>{},
      );
      expect(res, isNotNull);
      expect(activeIds, isEmpty);
    });

    test('an unrecognized root method surfaces as MissingPluginException', () async {
      await expectLater(
        rootChannel.invokeMethod<void>('unknownRootMethod', <String, dynamic>{}),
        throwsA(isA<MissingPluginException>()),
      );
    });
  });
}
