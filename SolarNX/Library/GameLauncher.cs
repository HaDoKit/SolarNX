using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using SolarNX.BigPicture;
using SolarNX.Native;

namespace SolarNX.Library;

/// <summary>
/// Starts a library entry the way its type requires and returns the process to follow for "is the
/// game still running" (null = it couldn't be started). Never keeps a handle to the game: the
/// Process object from Process.Start is disposed immediately and the game is tracked by pid + start
/// time with limited-rights queries only (TrackedProcess) - a launcher holding a full-access handle
/// to a protected game is exactly what kernel anti-cheats look for. Nothing is ever injected; the
/// gamepad is the system-wide ViGEm virtual pad, which every game sees like a real controller.
/// </summary>
internal static class GameLauncher
{
    private static readonly TimeSpan SteamGameWindowTimeout = TimeSpan.FromSeconds(90);

    public static async Task<TrackedProcess?> LaunchAsync(GameEntry entry, SteamLauncher steam, CancellationToken ct)
    {
        try
        {
            switch (entry.Type)
            {
                case GameEntryType.Native:
                    return StartDetached(entry.ExePath, entry.Arguments,
                        NonEmpty(entry.WorkingDirectory) ?? Path.GetDirectoryName(entry.ExePath));

                case GameEntryType.Emulator:
                    var args = (NonEmpty(entry.ArgumentTemplate) ?? "\"{rom}\"").Replace("{rom}", entry.RomPath);
                    return StartDetached(entry.EmulatorPath, args, Path.GetDirectoryName(entry.EmulatorPath));

                case GameEntryType.Script:
                    return StartScript(entry);

                case GameEntryType.Steam:
                    return await StartSteamGameAsync(entry, steam, ct);

                default:
                    return null;
            }
        }
        catch (Exception ex)
        {
            Logger.Log("GameLauncher", $"failed to launch '{entry.Name}' ({entry.Type}): {ex.Message}");
            return null;
        }
    }

    private static TrackedProcess? StartDetached(string exePath, string arguments, string? workingDirectory)
    {
        if (string.IsNullOrWhiteSpace(exePath) || !File.Exists(exePath))
        {
            Logger.Log("GameLauncher", $"exe not found: '{exePath}'");
            return null;
        }

        // UseShellExecute: honours the exe's own manifest (UAC elevation prompt for games that require
        // admin) the same way double-clicking it would.
        var psi = new ProcessStartInfo(exePath, arguments ?? "")
        {
            UseShellExecute = true,
            WorkingDirectory = workingDirectory ?? "",
        };
        using var process = Process.Start(psi);
        if (process is null) return null;
        Logger.Log("GameLauncher", $"started {exePath} {arguments} (pid={process.Id})");
        // Descendants included: launchers routinely start the real game and exit.
        return TrackedProcess.Capture(process.Id, includeDescendants: true);
    }

    private static TrackedProcess? StartScript(GameEntry entry)
    {
        if (string.IsNullOrWhiteSpace(entry.Script)) return null;

        var dir = Path.Combine(Path.GetTempPath(), "SolarNX");
        Directory.CreateDirectory(dir);
        var scriptPath = Path.Combine(dir, $"game-{entry.Id}.cmd");
        // chcp 65001 + UTF-8 without BOM: the script may contain non-ASCII paths/names (cmd would
        // otherwise read the file in the OEM code page and mangle them).
        File.WriteAllText(scriptPath, "@chcp 65001 >nul\r\n" + entry.Script.Replace("\r\n", "\n").Replace("\n", "\r\n") + "\r\n",
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

        var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"), $"/c \"{scriptPath}\"")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = NonEmpty(entry.WorkingDirectory) ?? dir,
        };
        using var process = Process.Start(psi);
        if (process is null) return null;
        Logger.Log("GameLauncher", $"started script for '{entry.Name}' (cmd pid={process.Id})");
        // A script usually `start`s the game and returns - the game is followed as a descendant of cmd.
        return TrackedProcess.Capture(process.Id, includeDescendants: true);
    }

    private static async Task<TrackedProcess?> StartSteamGameAsync(GameEntry entry, SteamLauncher steam, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(entry.SteamAppId)) return null;
        if (!await steam.EnsureSteamRunningAsync(ct)) return null;

        Process.Start(new ProcessStartInfo($"steam://rungameid/{entry.SteamAppId.Trim()}") { UseShellExecute = true });
        Logger.Log("GameLauncher", $"asked Steam to run appid {entry.SteamAppId} ('{entry.Name}')");

        // Steam starts the game itself (possibly after an update/cloud sync), so there's no process to
        // capture up front - wait for a Steam-family game window, the same detection Big Picture
        // sessions use, and follow its owner from there.
        var deadline = DateTime.UtcNow + SteamGameWindowTimeout;
        while (DateTime.UtcNow < deadline)
        {
            if (steam.FindActiveContentWindow() == true && steam.LastActiveContentProcessId != 0)
                return TrackedProcess.Capture(steam.LastActiveContentProcessId, includeDescendants: true);
            await Task.Delay(1000, ct);
        }
        Logger.Log("GameLauncher", $"no game window from Steam appid {entry.SteamAppId} within {SteamGameWindowTimeout.TotalSeconds:0}s");
        return null;
    }

    /// <summary>Largest visible top-level window owned by the tracked process family, or Zero - used
    /// to hand the game real foreground focus once it shows up.</summary>
    public static IntPtr FindMainWindow(TrackedProcess game)
    {
        var family = new HashSet<int>(game.FamilyPids());
        if (family.Count == 0) return IntPtr.Zero;

        var best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows((hWnd, _) =>
        {
            if (!IsWindowVisible(hWnd)) return true;
            GetWindowThreadProcessId(hWnd, out var pid);
            if (!family.Contains((int)pid)) return true;
            GetWindowRect(hWnd, out var r);
            var area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = hWnd; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    /// <summary>A minimized window produces no capture frames - the stall watchdog must not read that
    /// as a hung encoder.</summary>
    public static bool IsMinimized(IntPtr hwnd) => hwnd != IntPtr.Zero && IsIconic(hwnd);

    private static string? NonEmpty(string? value) => string.IsNullOrWhiteSpace(value) ? null : value;

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);
}
