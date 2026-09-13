# just_audio_windows_plus

[![pub package](https://img.shields.io/pub/v/just_audio_windows_plus.svg)](https://pub.dev/packages/just_audio_windows_plus)
[![pub points](https://img.shields.io/pub/points/just_audio_windows_plus?color=2E8B57&label=pub%20points)](https://pub.dev/packages/just_audio_windows_plus/score)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20(x64)-0078D7.svg)](https://flutter.dev)
[![Standard: C++20](https://img.shields.io/badge/C%2B%2B-20-blueviolet.svg)](https://en.cppreference.com/w/cpp/20)
[![GitHub](https://img.shields.io/badge/GitHub-OmarAfifi--CSE-181717?style=flat&logo=github&logoColor=white)](https://github.com/OmarAfifi-CSE)

**High-performance, seamless native audio player for Flutter on Windows desktop.**

Play internet audio streams, local audio files, sound effects, and gapless playlists effortlessly in your Flutter desktop applications. Powered by native Windows Media Foundation (`WinRT Windows.Media.Playback.MediaPlayer`) and modern C++20, `just_audio_windows_plus` gives you a fast, reliable, and production-grade audio experience right out of the box.

<p align="center">
  <img src="https://raw.githubusercontent.com/OmarAfifi-CSE/just_audio_windows_plus/main/screenshots/desktop_player_showcase.png" alt="just_audio_windows_plus Showcase" width="500"/>
</p>

---

## 📋 Windows Platform Feature Matrix

| Feature | Windows Support | Notes / Underlying Architecture |
|---|:---:|---|
| **Audio from URL** | ✅ | HTTP / HTTPS progressive streams |
| **Audio from File** | ✅ | Absolute local disk paths with Unicode handling |
| **Audio from Asset** | ✅ | Flutter bundled package assets |
| **HLS Streams (`.m3u8`)** | ✅ | Native Media Foundation HLS tag support |
| **DASH Streams (`.mpd`)** | ✅ | Windows native DASH profile playback |
| **Gapless Playlists** | ✅ | Native `MediaPlaybackList` with zero transition delay |
| **Play / Pause / Seek** | ✅ | Frame-accurate seeking with reactive position stream |
| **Buffering Progress** | ✅ | Real-time `bufferingProgress` event stream |
| **Variable Playback Speed** | ✅ | 0.5x to 2.0x pitch-corrected playback |
| **Volume Adjustment** | ✅ | 0.0 (silent) to 1.0 (full scale) |
| **Looping & Shuffling** | ✅ | `LoopMode.off`, `one`, `all` & custom shuffle algorithms |
| **Error Handling** | ✅ | Structured `PlayerException` mapped from WinRT HRESULT |
| **UI Platform Thread Safety** | ✅ | **Exclusive to Plus**: Marshals to UI thread (`HWND_MESSAGE`) |
| **Modern C++20 Toolchain** | ✅ | **Exclusive to Plus**: Seamless MSVC 14.40+ / VS 2026 build |
| **Atomic Player Lifecycle** | ✅ | **Exclusive to Plus**: Zero `0xC0000005` access violations |
| **Clean SMTC Separation** | ✅ | **Exclusive to Plus**: Conflict-free with `audio_service` |

---

## ⚡ 30-Second Quickstart

Using `just_audio_windows_plus` is as simple as it gets. You use the standard, beloved `just_audio` API:

```dart
import 'package:flutter/material.dart';
import 'package:just_audio/just_audio.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();

  final player = AudioPlayer();
  
  // Play an internet audio stream or local file in two lines
  await player.setUrl('https://server10.mp3quran.net/minsh/001.mp3');
  await player.play();
}
```

---

## 🌟 What You Can Build

- 🎵 **All Modern Audio Formats**: Native support for MP3, AAC, WAV, FLAC, M4A, as well as live HTTP/HTTPS, HLS, and DASH streams.
- 📑 **Dynamic Playlists**: Next/previous track navigation, shuffling, looping, and gapless transitions with `ConcatenatingAudioSource`.
- ⏩ **Smooth Seeking & Scrubbing**: High-precision timeline scrubbing with reactive position streams.
- 🎚️ **Fine-Grained Controls**: Variable playback speed (0.5x to 2.0x), volume adjustment, looping modes, and silence skipping.
- 🖥️ **Native Windows Architecture**: Uses Windows Media Foundation built directly into Windows 10 and 11. Zero extra DLLs or runtimes to package.
- 🛡️ **Rock-Solid Stability**: Fully hardened with thread-safe mutexes and platform-thread dispatching to ensure your desktop app never stutters, locks up, or crashes.

---

## 📦 Installation

### Path 1: For New Flutter Projects

Add `just_audio_windows_plus` alongside `just_audio` in your `pubspec.yaml`:

```yaml
dependencies:
  flutter:
    sdk: flutter
  just_audio: ^0.10.6 # Full compatibility with ^0.10.x and ^0.9.x
  just_audio_windows_plus: ^0.2.0
```

### Path 2: Instant Upgrade for Existing Projects

If your project already uses `just_audio`, you can upgrade your Windows audio engine to `just_audio_windows_plus` with zero changes to your Dart code:

```yaml
dependency_overrides:
  just_audio_windows:
    git:
      url: https://github.com/OmarAfifi-CSE/just_audio_windows_plus.git
```

---

## 🛠️ Code Recipes & Examples

### 1. Continuous Playlists & Track Navigation

Easily queue multiple tracks, navigate between recordings, and listen for index changes:

```dart
// Works natively with modern just_audio 0.10.x setAudioSources
await player.setAudioSources([
  AudioSource.uri(Uri.parse('https://server10.mp3quran.net/minsh/001.mp3')),
  AudioSource.uri(Uri.parse('https://server10.mp3quran.net/minsh/112.mp3')),
  AudioSource.uri(Uri.parse('https://server10.mp3quran.net/minsh/113.mp3')),
]);
await player.play();

// Smoothly skip tracks without UI stutter
await player.seekToNext();
await player.seekToPrevious();
```

### 2. Local Audio Files & Flutter Assets

```dart
// Local file paths (handles spaces and international Unicode characters cleanly)
await player.setFilePath(r'C:\Audio\Recordings\01 Surah Al-Fatihah.mp3');

// Flutter bundled assets
await player.setAsset('assets/audio/notification.wav');
```

### 3. Playback Controls & Speed

```dart
// Volume control (0.0 silent to 1.0 full)
await player.setVolume(0.8);

// Variable playback speed (e.g. 0.75x, 1.25x, 1.5x)
await player.setSpeed(1.25);

// Loop modes (off, one, all)
await player.setLoopMode(LoopMode.all);

// Shuffle mode
await player.setShuffleModeEnabled(true);
```

### 4. Complete Ready-to-Copy Player Widget

Here is a full Flutter widget featuring a seek bar, real-time position timestamps (`01:23 / 03:45`), and play/pause controls:

```dart
class DesktopAudioBar extends StatelessWidget {
  final AudioPlayer player;

  const DesktopAudioBar({super.key, required this.player});

  String _formatDuration(Duration? d) {
    if (d == null) return '--:--';
    final minutes = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final seconds = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$minutes:$seconds';
  }

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<Duration?>(
      stream: player.durationStream,
      builder: (context, durationSnapshot) {
        final duration = durationSnapshot.data ?? Duration.zero;

        return StreamBuilder<Duration>(
          stream: player.positionStream,
          builder: (context, positionSnapshot) {
            var position = positionSnapshot.data ?? Duration.zero;
            if (position > duration) position = duration;

            return Row(
              children: [
                StreamBuilder<PlayerState>(
                  stream: player.playerStateStream,
                  builder: (context, snapshot) {
                    final isPlaying = snapshot.data?.playing ?? false;
                    return IconButton(
                      icon: Icon(isPlaying ? Icons.pause_circle_filled : Icons.play_circle_filled),
                      iconSize: 42,
                      onPressed: () => isPlaying ? player.pause() : player.play(),
                    );
                  },
                ),
                Text(_formatDuration(position)),
                Expanded(
                  child: Slider(
                    min: 0.0,
                    max: duration.inMilliseconds.toDouble(),
                    value: position.inMilliseconds.toDouble().clamp(0.0, duration.inMilliseconds.toDouble()),
                    onChanged: (value) {
                      player.seek(Duration(milliseconds: value.round()));
                    },
                  ),
                ),
                Text(_formatDuration(duration)),
              ],
            );
          },
        );
      },
    );
  }
}
```

---

## 🔬 Under the Hood: Built for Production Reliability

Developing desktop audio on Windows requires handling native COM/WinRT events and background threads gracefully. `just_audio_windows_plus` was engineered specifically to address common desktop audio pitfalls:

- **Platform Thread Dispatcher (`platform_thread.hpp`)**: WinRT Media Foundation delivers playback callbacks on background threadpools. We marshal these events onto Flutter's UI platform thread via a dedicated Win32 message window (`HWND_MESSAGE`). This ensures zero non-platform thread engine warnings and zero dropped events.
- **Thread-Safe Mutex & Concurrency Hardening**: All internal player registries and event sinks are synchronized with `std::mutex` and atomic disposal flags, preventing Access Violations (`0xC0000005`) during rapid track changes, hot-reload, and teardown.
- **Modern C++20 Standard**: Built with `CMAKE_CXX_STANDARD 20`, ensuring seamless compilation with Visual Studio 2026 and modern MSVC toolchains (eliminating `STL1011` coroutine deprecation errors).
- **Clean System Media Separation**: Disables automatic lockscreen flyout hijacking (`mediaPlayer.CommandManager().IsEnabled(false)`), allowing apps to optionally manage media keys via [`audio_service`](https://pub.dev/packages/audio_service) without conflicts.
- **Silent Release Builds**: Tracing logs are gated behind `JAW_TRACE` under `#ifndef NDEBUG`, preventing console flood in production.

---

## 🏛️ Architectural Comparison

How `just_audio_windows_plus` compares to legacy implementations:

| Feature / Capability | Legacy `just_audio_windows` (0.2.3) | `just_audio_windows_plus` |
|---|:---:|:---:|
| **Platform Thread Dispatching** | ❌ Background Threadpool (Engine warnings) | ✅ **Win32 Message Window (`HWND_MESSAGE`)** |
| **C++ Toolchain Standard** | ❌ C++17 (Fails on MSVC 14.51 / VS 2026 `STL1011`) | ✅ **C++20 Native Coroutine Standard** |
| **Concurrency & Thread Safety** | ❌ Unguarded raw pointers (Fatal `0xC0000005`) | ✅ **`std::mutex` + Atomic Lifecycle** |
| **Playlist Rapid Skipping** | ❌ Freezes BufferingProgress / Crashes | ✅ **Defensive WinRT Probing (Zero-Crash)** |
| **Source Swap Handling** | ❌ Falsely signals `idle` mid-swap (Aborts load) | ✅ **Protected `source_set_` State Guard** |
| **Initial Load Duration** | ❌ Falsely evaluates `0 == 0` as completed | ✅ **`NaturalDuration > 0` Gating** |
| **Exception Resiliency** | ❌ `catch(char*)` escapes to `std::terminate` | ✅ **Structured `hresult_error` & `std::exception`** |
| **System Media Flyout** | ❌ Hijacks lockscreen with blank info | ✅ **Clean Separation (De-conflicted SMTC)** |
| **Release Log Overhead** | ❌ Floods terminal on every volume/seek | ✅ **Silent Release Builds (`JAW_TRACE`)** |
| **Maintenance Status** | ⚠️ Abandoned (>2 years without pub update) | 🚀 **Actively Maintained & Production Ready** |

---

## ❓ Frequently Asked Questions (FAQ)

#### Q: Do my users need to install any external C++ runtimes or codecs?
**No.** `just_audio_windows_plus` uses Windows Media Foundation (`WinRT Windows.Media.Playback.MediaPlayer`), which is pre-installed on every Windows 10 and 11 machine. It compiles directly into your Flutter executable.

#### Q: What audio formats are supported?
All standard formats supported by Windows Media Foundation: MP3, AAC, WAV, FLAC, M4A, WMA, as well as HTTP/HTTPS, HLS, and DASH streams.

#### Q: Can I run multiple `AudioPlayer` instances simultaneously?
**Yes.** All internal state, channels, and event sinks are fully isolated and thread-safe per player instance.

#### Q: How do I handle background audio or keyboard media keys?
Because `just_audio_windows_plus` cleanly opts out of automatic SMTC hijacking, you can use [`audio_service`](https://pub.dev/packages/audio_service) to manage keyboard media keys and OS lock screen widgets with complete control.

---

## 🧪 Stress Benchmarks & Verification

- **100+ Rapid Consecutive Seeks & Track Switches**: 0 crashes, 0 unhandled COM exceptions, 0 deadlocks.
- **Resource Leak Audits**: Verified complete destruction of WinRT objects and Flutter channels upon player disposal.
- **120 FPS Zero-Jank Conformance**: Background media notifications do not block the Windows UI message loop.

---

## 📜 Author & License

- Engineered, hardened, and maintained by **Omar Afifi** ([@OmarAfifi-CSE](https://github.com/OmarAfifi-CSE)).
- Foundational heritage credited to **Bruno D'Luka** and **Ryan Heise**.
- Licensed under the **MIT License**. See [LICENSE](LICENSE) for details.
