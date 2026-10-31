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
  - hocket-stress: 200 rewrites at 20 a second during playback, all applied, the clip put back;
  - Ctrl+C stops --play cleanly, and the driver's xrun count, where it reports one, is 0.
Hearing the changes, and hearing no clicks or gaps, is the part left for a person.

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
$results = New-Object System.Collections.Generic.List[object]

function Add-Result ([string] $Check, [bool] $Passed, [string] $Detail) {
    $results.Add([pscustomobject] @{ Check = $Check; Result = $(if ($Passed) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
    $mark = if ($Passed) { 'PASS' } else { 'FAIL' }
    Write-Host "[$mark] $Check$(if ($Detail) { ": $Detail" })"
}

function Add-Note ([string] $Check, [string] $Detail) {
    $results.Add([pscustomobject] @{ Check = $Check; Result = 'NOTE'; Detail = $Detail })
    Write-Host "[NOTE] ${Check}: $Detail"
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

function Test-HocketRunning {
    if (-not (Test-Path $tokenFile)) { return $false }
    try {
        $info = Get-Content $tokenFile -Raw | ConvertFrom-Json
        return $null -ne (Get-Process -Id $info.pid -ErrorAction SilentlyContinue)
    }
    catch { return $false }
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

# The demo with an empty clip: it plays silence.
$project = Join-Path $work 'm0-acceptance'
Copy-Item -Recurse (Join-Path $repo 'examples\m0-demo') $project
$clipFile = Join-Path $project 'clips\bass.notes'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($clipFile, "@length 2bars`n", $utf8)

$playLog = Join-Path $work 'play.log'
$playArguments = "--play `"$project`" --watch --user Lee"
if ($Driver) { $playArguments += " --driver `"$Driver`"" }
if ($Device) { $playArguments += " --device `"$Device`"" }

# Through cmd, with a console of its own and no window, so Ctrl+C can reach it.
$info = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', "/c `"`"$headless`" $playArguments > `"$playLog`" 2>&1`""
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$play = [Diagnostics.Process]::Start($info)

$deadline = (Get-Date).AddSeconds(30)
$looping = $false
while (-not $looping -and -not $play.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    $looping = (Test-Path $playLog) -and (Select-String -Path $playLog -Pattern 'on a loop' -Quiet) -and (Test-Path $tokenFile)
}

$device = if (Test-Path $playLog) { (Select-String -Path $playLog -Pattern '^Playing on' | Select-Object -First 1).Line } else { '' }
Add-Result '--play opens the device and loops' $looping $(if ($looping) { $device } else { "see $playLog" })

$bridge = $null
try {
    if (-not $looping) { throw "--play didn't start" }

    #--------------------------------------------------------------------------
    # hocket-mcp, started in the project folder as Claude Code starts it in its session's folder.
    $info = New-Object Diagnostics.ProcessStartInfo $mcp
    $info.UseShellExecute = $false
    $info.WorkingDirectory = $project
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8
    $bridge = [Diagnostics.Process]::Start($info)
    $bridgeErrors = $bridge.StandardError.ReadToEndAsync()
    $stdin = New-Object IO.StreamWriter($bridge.StandardInput.BaseStream, $utf8)
    $stdin.NewLine = "`n"
    $script:requestId = 0

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

    $init = Send-Request 'initialize' @{ protocolVersion = '2025-11-25'; capabilities = @{}; clientInfo = @{ name = 'claude-code'; version = 'acceptance' } }
    $toolCount = @((Send-Request 'tools/list' @{}).result.tools).Count
    Add-Result 'hocket-mcp starts and lists the tools' ($init.result.serverInfo.name -eq 'hocket-mcp' -and $toolCount -eq 9) "$toolCount tools, MCP $($init.result.protocolVersion)"

    $original = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    $write = Invoke-Tool 'write_clip' @{ clip_id = 'clp-bass'; notes = "# acceptance: Claude's change`n@length 2bars`n" }
    $changed = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    Add-Result 'write_clip from Claude changes the clip' ((-not $write.IsError) -and $changed -like "*Claude's change*") ($write.Text -split "`n")[0]

    $undo = Invoke-Tool 'undo'
    $restored = (Invoke-Tool 'read_clip' @{ clip_id = 'clp-bass' }).Blocks[1]
    Add-Result 'undo restores the old clip' ((-not $undo.IsError) -and $restored -eq $original) $undo.Text

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
        $stdin.Close()
        $bridgeExited = $bridge.WaitForExit(10000)
        if (-not $bridgeExited) { $bridge.Kill() }
        [IO.File]::WriteAllText((Join-Path $work 'hocket-mcp.stderr.log'), $bridgeErrors.Result, $utf8)
        Add-Result 'hocket-mcp exits when its client closes stdin' ($bridgeExited -and $bridge.ExitCode -eq 0) ''
    }

    #--------------------------------------------------------------------------
    if (-not $play.HasExited) {
        $ctrlC = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Send-CtrlC.ps1'), $play.Id `
                                           -WindowStyle Hidden -Wait -PassThru
        if (-not $play.WaitForExit(15000)) {
            $play.Kill()
            Add-Result '--play stops cleanly on Ctrl+C' $false "it didn't stop (Send-CtrlC exit code $($ctrlC.ExitCode)), so it was killed"
        }
    }

    $stopped = if (Test-Path $playLog) { (Select-String -Path $playLog -Pattern '^stopped' | Select-Object -Last 1).Line } else { $null }
    if ($stopped) {
        Add-Result '--play stops cleanly on Ctrl+C' (-not (Test-Path $tokenFile)) "$stopped; token file removed: $(-not (Test-Path $tokenFile))"

        if ($stopped -match 'reported (\d+) xrun') {
            Add-Result 'No xruns' ([int] $Matches[1] -eq 0) $stopped
        }
        else {
            Add-Note 'Xruns' "this driver doesn't report them (WASAPI doesn't; ASIO drivers usually do)"
        }
    }
}

#==============================================================================
Write-Host ''
$results | Format-Table -AutoSize -Wrap | Out-String -Width 200 | Write-Host
$failures = @($results | Where-Object { $_.Result -eq 'FAIL' }).Count
Write-Host $(if ($failures -eq 0) { "All checks passed. Logs: $work" } else { "$failures check(s) failed. Logs: $work" })
exit $(if ($failures -eq 0) { 0 } else { 1 })
