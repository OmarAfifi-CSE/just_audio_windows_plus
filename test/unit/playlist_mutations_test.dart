import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import '../helpers/mock_audio_engine.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('Playlist Mutations (concatenatingInsertAll, concatenatingRemoveRange, concatenatingMove)', () {
    const playerId = 'playlist_player';
    const channelName = 'com.ryanheise.just_audio.methods.$playerId';
    late MethodChannel channel;

    setUp(() async {
      MockAudioEngine.install();
      await MockAudioEngine.rootChannel.invokeMethod('init', <String, dynamic>{
        'id': playerId,
      });
      channel = const MethodChannel(channelName);

      // Load initial concatenating playlist with 3 tracks
      await channel.invokeMethod('load', <String, dynamic>{
        'audioSource': <String, dynamic>{
          'type': 'concatenating',
          'children': <dynamic>[
            <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/001.mp3'},
            <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/002.mp3'},
            <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/003.mp3'},
          ],
        },
      });
    });

    tearDown(() {
      MockAudioEngine.uninstall();
    });

    test('concatenatingInsertAll inserts items at beginning, middle, and end', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.playlist.length, 3);

      // 1. Insert at beginning (index 0)
      await channel.invokeMethod('concatenatingInsertAll', <String, dynamic>{
        'index': 0,
        'children': <dynamic>[
          <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/112.mp3'},
        ],
      });
      expect(player?.playlist.length, 4);
      expect(player?.playlist[0]['uri'], 'https://server10.mp3quran.net/minsh/112.mp3');

      // 2. Insert at middle (index 2)
      await channel.invokeMethod('concatenatingInsertAll', <String, dynamic>{
        'index': 2,
        'children': <dynamic>[
          <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/113.mp3'},
        ],
      });
      expect(player?.playlist.length, 5);
      expect(player?.playlist[2]['uri'], 'https://server10.mp3quran.net/minsh/113.mp3');

      // 3. Insert at end (index 5)
      await channel.invokeMethod('concatenatingInsertAll', <String, dynamic>{
        'index': 5,
        'children': <dynamic>[
          <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/114.mp3'},
        ],
      });
      expect(player?.playlist.length, 6);
      expect(player?.playlist.last['uri'], 'https://server10.mp3quran.net/minsh/114.mp3');
    });

    test('concatenatingInsertAll throws on negative or out of bounds index', () async {
      expect(
        () => channel.invokeMethod('concatenatingInsertAll', <String, dynamic>{
          'index': -1,
          'children': <dynamic>[
            <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/001.mp3'},
          ],
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingInsertAll_error',
        )),
      );

      expect(
        () => channel.invokeMethod('concatenatingInsertAll', <String, dynamic>{
          'index': 100, // playlist length is 3
          'children': <dynamic>[
            <String, dynamic>{'type': 'progressive', 'uri': 'https://server10.mp3quran.net/minsh/001.mp3'},
          ],
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingInsertAll_error',
        )),
      );
    });

    test('concatenatingRemoveRange removes single item and entire playlist slice', () async {
      final player = MockAudioEngine.getPlayer(playerId);
      expect(player?.playlist.length, 3);

      // Remove item at index 1
      await channel.invokeMethod('concatenatingRemoveRange', <String, dynamic>{
        'startIndex': 1,
        'endIndex': 2,
      });
      expect(player?.playlist.length, 2);
      expect(player?.playlist[0]['uri'], 'https://server10.mp3quran.net/minsh/001.mp3');
      expect(player?.playlist[1]['uri'], 'https://server10.mp3quran.net/minsh/003.mp3');

      // Remove remaining items
      await channel.invokeMethod('concatenatingRemoveRange', <String, dynamic>{
        'startIndex': 0,
        'endIndex': 2,
      });
      expect(player?.playlist.isEmpty, isTrue);
    });

    test('concatenatingRemoveRange throws on inverted, zero-length, or out of bounds ranges', () async {
      // 1. Inverted range (start > end)
      expect(
        () => channel.invokeMethod('concatenatingRemoveRange', <String, dynamic>{
          'startIndex': 2,
          'endIndex': 1,
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingRemoveRange_error',
        )),
      );

      // 2. Zero-length range (start == end)
      expect(
        () => channel.invokeMethod('concatenatingRemoveRange', <String, dynamic>{
          'startIndex': 1,
          'endIndex': 1,
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingRemoveRange_error',
        )),
      );

      // 3. Negative startIndex
      expect(
        () => channel.invokeMethod('concatenatingRemoveRange', <String, dynamic>{
          'startIndex': -1,
          'endIndex': 2,
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingRemoveRange_error',
        )),
      );

      // 4. Out of bounds endIndex
      expect(
        () => channel.invokeMethod('concatenatingRemoveRange', <String, dynamic>{
          'startIndex': 0,
          'endIndex': 10,
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingRemoveRange_error',
        )),
      );
    });

    test('concatenatingMove rearranges items forward, backward, and to same index', () async {
      final player = MockAudioEngine.getPlayer(playerId);

      // Move track 0 (001.mp3) to position 2
      await channel.invokeMethod('concatenatingMove', <String, dynamic>{
        'currentIndex': 0,
        'newIndex': 2,
      });
      expect(player?.playlist[2]['uri'], 'https://server10.mp3quran.net/minsh/001.mp3');

      // Move track 2 back to position 0
      await channel.invokeMethod('concatenatingMove', <String, dynamic>{
        'currentIndex': 2,
        'newIndex': 0,
      });
      expect(player?.playlist[0]['uri'], 'https://server10.mp3quran.net/minsh/001.mp3');

      // Out of bounds move
      expect(
        () => channel.invokeMethod('concatenatingMove', <String, dynamic>{
          'currentIndex': 5,
          'newIndex': 10,
        }),
        throwsA(isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'concatenatingMove_error',
        )),
      );
    });
  });
}
