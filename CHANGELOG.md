## 0.1.0

* Initial release of `just_audio_windows_plus`.
* **Thread-Safe EventSink**: Synchronized Flutter `EventSink` calls via `std::mutex` to prevent concurrent writes from background WinRT threadpool worker threads.
* **Atomic Player Lifecycle**: Introduced `std::atomic<bool> disposed_` and `std::recursive_mutex player_mutex_` to safely coordinate player destruction and prevent Use-After-Free Access Violations (`0xC0000005`).
* **Safe Plugin Registrar**: Protected the global player registry (`players_` vector) with a dedicated mutex during concurrent creations and disposals.
* **Defensive WinRT Property Probing**: Wrapped `NaturalDuration`, `BufferingProgress`, and `Position` access in exception handlers to gracefully handle transient states during playlist transitions.
* **Full Drop-in Compatibility**: Fully implements the `just_audio` platform interface for Windows.
