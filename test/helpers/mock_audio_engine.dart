import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

/// A contract-accurate fake of the native Windows engine.
///
/// Every rule enforced here mirrors a rule the real plugin enforces in
/// `windows/player.hpp` and `windows/source_model.hpp`: the same validation,
/// the same bounds, and the same error codes. A test written against this fake
/// is therefore a statement about the plugin's actual channel contract. If the
/// C++ contract changes, this file must change with it.
///
/// Scope limits, deliberately: this fake resolves `load` synchronously, so it
/// does not model a load that is replaced or aborted while still pending, and
/// it tracks a single outstanding `play()` future rather than the native
/// `pending_play_` queue. Both are covered against the real plugin by the
/// integration suite in `example/integration_test/native_audio_test.dart`.
class ChannelContractError extends PlatformException {
  ChannelContractError(String code, String message)
      : super(code: code, message: message);
}

/// Mirrors `jaw::ArgumentError`, which the native layer converts into a
/// `PlatformException` with code `argument_error`.
Never _argumentError(String message) =>
    throw ChannelContractError('argument_error', message);

/// The base duration reported for a leaf the fake cannot actually decode.
const int _leafDurationUs = 180000000;

// ---------------------------------------------------------------------------
// Argument shapes.
//
// These reproduce the exact source maps `just_audio` puts on the wire. They are
// the shapes the integration suite proves the real plugin accepts, and every
// native source is required to carry an `id` and a valid `shuffleOrder` -
// shapes that are easy to omit and that the native parser rejects.
// ---------------------------------------------------------------------------

Map<String, dynamic> progressiveSource(String id, String uri) =>
    <String, dynamic>{'id': id, 'type': 'progressive', 'uri': uri, 'headers': null};

Map<String, dynamic> streamingSource(String id, String type, String uri) =>
    <String, dynamic>{'id': id, 'type': type, 'uri': uri, 'headers': null};

Map<String, dynamic> clippingSource(
  String id,
  String childId,
  String uri, {
  int start = 0,
  int? end,
}) =>
    <String, dynamic>{
      'id': id,
      'type': 'clipping',
      'start': start,
      if (end != null) 'end': end,
      'child': progressiveSource(childId, uri),
    };

Map<String, dynamic> loopingSource(
  String id,
  String childId,
  String uri,
  int count,
) =>
    <String, dynamic>{
      'id': id,
      'type': 'looping',
      'count': count,
      'child': progressiveSource(childId, uri),
    };

Map<String, dynamic> concatenatingSource(
  String id,
  List<Map<String, dynamic>> children,
  List<int> shuffleOrder,
) =>
    <String, dynamic>{
      'id': id,
      'type': 'concatenating',
      'children': children,
      'shuffleOrder': shuffleOrder,
      'useLazyPreparation': true,
    };

/// A three-item concatenating playlist in natural order.
Map<String, dynamic> threeTrackPlaylist([String id = 'root']) => concatenatingSource(
      id,
      <Map<String, dynamic>>[
        progressiveSource('$id-0', 'file:///audio/0.mp3'),
        progressiveSource('$id-1', 'file:///audio/1.mp3'),
        progressiveSource('$id-2', 'file:///audio/2.mp3'),
      ],
      <int>[0, 1, 2],
    );


/// Mirrors `SourceNode::Parse`, `Size`, `Flatten` and `Order` from
/// `windows/source_model.hpp`.
class _SourceNode {
  _SourceNode({
    required this.id,
    required this.type,
    required this.data,
    List<_SourceNode>? children,
    List<int>? shuffle,
    this.count = 1,
  })  : children = children ?? <_SourceNode>[],
        shuffle = shuffle ?? <int>[];

  final String id;
  final String type;
  final Map<String, dynamic> data;
  final List<_SourceNode> children;
  List<int> shuffle;
  final int count;

  static const int maxSize = 100000;
  static const int maxDepth = 64;

  static int integer(Object? value, String key) {
    if (value is int) return value;
    if (value is double && value.isFinite && value == value.roundToDouble()) {
      return value.toInt();
    }
    _argumentError('Missing $key');
  }

  static int optionalInteger(Object? value, {int fallback = 0}) =>
      value == null ? fallback : integer(value, 'value');

