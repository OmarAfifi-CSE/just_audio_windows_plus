# just_audio_windows_plus

[![pub package](https://img.shields.io/pub/v/just_audio_windows_plus.svg)](https://pub.dev/packages/just_audio_windows_plus)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows-0078D7.svg)](https://flutter.dev)

A hardened, **thread-safe Windows platform implementation** of [`just_audio`](https://pub.dev/packages/just_audio) utilizing the Windows Media Foundation (`WinRT Windows.Media.Playback.MediaPlayer`) backend.

This package is a maintained community fork of `just_audio_windows` designed specifically to eliminate multi-threading race conditions, memory corruption, and **fatal Access Violation (`0xC0000005`) crashes** on Windows.

---

## 🛑 The Problem in Upstream `just_audio_windows`

In the original `just_audio_windows`:
1. **Threadpool Callback Contention**: WinRT Media Foundation delivers playback state notifications (`PlaybackStateChanged`, `PositionChanged`, `MediaOpened`) asynchronously on background Windows Threadpool worker threads.
2. **Use-After-Free (`0xC0000005`)**: When switching tracks, reciters, or audio sources rapidly in Flutter, the platform/UI thread calls `disposePlayer`. The original C++ code reset `event_sink_` and deleted the player instance **without mutex synchronization**.
3. While `Dispose()` was resetting the pointer on Thread A, a background WinRT callback on Thread B was simultaneously calling `event_sink_->Success(...)`, dereferencing a dangling or null pointer and instantly crashing the desktop application with:
   ```text
   Exception Code: 0xC0000005 (Access Violation)
   Fault Offset: 0x47c7f (just_audio_windows_plugin.dll)
   Lost connection to device. Exited.
   ```
4. Upstream maintenance of `just_audio_windows` has stalled, leaving open issues and unmerged pull requests.

---

## 🛡️ The Architectural Solution

`just_audio_windows_plus` re-engineers the C++ plugin core with robust multi-threaded primitives:

- **Thread-Safe EventSink**: All `EventSink` emissions are guarded by `std::mutex sink_mutex_`.
- **Atomic Lifecycle Transitions**: Introduces `std::atomic<bool> disposed_{false}` and `std::recursive_mutex player_mutex_` to guarantee that player destruction and background WinRT events never collide.
- **Synchronized Global Registry**: The `players_` vector in `just_audio_windows_plugin.cpp` is protected with `std::mutex players_mutex_`. During disposal, the target player is moved out of the registry and destroyed safely outside the lock, preventing deadlocks.
- **Defensive WinRT Property Probing**: Wrapped `NaturalDuration`, `BufferingProgress`, and `Position` getters in structured exception handlers to prevent unhandled COM/WinRT exceptions during playlist track transitions.

---

## 🚀 Getting Started

### Installation

Add `just_audio_windows_plus` to your `pubspec.yaml`:

```yaml
dependencies:
  just_audio: ^0.9.44
  just_audio_windows_plus: ^0.1.0
```

### Overriding the Default Windows Plugin

Because `just_audio` specifies `just_audio_windows` as its default Windows package, you can enforce `just_audio_windows_plus` across your entire project using `dependency_overrides`:

```yaml
dependency_overrides:
  just_audio_windows:
    # Option A: Use git
    git:
      url: https://github.com/OmarAfifi-CSE/just_audio_windows_plus.git

    # Option B: Use local path
    # path: packages/just_audio_windows_plus
```

Or simply register it alongside `just_audio` in your `dependencies:`.

---

## 💻 Usage

Use `just_audio` exactly as you normally would. No changes to your Dart application code are required:

```dart
import 'package:flutter/material.dart';
import 'package:just_audio/just_audio.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final player = AudioPlayer();
  await player.setUrl('https://example.com/audio.mp3');
  await player.play();
}
```

Rapid track switching, continuous playlists, seeking, and concurrent audio operations will now execute reliably on Windows without process crashes.

---

## 🧪 Verification & Stability

Tested under rigorous stress scenarios:
- **100+ consecutive track switches** under heavy GPU/video load: **0 crashes**.
- Verified zero memory leaks during repeated `AudioPlayer` creation and disposal loops.
- Fully compatible with 64-bit Windows 10 and Windows 11.

---

## 📜 Credits & License

- Built upon the foundational work by **Bruno D'Luka** ([@bdlukaa](https://github.com/bdlukaa)) and **Ryan Heise** ([@ryanheise](https://github.com/ryanheise)).
- Hardened, re-engineered, and maintained by **Omar Afifi**.
- Licensed under the **MIT License**. See [LICENSE](LICENSE) for details.
