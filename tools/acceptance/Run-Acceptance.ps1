<#
.SYNOPSIS
Milestone 0's acceptance checks (SPEC §10.0) that don't need ears, run silently.

.DESCRIPTION
Plays a copy of examples\m0-demo whose clip is empty, and only ever writes empty clips,
so it makes no sound. What it checks:
  - with -FreshClone: a fresh clone builds with CLAUDE.md's commands, and its tests pass;
  - the tests pass, and the engine stress test's numbers (xruns, dropped blocks);
  - hocket-headless --play opens the device and loops;
  - hocket-mcp, started as Claude Code starts it: the tools are listed, write_clip and undo
    work, history names each author (Claude, and Lee through --watch), and Claude's undo
    refuses Lee's change;
  - the recovery copy: written a few seconds after Claude's change, removed by the undo
    back to the saved state, and none left when --play ends with nothing unsaved;
  - after --play is killed outright with a change unsaved: plain --play stops and asks,
    without opening the device; --recover plays the change as one entry, which undo takes
    back; and, after a second change and kill, --discard-recovery files the copy as a kept
    save (which --check reads) and plays the files as saved;
  - --play's keys, typed into its console: S saves; S won't save over a clip file edited by
    hand, and offers O, which does, keeping the edit as a kept save first;
  - hocket-stress: 200 rewrites at 20 a second during playback, all applied, the clip put back;
  - Ctrl+C stops --play cleanly (exit code 0, its "stopped" line, the token file removed), and
    the driver's xrun count, where it reports one, is 0. JUCE's count of late audio callbacks is
    a note, and so is the peak output level, which should be silence. AcceptanceChecks.ps1 makes
    these results, and tests/acceptance tests it.
Hearing the changes, and hearing no clicks or gaps, is the part left for a person.

Each run plays a project in a new folder, so a recovery copy an earlier run left (one that
crashed with changes unsaved) can never stop its --play: --play only asks about a copy of
the folder it plays. Copies of earlier runs' projects are cleared away before it starts.

Run it from a PowerShell window, after building Release:
  powershell -ExecutionPolicy Bypass -File tools\acceptance\Run-Acceptance.ps1 [-Driver ASIO -Device "ASIO4ALL v2"]

.PARAMETER Driver
Passed to --play, e.g. ASIO. Without it, --play uses the device it used last.

.PARAMETER Device
Passed to --play, e.g. "ASIO4ALL v2".

.PARAMETER FreshClone
Also clone the repository into a temporary folder, from GitHub (or -CloneFrom), and build and
test it with CLAUDE.md's commands. Takes several minutes and downloads the dependencies.

.PARAMETER CloneFrom
What -FreshClone clones. Default: this repository's origin.

