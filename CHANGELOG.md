## 0.5.0

* **Reliable Source Loading & Error Reporting**: Pending loads now complete with the real duration or an actionable failure, including replacement and disposal; asynchronous playback failures surface through the playback event contract, so missing files and HTTP 404s no longer leave callers waiting.
* **`play()` Completion Semantics**: `await play()` now resolves when playback pauses or finishes instead of returning immediately, and `playing` stays true across buffering and completion until `pause()`, matching `just_audio`'s contract.
* **Teardown & Ownership Hardening**: Native WinRT callbacks capture weak owners and post to the platform thread, and player ownership is scoped to the plugin registration; disposal settles pending loads and play futures exactly once.
* **Loop, Nested Sources & Shuffle Correctness**: `LoopMode.all` configured before loading now advances through every item; nested concatenating/looping/clipping source trees load and mutate correctly; shuffle orders supplied with loads and mutations are honored in automatic progression.
* **Native Error Integrity & Unsupported Operations**: Exception messages are owned so text survives to Dart unchanged; pitch and silence skipping return errors for unsupported values instead of silently succeeding, and direct custom request headers fail clearly with guidance toward `just_audio`'s proxy.
* **Empty-Playlist Mutations Attach the Source**: Inserting into a playlist that was loaded empty now attaches the playback list to the player, so the first added item actually plays instead of staying silent.
* **Truthful Index Broadcasting**: While the initial load is pending, the requested initial index is reported instead of a transient native default, and an unknown current index is reported as null rather than a spurious `0`, keeping Dart's sequence state synchronized from the first event.
* **Windows Playback Regression Runner**: Added `tool/test_windows.ps1` executing 25 playback scenarios against the compiled Windows plugin on real channels (Debug and Release), plus native C++ helper tests and CI execution.
* **Honest Documentation**: Documented codec/deployment requirements, unsupported operations, and verification limits; removed unverified crash-free, gapless, and frame-rate claims.

## 0.4.0

* **Linear Permutation Shuffle Engine**: Replaced quadratic index-shifting with an $O(N)$ permutation mapping (`ReorderByShuffleOrder`). Validates bounds, enforces element uniqueness, and rejects malformed shuffle arrays without corrupting playlist state.
* **Buffering Progress Clamping & NaN Defense**: Introduced `ClampBufferedPosition` with `std::isfinite` validation. Safely bounds buffer calculations within `[0, duration]`, eliminating C++ undefined behavior and Dart assertion failures caused by transient `NaN` or `>1.0` progress values during live streaming.
* **Atomic Concurrency Synchronization**: Upgraded `source_set_`, `loop_mode_`, and `shuffle_mode_` to `std::atomic` in `player.hpp`, eliminating data races between background WinRT threadpool callbacks and Flutter UI thread method calls.
* **Negative Seek Protection**: Enforced `std::max<int64_t>(0, microseconds)` in `seekToPosition` to prevent native WinRT `E_INVALIDARG` (0x80070057) exceptions.
* **Observable Native Error Logging & Brand Modernization**: Modernized all native diagnostic logs and error reporting macros to `[just_audio_windows_plus]`, replacing silent empty `catch (...) {}` blocks across primary playback and playlist operations (`play`, `pause`, `stop`, `setVolume`, `setSpeed`, `setLoopMode`, `setShuffleMode`, `setShuffleOrder`, `seekToItem`, `seekToPosition`) with `JAW_ERROR` macros logging to `std::cerr` for clear diagnostic visibility without crashing.
* **Method Arguments Null Safety**: Added defensive null-check guards across all parameterized method calls (`load`, `seek`, `setVolume`, `setSpeed`, `setLoopMode`, `setShuffleMode`, `setShuffleOrder`, `concatenatingInsertAll`, `concatenatingRemoveRange`, `concatenatingMove`), returning descriptive channel errors instead of crashing from native null pointer dereferences.
* **Source State Transition Integrity**: Deferred `source_set_` state settlement until after source loading and initial seek settle successfully, with guaranteed rollback to `false` on any failure or disposal to prevent the player from getting stuck in `loading` (state 1).
* **Thread-Safe Dispatcher Teardown & Worker Thread Isolation**: Synchronized window teardown and queue clearing under mutex in `~PlatformThreadDispatcher()`. Hardened `OnPlatformThread` to safely drop and trace events if the dispatcher is unavailable, strictly preventing background WinRT worker threads from invoking Flutter binary messenger or event sinks off the UI platform thread.
* **Unified Sovereign C++ Architecture**: Renamed internal implementation to `windows/just_audio_windows_plus_plugin.cpp` and class to `JustAudioWindowsPlusPlugin`. Cleaned legacy exports to export exclusively `JustAudioWindowsPlusPluginRegisterWithRegistrar`.
* **Native C++ CI Quality Gate**: Added automated Windows debug build (`flutter build windows --debug`) in GitHub Actions `ci.yml` to continuously verify MSVC compilation and C++ linkage.
* **Native Unit Test Suite**: Added GoogleTest test cases in `uri_utils_test.cpp` covering shuffle permutations, duplicate rejection, and buffer clamping edge cases.

## 0.3.0

* **64-bit Timestamp Safety**: Introduced `TryGetInt64` across all seeking, duration, and playlist mutations. StandardMessageCodec encodes timestamps exceeding 35.7 minutes ($2^{31}-1$ microseconds) as `int64_t`, which previously failed silently under 32-bit `std::get_if<int>`.
* **Exact Microsecond Precision**: Replaced lossy truncated millisecond arithmetic with exact 64-bit microsecond conversions (`TimeSpanToMicroseconds`), preserving sub-millisecond timeline accuracy.
* **Event Channel Teardown Hygiene**: Explicitly unregistered binary message handlers in `JustAudioEventSink` destructor to eliminate dangling callbacks and post-disposal access violations (`0xC0000005`).
* **Defensive Playlist Mutations & Bounds**: Fully bounds-checked `concatenatingMove`, `concatenatingInsertAll`, and `concatenatingRemoveRange` to eliminate native WinRT `E_BOUNDS` exceptions during rapid playlist modifications.
* **Accurate Looping & Shuffling**: Ensured `LoopMode.all` loops single tracks seamlessly and synchronized internal `loopMode` and `shuffleMode` state broadcasts back to Flutter.
* **Native MediaEnded Event**: Subscribed to WinRT `MediaPlayer.MediaEnded` for instantaneous `completed` state signaling upon track finish.
* **Clipping Audio Source Hardening**: Enforced non-negative start offsets and validated duration boundaries for `ClippingAudioSource`.
* **Clean Codebase Hygiene**: Removed obsolete legacy files (`url_code.hpp`) and unused methods (`GetPlayerByPlayerId`).
* **Pub.dev Package Showcase**: Added official package screenshots metadata in `pubspec.yaml`.

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

