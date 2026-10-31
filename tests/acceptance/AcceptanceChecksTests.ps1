# Tests for tools/acceptance/AcceptanceChecks.ps1: what Run-Acceptance.ps1 makes
# of the end of a --play run. Exits 1 if any case fails. CTest runs it.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\tools\acceptance\AcceptanceChecks.ps1')

# A folder whose name is outside every Western ANSI code page, for the logs.
$folder = Join-Path ([IO.Path]::GetTempPath()) ("hocket-acceptance-tests $([char] 0x03B5)$([char] 0x03BB)$([char] 0x03BB) " + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $folder | Out-Null
$failures = 0

function Test-Stop ([string] $Name, [string] $Log, [string] $HowItEnded, [int] $ExitCode, [bool] $TokenFileLeft, [string[]] $Expected) {
    $logFile = Join-Path $folder 'play.log'
    [IO.File]::WriteAllText($logFile, $Log, (New-Object System.Text.UTF8Encoding($false)))

    $results = @(Get-StopResults -PlayLog $logFile -HowItEnded $HowItEnded -ExitCode $ExitCode -TokenFileLeft $TokenFileLeft)
    $actual = @($results | ForEach-Object { "$($_.Result) $($_.Check): $($_.Detail)" })
    $matched = $actual.Count -eq $Expected.Count

    for ($i = 0; $matched -and $i -lt $Expected.Count; $i++) {
        $matched = $actual[$i] -like $Expected[$i]
    }

    if ($matched) {
        Write-Host "ok: $Name"
    }
    else {
        $script:failures++
        Write-Host "FAILED: $Name"
        Write-Host "  expected:"
        $Expected | ForEach-Object { Write-Host "    $_" }
        Write-Host "  got:"
        $actual | ForEach-Object { Write-Host "    $_" }
    }
}

$playing = "Playing on Test: Speakers, 44100 Hz, 256 samples (5.8 ms)`nplaying 1.1 to 3.1 on a loop. Ctrl+C to stop.`n"

try {
    Test-Stop 'a clean stop over ASIO' "${playing}stopped; the driver reported 0 xruns, and no audio callback took longer than its block`n" 'ctrl-c' 0 $false @(
        'PASS --play stops cleanly on Ctrl+C: stopped; *token file removed*',
        'PASS No xruns: the driver reported 0 xruns',
        'NOTE Late audio callbacks: no audio callback took longer than its block')

    Test-Stop 'xruns from the driver fail' "${playing}stopped; the driver reported 3 xruns, and no audio callback took longer than its block`n" 'ctrl-c' 0 $false @(
        'PASS --play stops cleanly on Ctrl+C: *',
        'FAIL No xruns: the driver reported 3 xruns',
        'NOTE Late audio callbacks: *')

    Test-Stop 'a driver that keeps no count, with late callbacks' "${playing}stopped; this driver doesn't report xruns, and 2 audio callbacks took longer than their block`n" 'ctrl-c' 0 $false @(
        'PASS --play stops cleanly on Ctrl+C: *',
        "NOTE Xruns: this driver doesn't report them*",
        'NOTE Late audio callbacks: 2 audio callbacks took longer than their block')

    Test-Stop 'a crash on the way out, before "stopped" was written' $playing 'ctrl-c' -1073741819 $true @(
        "FAIL --play stops cleanly on Ctrl+C: it exited with code -1073741819 (0xC0000005)*no 'stopped' line*")

    Test-Stop 'a crash after "stopped" was written' "${playing}stopped; the driver reported 0 xruns, and no audio callback took longer than its block`n" 'ctrl-c' -1073741819 $false @(
        'FAIL --play stops cleanly on Ctrl+C: it exited with code -1073741819 (0xC0000005)*',
        'PASS No xruns: *',
        'NOTE Late audio callbacks: *')

    Test-Stop 'a clean exit code with no "stopped" line' $playing 'ctrl-c' 0 $false @(
        "FAIL --play stops cleanly on Ctrl+C: its log has no 'stopped' line*")

    Test-Stop 'it had stopped before Ctrl+C' "${playing}some error`n" 'already-exited' 1 $false @(
        'FAIL --play stops cleanly on Ctrl+C: it had already stopped before Ctrl+C was sent, with exit code 1*')

    Test-Stop 'it ignored Ctrl+C and was killed' $playing 'killed' -1 $true @(
        "FAIL --play stops cleanly on Ctrl+C: it didn't stop within 15 s of Ctrl+C, so it was killed")

    Test-Stop 'the token file was left behind' "${playing}stopped; the driver reported 0 xruns, and no audio callback took longer than its block`n" 'ctrl-c' 0 $true @(
        'FAIL --play stops cleanly on Ctrl+C: *token file removed: False*',
        'PASS No xruns: *',
        'NOTE Late audio callbacks: *')
}
finally {
    Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Host "$failures case(s) failed"
    exit 1
}

Write-Host 'All cases passed'
exit 0