  /// Mirrors `SourceNode::Permutation`: the order must be an exact
  /// permutation, so the length must match and every index must be unique and
  /// in range.
  static List<int> permutation(Object? value, int size) {
    if (value is! List || value.length != size) {
      _argumentError('shuffleOrder has incorrect length');
    }
    final seen = List<bool>.filled(size, false);
    final order = <int>[];
    for (final entry in value) {
      final index = integer(entry, 'shuffleOrder');
      if (index < 0 || index >= size || seen[index]) {
        _argumentError('shuffleOrder must be a permutation');
      }
      seen[index] = true;
      order.add(index);
    }
    return order;
  }

  static _SourceNode parse(Map<String, dynamic> data, [int depth = 0]) {
    if (depth > maxDepth) _argumentError('Audio source nesting is too deep');
    final id = data['id'];
    final type = data['type'];
    if (id is! String) _argumentError('Missing or invalid id');
    if (type is! String) _argumentError('Missing or invalid type');

    switch (type) {
      case 'concatenating':
        final rawChildren = data['children'];
        if (rawChildren is! List) _argumentError('Missing or invalid children');
        final children = <_SourceNode>[];
        for (final child in rawChildren) {
          if (child is! Map) _argumentError('Source child must be a map');
          children.add(parse(Map<String, dynamic>.from(child), depth + 1));
        }
        return _SourceNode(
          id: id,
          type: type,
          data: data,
          children: children,
          shuffle: permutation(data['shuffleOrder'], children.length),
        );

      case 'looping':
        final count = integer(data['count'], 'count');
        if (count < 0 || count > maxSize) {
          _argumentError('Invalid looping count');
        }
        final child = data['child'];
        if (child is! Map) _argumentError('Missing or invalid child');
        return _SourceNode(
          id: id,
          type: type,
          data: data,
          children: <_SourceNode>[
            parse(Map<String, dynamic>.from(child), depth + 1),
          ],
          count: count,
        );

      case 'clipping':
        final child = data['child'];
        if (child is! Map) _argumentError('Missing or invalid child');
        final leaf = parse(Map<String, dynamic>.from(child), depth + 1);
        if (leaf.type != 'progressive' &&
            leaf.type != 'hls' &&
            leaf.type != 'dash') {
          _argumentError('Clipping requires a URI source');
        }
        final start = optionalInteger(data['start']);
        final end = optionalInteger(data['end'], fallback: -1);
        if (start < 0 || (end != -1 && end <= start)) {
          _argumentError('Invalid clipping range');
        }
        return _SourceNode(
          id: id,
          type: type,
          data: data,
          children: <_SourceNode>[leaf],
        );

      case 'progressive':
      case 'hls':
      case 'dash':
        final uri = data['uri'];
        if (uri is! String || uri.isEmpty) _argumentError('Source URI is empty');
        final headers = data['headers'];
        if (headers != null) {
          if (headers is! Map) _argumentError('headers must be a map');
          if (headers.isNotEmpty) {
            _argumentError(
              "Direct HTTP headers are unsupported; use just_audio's proxy",
            );
          }
        }
        return _SourceNode(id: id, type: type, data: data);

      default:
        _argumentError('Unsupported audio source type: $type');
    }
  }

  int get size {
    if (type == 'looping') {
      final childSize = children.first.size;
      if (childSize != 0 && count > maxSize ~/ childSize) {
        _argumentError('Audio source is too large');
      }
      return count * childSize;
    }
    if (type != 'concatenating') return 1;
    var total = 0;
    for (final child in children) {
      total += child.size;
      if (total > maxSize) _argumentError('Audio source is too large');
    }
    return total;
  }

  /// Mirrors `SourceNode::Flatten`: the physical item order the WinRT
  /// playback list is built from.
  void flatten(List<Map<String, dynamic>> into) {
    if (type == 'concatenating') {
      for (final child in children) {
        child.flatten(into);
      }
    } else if (type == 'looping') {
      for (var i = 0; i < count; i++) {
        children.first.flatten(into);
      }
    } else {
      into.add(data);
    }
  }

  /// Mirrors `SourceNode::Order`: the automatic progression order, which is
  /// the flattened leaves reordered by each concatenating level's shuffle.
  void order(List<int> into, [int offset = 0]) {
    if (type == 'concatenating') {
      final offsets = <int>[];
      var next = offset;
      for (final child in children) {
        offsets.add(next);
        next += child.size;
      }
      for (final index in shuffle) {
        children[index].order(into, offsets[index]);
      }
    } else if (type == 'looping') {
      final child = children.first;
      for (var i = 0; i < count; i++) {
        child.order(into, offset + i * child.size);
      }
    } else {
      into.add(offset);
    }
  }

