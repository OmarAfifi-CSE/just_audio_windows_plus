## 0.2.0

* **Platform Thread Dispatcher**: Marshals all WinRT event callbacks (`PlaybackStateChanged`, `MediaFailed`, `CurrentItemChanged`, `ItemFailed`) onto Flutter's UI platform thread via a Win32 message-only window (`HWND_MESSAGE`). Completely eliminates engine warnings: `channel sent a message from native to Flutter on a non-platform thread`.
* **C++20 Toolchain Upgrade**: Built with `CMAKE_CXX_STANDARD 20` to fix compilation error `C2338 / STL1011` under Visual Studio 2026 / MSVC 14.51 where experimental coroutines are deprecated.
* **Defensive Playlist Transitions**: Eliminates crashes and buffering errors when skipping tracks rapidly in `ConcatenatingAudioSource`. Removed premature `broadcastState()` in `seekToItem()` and defensively guarded `NaturalDuration`, `Position`, and `BufferingProgress` WinRT property queries.
* **Source Swap Interruption Fix**: Fixes `PlayerInterruptedException('Loading interrupted')` on source swaps by preventing gaps during playlist clearing from being falsely reported as `idle`.
* **Duration Gated Completion**: Requires `NaturalDuration() > 0` before emitting `completed` (state 4), preventing freshly loaded clips from prematurely reporting completed.
* **Robust Exception Handling**: Catches `winrt::hresult_error` and `std::exception` in `load` and method calls, returning proper Dart errors instead of crashing the process via `std::terminate`.
* **SMTC De-conflict**: Disables `mediaPlayer.CommandManager().IsEnabled(false)` to prevent blank OS media overlays and desynchronized hardware media keys.
* **Debug-Gated Tracing**: Verbose method call logging is gated behind `JAW_TRACE` under `#ifndef NDEBUG`, preventing console flood in production.
* **Thread-Safe Mutex Hardening**: Added `std::mutex players_mutex_` for the global player registry and `std::mutex sink_mutex_` for event sinks. Safely destroys players outside lock to eliminate `0xC0000005` Access Violations on player disposal, stop, and hot restart.
* **Full Drop-in Compatibility**: Fully implements the `just_audio` platform interface for Windows.

## 0.1.0

* Initial release of `just_audio_windows_plus`.
* **Thread-Safe EventSink**: Synchronized Flutter `EventSink` calls via `std::mutex` to prevent concurrent writes from background WinRT threadpool worker threads.
* **Atomic Player Lifecycle**: Introduced `std::atomic<bool> disposed_` and `std::recursive_mutex player_mutex_` to safely coordinate player destruction and prevent Use-After-Free Access Violations (`0xC0000005`).
* **Safe Plugin Registrar**: Protected the global player registry (`players_` vector) with a dedicated mutex during concurrent creations and disposals.
* **Defensive WinRT Property Probing**: Wrapped `NaturalDuration`, `BufferingProgress`, and `Position` access in exception handlers to gracefully handle transient states during playlist transitions.
* **Full Drop-in Compatibility**: Fully implements the `just_audio` platform interface for Windows.

