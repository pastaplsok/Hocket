<#
.SYNOPSIS
Step 8's check (Lee, 2026-11-18): Ctrl+C on a real --play while a drum kit is
still loading stops it cleanly.

.DESCRIPTION
Plays a copy of examples\electronic-kit with every clip emptied, so it makes no
sound, at a sample rate other than the kit's 44.1 kHz (48 kHz unless
-SampleRate says otherwise), so the kit takes a moment to convert as it loads.
A Ctrl+C sender, attached to --play's console before --play starts, sends
Ctrl+C the moment --play prints "Loading kit".

  PASS          --play says it stopped while the kits were loading, exits with 0,
                and leaves no token file.
  INCONCLUSIVE  the kit had loaded before the Ctrl+C landed: try a higher
                -SampleRate, which gives it more to convert.
  FAIL          anything else, with --play's log.

It opens the audio device, silently: run it at the desk. Run it from a
PowerShell window, after building Release:
  powershell -ExecutionPolicy Bypass -File tools\acceptance\Check-CtrlCWhileLoading.ps1 [-Driver ASIO -Device "FL Studio ASIO"] [-SampleRate 48000] [-BufferSize 1024]
#>
param(
    [string] $Driver,
    [string] $Device,
    [int] $SampleRate = 48000,
    [int] $BufferSize
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$headless = Join-Path $repo 'build\vs2026\src\app\hocket_headless_artefacts\Release\hocket-headless.exe'
$tokenFile = Join-Path $env:LOCALAPPDATA 'Hocket\api.json'

if (-not (Test-Path $headless)) { throw "build Release first: $headless isn't there" }
if (Test-Path $tokenFile) { throw "a Hocket may be running (see $tokenFile); stop it first" }

$work = Join-Path ([IO.Path]::GetTempPath()) ("hocket-ctrlc-" + [IO.Path]::GetRandomFileName().Replace('.', ''))
New-Item -ItemType Directory -Path $work | Out-Null
Write-Host "Logs and the silent project are in $work"

# The audition project with every clip emptied: each keeps only its @length.
$project = Join-Path $work 'electronic-kit'
Copy-Item -Recurse (Join-Path $repo 'examples\electronic-kit') $project
$utf8 = New-Object System.Text.UTF8Encoding($false)

foreach ($clip in Get-ChildItem (Join-Path $project 'clips') -Filter '*.notes') {
    $length = (Get-Content $clip.FullName | Where-Object { $_ -match '^@length' } | Select-Object -First 1)
    [IO.File]::WriteAllText($clip.FullName, "$length`n", $utf8)
}

$playLog = Join-Path $work 'play.log'
$readyFile = Join-Path $work 'sender-ready.txt'
$playArguments = "--play `"$project`" --sample-rate $SampleRate"
if ($BufferSize) { $playArguments += " --buffer-size $BufferSize" }
if ($Driver) { $playArguments += " --driver `"$Driver`"" }
if ($Device) { $playArguments += " --device `"$Device`"" }

# Through cmd, with a console of its own and no window, so Ctrl+C can reach it.
# ping waits about 4 s first, so the sender is attached and watching before
# --play starts.
$command = "/c `"ping -n 5 127.0.0.1 > nul & `"$headless`" $playArguments > `"$playLog`" 2>&1`""
$info = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', $command
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$play = [Diagnostics.Process]::Start($info)

$sender = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Send-CtrlC.ps1'),
                                                  $play.Id, '-Log', $playLog, '-WhenLogSays', '^Loading kit', '-ReadyFile', $readyFile, '-TimeoutSeconds', 60 `
                                   -WindowStyle Hidden -PassThru
$null = $sender.Handle # without it, Windows PowerShell can't read its exit code later

$sender.WaitForExit()
$senderReadyFirst = (Test-Path $readyFile) -and ((-not (Test-Path $playLog)) -or ((Get-Item $readyFile).LastWriteTime -lt (Get-Item $playLog).CreationTime))

if (-not $play.WaitForExit(30000)) {
    # It didn't stop: stop it the plain way, so the device is let go.
    Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Send-CtrlC.ps1'), $play.Id `
                  -WindowStyle Hidden -Wait | Out-Null
    if (-not $play.WaitForExit(15000)) { $play.Kill() }
    $play.WaitForExit()
    $result = 'FAIL'
    $detail = "--play didn't stop within 30 s of the Ctrl+C (the sender's exit code was $($sender.ExitCode))"
}
else {
    $log = if (Test-Path $playLog) { Get-Content $playLog -Raw } else { '' }
    $stoppedLoading = $log -match 'stopped before playing, while the kits were loading'
    $loaded = $log -match '(?m)^Loaded in'
    $clean = ($play.ExitCode -eq 0) -and -not (Test-Path $tokenFile)

    if ($sender.ExitCode -ne 0) {
        $result = 'FAIL'
        $detail = "the Ctrl+C sender failed (exit code $($sender.ExitCode); 4 means --play never said it was loading a kit)"
    }
    elseif ($stoppedLoading -and $clean) {
        $result = 'PASS'
        $detail = "stopped while the kit was loading; exit code 0; no token file left"
    }
    elseif ($loaded -and $clean) {
        $result = 'INCONCLUSIVE'
        $detail = "the kit had loaded before the Ctrl+C landed, so this run proves nothing (it did stop cleanly); try -SampleRate 96000"
    }
    else {
        $result = 'FAIL'
        $detail = "exit code $($play.ExitCode); token file left: $(Test-Path $tokenFile)"
    }
}

Write-Host ''
Write-Host "--play's log:"
if (Test-Path $playLog) { Get-Content $playLog | ForEach-Object { Write-Host "  $_" } }
if (-not $senderReadyFirst) { Write-Host "Note: the sender may not have been watching before --play started, so its Ctrl+C may have come late." }
Write-Host ''
Write-Host "[$result] Ctrl+C while a kit loads: $detail"
exit $(if ($result -eq 'PASS') { 0 } elseif ($result -eq 'INCONCLUSIVE') { 2 } else { 1 })