  _SourceNode? findById(String target) {
    if (id == target) return this;
    for (final child in children) {
      final match = child.findById(target);
      if (match != null) return match;
    }
    return null;
  }
}

/// Mirrors the `AudioPlayer` state machine in `windows/player.hpp`.
class MockPlayerInstance {
  MockPlayerInstance(this.id);

  final String id;

  bool isDisposed = false;
  bool sourceSet = false;
  bool loading = false;
  bool failed = false;
  bool completed = false;
  bool playing = false;
  /// Mirrors `seek_in_progress_`: the session still reports the pre-seek
  /// position until the media pipeline acknowledges the seek.
  bool seekInProgress = false;
  /// Mirrors `seek_pending_`: a seek queued for the item that is still opening.
  bool seekPending = false;
  double volume = 1.0;
  double speed = 1.0;
  int loopMode = 0;
  int shuffleMode = 0;
  int positionUs = 0;
  int durationUs = 0;
  int requestedIndex = 0;
  int currentIndex = -1;

  _SourceNode _tree =
      _SourceNode(id: '', type: '', data: <String, dynamic>{});
  final List<Map<String, dynamic>> leaves = <Map<String, dynamic>>[];

  final List<MethodCall> methodCalls = <MethodCall>[];
  final StreamController<dynamic> eventController =
      StreamController<dynamic>.broadcast();
  final StreamController<dynamic> dataController =
      StreamController<dynamic>.broadcast();

  /// The last map handed to the data stream.
  Map<String, dynamic> publishedData = <String, dynamic>{};

  /// The data maps actually delivered, in order.
  final List<Map<String, dynamic>> deliveredData = <Map<String, dynamic>>[];

  /// Mirrors the native `pending_play_` queue, which only `CompletePlay()`
  /// drains. A single slot rather than a queue, which is enough for the
  /// pause/completion semantics the contract tests assert.
  final Completer<void> pendingPlay = Completer<void>();

  static String methodChannelName(String id) =>
      'com.ryanheise.just_audio.methods.$id';
  static String eventChannelName(String id) =>
      'com.ryanheise.just_audio.events.$id';
  static String dataChannelName(String id) =>
      'com.ryanheise.just_audio.data.$id';

  MethodChannel get methodChannel =>
      MethodChannel(methodChannelName(id));
  EventChannel get eventChannel => EventChannel(eventChannelName(id));
  EventChannel get dataChannel => EventChannel(dataChannelName(id));

  /// Mirrors `ProcessingState()`.
  int get processingState {
    if (failed || !sourceSet) return 0;
    if (completed) return 4;
    if (loading) return 1;
    return 3;
  }

  /// The buffer fraction the fake reports. Mirrors `Broadcast()`, which treats
  /// a local `file:` URI as fully buffered because there is nothing to fetch.
  double _progressForIndex(int index) {
    if (index < 0 || index >= leaves.length) return 0.0;
    final leaf = leaves[index];
    final source = leaf['type'] == 'clipping' ? leaf['child'] : leaf;
    final uri = source is Map ? source['uri'] as String? : null;
    return uri != null && uri.startsWith('file:') ? 1.0 : 0.0;
  }

  /// Mirrors the state map published by `Broadcast()`.
  Map<String, dynamic> get stateEvent {
    final progress = _progressForIndex(currentIndex);
    final buffered = durationUs > 0
        ? (positionUs > durationUs
            ? durationUs
            : (positionUs > durationUs * progress ? positionUs : (durationUs * progress).round()))
        : positionUs;
    return <String, dynamic>{
      'processingState': processingState,
      'updatePosition': positionUs,
      'updateTime': DateTime.now().millisecondsSinceEpoch,
      'bufferedPosition': buffered,
      'duration': durationUs > 0 ? durationUs : null,
      'currentIndex': currentIndex < 0 ? null : currentIndex,
    };
  }

  Map<String, dynamic> get dataEvent => <String, dynamic>{
        'playing': playing,
        'volume': volume,
        'speed': speed,
        'loopMode': loopMode,
        'shuffleMode': shuffleMode,
      };