.PARAMETER SkipTests
Don't run the tests here (when they've just been run).
#>
param(
    [string] $Driver,
    [string] $Device,
    [switch] $FreshClone,
    [string] $CloneFrom,
    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $repo 'build\vs2026'
$headless = Join-Path $build 'src\app\hocket_headless_artefacts\Release\hocket-headless.exe'
$mcp = Join-Path $build 'src\mcp\Release\hocket-mcp.exe'
$stress = Join-Path $build 'tools\stress\Release\hocket-stress.exe'
$engineTests = Join-Path $build 'tests\engine\Release\hocket_engine_tests.exe'
$tokenFile = Join-Path $env:LOCALAPPDATA 'Hocket\api.json'
$recoveryFolder = Join-Path $env:LOCALAPPDATA 'Hocket\Recovery'
$results = New-Object System.Collections.Generic.List[object]

. (Join-Path $PSScriptRoot 'AcceptanceChecks.ps1')

function Write-Results ($NewResults) {
    foreach ($result in @($NewResults)) {
        $results.Add($result)
        Write-Host "[$($result.Result)] $($result.Check)$(if ($result.Detail) { ": $($result.Detail)" })"
    }
}

function Add-Result ([string] $Check, [bool] $Passed, [string] $Detail) {
    Write-Results (New-Check $Check $Passed $Detail)
}

function Add-Note ([string] $Check, [string] $Detail) {
    Write-Results (New-Result $Check 'NOTE' $Detail)
}

function Find-CMakeTool ([string] $Name) {
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $inVisualStudio = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\$Name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($inVisualStudio) { return $inVisualStudio.FullName }

    throw "can't find ${Name}: open the Developer PowerShell for VS, or put CMake's bin folder on PATH"
}

# Runs a program with its output in a log, through cmd: Windows PowerShell would turn
# its stderr into errors, and write the log as UTF-16. Returns the exit code.
function Invoke-Logged ([string] $Program, [string[]] $Arguments, [string] $Log, [string] $Folder = $repo) {
    $line = (@($Program) + $Arguments | ForEach-Object { "`"$_`"" }) -join ' '
    $info = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', "/c `"$line > `"$Log`" 2>&1`""
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $Folder
    $process = [Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    return $process.ExitCode
}

# The recovery copies (SPEC §4.5) of projects whose folder is -like $Like, by the project
# each copy's .hocket\recovery.json names: Place (its project's place in the recovery
# folder), Copy (the copy's folder) and Project.
function Get-RecoveryCopies ([string] $Like) {
    if (-not (Test-Path $recoveryFolder)) { return }

    foreach ($place in Get-ChildItem -LiteralPath $recoveryFolder -Directory) {
        foreach ($copy in Get-ChildItem -LiteralPath $place.FullName -Directory | Where-Object { $_.Name -match '^\d+$' }) {
            $info = Join-Path $copy.FullName '.hocket\recovery.json'
            if (-not (Test-Path -LiteralPath $info)) { continue }
            try { $project = ([IO.File]::ReadAllText($info) | ConvertFrom-Json).project } catch { continue }
            if ($project -like $Like) { [pscustomobject] @{ Place = $place.FullName; Copy = $copy.FullName; Project = $project } }
        }
    }
}

function Test-HocketRunning {
    if (-not (Test-Path $tokenFile)) { return $false }
    try {
        $info = Get-Content $tokenFile -Raw | ConvertFrom-Json
        return $null -ne (Get-Process -Id $info.pid -ErrorAction SilentlyContinue)
    }
    catch { return $false }
}

# --play with these arguments, through cmd, with a console of its own and no window, so
# Ctrl+C can reach it, and its output in $Log. Returns the cmd.exe, whose exit code is --play's.
function Start-Play ([string] $Arguments, [string] $Log) {
    $info = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', "/c `"`"$headless`" $Arguments > `"$Log`" 2>&1`""
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    return [Diagnostics.Process]::Start($info)
}

# Whether --play reached its loop, serving the API, within 30 s.
function Wait-Looping ($Process, [string] $Log) {
    $deadline = (Get-Date).AddSeconds(30)

    while (-not $Process.HasExited -and (Get-Date) -lt $deadline) {
        if ((Test-Path $Log) -and (Select-String -Path $Log -Pattern 'on a loop' -Quiet) -and (Test-Path $tokenFile)) { return $true }
        Start-Sleep -Milliseconds 200
    }

    return $false
}

# Ends the hocket-headless that $Process (the cmd.exe running it) runs, at once, as a crash
# would: no Ctrl+C, and nothing of its own cleaned up. Returns its process ID, or $null if
# none was running.
function Stop-PlayOutright ($Process) {
    $running = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($Process.Id)" | Where-Object { $_.Name -eq 'hocket-headless.exe' } | Select-Object -First 1
    if (-not $running) { return $null }

    Stop-Process -Id $running.ProcessId -Force
    $Process.WaitForExit(10000) | Out-Null
    return $running.ProcessId
}

# Stops --play with Ctrl+C, as a person would, and waits for it: 'ctrl-c'; 'killed' if it
# hadn't stopped 15 s later; or 'already-exited' if it had stopped before.
function Stop-Play ($Process) {
    if ($Process.HasExited) { return 'already-exited' }

    $ctrlC = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Send-CtrlC.ps1'), $Process.Id `
                                       -WindowStyle Hidden -Wait -PassThru

    if ($Process.WaitForExit(15000)) {
        $Process.WaitForExit()
        return 'ctrl-c'
    }

    Write-Host "Send-CtrlC's exit code was $($ctrlC.ExitCode)"
    Stop-PlayOutright $Process | Out-Null
    if (-not $Process.HasExited) { $Process.Kill() }
    $Process.WaitForExit()
    return 'killed'
}

# hocket-mcp, started in the project folder as Claude Code starts it in its session's
# folder, and initialized. Returns the reply to initialize.
function Start-Bridge {
    $info = New-Object Diagnostics.ProcessStartInfo $mcp
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $project
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8
    $script:bridge = [Diagnostics.Process]::Start($info)
    $script:bridgeErrors = $script:bridge.StandardError.ReadToEndAsync()
    $script:stdin = New-Object IO.StreamWriter($script:bridge.StandardInput.BaseStream, $utf8)
    $script:stdin.NewLine = "`n"
    $script:requestId = 0
    return Send-Request 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'claude-code'; version = 'acceptance' } }
}

# Closes hocket-mcp's stdin, as Claude Code does, keeping what it wrote to stderr in
# $ErrorLog. Whether it then exited by itself, with 0.
function Stop-Bridge ([string] $ErrorLog) {
    $script:stdin.Close()
    $exited = $script:bridge.WaitForExit(10000)
    if (-not $exited) { $script:bridge.Kill() }
    [IO.File]::WriteAllText($ErrorLog, $script:bridgeErrors.Result, $utf8)
    return $exited -and $script:bridge.ExitCode -eq 0
}

function Send-Request ([string] $Method, $Params) {
    $script:requestId++
    $stdin.WriteLine((@{ jsonrpc = '2.0'; id = $script:requestId; method = $Method; params = $Params } | ConvertTo-Json -Depth 10 -Compress))
    $stdin.Flush()
    return $bridge.StandardOutput.ReadLine() | ConvertFrom-Json
}

function Invoke-Tool ([string] $Tool, $Arguments = @{}) {
    $reply = Send-Request 'tools/call' @{ name = $Tool; arguments = $Arguments }
    return [pscustomobject] @{
        IsError = [bool] $reply.result.isError
        Text = (@($reply.result.content | ForEach-Object { $_.text }) -join "`n")
        Blocks = @($reply.result.content | ForEach-Object { $_.text })
    }
}

# This run's recovery copy, once it's there, within 8 s: its Copy folder, and Seconds
# waited. $null if none came.
function Wait-RecoveryCopy {
    $waited = [Diagnostics.Stopwatch]::StartNew()

    while ($waited.Elapsed.TotalSeconds -lt 8) {
        $copies = @(Get-RecoveryCopies $ourCopies)
        if ($copies.Count -gt 0) { return [pscustomobject] @{ Copy = $copies[0].Copy; Seconds = [math]::Round($waited.Elapsed.TotalSeconds, 1) } }
        Start-Sleep -Milliseconds 100
    }

    return $null
}

# Types keys into --play's console, as if pressed there. Send-Keys.ps1's exit code: 0 when typed.
function Send-Keys ($Process, [string] $Keys) {
    $typed = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Send-Keys.ps1'), $Process.Id, $Keys `
                                       -WindowStyle Hidden -Wait -PassThru
    return $typed.ExitCode
}

# How many lines of $Log match $Pattern.
function Measure-LogLines ([string] $Log, [string] $Pattern) {
    if (-not (Test-Path $Log)) { return 0 }
    return @(Select-String -Path $Log -Pattern $Pattern).Count
}

# The first line of $Log matching $Pattern after the $Before already there, within 5 s; $null if none came.
function Wait-NewLogLine ([string] $Log, [string] $Pattern, [int] $Before) {
    $deadline = (Get-Date).AddSeconds(5)

    while ((Get-Date) -lt $deadline) {
        $lines = @(Select-String -Path $Log -Pattern $Pattern -ErrorAction SilentlyContinue)
        if ($lines.Count -gt $Before) { return $lines[$Before].Line }
        Start-Sleep -Milliseconds 100
    }

    return $null
}

# Seconds until this run's recovery copies are gone, within 3 s; $null if they're still there.
function Wait-RecoveryCopyGone {
    $waited = [Diagnostics.Stopwatch]::StartNew()

    while ($waited.Elapsed.TotalSeconds -lt 3) {
        if (@(Get-RecoveryCopies $ourCopies).Count -eq 0) { return [math]::Round($waited.Elapsed.TotalSeconds, 1) }
        Start-Sleep -Milliseconds 50
    }

    return $null
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("hocket-acceptance-" + [IO.Path]::GetRandomFileName().Replace('.', ''))
New-Item -ItemType Directory -Path $work | Out-Null
Write-Host "Logs and the silent project are in $work"

#==============================================================================
if ($FreshClone) {
    $from = if ($CloneFrom) { $CloneFrom } else { (& git -C $repo remote get-url origin) }
    $clone = Join-Path ([IO.Path]::GetTempPath()) ("hocket-fresh-" + [IO.Path]::GetRandomFileName().Replace('.', ''))
    $cmake = Find-CMakeTool 'cmake'
    $ctest = Find-CMakeTool 'ctest'
    $started = Get-Date
    Write-Host "Fresh clone of $from into $clone (several minutes)..."

    $steps = @(
        @{ Name = 'git clone'; Program = 'git'; Arguments = @('clone', $from, $clone); Folder = $repo },
        @{ Name = 'git submodule update --init'; Program = 'git'; Arguments = @('submodule', 'update', '--init'); Folder = $clone },
        @{ Name = 'cmake --preset vs2026'; Program = $cmake; Arguments = @('--preset', 'vs2026'); Folder = $clone },
        @{ Name = 'cmake --build --preset vs2026-release'; Program = $cmake; Arguments = @('--build', '--preset', 'vs2026-release'); Folder = $clone },
        @{ Name = 'ctest --preset vs2026-release'; Program = $ctest; Arguments = @('--preset', 'vs2026-release'); Folder = $clone }
    )

    $failed = $null
    $n = 0

    foreach ($step in $steps) {
        $n++
        $log = Join-Path $work "fresh-clone-$n.log"
        if ((Invoke-Logged $step.Program $step.Arguments $log $step.Folder) -ne 0) { $failed = "$($step.Name) failed; see $log"; break }
    }

    $minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)
    Add-Result 'A fresh clone builds and its tests pass' ($null -eq $failed) $(if ($failed) { $failed } else { "$minutes min, from $from" })

    if (-not $failed) { Remove-Item -Recurse -Force $clone -ErrorAction SilentlyContinue }
}

#==============================================================================
foreach ($program in @($headless, $mcp, $stress, $engineTests)) {
    if (-not (Test-Path $program)) { throw "$program is missing: build Release first (cmake --build --preset vs2026-release)" }
}

if (-not $SkipTests) {
    $ctest = Find-CMakeTool 'ctest'
    $log = Join-Path $work 'ctest.log'
    $exit = Invoke-Logged $ctest @('--preset', 'vs2026-release', '-E', 'hocket_engine_tests') $log
    $summary = (Select-String -Path $log -Pattern 'tests passed' | Select-Object -Last 1).Line
    Add-Result 'The tests pass (all but the engine tests)' ($exit -eq 0) "$summary; see $log"

    # Run directly, so the stress test's messages show even when it passes.
    $log = Join-Path $work 'engine-tests.log'
    $exit = Invoke-Logged $engineTests @() $log
    $cases = (Select-String -Path $log -Pattern 'test cases:' | Select-Object -Last 1).Line
    Add-Result 'The engine tests pass, the stress tests among them' ($exit -eq 0) "$($cases -replace '\[doctest\]\s*', ''); see $log"

    foreach ($line in (Select-String -Path $log -Pattern 'MESSAGE: (during the rewrites|moving notes|dropped blocks)')) {
        Add-Note 'Engine stress test' ($line.Line -replace '^.*MESSAGE: ', '')
    }
}

#==============================================================================
if (Test-HocketRunning) { throw "a Hocket is already running (see $tokenFile); stop it first" }

# Recovery copies of this script's projects from earlier runs, which crashed with changes
# unsaved: no Hocket is running, and this run's project is new, so none is in use.
$stale = @(Get-RecoveryCopies '*\hocket-acceptance-*\m0-acceptance' | Select-Object -ExpandProperty Place -Unique)
foreach ($place in $stale) { Remove-Item -Recurse -Force -LiteralPath $place }
if ($stale.Count -gt 0) { Add-Note 'Recovery copies' "removed $($stale.Count) left by earlier runs that crashed" }
$ourCopies = "*\$(Split-Path $work -Leaf)\m0-acceptance"

# The demo with an empty clip, plus a drums track playing the bundled electronic
# kit with an empty clip too: it plays silence, and every run loads a kit.
$project = Join-Path $work 'm0-acceptance'
Copy-Item -Recurse (Join-Path $repo 'examples\m0-demo') $project
$clipFile = Join-Path $project 'clips\bass.notes'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($clipFile, "@length 2bars`n", $utf8)
[IO.File]::WriteAllText((Join-Path $project 'clips\beat.notes'), "@length 2bars`n", $utf8)

$drumsTrack = @'
    },
    {
      "id": "trk-drums",
      "name": "Drums",
      "type": "drums",
      "volume": "0dB",
      "pan": "C",
      "mute": false,
      "solo": false,
      "devices": [
        {
          "id": "dev-drums-kit",
          "type": "hocket:drum-sampler",
          "kit": "electronic"
        }
      ],
      "drumMap": {},
      "clips": [
        {
          "id": "clp-beat",
          "file": "clips/beat.notes",
          "start": "1.1"
        }
      ]
    }
  ]
}
'@
$projectFile = Join-Path $project 'project.json'
$projectText = [IO.File]::ReadAllText($projectFile).Replace("`r`n", "`n")
$ending = "    }`n  ]`n}`n"
if (-not $projectText.EndsWith($ending)) { throw "examples\m0-demo\project.json doesn't end the way this script expects" }
$projectText = $projectText.Substring(0, $projectText.Length - $ending.Length) + $drumsTrack.Replace("`r`n", "`n") + "`n"
[IO.File]::WriteAllText($projectFile, $projectText, $utf8)

