import 'package:flutter/foundation.dart';

/// The Windows implementation of `just_audio`.
///
/// This package provides the native C++ WinRT Media Player backend
/// with thread-safe EventSink and atomic disposal management, eliminating
/// Access Violation (0xC0000005) crashes on Windows.
class JustAudioWindowsPlus {
  /// Internal marker to ensure the library is loaded.
  @visibleForTesting
  static const bool isSupported = true;
}