  /// Mirrors the data-sink dedupe in `Broadcast()`: intent is published only
  /// when a value actually changes. A redundant copy can overwrite a newer
  /// decision the Dart side already made, because just_audio applies every
  /// value it receives.
  ///
  /// Compared field by field because Dart's `Map.==` is identity, while the
  /// native `EncodableMap` compares by value.
  bool get dataWouldChange => !_sameData(publishedData, dataEvent);

  static bool _sameData(Map<String, dynamic> a, Map<String, dynamic> b) {
    for (final key in <String>[
      'playing',
      'volume',
      'speed',
      'loopMode',
      'shuffleMode',
    ]) {
      if (a[key] != b[key]) return false;
    }
    return true;
  }

  /// The physical leaves in the order the native playback list holds them.
  List<Map<String, dynamic>> get playlist => List<Map<String, dynamic>>.unmodifiable(leaves);

  /// The automatic progression order produced by the shuffle orders.
  ///
  /// `just_audio` sends a permuted `shuffleOrder` with every load, whether or
  /// not shuffling is enabled, so this is the order the tree *declares*.
  List<int> get progressionOrder {
    final order = <int>[];
    _tree.order(order);
    return order;
  }

  /// The order `MediaPlaybackList` actually advances in, mirroring `Load()`,
  /// `Mutate()` and `ApplyModes()` in `windows/player.hpp`.
  ///
  /// The playlist holds the items in physical leaf order. `SetShuffledItems`
  /// reorders that progression, but it defines the progression whether or not
  /// `ShuffleEnabled` is set, so it is only applied while shuffling is on.
  /// Without that guard a shuffle-off playlist would play in the declared
  /// permutation instead of its natural order.
  List<int> get playbackOrder {
    if (shuffleMode != 1) {
      return List<int>.generate(leaves.length, (i) => i, growable: false);
    }
    return progressionOrder;
  }

  int durationOf(Map<String, dynamic>? leaf) {
    if (leaf == null) return 0;
    if (leaf['type'] == 'clipping') {
      final start = (leaf['start'] as int?) ?? 0;
      final end = (leaf['end'] as int?) ?? -1;
      return end >= 0 ? end - start : _leafDurationUs;
    }
    return _leafDurationUs;
  }

  void completePlay() {
    if (!pendingPlay.isCompleted) pendingPlay.complete();
  }

  /// Mirrors `Fail()`: roll the source back and surface the error.
  void fail(String message) {
    if (failed || isDisposed) return;
    failed = true;
    loading = false;
    completed = false;
    sourceSet = false;
    seekInProgress = false;
    seekPending = false;
    currentIndex = -1;
    broadcastState(errorMessage: message);
    completePlay();
  }

  void broadcastState({String? errorMessage, bool forceData = false}) {
    if (isDisposed) return;
    final event = stateEvent;
    if (errorMessage != null) {
      event['errorCode'] = 1;
      event['errorMessage'] = errorMessage;
    }
    // The state map is a sample and is always published.
    eventController.add(event);
    if (forceData || dataWouldChange) publishData();
  }

  /// Mirrors the data half of `Broadcast()`.
  void publishData() {
    publishedData = dataEvent;
    deliveredData.add(publishedData);
    dataController.add(publishedData);
  }

  /// Mirrors `Load()`.
  Map<String, dynamic> load(Map<String, dynamic> args) {
    final rawSource = args['audioSource'];
    if (rawSource is! Map) _argumentError('Missing or invalid audioSource');
    final parsed = _SourceNode.parse(Map<String, dynamic>.from(rawSource));

    final size = parsed.size;
    final index = _SourceNode.optionalInteger(args['initialIndex']);
    final position = _SourceNode.optionalInteger(args['initialPosition']);
    if (index < 0 ||
        (size != 0 && index >= size) ||
        (size == 0 && index != 0) ||
        position < 0) {
      _argumentError('Invalid initial index or position');
    }

    _tree = parsed;
    leaves
      ..clear()
      ..addAll(_flattenTo(parsed));
    sourceSet = true;
    loading = leaves.isNotEmpty;
    failed = false;
    completed = false;
    seekInProgress = true;
    seekPending = leaves.isNotEmpty;
    requestedIndex = index;
    positionUs = position;
    currentIndex = leaves.isEmpty ? -1 : index;
    durationUs = durationOf(leaves.isEmpty ? null : leaves[index]);

    if (leaves.isEmpty) {
      // An empty playlist can never open, so the load settles with a null
      // duration and any outstanding play is completed.
      completePlay();
      return <String, dynamic>{'duration': null};
    }
    // A successful open clears `loading_` and consumes the deferred seek.
    loading = false;
    seekPending = false;
    return <String, dynamic>{'duration': durationUs > 0 ? durationUs : null};
  }