$deviceArguments = ''
if ($Driver) { $deviceArguments += " --driver `"$Driver`"" }
if ($Device) { $deviceArguments += " --device `"$Device`"" }

$playLog = Join-Path $work 'play.log'
$play = Start-Play "--play `"$project`" --watch --user Lee$deviceArguments" $playLog
$looping = Wait-Looping $play $playLog

$device = if (Test-Path $playLog) { (Select-String -Path $playLog -Pattern '^Playing on' | Select-Object -First 1).Line } else { '' }
Add-Result '--play opens the device and loops' $looping $(if ($looping) { $device } else { "see $playLog" })

if ($looping) {
    $kitLines = @(Select-String -Path $playLog -Pattern '^(Loading kit|Loaded in)' | ForEach-Object { $_.Line })
    Add-Result '--play loads the bundled kit before playing' ($kitLines.Count -eq 2) ($kitLines -join '; ')
}

$bridge = $null
try {
    if (-not $looping) { throw "--play didn't start" }

    #--------------------------------------------------------------------------
    $init = Start-Bridge
    $toolCount = @((Send-Request 'tools/list' @{}).result.tools).Count
    Add-Result 'hocket-mcp starts and lists the tools' ($init.result.serverInfo.name -eq 'hocket-mcp' -and $toolCount -eq 9) "$toolCount tools, MCP $($init.result.protocolVersion)"

    $original = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $write = Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: Claude's change`n@length 2bars`n" }
    $changed = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    Add-Result 'write_clip from Claude changes the clip' ((-not $write.IsError) -and $changed -like "*Claude's change*") ($write.Text -split "`n")[0]

    # The recovery copy (SPEC §4.5): about 3 s after changes stop.
    $copy = Wait-RecoveryCopy
    Add-Result 'A recovery copy is written a few seconds after a change' ($null -ne $copy) `
        $(if ($copy) { "after $($copy.Seconds) s: $($copy.Copy)" } else { "none within 8 s in $recoveryFolder" })

    $undo = Invoke-Tool 'undo'
    $restored = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    Add-Result 'undo restores the old clip' ((-not $undo.IsError) -and $restored -eq $original) $undo.Text

    # Back to the saved state, so nothing is unsaved: removed at once.
    $gone = Wait-RecoveryCopyGone
    Add-Result 'Undo back to the saved state removes the recovery copy' ($null -ne $gone) $(if ($null -ne $gone) { "within $gone s" } else { 'still there after 3 s' })

    # Lee's change: saving the clip file, which --watch sends as Lee's write_clip.
    Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: Claude's second change`n@length 2bars`n" } | Out-Null
    [IO.File]::WriteAllText($clipFile, "# acceptance: Lee's change, saved in the file`n@length 2bars`n", $utf8)

    $history = ''
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline -and $history -notmatch 'Lee:') {
        Start-Sleep -Milliseconds 100
        $history = (Invoke-Tool 'history').Text
    }

    $authors = @($history -split "`n" | Where-Object { $_ -match '^#\d+ ' })
    Add-Result 'history shows who made each change' ($history -match '#\d+ Lee: wrote clip' -and $history -match '#\d+ you: wrote clip') ($authors -join ' | ')

    $refused = Invoke-Tool 'undo'
    Add-Result "Claude's undo refuses Lee's change" ($refused.IsError -and $refused.Text -match 'was made by Lee') ($refused.Text -split "`n")[0]

    #--------------------------------------------------------------------------
    $log = Join-Path $work 'stress.log'
    $exit = Invoke-Logged $stress @('--count', '200', '--rate', '20') $log
    $report = (Get-Content $log) -join ' / '
    Add-Result 'hocket-stress: 200 rewrites at 20 a second, all applied, the clip put back' ($exit -eq 0) $report
}
catch {
    Add-Result 'The checks against --play ran to the end' $false $_.Exception.Message
}
finally {
    if ($bridge) {
        $bridgeExited = Stop-Bridge (Join-Path $work 'hocket-mcp.stderr.log')
        Add-Result 'hocket-mcp exits when its client closes stdin' $bridgeExited ''
    }

    #--------------------------------------------------------------------------
    # $play is the cmd.exe running --play, so its exit code is --play's.
    $howItEnded = Stop-Play $play
    Write-Results (Get-StopResults -PlayLog $playLog -HowItEnded $howItEnded -ExitCode $play.ExitCode -TokenFileLeft (Test-Path $tokenFile))

    # Lee's change was taken in from its file, and hocket-stress put the clip back as it found
    # it, so the project is as its files are: --play keeps no recovery copy.
    $left = @(Get-RecoveryCopies $ourCopies)
    Add-Result '--play ends leaving no recovery copy, with nothing unsaved' ($left.Count -eq 0) $(if ($left.Count -gt 0) { $left[0].Copy } else { '' })
}

