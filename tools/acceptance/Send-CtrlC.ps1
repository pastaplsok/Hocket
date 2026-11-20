# Sends Ctrl+C to the console of a process, e.g. the cmd.exe running
# hocket-headless --play, so it stops the way it does at the keyboard.
# Run it in a PowerShell of its own: it detaches from its own console first.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File Send-CtrlC.ps1 <process ID>
#
# With -Log and -WhenLogSays, it attaches first, then watches the log and sends
# Ctrl+C the moment a line matches, so it lands within a few ms of that line:
# PowerShell takes far longer than that to start. It writes -ReadyFile once it's
# watching, and exits with 4 if no line matches within -TimeoutSeconds, or 5 if
# the process ends first.

param(
    [Parameter(Mandatory)] [uint32] $ProcessId,
    [string] $Log,
    [string] $WhenLogSays,
    [string] $ReadyFile,
    [int] $TimeoutSeconds = 60
)

Add-Type -Namespace HocketAcceptance -Name Console -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern bool FreeConsole();
[DllImport("kernel32.dll")] public static extern bool AttachConsole(uint id);
[DllImport("kernel32.dll")] public static extern bool SetConsoleCtrlHandler(System.IntPtr handler, bool add);
[DllImport("kernel32.dll")] public static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroup);
'@

[HocketAcceptance.Console]::FreeConsole() | Out-Null

if (-not [HocketAcceptance.Console]::AttachConsole($ProcessId)) { exit 2 }

# Ignore the Ctrl+C here, then send it to everything on that console.
[HocketAcceptance.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null

if ($WhenLogSays) {
    $target = [Diagnostics.Process]::GetProcessById($ProcessId)
    if ($ReadyFile) { [IO.File]::WriteAllText($ReadyFile, (Get-Date).ToString('o')) }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $seen = $false

    while (-not $seen -and (Get-Date) -lt $deadline) {
        if ($target.HasExited) { exit 5 }

        if (Test-Path $Log) {
            # Shared, since cmd is still writing it.
            $stream = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
            try { $text = (New-Object IO.StreamReader($stream)).ReadToEnd() } finally { $stream.Dispose() }
            $seen = $text -match "(?m)$WhenLogSays"
        }

        if (-not $seen) { Start-Sleep -Milliseconds 2 }
    }

    if (-not $seen) { exit 4 }
}

if (-not [HocketAcceptance.Console]::GenerateConsoleCtrlEvent(0, 0)) { exit 3 }

exit 0