  static List<Map<String, dynamic>> _flattenTo(_SourceNode node) {
    final leaves = <Map<String, dynamic>>[];
    node.flatten(leaves);
    return leaves;
  }

  /// Mirrors the `concatenating*` family. `SourceNode::Mutate` requires both a
  /// target `id` and a `shuffleOrder` that is a valid permutation of the
  /// resulting child count, so both are mandatory here too.
  void mutate(String method, Map<String, dynamic> args) {
    final targetId = args['id'];
    if (targetId is! String) _argumentError('Missing id');
    final target = _tree.findById(targetId);
    if (target == null || target.type != 'concatenating') {
      _argumentError('Unknown concatenating source id');
    }

    int check(Object? value, String key, {required bool allowEnd}) {
      final index = _SourceNode.integer(value, key);
      final size = target.children.length;
      if (index < 0 || index > size || (!allowEnd && index == size)) {
        _argumentError('Playlist index is out of bounds');
      }
      return index;
    }

    switch (method) {
      case 'concatenatingInsertAll':
        final index = check(args['index'], 'index', allowEnd: true);
        final rawChildren = args['children'];
        if (rawChildren is! List) _argumentError('Missing or invalid children');
        final added = <_SourceNode>[];
        for (final child in rawChildren) {
          if (child is! Map) _argumentError('Source child must be a map');
          added.add(_SourceNode.parse(Map<String, dynamic>.from(child)));
        }
        target.children.insertAll(index, added);

      case 'concatenatingRemoveRange':
        final start = check(args['startIndex'], 'startIndex', allowEnd: true);
        final end = check(args['endIndex'], 'endIndex', allowEnd: true);
        if (end < start) _argumentError('Invalid playlist range');
        target.children.removeRange(start, end);

      case 'concatenatingMove':
        final from =
            check(args['currentIndex'], 'currentIndex', allowEnd: false);
        final to = check(args['newIndex'], 'newIndex', allowEnd: false);
        final moved = target.children.removeAt(from);
        target.children.insert(to, moved);

      default:
        _argumentError('Unknown playlist mutation');
    }

    target.shuffle =
        _SourceNode.permutation(args['shuffleOrder'], target.children.length);
    // Re-runs the size and depth validation the native side performs.
    target.size;

    leaves
      ..clear()
      ..addAll(_flattenTo(_tree));
    if (currentIndex >= leaves.length) currentIndex = -1;
    if (leaves.isEmpty) {
      completed = true;
      completePlay();
    }
  }

  void dispose() {
    if (isDisposed) return;
    isDisposed = true;
    loading = false;
    sourceSet = false;
    // The native engine settles outstanding plays with success on disposal.
    completePlay();
    unawaited(eventController.close());
    unawaited(dataController.close());
  }
}

/// Registers [MockAudioEngine] on the root and per-player channels.
class MockAudioEngine {
  static const String rootChannelName = 'com.ryanheise.just_audio.methods';
  static const MethodChannel rootChannel = MethodChannel(rootChannelName);

  static final Map<String, MockPlayerInstance> _activePlayers =
      <String, MockPlayerInstance>{};
  static final List<MethodCall> _rootMethodCalls = <MethodCall>[];

  static Map<String, MockPlayerInstance> get activePlayers => _activePlayers;
  static List<MethodCall> get rootMethodCalls => _rootMethodCalls;

  static MockPlayerInstance? getPlayer(String id) => _activePlayers[id];

