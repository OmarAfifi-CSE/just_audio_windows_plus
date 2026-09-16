import 'package:flutter/foundation.dart';

/// Windows implementation of `just_audio`, using WinRT MediaPlayer.
///
/// Add this package alongside `just_audio` to register the native Windows
/// implementation automatically. Applications use the `just_audio` Dart API.
/// Format support depends on installed Windows media components. Independent
/// pitch adjustment and silence skipping are unsupported.
///
/// ```dart
/// final player = AudioPlayer(); // From package:just_audio/just_audio.dart.
/// try {
///   await player.setFilePath(r'C:\Audio\recording.wav');
///   await player.play(); // Waits for pause or completion.
/// } finally {
///   await player.dispose();
/// }
/// ```
class JustAudioWindowsPlus {
  /// Package marker; this constant does not detect runtime codec availability
  /// or verify native plugin registration.
  @visibleForTesting
  static const bool isSupported = true;
}
