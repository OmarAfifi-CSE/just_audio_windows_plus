import 'package:flutter/foundation.dart';

/// # just_audio_windows_plus
///
/// The premier, production-grade Windows audio engine for Flutter.
///
/// Powered by native Windows Media Foundation (`WinRT Windows.Media.Playback.MediaPlayer`),
/// engineered with modern C++20, and hardened with multi-threaded mutex synchronization,
/// this plugin provides rock-solid, crash-proof audio for Flutter on Windows desktop.
///
/// ## Quickstart
///
/// ```dart
/// import 'package:just_audio/just_audio.dart';
///
/// void main() async {
///   final player = AudioPlayer();
///   await player.setUrl('https://example.com/audio.mp3');
///   await player.play();
/// }
/// ```
class JustAudioWindowsPlus {
  /// Internal marker ensuring the native Windows plugin library is registered.
  @visibleForTesting
  static const bool isSupported = true;
}
