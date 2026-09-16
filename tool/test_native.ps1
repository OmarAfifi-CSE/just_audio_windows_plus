param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'example/build/windows/x64'
$cache = Join-Path $build 'CMakeCache.txt'
if (!(Test-Path -LiteralPath $cache)) { throw 'Build the Windows example first (tools/test_windows.ps1).' }
$entry = Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$'
if (!$entry) { throw 'Cannot locate the CMake executable in the build cache.' }
$cmake = $entry.Matches[0].Groups[1].Value
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
& $cmake -S (Join-Path $repo 'example/windows') -B $build -Dinclude_just_audio_windows_plus_tests=ON
if ($LASTEXITCODE -ne 0) { throw 'Native test configuration failed.' }
& $cmake --build $build --config $Configuration --target just_audio_windows_plus_test
if ($LASTEXITCODE -ne 0) { throw 'Native test compilation failed.' }
& $ctest --test-dir (Join-Path $build 'plugins/just_audio_windows_plus') -C $Configuration --output-on-failure --timeout 30 --no-tests=error
if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
