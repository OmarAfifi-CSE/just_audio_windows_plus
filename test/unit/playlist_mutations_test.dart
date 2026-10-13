import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../helpers/mock_audio_engine.dart';

/// Pins the playlist mutation contract: `SourceNode::Mutate` in
/// `windows/source_model.hpp` requires a target `id` and a `shuffleOrder`
/// that is a valid permutation of the resulting child count, and the engine
/// keeps its leaves in the same order the WinRT playback list is built from.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const playerId = 'playlist_player';
  const channelName = 'com.ryanheise.just_audio.methods.$playerId';

  late MethodChannel channel;
  late MockPlayerInstance player;

  final Matcher argumentError = throwsA(isA<PlatformException>()
      .having((e) => e.code, 'code', 'argument_error'));

  setUp(() async {
    MockAudioEngine.install();
    await MockAudioEngine.rootChannel
        .invokeMethod<void>('init', <String, dynamic>{'id': playerId});
    channel = const MethodChannel(channelName);
    player = MockAudioEngine.getPlayer(playerId)!;
    await channel.invokeMethod<void>('load', <String, dynamic>{
      'audioSource': threeTrackPlaylist(),
    });
  });

  tearDown(MockAudioEngine.uninstall);

  Future<void> insert(int index, List<Map<String, dynamic>> children, List<int> order) =>
      channel.invokeMethod<void>('concatenatingInsertAll', <String, dynamic>{
        'id': 'root',
        'index': index,
        'children': children,
        'shuffleOrder': order,
      });

  Future<void> removeRange(int start, int end, List<int> order) =>
      channel.invokeMethod<void>('concatenatingRemoveRange', <String, dynamic>{
        'id': 'root',
        'startIndex': start,
        'endIndex': end,
        'shuffleOrder': order,
      });

  Future<void> move(int from, int to, List<int> order) =>
      channel.invokeMethod<void>('concatenatingMove', <String, dynamic>{
        'id': 'root',
        'currentIndex': from,
        'newIndex': to,
        'shuffleOrder': order,
      });

  group('concatenatingInsertAll', () {
    test('inserts at the beginning, the middle and the end', () async {
      expect(player.leaves, hasLength(3));

      await insert(0, <Map<String, dynamic>>[
        progressiveSource('x', 'file:///audio/x.mp3'),
      ], <int>[0, 1, 2, 3]);
      expect(player.leaves, hasLength(4));
      expect(player.leaves.first['id'], 'x');

      await insert(2, <Map<String, dynamic>>[
        progressiveSource('y', 'file:///audio/y.mp3'),
      ], <int>[0, 1, 2, 3, 4]);
      expect(player.leaves, hasLength(5));
      expect(player.leaves[2]['id'], 'y');

      await insert(5, <Map<String, dynamic>>[
        progressiveSource('z', 'file:///audio/z.mp3'),
      ], <int>[0, 1, 2, 3, 4, 5]);
      expect(player.leaves, hasLength(6));
      expect(player.leaves.last['id'], 'z');
    });

    test('accepts index == child count, the append position', () async {
      await insert(3, <Map<String, dynamic>>[
        progressiveSource('x', 'file:///audio/x.mp3'),
      ], <int>[0, 1, 2, 3]);
      expect(player.leaves, hasLength(4));
    });

    test('rejects a negative or out-of-range index', () async {
      await expectLater(
        insert(-1, <Map<String, dynamic>>[
          progressiveSource('x', 'file:///audio/x.mp3'),
        ], <int>[0, 1, 2, 3]),
        argumentError,
      );
      await expectLater(
        insert(4, <Map<String, dynamic>>[
          progressiveSource('x', 'file:///audio/x.mp3'),
        ], <int>[0, 1, 2, 3]),
        argumentError,
      );
    });

    test('rejects a missing or unknown target id', () async {
      await expectLater(
        channel.invokeMethod<void>('concatenatingInsertAll', <String, dynamic>{
          'index': 0,
          'children': <Map<String, dynamic>>[
            progressiveSource('x', 'file:///audio/x.mp3'),
          ],
          'shuffleOrder': <int>[0, 1, 2, 3],
        }),
        argumentError,
      );
      await expectLater(
        channel.invokeMethod<void>('concatenatingInsertAll', <String, dynamic>{
          'id': 'not-a-real-source',
          'index': 0,
          'children': <Map<String, dynamic>>[
            progressiveSource('x', 'file:///audio/x.mp3'),
          ],
          'shuffleOrder': <int>[0, 1, 2, 3],
        }),
        argumentError,
      );
    });

    test('targets a leaf id and is rejected, because leaves are not mutable',
        () async {
      await expectLater(
        channel.invokeMethod<void>('concatenatingInsertAll', <String, dynamic>{
          'id': 'root-0',
          'index': 0,
          'children': <Map<String, dynamic>>[
            progressiveSource('x', 'file:///audio/x.mp3'),
          ],
          'shuffleOrder': <int>[0, 1, 2, 3],
        }),
        argumentError,
      );
    });

    test('rejects a shuffleOrder that is not a permutation of the new size',
        () async {
      for (final order in <List<int>>[
        <int>[0, 0, 0, 0],
        <int>[0, 1, 2],
        <int>[0, 1, 2, 4],
        <int>[],
      ]) {
        await expectLater(
          insert(0, <Map<String, dynamic>>[
            progressiveSource('x', 'file:///audio/x.mp3'),
          ], order),
          argumentError,
        );
      }
    });

    test('a rejected mutation leaves the playlist untouched', () async {
      final before = player.leaves.map((leaf) => leaf['id']).toList();
      await expectLater(
        insert(0, <Map<String, dynamic>>[
          progressiveSource('x', 'file:///audio/x.mp3'),
        ], <int>[0, 0, 0, 0]),
        argumentError,
      );
      expect(player.leaves.map((leaf) => leaf['id']).toList(), before);
    });
  });

  group('concatenatingRemoveRange', () {
    test('removes a single item and then a whole slice', () async {
      await removeRange(1, 2, <int>[0, 1]);
      expect(player.leaves.map((leaf) => leaf['id']),
          <String>['root-0', 'root-2']);

      await removeRange(0, 2, <int>[]);
      expect(player.leaves, isEmpty);
    });

    test('accepts an empty range as a no-op', () async {
      await removeRange(1, 1, <int>[0, 1, 2]);
      expect(player.leaves, hasLength(3));
    });

    test('rejects an inverted range', () async {
      await expectLater(removeRange(2, 1, <int>[0, 1, 2]), argumentError);
    });

    test('rejects a negative or out-of-range bound', () async {
      await expectLater(removeRange(-1, 2, <int>[0, 1, 2]), argumentError);
      await expectLater(removeRange(0, 4, <int>[0, 1, 2]), argumentError);
    });
  });

  group('concatenatingMove', () {
    test('moves an item forward, backward and onto itself', () async {
      await move(0, 2, <int>[1, 2, 0]);
      expect(player.leaves.map((leaf) => leaf['id']),
          <String>['root-1', 'root-2', 'root-0']);

      await move(2, 0, <int>[2, 0, 1]);
      expect(player.leaves.map((leaf) => leaf['id']),
          <String>['root-0', 'root-1', 'root-2']);

      await move(1, 1, <int>[0, 1, 2]);
      expect(player.leaves.map((leaf) => leaf['id']),
          <String>['root-0', 'root-1', 'root-2']);
    });

    test('rejects an index equal to the child count, which is not an item',
        () async {
      await expectLater(move(0, 3, <int>[1, 2, 0]), argumentError);
      await expectLater(move(3, 0, <int>[1, 2, 0]), argumentError);
    });

    test('rejects negative indices', () async {
      await expectLater(move(-1, 0, <int>[1, 2, 0]), argumentError);
    });
  });

  group('nested mutations', () {
    setUp(() async {
      await channel.invokeMethod<void>('load', <String, dynamic>{
        'audioSource': concatenatingSource(
          'root',
          <Map<String, dynamic>>[
            progressiveSource('solo', 'file:///audio/solo.mp3'),
            concatenatingSource(
              'group',
              <Map<String, dynamic>>[
                progressiveSource('g0', 'file:///audio/g0.mp3'),
                progressiveSource('g1', 'file:///audio/g1.mp3'),
              ],
              <int>[0, 1],
            ),
          ],
          <int>[0, 1],
        ),
      });
    });

    test('flattens nested children into physical leaf order', () {
      expect(player.leaves.map((leaf) => leaf['id']),
          <String>['solo', 'g0', 'g1']);
      expect(player.progressionOrder, <int>[0, 1, 2]);
    });

    test('mutates a nested concatenating source by its own id', () async {
      await channel.invokeMethod<void>('concatenatingInsertAll', <String, dynamic>{
        'id': 'group',
        'index': 1,
        'children': <Map<String, dynamic>>[
          progressiveSource('g-new', 'file:///audio/gnew.mp3'),
        ],
        'shuffleOrder': <int>[0, 1, 2],
      });
      expect(player.leaves.map((leaf) => leaf['id']),
          <String>['solo', 'g0', 'g-new', 'g1']);
      expect(player.progressionOrder, <int>[0, 1, 2, 3]);
    });

    test('an inner shuffle order reshuffles only the inner block', () async {
      await channel.invokeMethod<void>('concatenatingMove', <String, dynamic>{
        'id': 'group',
        'currentIndex': 0,
        'newIndex': 1,
        'shuffleOrder': <int>[1, 0],
      });
      expect(player.progressionOrder, <int>[0, 2, 1]);
    });

    test('an outer shuffle order permutes whole blocks', () async {
      await channel.invokeMethod<void>('setShuffleOrder', <String, dynamic>{
        'audioSource': concatenatingSource(
          'root',
          <Map<String, dynamic>>[
            progressiveSource('solo', 'file:///audio/solo.mp3'),
            concatenatingSource(
              'group',
              <Map<String, dynamic>>[
                progressiveSource('g0', 'file:///audio/g0.mp3'),
                progressiveSource('g1', 'file:///audio/g1.mp3'),
              ],
              <int>[0, 1],
            ),
          ],
          <int>[1, 0],
        ),
      });
      expect(player.progressionOrder, <int>[1, 2, 0]);
    });
  });

  group('mutating an empty playlist', () {
    test('inserting the first item attaches a source', () async {
      await channel.invokeMethod<void>('load', <String, dynamic>{
        'audioSource': concatenatingSource('root', <Map<String, dynamic>>[], <int>[]),
      });
      expect(player.leaves, isEmpty);
      expect(player.sourceSet, isTrue);

      await insert(0, <Map<String, dynamic>>[
        progressiveSource('first', 'file:///audio/first.mp3'),
      ], <int>[0]);

      expect(player.leaves, hasLength(1));
      expect(player.completed, isFalse);
      expect(player.sourceSet, isTrue);
    });

    test('emptying the playlist again completes any outstanding play', () async {
      await removeRange(0, 3, <int>[]);
      expect(player.leaves, isEmpty);
      expect(player.completed, isTrue);
    });
  });
}
