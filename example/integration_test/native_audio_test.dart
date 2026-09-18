// Standalone Windows integration entry point. Run with tools/test_windows.ps1.
// This intentionally uses real platform channels and WinRT, not mock handlers.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:just_audio/just_audio.dart';

final _results = <Map<String, Object?>>[];
late Directory _artifacts;
late File _longWav;
late File _shortWav;
final _players = <AudioPlayer>[];

// Deterministic order with index 0 first and remaining items in reverse order.
// Expected 0,2,1 is specified explicitly by the test, not calculated by WinRT.
class ReverseShuffle extends ShuffleOrder {
  @override
  final indices = <int>[];
  @override
  void clear() => indices.clear();
  @override
  void insert(int index, int count) {
    final size = indices.length + count;
    indices.clear();
    indices.addAll(List.generate(size, (i) => i));
  }

  @override
  void removeRange(int start, int end) {
    final size = indices.length - (end - start);
    indices.clear();
    indices.addAll(List.generate(size, (i) => i));
  }

  @override
  void shuffle({int? initialIndex}) {
    final first = initialIndex ?? 0;
    final rest = indices.where((i) => i != first).toList()
      ..sort((a, b) => b.compareTo(a));
    indices.clear();
    indices.addAll([if (rest.isNotEmpty || first == 0) first, ...rest]);
  }
}

class RawPlayer {
  static const root = MethodChannel('com.ryanheise.just_audio.methods');
  final String id;
  late final MethodChannel channel =
      MethodChannel('com.ryanheise.just_audio.methods.$id');
  final events = <Map<dynamic, dynamic>>[];
  StreamSubscription<dynamic>? subscription;
  RawPlayer(this.id);
  Future<void> init() async {
    await root.invokeMethod<void>('init', {'id': id});
    subscription = EventChannel('com.ryanheise.just_audio.events.$id')
        .receiveBroadcastStream()
        .listen((event) {
      events.add(event as Map<dynamic, dynamic>);
    }, onError: (Object _) {});
    await call('setVolume', {'volume': 0.0});
  }

  Future<dynamic> call(String name, [Map<String, Object?>? args]) =>
      channel.invokeMethod<dynamic>(name, args ?? {});
  Future<void> dispose() async {
    await subscription?.cancel();
    await root.invokeMethod<void>('disposePlayer', {'id': id});
  }
}

Map<String, Object?> leaf(String id, [Uri? uri]) => {
      'id': id,
      'type': 'progressive',
      'uri': (uri ?? _shortWav.uri).toString(),
      'headers': null
    };
Map<String, Object?> playlist(
        String id, List<Map<String, Object?>> children, List<int> order) =>
    {
      'id': id,
      'type': 'concatenating',
      'children': children,
      'shuffleOrder': order,
      'useLazyPreparation': true
    };

Future<Object?> caught(Future<dynamic> future) async {
  try {
    await future;
    return null;
  } catch (e) {
    return e;
  }
}

void require(bool condition, String message) {
  if (!condition) throw StateError(message);
}

