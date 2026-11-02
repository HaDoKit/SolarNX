using System.Runtime.InteropServices;
using System.Text;

namespace SolarNX.Native;

/// <summary>
/// Full exe path of another process, opened with PROCESS_QUERY_LIMITED_INFORMATION only - the
/// minimal right Task Manager itself uses, which anti-cheat drivers (EAC/BattlEye/...) don't strip
/// or flag.
///
/// Replaces Process.MainModule for this purpose: MainModule enumerates the target's loaded modules,
/// which needs a PROCESS_QUERY_INFORMATION | PROCESS_VM_READ handle - a memory-read handle to the
/// game process, opened every watcher tick (1s), is exactly the access pattern of an external cheat
/// and what kernel anti-cheats watch for. It also failed outright across bitness/integrity levels,
/// which QueryFullProcessImageName doesn't.
/// </summary>
internal static class ProcessImagePath
{
    private const uint ProcessQueryLimitedInformation = 0x1000;

    /// <summary>Null if the process is gone or the path can't be queried.</summary>
    public static string? TryGet(int processId)
    {
        var hProcess = OpenProcess(ProcessQueryLimitedInformation, false, (uint)processId);
        if (hProcess == IntPtr.Zero) return null;

        try
        {
            var buffer = new StringBuilder(1024);
            var size = (uint)buffer.Capacity;
            return QueryFullProcessImageName(hProcess, 0, buffer, ref size) ? buffer.ToString(0, (int)size) : null;
        }
        finally
        {
            CloseHandle(hProcess);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "QueryFullProcessImageNameW")]
    private static extern bool QueryFullProcessImageName(IntPtr hProcess, uint dwFlags, StringBuilder lpExeName, ref uint lpdwSize);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr hObject);
}
