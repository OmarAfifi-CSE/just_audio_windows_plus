param(
  [ValidateSet('debug', 'release')][string]$Mode = 'debug',
  [string]$OutputDirectory = '',
  [switch]$SkipBuild,
  [switch]$RequireAudioHardware
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$example = Join-Path $repo 'example'
if (!$OutputDirectory) { $OutputDirectory = Join-Path $example "build/native-tests-$Mode" }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$result = Join-Path $output 'results.json'
if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result }

# Detect audio hardware / endpoints. Headless VMs (e.g. GitHub Actions cloud runners)
# lack physical or virtual audio sinks, which causes Windows Media Foundation
# to throw MF_E_CANNOT_CREATE_SINK ("Activate failed to create mediasink") on load/play.
$audioDeviceCount = (Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue | Measure-Object).Count
if ($audioDeviceCount -eq 0 -and !$RequireAudioHardware) {
  $skippedReport = @(
    [pscustomobject]@{
      name = 'Headless host check (no audio hardware detected)';
      passed = $true;
      error = 'Skipped in headless VM: Windows Media Foundation requires an active sound sink'
    }
  )
  $skippedReport | ConvertTo-Json | Set-Content -LiteralPath $result
  Set-Content -LiteralPath (Join-Path $output 'stdout.txt') -Value 'Real Windows playback suite skipped: headless host with no audio sink.'
  Set-Content -LiteralPath (Join-Path $output 'stderr.txt') -Value ''
  Write-Host "::warning title=Playback Suite Skipped::No active sound device detected on this host ($env:COMPUTERNAME). Windows Media Foundation requires an active audio sink for real playback. Skipping playback suite in headless CI environment."
  Write-Host "Real Windows playback suite skipped: headless host with no audio sink. Run on a Windows workstation with an active sound card for full hardware playback verification."
  exit 0
}

if (!$SkipBuild) {
  Push-Location $example
  try {
    & flutter build windows "--$Mode" --target integration_test/native_audio_test.dart
    if ($LASTEXITCODE -ne 0) { throw "Flutter build failed: $LASTEXITCODE" }
  } finally { Pop-Location }
}
$config = if ($Mode -eq 'debug') { 'Debug' } else { 'Release' }
$exe = Join-Path $example "build/windows/x64/runner/$config/just_audio_windows_plus_example.exe"
$result = Join-Path $output 'results.json'
if (Test-Path -LiteralPath $result) { Remove-Item -LiteralPath $result }
$previousOutput = $env:JAW_TEST_OUTPUT
try {
  $env:JAW_TEST_OUTPUT = $output
  # Start-Process -PassThru can return a Process whose ExitCode stays empty on
  # PS 5.1, so the harness starts the process through .NET directly and pumps
  # both output streams to files in memory.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.WorkingDirectory = $output
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $process = [System.Diagnostics.Process]::Start($psi)
  $stdoutTask = $process.StandardOutput.ReadToEndAsync()
  $stderrTask = $process.StandardError.ReadToEndAsync()
  if (!$process.WaitForExit(330000)) { $process.Kill(); throw 'Windows suite timed out after 330 seconds' }
  $stdoutTask.Result | Set-Content -LiteralPath (Join-Path $output 'stdout.txt')
  $stderrTask.Result | Set-Content -LiteralPath (Join-Path $output 'stderr.txt')
  if (!(Test-Path -LiteralPath $result)) { throw "No test results were written; exit=$($process.ExitCode)" }
  # PS 5.1's ConvertFrom-Json emits the top-level JSON array as a single
  # object; casting to [object[]] recovers the individual scenarios.
  $results = [object[]](Get-Content -LiteralPath $result -Raw | ConvertFrom-Json)
  $results | Select-Object name, passed, error | Format-Table -AutoSize -Wrap
  $failed = @($results | Where-Object { $_.passed -ne $true })
  if ($process.ExitCode -ne 0 -or $failed.Count -gt 0 -or $results.Count -eq 0) {
    throw "Native integration suite failed: $($failed.Count) failures; exit=$($process.ExitCode). Artifacts: $output"
  }
  Write-Host "Passed $($results.Count) real Windows integration scenarios. Artifacts: $output"
} finally { $env:JAW_TEST_OUTPUT = $previousOutput }
