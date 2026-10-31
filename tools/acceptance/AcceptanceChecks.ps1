# What Run-Acceptance.ps1 makes of the runs it starts, kept apart so tests can
# check it (tests/acceptance). Dot-source it: . (Join-Path $PSScriptRoot 'AcceptanceChecks.ps1')

function New-Result ([string] $Check, [string] $Result, [string] $Detail) {
    [pscustomobject] @{ Check = $Check; Result = $Result; Detail = $Detail }
}

function New-Check ([string] $Check, [bool] $Passed, [string] $Detail) {
    New-Result $Check $(if ($Passed) { 'PASS' } else { 'FAIL' }) $Detail
}

<#
.SYNOPSIS
The results for the end of a --play run: whether it stopped cleanly, and its xruns.

.PARAMETER PlayLog
--play's output.

.PARAMETER HowItEnded
'ctrl-c' (it stopped after Ctrl+C), 'killed' (it didn't, so it was killed), or
'already-exited' (it had stopped before Ctrl+C was sent).

.PARAMETER ExitCode
--play's exit code.

.PARAMETER TokenFileLeft
Whether the API's token file was still there afterwards.
#>
function Get-StopResults ([string] $PlayLog, [string] $HowItEnded, [int] $ExitCode, [bool] $TokenFileLeft) {
    $check = '--play stops cleanly on Ctrl+C'

    # A crash on the way out can lose the buffered output, "stopped" line and
    # all, so the exit code is checked too, and a missing line is a failure.
    if ($HowItEnded -eq 'killed') {
        New-Check $check $false "it didn't stop within 15 s of Ctrl+C, so it was killed"
        return
    }

    $stopped = if (Test-Path $PlayLog) { (Select-String -Path $PlayLog -Pattern '^stopped' | Select-Object -Last 1).Line } else { $null }
    # Crashes are NTSTATUS codes, which read better in hex: -1073741819 is 0xC0000005.
    $code = "$ExitCode$(if ($ExitCode -lt 0) { ' (0x{0:X8})' -f $ExitCode })"

    if ($HowItEnded -eq 'already-exited') {
        New-Check $check $false "it had already stopped before Ctrl+C was sent, with exit code $code; see $PlayLog"
    }
    elseif ($ExitCode -ne 0) {
        $after = if ($stopped) { " after '$stopped'" } else { ", and its log has no 'stopped' line" }
        New-Check $check $false "it exited with code $code$after; see $PlayLog"
    }
    elseif (-not $stopped) {
        New-Check $check $false "its log has no 'stopped' line, though it exited with code 0; see $PlayLog"
    }
    else {
        New-Check $check (-not $TokenFileLeft) "$stopped; token file removed: $(-not $TokenFileLeft)"
    }

    if (-not $stopped) {
        return
    }

    # "stopped; the driver reported 0 xruns, and no audio callback took longer than its block"
    if ($stopped -match 'the driver reported (\d+) xruns?') {
        New-Check 'No xruns' ([int] $Matches[1] -eq 0) $Matches[0]
    }
    else {
        New-Result 'Xruns' 'NOTE' "this driver doesn't report them (WASAPI doesn't; ASIO drivers usually do)"
    }

    # JUCE's own count. Not a failure: one late callback needn't be heard, with a driver that buffers.
    if ($stopped -match '(no audio callback took longer than its block|\d+ audio callbacks? took longer than (its|their) block)') {
        New-Result 'Late audio callbacks' 'NOTE' $Matches[1]
    }
}