  static void install() {
    _activePlayers.clear();
    _rootMethodCalls.clear();

    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(rootChannel, (MethodCall call) async {
      _rootMethodCalls.add(call);
      final args = _args(call);

      switch (call.method) {
        case 'init':
          final id = _requiredId(args);
          _activePlayers.remove(id)?.dispose();
          _unregisterPlayerChannels(id);
          final player = MockPlayerInstance(id);
          _activePlayers[id] = player;
          _registerPlayerChannels(player);
          return <String, dynamic>{};

        case 'disposePlayer':
          final id = _requiredId(args);
          _activePlayers.remove(id)?.dispose();
          _unregisterPlayerChannels(id);
          return <String, dynamic>{};

        case 'disposeAllPlayers':
          for (final player in _activePlayers.values) {
            player.dispose();
            _unregisterPlayerChannels(player.id);
          }
          _activePlayers.clear();
          return <String, dynamic>{};

        default:
          throw MissingPluginException('Not implemented: ${call.method}');
      }
    });
  }

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

  static Map<String, dynamic> _args(MethodCall call) {
    final args = call.arguments;
    if (args == null) return <String, dynamic>{};
    if (args is! Map) _argumentError('Method arguments must be a map');
    return Map<String, dynamic>.from(args);
  }

  static String _requiredId(Map<String, dynamic> args) {
    final id = args['id'];
    if (id is! String) _argumentError('id argument missing');
    return id;
  }

  static double _number(Map<String, dynamic> args, String key) {
    final value = args[key];
    if (value is num) return value.toDouble();
    _argumentError('Missing or invalid $key');
  }

  /// Mirrors `HandleMethodCall`, including the deferred `play()` semantics the
  /// native engine implements through `pending_play_`.
  static Future<Object?> _handlePlayer(
    MockPlayerInstance player,
    MethodCall call,
  ) async {
    player.methodCalls.add(call);
    if (player.isDisposed) {
      throw ChannelContractError('disposed', 'Player has been disposed');
    }
    final args = _args(call);

    switch (call.method) {
      case 'load':
        final result = player.load(args);
        player.broadcastState();
        return result;

      case 'play':
        player.playing = true;
        // Mirrors the native settle rule: a play is settled only when the state
        // is *stably* unplayable. A missing source is not one of those cases,
        // because just_audio's activation sends `play` before `load`; settling
        // it there would resolve `await player.play()` before playback starts.
        if (player.completed ||
            player.failed ||
            (player.sourceSet && player.leaves.isEmpty)) {
          player.completePlay();
        }
        player.completed = false;
        player.broadcastState();
        await player.pendingPlay.future;
        return <String, dynamic>{};

      case 'pause':
        player.playing = false;
        player.completePlay();
        player.broadcastState();
        return <String, dynamic>{};

      case 'stop':
        // Rewinds to the start, so both the in-flight and the deferred seek are
        // dropped: applying the deferred one would undo the rewind.
        player.playing = false;
        player.positionUs = 0;
        player.seekInProgress = false;
        player.seekPending = false;
        player.completePlay();
        player.broadcastState();
        return <String, dynamic>{};

      case 'seek':
        final index = args['index'] == null
            ? -1
            : _SourceNode.integer(args['index'], 'index');
        final position = args['position'] == null
            ? 0
            : _SourceNode.integer(args['position'], 'position');
        if (index < -1 ||
            (index >= 0 && index >= player.leaves.length) ||
            position < 0) {
          _argumentError('Invalid seek index or position');
        }
        player.completed = false;
        // Mirrors the native split: switching item defers the seek until the
        // target opens, a same-item seek lands immediately.
        if (index >= 0) {
          final switchesItem = index != player.currentIndex;
          player.requestedIndex = index;
          player.currentIndex = index;
          player.seekPending = switchesItem;
        }
        player.seekInProgress = true;
        player.positionUs = position;
        player.durationUs = player.durationOf(
          player.leaves.isEmpty
              ? null
              : player.leaves[player.currentIndex.clamp(0, player.leaves.length - 1)],
        );
        player.broadcastState();
        return <String, dynamic>{};

      case 'setVolume':
        final value = _number(args, 'volume');
        if (!value.isFinite || value < 0 || value > 1) {
          _argumentError('Invalid volume or speed');
        }
        player.volume = value;
        player.broadcastState();
        return <String, dynamic>{};

      case 'setSpeed':
        final value = _number(args, 'speed');
        if (!value.isFinite || value <= 0) {
          _argumentError('Invalid volume or speed');
        }
        player.speed = value;
        player.broadcastState();
        return <String, dynamic>{};

      case 'setPitch':
        if (_number(args, 'pitch') == 1.0) return <String, dynamic>{};
        throw ChannelContractError(
          'unsupported',
          'setPitch is not supported on Windows',
        );

      case 'setSkipSilence':
        final enabled = args['enabled'];
        if (enabled is! bool) _argumentError('Missing or invalid enabled');
        if (!enabled) return <String, dynamic>{};
        throw ChannelContractError(
          'unsupported',
          'setSkipSilence is not supported on Windows',
        );

      case 'setLoopMode':
        final mode = _SourceNode.integer(args['loopMode'], 'loopMode');
        if (mode < 0 || mode > 2) _argumentError('Invalid playback mode');
        player.loopMode = mode;
        player.broadcastState();
        return <String, dynamic>{};

      case 'setShuffleMode':
        final mode = _SourceNode.integer(args['shuffleMode'], 'shuffleMode');
        if (mode < 0 || mode > 1) _argumentError('Invalid playback mode');
        player.shuffleMode = mode;
        player.broadcastState();
        return <String, dynamic>{};

      case 'setShuffleOrder':
        final raw = args['audioSource'];
        if (raw is! Map) _argumentError('Missing or invalid audioSource');
        final other = _SourceNode.parse(Map<String, dynamic>.from(raw));
        if (other.id != player._tree.id || other.type != player._tree.type) {
          _argumentError('Shuffle source does not match loaded tree');
        }
        player._tree.shuffle = other.shuffle;
        return <String, dynamic>{};

      case 'concatenatingInsertAll':
      case 'concatenatingRemoveRange':
      case 'concatenatingMove':
        player.mutate(call.method, args);
        player.broadcastState();
        return <String, dynamic>{};

      case 'setAndroidAudioAttributes':
        // just_audio sends this during platform initialization on every OS.
        return <String, dynamic>{};

      case 'dispose':
        player.dispose();
        return <String, dynamic>{};

      default:
        // The native layer answers NotImplemented, which reaches Dart as a
        // MissingPluginException. The Android audio-effect methods fall here:
        // they have no Windows counterpart.
        throw MissingPluginException('Not implemented: ${call.method}');
    }
  }

