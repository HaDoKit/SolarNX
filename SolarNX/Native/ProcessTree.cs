using System.Runtime.InteropServices;

namespace SolarNX.Native;

/// <summary>
/// Parent/child relationships between running processes, from a Toolhelp32 snapshot. The snapshot
/// lists every process with its parent pid without opening any of them - no process handle at all,
/// so nothing an anti-cheat driver could object to (unlike WMI-free alternatives that query each
/// process). Each child's recorded parent pid survives the parent's exit, which is what lets a
/// launcher/script that starts the real game and quits still be followed to that game.
/// </summary>
internal static class ProcessTree
{
    private const uint Th32csSnapProcess = 0x00000002;
    private static readonly IntPtr InvalidHandleValue = new(-1);

    /// <summary>pid → parent pid for every process right now (empty on failure).</summary>
    public static Dictionary<int, int> SnapshotParents()
    {
        var map = new Dictionary<int, int>();
        var snapshot = CreateToolhelp32Snapshot(Th32csSnapProcess, 0);
        if (snapshot == InvalidHandleValue) return map;
        try
        {
            var entry = new PROCESSENTRY32W { dwSize = (uint)Marshal.SizeOf<PROCESSENTRY32W>() };
            if (!Process32FirstW(snapshot, ref entry)) return map;
            do
            {
                map[(int)entry.th32ProcessID] = (int)entry.th32ParentProcessID;
            }
            while (Process32NextW(snapshot, ref entry));
        }
        finally
        {
            CloseHandle(snapshot);
        }
        return map;
    }

    /// <summary>Every currently running process whose ancestor chain reaches <paramref name="rootPid"/>
    /// (bounded hop count guards against cycles from reused pids).</summary>
    public static List<int> LiveDescendants(int rootPid, Dictionary<int, int> parents)
    {
        const int maxHops = 16;
        var result = new List<int>();
        foreach (var pid in parents.Keys)
        {
            if (pid == rootPid) continue;
            var current = pid;
            for (var hop = 0; hop < maxHops && parents.TryGetValue(current, out var parent); hop++)
            {
                if (parent == rootPid) { result.Add(pid); break; }
                if (parent == current || parent == 0) break;
                current = parent;
            }
        }
        return result;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct PROCESSENTRY32W
    {
        public uint dwSize;
        public uint cntUsage;
        public uint th32ProcessID;
        public IntPtr th32DefaultHeapID;
        public uint th32ModuleID;
        public uint cntThreads;
        public uint th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szExeFile;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool Process32FirstW(IntPtr hSnapshot, ref PROCESSENTRY32W lppe);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool Process32NextW(IntPtr hSnapshot, ref PROCESSENTRY32W lppe);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr hObject);
}
