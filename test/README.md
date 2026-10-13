# Running the test suites

Three independent suites verify this package. Run all of them before a release.

## 1. Dart channel-contract tests (100 tests, any OS)

From the repository root:

```powershell
flutter pub get
flutter analyze --fatal-infos
flutter test
```

These run against `test/helpers/mock_audio_engine.dart`, a fake that mirrors
the native engine: the same validation, the same bounds, and the same error
codes as `windows/player.hpp` and `windows/source_model.hpp`. That is what makes
them worth running - a test written against this fake is a statement about the
plugin's actual channel contract, so the native side and the fake must be
changed together.

Two things the fake deliberately does **not** model, because they need a real
media pipeline: it resolves `load` synchronously rather than tracking a load
that is replaced or aborted while still pending, and it keeps a single
outstanding `play()` future rather than the native `pending_play_` queue. Both
are covered against the real plugin by suite 2.

The fake does model the part of `Broadcast()` that decides *what* is published:
the state map is a sample and always goes out, while the data map is intent and
is published only when a value changes. If the native dedupe or the play-settle
rule changes, the Dart suite is where that divergence shows up.

The example's widget test runs separately:

```powershell
Push-Location example
flutter pub get
flutter test
Pop-Location
```

## 2. Real Windows playback regressions (34 scenarios)

Builds the example with `integration_test/native_audio_test.dart` as its entry point, runs the scenarios against the actual compiled plugin on real platform channels, and records machine-readable results.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tool/test_windows.ps1 -Mode debug
powershell -NoProfile -ExecutionPolicy Bypass -File tool/test_windows.ps1 -Mode release
```

The fixture is generated silent PCM WAV audio plus a loopback HTTP server, avoiding external media servers. The native playback entry point is a standalone Flutter application; run it through the Windows runner — ordinary `flutter test` does not exercise it against the Windows plugin.

A successful build alone is not a passing playback run. Inspect the runner exit status and its `results.json`; timeouts or missing results are failures. **Run both Debug and Release whenever you touch native event delivery, error handling, or lifecycle code.** Compiler configuration genuinely changes behaviour here: a defect that made a Release build resolve `await player.play()` without starting the media was invisible in Debug, because the slower build let the first event-channel snapshot reach Dart before the play request was dispatched. Re-run Release more than once when the change is in delivery timing; `tool/test_windows.ps1 -SkipBuild` makes a repeat run cheap.

## 3. Native C++ helper tests (40 tests)

Exercises the production source-tree parser, URI encoding, and the real Win32 dispatcher through a message pump. These include `source_model.hpp` itself, so they are the fastest way to check a parsing or bounds change.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tool/test_native.ps1
```

(Configure the example build first with any `test_windows.ps1` run; the native tests reconfigure CMake with `include_just_audio_windows_plus_tests=ON` themselves.)

## Scope of evidence

The playback suite targets loading success/failure, duration, play completion semantics, playing intent, playlist sequencing, shuffle-on and shuffle-off ordering, mode changes during playback, and lifecycle regressions using local files. Helper tests cover pure conversion, validation, source-tree logic, and the dispatcher's containment of a throwing task. Consult the test source for the exact assertions in a given revision.

These checks do not establish universal codec support, sample-accurate or gapless playback, network/adaptive-stream compatibility, independent Flutter-engine isolation, leak freedom, or UI frame-rate performance. Disposal stress can detect observed failures but does not replace a memory diagnostic tool. Exercise the network sources, deployment environment, and concurrent-engine configurations your application actually uses.