  static void _registerPlayerChannels(MockPlayerInstance player) {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

    messenger.setMockMethodCallHandler(
      player.methodChannel,
      (MethodCall call) => _handlePlayer(player, call),
    );

    // Mirrors the native EventChannel: `listen` attaches a sink, `cancel`
    // detaches it. Events are delivered as success envelopes on the channel
    // name, and failures as error envelopes.
    messenger.setMockMethodCallHandler(
      MethodChannel(MockPlayerInstance.eventChannelName(player.id)),
      (MethodCall call) async {
        if (call.method == 'listen') {
          player.eventController.stream.listen(
            (dynamic event) => _deliver(
              MockPlayerInstance.eventChannelName(player.id),
              event,
            ),
            onError: (Object error) {
              if (error is PlatformException) {
                _deliverError(
                  MockPlayerInstance.eventChannelName(player.id),
                  error,
                );
              }
            },
          );
        }
        return null;
      },
    );

    messenger.setMockMethodCallHandler(
      MethodChannel(MockPlayerInstance.dataChannelName(player.id)),
      (MethodCall call) async {
        if (call.method == 'listen') {
          player.dataController.stream.listen(
            (dynamic event) => _deliver(
              MockPlayerInstance.dataChannelName(player.id),
              event,
            ),
          );
        }
        return null;
      },
    );
  }

  static void _deliver(String channelName, dynamic event) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .handlePlatformMessage(
      channelName,
      const StandardMethodCodec().encodeSuccessEnvelope(event),
      (ByteData? reply) {},
    );
  }

  static void _deliverError(String channelName, PlatformException error) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .handlePlatformMessage(
      channelName,
      const StandardMethodCodec().encodeErrorEnvelope(
        code: error.code,
        message: error.message,
        details: error.details,
      ),
      (ByteData? reply) {},
    );
  }

  static void _unregisterPlayerChannels(String id) {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger
      ..setMockMethodCallHandler(
        MethodChannel(MockPlayerInstance.methodChannelName(id)),
        null,
      )
      ..setMockMethodCallHandler(
        MethodChannel(MockPlayerInstance.eventChannelName(id)),
        null,
      )
      ..setMockMethodCallHandler(
        MethodChannel(MockPlayerInstance.dataChannelName(id)),
        null,
      );
  }
}
