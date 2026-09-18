# Running the test suites

Three independent suites verify this package. Run all of them before a release.

## 1. Dart channel-contract tests (any OS)

From the repository root:

```powershell
flutter pub get
flutter analyze --fatal-infos
flutter test
```

These use mock method/event channels. They check Dart/channel behavior, not native decoding, WinRT events, or Windows resource lifetime.

The example's widget test runs separately:

```powershell
Push-Location example
flutter pub get
flutter test
Pop-Location
```

## 2. Real Windows playback regressions

Builds the example with `integration_test/native_audio_test.dart` as its entry point, runs 26 scenarios against the actual compiled plugin on real platform channels, and records machine-readable results.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tool/test_windows.ps1 -Mode debug
powershell -NoProfile -ExecutionPolicy Bypass -File tool/test_windows.ps1 -Mode release
```

The fixture is generated silent PCM WAV audio plus a loopback HTTP server, avoiding external media servers. The native playback entry point is a standalone Flutter application; run it through the Windows runner — ordinary `flutter test` does not exercise it against the Windows plugin.

A successful build alone is not a passing playback run. Inspect the runner exit status and its `results.json`; timeouts or missing results are failures. Test Debug and Release when changing native error handling or lifecycle code, because compiler configuration can affect those paths.

## 3. Native C++ helper tests

Exercises the production source-tree parser, URI encoding, and the real Win32 dispatcher through a message pump.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tool/test_native.ps1
```

(Configure the example build first with any `test_windows.ps1` run; the native tests reconfigure CMake with `include_just_audio_windows_plus_tests=ON` themselves.)

## Scope of evidence

The playback suite targets loading success/failure, duration, play completion semantics, playing intent, playlist sequencing, and lifecycle regressions using local files. Helper tests cover pure conversion, validation, and source-tree logic. Consult the test source for the exact assertions in a given revision.

These checks do not establish universal codec support, sample-accurate or gapless playback, network/adaptive-stream compatibility, independent Flutter-engine isolation, leak freedom, or UI frame-rate performance. Disposal stress can detect observed failures but does not replace a memory diagnostic tool. Exercise the network sources, deployment environment, and concurrent-engine configurations your application actually uses.