Future<void> eventually(bool Function() predicate, String description,
    {Duration timeout = const Duration(seconds: 6)}) async {
  final watch = Stopwatch()..start();
  while (!predicate()) {
    if (watch.elapsed > timeout) throw TimeoutException(description, timeout);
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}

void _save() => File('${_artifacts.path}/results.json')
    .writeAsStringSync(const JsonEncoder.withIndent('  ').convert(_results));

Future<void> scenario(String name, Future<void> Function() body) async {
  final watch = Stopwatch()..start();
  try {
    await body().timeout(const Duration(seconds: 18));
    _results.add({'name': name, 'passed': true});
  } catch (error, stack) {
    _results.add(
        {'name': name, 'passed': false, 'error': '$error', 'stack': '$stack'});
  } finally {
    for (final player in _players) {
      try {
        await player.dispose().timeout(const Duration(seconds: 3));
      } catch (error) {
        _results
            .add({'name': '$name cleanup', 'passed': false, 'error': '$error'});
      }
    }
    _players.clear();
    _results.last['elapsedMs'] = watch.elapsedMilliseconds;
    _save();
  }
}

Future<AudioPlayer> newPlayer() async {
  final player = AudioPlayer();
  _players.add(player);
  await player.setVolume(0);
  return player;
}

File makeWav(String name, int milliseconds) {
  final data = ByteData(44 + 32 * milliseconds);
  void text(int offset, String value) {
    for (var i = 0; i < value.length; i++) {
      data.setUint8(offset + i, value.codeUnitAt(i));
    }
  }

  text(0, 'RIFF');
  data.setUint32(4, data.lengthInBytes - 8, Endian.little);
  text(8, 'WAVE');
  text(12, 'fmt ');
  data.setUint32(16, 16, Endian.little);
  data.setUint16(20, 1, Endian.little);
  data.setUint16(22, 1, Endian.little);
  data.setUint32(24, 16000, Endian.little);
  data.setUint32(28, 32000, Endian.little);
  data.setUint16(32, 2, Endian.little);
  data.setUint16(34, 16, Endian.little);
  text(36, 'data');
  data.setUint32(40, data.lengthInBytes - 44, Endian.little);
  return File('${_artifacts.path}/$name')
    ..writeAsBytesSync(data.buffer.asUint8List());
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  _artifacts = Directory(Platform.environment['JAW_TEST_OUTPUT'] ??
      '${Directory.current.path}/native-test-results');
  _artifacts.createSync(recursive: true);
  final watchdog = Timer(const Duration(minutes: 5), () {
    _results.add({'name': 'suite watchdog', 'passed': false});
    _save();
    exit(2);
  });
  _longWav = makeWav('three seconds.wav', 3000);
  _shortWav = makeWav('short.wav', 400);
  await scenario('load returns known duration', () async {
    final p = await newPlayer();
    final duration = await p.setFilePath(_longWav.path);
    require(duration == const Duration(seconds: 3),
        'Expected 3 seconds from load, got $duration');
    require(p.duration == duration, 'Load and event durations disagree');
  });
  await scenario('play future waits for pause', () async {
    final p = await newPlayer();
    await p.setFilePath(_longWav.path);
    var completed = false;
    final play = p.play().then((_) {
      completed = true;
    });
    await Future<void>.delayed(const Duration(milliseconds: 150));
    require(!completed, 'play Future completed before pause/end');
    await p.pause();
    await play.timeout(const Duration(seconds: 2));
    require(!p.playing, 'pause did not clear playing intent');
  });
  await scenario('EOF completes play and keeps playing intent', () async {
    final p = await newPlayer();
    await p.setFilePath(_shortWav.path);
    await p.play().timeout(const Duration(seconds: 5));
    require(p.processingState == ProcessingState.completed,
        'play returned before EOF');
    require(p.playing, 'EOF cleared playing intent');
    await p.seek(Duration.zero);
    await eventually(() => p.position > const Duration(milliseconds: 30),
        'seek after EOF did not resume');
  });
  await scenario('app-style restart from completed survives repeated passes',
      () async {
    // Replicates the Quran-video-studio recovery sequence exactly: a natural
    // end of track, pause() (the app's merged reset), seek(0), play() again -
    // repeated back-to-back so any intermittent wedge shows up. Also proves
    // the completed -> ready transition re-arms completion listeners.
    final p = await newPlayer();
    final completions = <int>[];
    final sub = p.playerStateStream.listen((state) {
      if (state.processingState == ProcessingState.completed) {
        completions.add(completions.length);
      }
    });
    await p.setFilePath(_shortWav.path);
    for (var pass = 0; pass < 5; pass++) {
      await p.play().timeout(const Duration(seconds: 6));
      require(p.processingState == ProcessingState.completed,
          'pass $pass: play returned before EOF');
      // The app's merged-mode reset: pause first, then seek back to zero.
      await p.pause();
      require(!p.playing, 'pass $pass: pause intent');
      await p.seek(Duration.zero);
      require(p.processingState != ProcessingState.completed,
          'pass $pass: seek kept the completed state');
    }
    // Bare play() straight from the completed engine (no seek first): the
    // contract path that used to wedge. pause() first so the Dart play intent
    // is cleared and the call actually reaches the platform, then play
    // WITHOUT any seek — exactly the app's toggle-from-completed path. Must
    // transition completed -> ready, restart audio, and re-arm completion.
    await p.play().timeout(const Duration(seconds: 6));
    require(p.processingState == ProcessingState.completed,
        'warm-up play did not reach EOF');
    await p.pause();
    require(!p.playing, 'pause did not clear playing intent');
    require(p.processingState == ProcessingState.completed,
        'pause should not erase the completed state');
    await p.play().timeout(const Duration(seconds: 6));
    await eventually(
        () => p.processingState == ProcessingState.ready,
        'bare play after EOF never left the completed state');
    await eventually(
        () => p.processingState == ProcessingState.completed,
        're-armed completion did not fire at the next EOF');
    require(completions.length >= 7,
        'each restart pass must deliver a fresh completed event, got ${completions.length}');
    await sub.cancel();
  });
  await scenario('missing file fails load and reports error', () async {
    final p = await newPlayer();
    final errors = <PlayerException>[];
    final sub = p.errorStream.listen(errors.add);
    Object? failure;
    try {
      await p
          .setFilePath('${_longWav.path}.missing')
          .timeout(const Duration(seconds: 4));
    } catch (error) {
      failure = error;
    }
    await sub.cancel();
    require(
        failure is PlayerException, 'Expected PlayerException, got $failure');
    require(errors.isNotEmpty, 'errorStream received no error');
  });
  for (final before in [true, false]) {
    await scenario(
        'loop all ${before ? 'before' : 'after'} load visits both items',
        () async {
      final p = await newPlayer();
      if (before) await p.setLoopMode(LoopMode.all);
      await p.setAudioSources(
          [AudioSource.uri(_shortWav.uri), AudioSource.uri(_shortWav.uri)]);
      if (!before) await p.setLoopMode(LoopMode.all);
      final indices = <int?>[];
      final sub = p.currentIndexStream.distinct().listen(indices.add);
      unawaited(p.play());
      try {
        await eventually(() => indices.length >= 3,
            'loop did not visit both tracks and return: $indices');
        require(indices.take(3).join(',') == '0,1,0',
            'Wrong loop sequence $indices');
      } finally {
        await sub.cancel();
      }
    });
  }
  await scenario('nested playlist loads and advances', () async {
    final p = await newPlayer();
    // ignore: deprecated_member_use
    final nested = ConcatenatingAudioSource(children: [
      AudioSource.uri(_shortWav.uri),
      AudioSource.uri(_shortWav.uri)
    ]);
    await p.setAudioSource(nested);
    unawaited(p.play());
    await eventually(
        () => p.currentIndex == 1, 'nested playlist did not advance');
  });
  await scenario('initial index and position, seek to zero', () async {
    final p = await newPlayer();
    await p.setAudioSources(
        [AudioSource.uri(_shortWav.uri), AudioSource.uri(_longWav.uri)],
        initialIndex: 1, initialPosition: const Duration(seconds: 1));
    require(p.currentIndex == 1, 'Wrong initial index ${p.currentIndex}');
    require((p.position.inMilliseconds - 1000).abs() < 100,
        'Wrong initial position ${p.position}');
    await p.seek(const Duration(seconds: 2), index: 1);
    await eventually(
        () => (p.position.inMilliseconds - 2000).abs() < 100, 'seek 2s failed');
    await p.seek(Duration.zero, index: 1);
    await eventually(
        () => p.position.inMilliseconds < 100, 'same-index zero seek failed');
  });
  await scenario('cross-item seek applies requested position', () async {
    final p = await newPlayer();
    await p.setAudioSources([
      AudioSource.uri(_shortWav.uri),
      AudioSource.uri(_longWav.uri),
      AudioSource.uri(_longWav.uri),
    ]);
    unawaited(p.play());
    await eventually(() => p.position > const Duration(milliseconds: 50),
        'first item did not start');
    // Target never opened yet: the pending position must survive the move.
    await p.seek(const Duration(seconds: 1), index: 1);
    await eventually(() => p.currentIndex == 1, 'seek to next index failed');
    await eventually(() => (p.position.inMilliseconds - 1000).abs() < 250,
        'position lost on cross-item seek: ${p.position}');
    // Target already played and kept open: the prefetched path.
    await p.seek(const Duration(milliseconds: 200), index: 0);
    await eventually(() => p.currentIndex == 0, 'seek back failed');
    await eventually(() => (p.position.inMilliseconds - 200).abs() < 250,
        'position lost seeking back to open item: ${p.position}');
  });
  await scenario('insert into empty playlist attaches source and plays',
      () async {
    final p = await newPlayer();
    // ignore: deprecated_member_use
    final playlist = ConcatenatingAudioSource(children: []);
    await p.setAudioSource(playlist);
    await playlist.add(AudioSource.uri(_shortWav.uri));
    unawaited(p.play());
    await eventually(
        () => p.processingState == ProcessingState.completed,
        'inserted item never played: '
        'state=${p.processingState} position=${p.position}');
  });
  await scenario('seek without loaded source is a safe no-op', () async {
    final p = RawPlayer('seek-empty');
    await p.init();
    try {
      final unindexed = await caught(p.call('seek', {'position': 500000}));
      require(unindexed == null,
          'unindexed seek on empty player errored: $unindexed');
      final indexed = await caught(p.call('seek', {
        'index': 0,
        'position': 0,
      }));
      require(indexed is PlatformException,
          'indexed seek on empty player must be rejected: $indexed');
    } finally {
      await p.dispose();
    }
  });
  await scenario('initial load reports no spurious index 0', () async {
    final p = RawPlayer('index-settle');
    await p.init();
    try {
      await p.call('load', {
        'audioSource':
            playlist('root', [leaf('a'), leaf('b'), leaf('c')], [0, 1, 2]),
        'initialIndex': 2,
        'initialPosition': 0,
      });
      await p.call('play').timeout(const Duration(seconds: 6));
      final indices =
          p.events.map((e) => e['currentIndex']).whereType<int>().toList();
      require(indices.isNotEmpty, 'no index events were received');
      require(indices.every((i) => i == 2),
          'spurious indices observed before settle: $indices');
    } finally {
      await p.dispose();
    }
  });
  await scenario('clipped duration and EOF', () async {
    final p = await newPlayer();
    await p.setAudioSource(ClippingAudioSource(
        child: AudioSource.uri(_longWav.uri),
        start: const Duration(seconds: 1),
        end: const Duration(seconds: 2)));
    require(p.duration == const Duration(seconds: 1),
        'Wrong clipped duration ${p.duration}');
    unawaited(p.play());
    await eventually(() => p.processingState == ProcessingState.completed,
        'clip did not finish');
    require(p.position == const Duration(seconds: 1),
        'Wrong clip end ${p.position}');
  });
  await scenario('unsupported methods report failure', () async {
    const root = MethodChannel('com.ryanheise.just_audio.methods');
    const channel =
        MethodChannel('com.ryanheise.just_audio.methods.unsupported-test');
    await root.invokeMethod<void>('init', {'id': 'unsupported-test'});
    try {
      for (final request in <String, Map<String, Object>>{
        'setPitch': {'pitch': 1.5},
        'setSkipSilence': {'enabled': true}
      }.entries) {
        Object? error;
        try {
          await channel.invokeMethod<void>(request.key, request.value);
        } catch (e) {
          error = e;
        }
        require(error is PlatformException || error is MissingPluginException,
            '${request.key} falsely succeeded');
      }
    } finally {
      await root
          .invokeMethod<void>('disposePlayer', {'id': 'unsupported-test'});
    }
  });
  await scenario('independent players', () async {
    final a = await newPlayer();
    final b = await newPlayer();
    await a.setFilePath(_longWav.path);
    await b.setFilePath(_longWav.path);
    unawaited(a.play());
    unawaited(b.play());
    await eventually(
        () => b.position.inMilliseconds > 100, 'second player did not start');
    await a.pause();
    final pos = b.position;
    await eventually(() => b.position > pos + const Duration(milliseconds: 100),
        'pausing first stopped second');
  });
  await scenario('initial shuffle order matches automatic progression',
      () async {
    final p = await newPlayer();
    await p.setShuffleModeEnabled(true);
    await p.setAudioSources(
        List.generate(3, (_) => AudioSource.uri(_shortWav.uri)),
        shuffleOrder: ReverseShuffle());
    final indices = <int?>[];
    final sub = p.currentIndexStream.distinct().listen(indices.add);
    try {
      unawaited(p.play());
      await eventually(() => p.processingState == ProcessingState.completed,
          'shuffle did not finish');
      require(
          indices.join(',') == '0,2,1', 'Expected 0,2,1, observed $indices');
    } finally {
      await sub.cancel();
    }
  });
  await scenario('loop one repeats current track and off completes', () async {
    final p = await newPlayer();
    await p.setAudioSources(
        [AudioSource.uri(_shortWav.uri), AudioSource.uri(_shortWav.uri)]);
    await p.setLoopMode(LoopMode.one);
    unawaited(p.play());
    await Future<void>.delayed(const Duration(milliseconds: 1200));
    require(
        p.currentIndex == 0 && p.processingState != ProcessingState.completed,
        'loop-one advanced or completed');
    await p.setLoopMode(LoopMode.off);
    await eventually(() => p.processingState == ProcessingState.completed,
        'turning loop off did not finish');
    require(p.currentIndex == 1, 'loop-off failed to visit second track');
  });
  await scenario('looping source expands finite sequence', () async {
    final p = await newPlayer();
    // ignore: deprecated_member_use
    await p.setAudioSource(
        // ignore: deprecated_member_use
        LoopingAudioSource(child: AudioSource.uri(_shortWav.uri), count: 2));
    final indices = <int?>[];
    final sub = p.currentIndexStream.distinct().listen(indices.add);
    try {
      unawaited(p.play());
      await eventually(() => p.processingState == ProcessingState.completed,
          'finite loop did not finish');
      require(indices.join(',') == '0,1',
          'Wrong expanded looping sequence $indices');
    } finally {
      await sub.cancel();
    }
  });
  await scenario('nested insertion removal and move target source ID',
      () async {
    final p = await newPlayer();
    // ignore: deprecated_member_use
    final group =
        // ignore: deprecated_member_use
        ConcatenatingAudioSource(children: [AudioSource.uri(_longWav.uri)]);
    await p.setAudioSources([AudioSource.uri(_shortWav.uri), group]);
    await group.insert(0, AudioSource.uri(_shortWav.uri));
    await p.seek(Duration.zero, index: 2);
    await eventually(
        () => p.currentIndex == 2 && p.duration == const Duration(seconds: 3),
        'nested insert targeted wrong index');
    await group.move(1, 0);
    await p.seek(Duration.zero, index: 1);
    await eventually(
        () => p.currentIndex == 1 && p.duration == const Duration(seconds: 3),
        'nested move lost original item');
    await group.removeRange(1, 2);
    require(p.sequence.length == 2, 'wrong Dart sequence after remove');
    await p.seek(Duration.zero, index: 0);
    await eventually(() => p.duration == const Duration(milliseconds: 400),
        'removal corrupted first item');
  });
  await scenario(
      'malformed requests return valid errors and leave loaded source usable',
      () async {
    final p = RawPlayer('validation');
    await p.init();
    try {
      await p.call('load', {
        'audioSource': playlist('root', [leaf('a')], [0])
      });
      for (final request in <(String, Map<String, Object?>)>[
        ('setVolume', {'volume': 'bad'}),
        ('setVolume', {'volume': double.nan}),
        ('setSpeed', {'speed': -1.0}),
        ('setLoopMode', {'loopMode': 7}),
        ('seek', {'index': 1 << 40}),
        (
          'concatenatingInsertAll',
          {
            'id': 'root',
            'index': 1,
            'children': [leaf('b')],
            'shuffleOrder': [0, 0]
          }
        ),
        (
          'setShuffleOrder',
          {
            'audioSource': playlist('root', [leaf('a')], [3])
          }
        ),
      ]) {
        final failure = await caught(p.call(request.$1, request.$2));
        require(failure is PlatformException,
            '${request.$1} accepted invalid input or invalid error: $failure');
      }
      await p.call('play').timeout(const Duration(seconds: 5));
      require(p.events.any((e) => e['processingState'] == 4),
          'invalid mutations corrupted existing source');
    } finally {
      await p.dispose();
    }
  });
  await scenario('unsupported source error preserves UTF-8 message', () async {
    final p = RawPlayer('error-text');
    await p.init();
    try {
      final error = await caught(p.call('load', {
        'audioSource': {
          'id': 'x',
          'type': 'unsupported-source-with-a-long-owned-message'
        }
      }));
      require(
          error is PlatformException, 'Expected platform error, got $error');
      require(
          (error as PlatformException)
                  .message
                  ?.contains('unsupported-source') ==
              true,
          'error message was lost');
    } finally {
      await p.dispose();
    }
  });
  await scenario('shuffle order after insertion is honored', () async {
    final p = RawPlayer('mutation-shuffle');
    await p.init();
    try {
      await p.call('load', {
        'audioSource': playlist('root', [leaf('a'), leaf('b')], [0, 1])
      });
      await p.call('concatenatingInsertAll', {
        'id': 'root',
        'index': 2,
        'children': [leaf('c')],
        'shuffleOrder': [0, 2, 1]
      });
      await p.call('setShuffleMode', {'shuffleMode': 1});
      await p.call('seek', {'index': 0, 'position': 0});
      p.events.clear();
      await p.call('play').timeout(const Duration(seconds: 6));
      final indices = <int>[];
      for (final e in p.events) {
        final index = e['currentIndex'];
        if (index is int && (indices.isEmpty || indices.last != index)) {
          indices.add(index);
        }
      }
      require(indices.join(',') == '0,2,1',
          'Inserted shuffle order ignored: $indices');
    } finally {
      await p.dispose();
    }
  });
  await scenario('local HTTP load and missing resource failure', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((request) async {
      if (request.uri.path == '/missing.wav') {
        request.response.statusCode = 404;
      } else {
        request.response.headers.contentType = ContentType('audio', 'wav');
        final bytes = _shortWav.readAsBytesSync();
        request.response.contentLength = bytes.length;
        request.response.add(bytes);
      }
      await request.response.close();
    });
    try {
      final p = await newPlayer();
      final base = 'http://127.0.0.1:${server.port}';
      final duration = await p.setUrl('$base/audio.wav');
      require(duration == const Duration(milliseconds: 400),
          'HTTP duration incorrect');
      final error = await caught(
          p.setUrl('$base/missing.wav').timeout(const Duration(seconds: 5)));
      require(error is PlayerException,
          'HTTP 404 not reported as PlayerException: $error');
      await p.setFilePath(_shortWav.path);
      await p.play().timeout(const Duration(seconds: 5));
      require(p.processingState == ProcessingState.completed,
          'player did not recover from network error');
    } finally {
      await server.close(force: true);
    }
  });
  await scenario('disposal and replacement settle pending loads', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((request) {
      /* Deliberately withhold response until server closes. */
    });
    final uri = Uri.parse('http://127.0.0.1:${server.port}/pending.wav');
    try {
      for (final replace in [false, true]) {
        final p = RawPlayer('pending-$replace');
        await p.init();
        final pending =
            caught(p.call('load', {'audioSource': leaf('slow', uri)}));
        await Future<void>.delayed(const Duration(milliseconds: 100));
        if (replace) {
          await p.call('load', {'audioSource': leaf('good')});
        } else {
          await p.dispose();
        }
        final error = await pending.timeout(const Duration(seconds: 3));
        require(
            error is PlatformException, 'Pending load did not abort: $error');
        if (replace) await p.dispose();
      }
    } finally {
      await server.close(force: true);
    }
  });
  await scenario('rapid create load seek dispose with callbacks in flight',
      () async {
    for (var i = 0; i < 30; i++) {
      final p = RawPlayer('stress-$i');
      await p.init();
      final pending = caught(p.call('load', {
        'audioSource': playlist('root', [leaf('a'), leaf('b')], [0, 1])
      }));
      await p.dispose();
      final outcome = await pending.timeout(const Duration(seconds: 2));
      require(outcome == null || outcome is PlatformException,
          'invalid disposed load result $outcome');
    }
    final p = await newPlayer();
    await p.setFilePath(_shortWav.path);
    await p.play().timeout(const Duration(seconds: 5));
    require(p.processingState == ProcessingState.completed,
        'engine unusable after disposal stress');
  });
  watchdog.cancel();
  _save();
  exit(_results.every((r) => r['passed'] == true) ? 0 : 1);
}
