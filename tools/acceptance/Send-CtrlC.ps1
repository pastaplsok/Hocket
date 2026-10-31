# Sends Ctrl+C to the console of a process, e.g. the cmd.exe running
# hocket-headless --play, so it stops the way it does at the keyboard.
# Run it in a PowerShell of its own: it detaches from its own console first.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File Send-CtrlC.ps1 <process ID>

param([Parameter(Mandatory)] [uint32] $ProcessId)

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

if (-not [HocketAcceptance.Console]::GenerateConsoleCtrlEvent(0, 0)) { exit 3 }

exit 0