#==============================================================================
# A crash with a change unsaved (SPEC §4.5; Lee, 2026-11-21): a change through the API,
# its recovery copy written, then --play killed outright, with no Ctrl+C. Started again
# on the same folder: plain --play stops and asks, without opening the device;
# --recover plays the change as one entry, which undo takes back; and, after a second
# change and kill, --discard-recovery files the copy as a kept save and plays the files
# as saved. The clips stay empty of notes, so every --play here is silent too.
$crash = 'After a forced kill'
$crashPlay = $null

try {
    if (-not $looping) { throw "--play didn't start above, so there's nothing to crash" }
    if (Test-HocketRunning) { throw "the --play above is still running" }

    $playLee = "--play `"$project`" --user Lee$deviceArguments"

    # A change through the API, its recovery copy, then the kill.
    $log = Join-Path $work 'crash-1-killed.log'
    $crashPlay = Start-Play $playLee $log
    if (-not (Wait-Looping $crashPlay $log)) { throw "--play didn't start; see $log" }

    Start-Bridge | Out-Null
    $saved = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $write = Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: the change a crash would lose`n@length 2bars`n" }
    if ($write.IsError) { throw "write_clip failed: $($write.Text)" }
    $lost = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $copy = Wait-RecoveryCopy
    $killed = Stop-PlayOutright $crashPlay
    Stop-Bridge (Join-Path $work 'crash-1-hocket-mcp.stderr.log') | Out-Null
    $kept = @(Get-RecoveryCopies $ourCopies)

    Add-Result "${crash}: --play killed outright with a change unsaved leaves its recovery copy" ($null -ne $copy -and $null -ne $killed -and $kept.Count -eq 1) `
        $(if (-not $copy) { "no copy within 8 s" } elseif ($null -eq $killed) { "no hocket-headless to kill" } else { "copy after $($copy.Seconds) s, process $killed killed: $($kept.Copy)" })

    # Plain --play: it plays nothing and says what to do, before opening the device.
    $log = Join-Path $work 'crash-2-plain.log'
    $crashPlay = Start-Play $playLee $log
    $exited = $crashPlay.WaitForExit(20000)
    if (-not $exited) { Stop-Play $crashPlay | Out-Null }
    $text = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
    $copyAfter = @(Get-RecoveryCopies $ourCopies)

    $asked = $exited -and $crashPlay.ExitCode -eq 1 -and $text -match 'This project has a recovery copy, written' `
        -and $text -match 'Changes: rewrote the notes of 1 clip file' -and $text -match 'Nothing was played' -and $text -match '  --recover  ' `
        -and $text -match '  --discard-recovery  ' -and $text -notmatch '(?m)^Playing on' -and $copyAfter.Count -eq 1 -and $copyAfter[0].Copy -eq $kept[0].Copy
    Add-Result "${crash}: plain --play stops and asks, without opening the device" $asked `
        $(if ($asked) { (($text -split "`r?`n") | Select-Object -First 1) } elseif (-not $exited) { "it was still running after 20 s; see $log" } else { "exit code $($crashPlay.ExitCode); see $log" })

    # --recover: the change as one entry, Lee's, which undo (with force: it isn't Claude's) takes back.
    $log = Join-Path $work 'crash-3-recover.log'
    $crashPlay = Start-Play "$playLee --recover" $log
    if (-not (Wait-Looping $crashPlay $log)) { throw "--play --recover didn't start; see $log" }

    Start-Bridge | Out-Null
    $recoveredLine = (Select-String -Path $log -Pattern '^Recovered: ' | Select-Object -First 1).Line
    $entries = @((Invoke-Tool 'history').Text -split "`n" | Where-Object { $_ -match '^#\d+ ' })
    $recovered = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $undo = Invoke-Tool 'undo' @{ force = $true }
    $undone = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $gone = Wait-RecoveryCopyGone

    $played = $recoveredLine -and $entries.Count -eq 1 -and $entries[0] -match '^#\d+ Lee: recovered the unsaved changes in the recovery copy written ' `
        -and $recovered -eq $lost -and -not $undo.IsError -and $undone -eq $saved -and $null -ne $gone
    Add-Result "${crash}: --recover plays the change as one entry, which undo takes back" $played `
        $(if ($played) { "$($entries[0]); undone, and the copy removed" } else { "history: $($entries -join ' | '); clip recovered: $($recovered -eq $lost); undo: $(($undo.Text -split "`n")[0]); clip back: $($undone -eq $saved); copy removed: $($null -ne $gone); see $log" })

    # A second change and kill, for --discard-recovery.
    $write = Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: a second change a crash would lose`n@length 2bars`n" }
    if ($write.IsError) { throw "write_clip failed: $($write.Text)" }
    if (-not (Wait-RecoveryCopy)) { throw "no recovery copy within 8 s of the second change" }
    if ($null -eq (Stop-PlayOutright $crashPlay)) { throw "no hocket-headless to kill after the second change" }
    Stop-Bridge (Join-Path $work 'crash-3-hocket-mcp.stderr.log') | Out-Null

    # --discard-recovery: filed as a kept save, a project folder that --check reads, and the files played as saved.
    $log = Join-Path $work 'crash-4-discard.log'
    $crashPlay = Start-Play "$playLee --discard-recovery" $log
    if (-not (Wait-Looping $crashPlay $log)) { throw "--play --discard-recovery didn't start; see $log" }

    $filedLine = (Select-String -Path $log -Pattern '^Filed the recovery copy written .* as save #\d+, a discarded recovery copy' | Select-Object -First 1).Line
    $keptSaves = @(Get-ChildItem -LiteralPath (Join-Path $project '.hocket\saves') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '* discarded recovery' })
    $keptClip = if ($keptSaves.Count -eq 1) { [IO.File]::ReadAllText((Join-Path $keptSaves[0].FullName 'clips\bass.notes')) } else { '' }
    $checkLog = Join-Path $work 'crash-4-check.log'
    $checked = $keptSaves.Count -eq 1 -and (Invoke-Logged $headless @('--check', $keptSaves[0].FullName) $checkLog) -eq 0
    Start-Bridge | Out-Null
    $playing = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    Stop-Bridge (Join-Path $work 'crash-4-hocket-mcp.stderr.log') | Out-Null
    $copiesLeft = @(Get-RecoveryCopies $ourCopies).Count

    $filed = $filedLine -and $keptSaves.Count -eq 1 -and $keptClip -like '*a second change a crash would lose*' -and $checked -and $playing -eq $saved -and $copiesLeft -eq 0
    Add-Result "${crash}: --discard-recovery files the copy as a kept save, and plays the files as saved" $filed `
        $(if ($filed) { "$($keptSaves[0].Name); --check: $((Get-Content $checkLog | Select-Object -First 1))" } else { "filed: '$filedLine'; kept saves: $($keptSaves.Count); kept clip has the change: $($keptClip -like '*a second change*'); --check OK: $checked; playing the saved clip: $($playing -eq $saved); copies left: $copiesLeft; see $log" })

    #--------------------------------------------------------------------------
    # --play's keys (SPEC §4.5): S saves as the person; a save refused for a file changed
    # outside Hocket offers O, which saves over it, keeping it first. This --play has no
    # --watch, so a file edited by hand isn't taken in.
    $keys = '--play''s console'
    Start-Bridge | Out-Null

    Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: saved with S`n@length 2bars`n" } | Out-Null
    $sText = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $before = Measure-LogLines $log '^Saved: '
    $typed = Send-Keys $crashPlay 's'
    $savedLine = Wait-NewLogLine $log '^Saved: ' $before
    $onDisk = [IO.File]::ReadAllText($clipFile)
    $sSaved = $typed -eq 0 -and $savedLine -like 'Saved: wrote clips/bass.notes; the version it replaced is kept as save #*' -and $onDisk -eq $sText
    Add-Result "${keys}: S saves, as the person at the console" $sSaved `
        $(if ($sSaved) { $savedLine } else { "Send-Keys exit code $typed; line: '$savedLine'; on disk: $($onDisk -eq $sText); see $log" })

    $byHand = "# acceptance: Lee's edit by hand, not taken in`n@length 2bars`n"
    [IO.File]::WriteAllText($clipFile, $byHand, $utf8)
    Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: saved over the edit with O`n@length 2bars`n" } | Out-Null
    $oText = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $before = Measure-LogLines $log '^Didn''t save, so as not to overwrite'
    Send-Keys $crashPlay 's' | Out-Null
    $refusedLine = Wait-NewLogLine $log '^Didn''t save, so as not to overwrite' $before
    $offered = Select-String -Path $log -Pattern '^Press O to save over it anyway' -Quiet
    $stillByHand = [IO.File]::ReadAllText($clipFile) -eq $byHand

    $before = Measure-LogLines $log '^Saved: '
    $typed = Send-Keys $crashPlay 'o'
    $overLine = Wait-NewLogLine $log '^Saved: ' $before
    $keptByHand = @(Get-ChildItem -LiteralPath (Join-Path $project '.hocket\saves') -Directory | Where-Object { $_.Name -like '* changed outside Hocket' } |
                    Where-Object { [IO.File]::ReadAllText((Join-Path $_.FullName 'clips\bass.notes')) -eq $byHand })
    $oSaved = $refusedLine -like '*clips/bass.notes changed outside Hocket*' -and $offered -and $stillByHand -and $typed -eq 0 `
        -and $overLine -like 'Saved: wrote clips/bass.notes; the version it replaced is kept as save #*, changed outside Hocket, *' `
        -and [IO.File]::ReadAllText($clipFile) -eq $oText -and $keptByHand.Count -eq 1
    Add-Result "${keys}: S won't save over a file changed outside Hocket, and O then does, keeping it first" $oSaved `
        $(if ($oSaved) { $overLine } else { "refused: '$refusedLine'; offered: $([bool] $offered); untouched until O: $stillByHand; after O: '$overLine'; kept: $($keptByHand.Count); see $log" })

    Stop-Bridge (Join-Path $work 'keys-hocket-mcp.stderr.log') | Out-Null

    # Ctrl+C: nothing unsaved, so nothing kept.
    $howItEnded = Stop-Play $crashPlay
    $stoppedLine = (Select-String -Path $log -Pattern '^stopped' | Select-Object -Last 1).Line
    $copiesLeft = @(Get-RecoveryCopies $ourCopies).Count
    $clean = $howItEnded -eq 'ctrl-c' -and $crashPlay.ExitCode -eq 0 -and $stoppedLine -and $copiesLeft -eq 0
    Add-Result "${crash}: the last --play stops cleanly on Ctrl+C, leaving no recovery copy" $clean `
        $(if ($clean) { '' } else { "ended: $howItEnded, exit code $($crashPlay.ExitCode), stopped line: '$stoppedLine', copies left: $copiesLeft; see $log" })
    $crashPlay = $null
}
catch {
    Add-Result "${crash}: the checks ran to the end" $false $_.Exception.Message
}
finally {
    if ($bridge -and -not $bridge.HasExited) { Stop-Bridge (Join-Path $work 'crash-hocket-mcp.stderr.log') | Out-Null }
    if ($crashPlay -and -not $crashPlay.HasExited) { Stop-Play $crashPlay | Out-Null }
}

#==============================================================================
Write-Host ''
$results | Format-Table -AutoSize -Wrap | Out-String -Width 200 | Write-Host
$failures = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host $(if ($failures -eq 0) { "All checks passed. Logs: $work" } else { "$failures check(s) failed. Logs: $work" })
exit $(if ($failures -eq 0) { 0 } else { 1 })
