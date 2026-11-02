using System.Diagnostics;

namespace SolarNX.Native;

/// <summary>
/// A specific process instance (pid + start time, so a reused pid isn't mistaken for it) that the
/// host follows to decide "is the game still running". Only ever queried with
/// PROCESS_QUERY_LIMITED_INFORMATION-level access (HasExited, StartTime) - nothing anti-cheat
/// drivers object to - and never holds a handle between checks.
///
/// <see cref="IncludeDescendants"/> (library games): the instance counts as alive while the root
/// or any process descended from it runs - launchers and cmd scripts typically start the real game
/// and exit immediately, and the game must not be declared "closed" the moment its launcher quits.
/// </summary>
internal sealed record TrackedProcess(int Pid, DateTime StartTime, bool IncludeDescendants = false)
{
    public static TrackedProcess Capture(int pid, bool includeDescendants = false)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            return new TrackedProcess(pid, process.StartTime, includeDescendants);
        }
        catch
        {
            return new TrackedProcess(pid, DateTime.MinValue, includeDescendants); // start time not queryable - pid only
        }
    }

    public bool IsAlive()
    {
        if (IsRootAlive()) return true;
        if (!IncludeDescendants) return false;
        return ProcessTree.LiveDescendants(Pid, ProcessTree.SnapshotParents()).Count > 0;
    }

    /// <summary>Root (if still the same instance) plus, with IncludeDescendants, every live
    /// descendant - the set whose windows/exit matter.</summary>
    public List<int> FamilyPids()
    {
        var pids = new List<int>();
        if (IsRootAlive()) pids.Add(Pid);
        if (IncludeDescendants) pids.AddRange(ProcessTree.LiveDescendants(Pid, ProcessTree.SnapshotParents()));
        return pids;
    }

    /// <summary>Asks every process in the family to close (WM_CLOSE to its main window), waits up to
    /// <paramref name="grace"/>, then kills what's left. Used when the Switch player leaves - same
    /// treatment Big Picture games get in SteamLauncher.CloseActiveGame. Best-effort throughout.</summary>
    public void CloseFamily(TimeSpan grace)
    {
        var pids = FamilyPids();
        foreach (var pid in pids)
        {
            try { using var p = Process.GetProcessById(pid); p.CloseMainWindow(); }
            catch { /* exited / not accessible */ }
        }

        var deadline = DateTime.UtcNow + grace;
        foreach (var pid in pids)
        {
            try
            {
                using var p = Process.GetProcessById(pid);
                var remaining = deadline - DateTime.UtcNow;
                if (remaining > TimeSpan.Zero && p.WaitForExit(remaining)) continue;
                Logger.Log("TrackedProcess", $"pid={pid} ({p.ProcessName}) didn't close in time - killing");
                p.Kill();
            }
            catch { /* exited / not accessible */ }
        }
    }

    private bool IsRootAlive()
    {
        Process process;
        try { process = Process.GetProcessById(Pid); }
        catch (ArgumentException) { return false; } // no longer in the process list

        using (process)
        {
            try
            {
                if (process.HasExited) return false;
                return StartTime == DateTime.MinValue || process.StartTime == StartTime;
            }
            catch
            {
                // Listed but not queryable (e.g. an anti-cheat-protected process) - it exists,
                // so treat it as still running rather than fake a "game closed".
                return true;
            }
        }
    }
}
