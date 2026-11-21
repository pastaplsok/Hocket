# Types keys into the console of a process, e.g. the cmd.exe running
# hocket-headless --play, as if pressed there: for --play's S and O keys.
# Run it in a PowerShell of its own: it detaches from its own console first.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File Send-Keys.ps1 <process ID> <keys>
#
# Exits with 2 if it can't attach to that console, 3 if it can't write to its input.

param(
    [Parameter(Mandatory)] [uint32] $ProcessId,
    [Parameter(Mandatory)] [string] $Keys
)

Add-Type -Namespace HocketAcceptance -Name Keys -MemberDefinition @'
[StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
public struct KeyEventRecord
{
    [FieldOffset(0)] public ushort EventType;      // KEY_EVENT = 1
    [FieldOffset(4)] public int KeyDown;
    [FieldOffset(8)] public ushort RepeatCount;
    [FieldOffset(10)] public ushort VirtualKeyCode;
    [FieldOffset(12)] public ushort VirtualScanCode;
    [FieldOffset(14)] public char UnicodeChar;
    [FieldOffset(16)] public uint ControlKeyState;
}

[DllImport("kernel32.dll")] public static extern bool FreeConsole();
[DllImport("kernel32.dll")] public static extern bool AttachConsole(uint id);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern System.IntPtr CreateFileW(string name, uint access, uint share, System.IntPtr security, uint creation, uint flags, System.IntPtr template);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool WriteConsoleInputW(System.IntPtr input, KeyEventRecord[] records, uint count, out uint written);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(System.IntPtr handle);
'@

[HocketAcceptance.Keys]::FreeConsole() | Out-Null

if (-not [HocketAcceptance.Keys]::AttachConsole($ProcessId)) { exit 2 }

# The console's input buffer: GENERIC_READ | GENERIC_WRITE, shared, OPEN_EXISTING.
$console = [HocketAcceptance.Keys]::CreateFileW('CONIN$', [uint32] 3221225472, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
if ($console -eq [IntPtr]::new(-1)) { exit 3 }

$records = foreach ($key in $Keys.ToCharArray()) {
    foreach ($down in 1, 0) {
        $record = New-Object HocketAcceptance.Keys+KeyEventRecord
        $record.EventType = 1
        $record.KeyDown = $down
        $record.RepeatCount = 1
        $record.VirtualKeyCode = [uint16][char]::ToUpperInvariant($key)
        $record.UnicodeChar = $key
        $record
    }
}

$written = 0
$ok = [HocketAcceptance.Keys]::WriteConsoleInputW($console, [HocketAcceptance.Keys+KeyEventRecord[]] $records, [uint32] $records.Count, [ref] $written)
[HocketAcceptance.Keys]::CloseHandle($console) | Out-Null

if (-not $ok -or $written -ne $records.Count) { exit 3 }
exit 0
