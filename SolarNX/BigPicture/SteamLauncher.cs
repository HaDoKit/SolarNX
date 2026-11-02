using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using SolarNX.Native;

namespace SolarNX.BigPicture;

/// <summary>
/// Launches Steam (and therefore Big Picture) on the normal interactive desktop and leaves its
/// windows exactly where Steam itself puts them - no hiding, no moving onto a separate virtual
/// monitor. Big Picture/games are visible on the real screen.
/// </summary>
public sealed class SteamLauncher
{
    // Matched by window class only, not title: Big Picture's SDL_app window can read "Steam"
    // instead of "Big Picture" during a transitional/returning-to-Big-Picture state, and "SDL_app"
    // is never used by more than one visible top-level window at a time, so the class name alone
    // is a reliable match.
    private static readonly string[] CandidateClassNames = { "SDL_app", "vguiPopupWindow" };

    /// <summary>
    /// Launches Steam if it isn't already running, then opens Big Picture. Deliberately does NOT
    /// shut down an already-running Steam first - a session's end only closes the game that was
    /// running and exits Big Picture (see CloseActiveGame/ExitBigPicture), leaving Steam itself
    /// running so the next connect can jump straight back into Big Picture without a relaunch.
    /// </summary>
    public async Task LaunchSteamAsync(CancellationToken ct)
    {
        if (!await EnsureSteamRunningAsync(ct)) return;

        OpenBigPicture();
        Logger.Log("SteamLauncher", "LaunchSteamAsync: OpenBigPicture invoked, returning");
    }

    /// <summary>Starts Steam (silently, no Big Picture) if it isn't running, and gives it time to reach
    /// its "ready for steam:// IPC" state. False if Steam's install path isn't in the registry. Also
    /// used by library entries of type Steam, which go straight to steam://rungameid.</summary>
    public async Task<bool> EnsureSteamRunningAsync(CancellationToken ct)
    {
        if (Process.GetProcessesByName("steam").Length > 0) return true;

        var steamExe = GetSteamExePath();
        Logger.Log("SteamLauncher", $"EnsureSteamRunningAsync: steamExe={steamExe ?? "(null)"}");
        if (steamExe is null)
        {
            Logger.Log("SteamLauncher", "Could not find Steam install path in registry (HKCU\\Software\\Valve\\Steam\\SteamExe)");
            return false;
        }

        Process.Start(new ProcessStartInfo(steamExe, "-silent") { UseShellExecute = true });
        Logger.Log("SteamLauncher", $"launched Steam ({steamExe})");

        // Give the client a moment to reach its "ready for IPC" state before sending it a steam:// URI.
        await Task.Delay(4000, ct);
        return true;
    }

    public void OpenBigPicture()
    {
        Process.Start(new ProcessStartInfo("steam://open/bigpicture") { UseShellExecute = true });
    }

    /// <summary>
    /// Leaves Big Picture Mode back to Steam's normal desktop UI - called when the client
    /// disconnects. The undocumented "steam://exitbigpicture" URI is a silent no-op on the current
    /// Steam client, so this posts WM_CLOSE to the Big Picture window instead, which correctly
    /// leaves Big Picture rather than quitting Steam entirely.
    /// </summary>
    /// <returns>The window WM_CLOSE was posted to, or IntPtr.Zero if none was found - lets a caller
    /// that wants to reopen Big Picture wait for exactly this window to go away first (see
    /// WaitForWindowClosedAsync).</returns>
    public IntPtr ExitBigPicture()
    {
        var hwnd = FindBigPictureWindow();
        if (hwnd == IntPtr.Zero)
        {
            Logger.Log("SteamLauncher", "ExitBigPicture: no Big Picture window found, nothing to close");
            return IntPtr.Zero;
        }

        PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        Logger.Log("SteamLauncher", $"ExitBigPicture: WM_CLOSE posted to hwnd={hwnd}");
        return hwnd;
    }

    /// <summary>Polls until <paramref name="hwnd"/> no longer exists. True if it went away within
    /// <paramref name="timeout"/>, false if it's still there. Waits on that specific window, not
    /// on "no Big Picture-classed window at all" - FindBigPictureWindow also matches unrelated
    /// vguiPopupWindow dialogs (e.g. Steam's own "steamwebhelper is not responding" box), which
    /// would make that kind of wait run to its full timeout.</summary>
    public static async Task<bool> WaitForWindowClosedAsync(IntPtr hwnd, TimeSpan timeout, CancellationToken ct)
    {
        if (hwnd == IntPtr.Zero) return true;

        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (!IsWindow(hwnd)) return true;
            await Task.Delay(200, ct);
        }
        return !IsWindow(hwnd);
    }

    /// <summary>
    /// Polls for the Big Picture window. Returns its handle, or IntPtr.Zero if it never appeared.
    ///
    /// Re-fires OpenBigPicture() every ~10s while still waiting, instead of trusting the single
    /// call the caller already made before this loop started - confirmed live (RestartBigPicture-
    /// AfterGameClosed's very first test): steam://open/bigpicture IPC to an already-running Steam
    /// instance can silently go nowhere, and without a retry this polls the full timeout for a
    /// window that was never actually going to appear.
    /// </summary>
    public async Task<IntPtr> WaitForWindowAsync(TimeSpan timeout, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        var nextReopenAtUtc = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            var hwnd = FindBigPictureWindow();
            if (hwnd != IntPtr.Zero) return hwnd;

            if (DateTime.UtcNow >= nextReopenAtUtc)
            {
                Logger.Log("SteamLauncher", "WaitForWindowAsync: still no Big Picture window - re-firing OpenBigPicture()");
                OpenBigPicture();
                nextReopenAtUtc = DateTime.UtcNow.AddSeconds(10);
            }

            await Task.Delay(300, ct);
        }
        Logger.Log("SteamLauncher", "WaitForWindowAsync: timed out without finding Big Picture window");
        return IntPtr.Zero;
    }

    /// <summary>
    /// Allow-list, not deny-list: a candidate window only counts as "content" if its owning
    /// process is steam.exe/steamwebhelper.exe by name, its executable lives under the Steam
    /// installation directory (covers games in the default library), or it's a process-tree
    /// descendant of steam.exe/steamwebhelper.exe (covers non-Steam-game shortcuts and games
    /// installed to a secondary library elsewhere on disk - confirmed live: a non-Steam-game
    /// shortcut whose exe lives entirely outside the Steam install dir, e.g. "Luna Abyss", was
    /// never recognized as a game by the path-only check, so its true isGame transition was
    /// never observed - the game-launched/game-closed recovery in AppOrchestrator
    /// (ForceForegroundWindow, SendBackNavigation) never fired, and
    /// CloseActiveGame below never closed it on disconnect either). Anything outside all three
    /// checks, no matter how large or long-lived, is left completely alone - a deny-list is not
    /// safe enough here, it risks
    /// grabbing an unrelated large visible window (e.g. the user's own applications).
    /// </summary>
    private static readonly string[] SteamProcessNames = { "steam", "steamwebhelper" };

    private const int MaxAncestryHops = 10;

    private static bool IsSteamFamilyProcess(Process process, string? steamInstallDir, IReadOnlyDictionary<int, int> parentPidMap, IReadOnlySet<int> steamRootPids)
    {
        if (Array.IndexOf(SteamProcessNames, process.ProcessName) >= 0) return true;

        if (steamInstallDir is not null)
        {
            // ProcessImagePath, not process.MainModule - see its doc comment (MainModule needs a
            // memory-read handle to what is usually a game process). Null (access denied, process
            // exited mid-query) falls through to the ancestry check rather than bailing.
            var exePath = ProcessImagePath.TryGet(process.Id);
            if (exePath is not null && exePath.StartsWith(steamInstallDir, StringComparison.OrdinalIgnoreCase)) return true;
        }

        return IsDescendantOfSteam(process.Id, parentPidMap, steamRootPids);
    }

    /// <summary>Walks the process's ancestor chain (via a pid-&gt;parent-pid map built in one WMI
    /// query - see BuildParentPidMap) up to MaxAncestryHops looking for a currently-running
    /// steam.exe/steamwebhelper.exe pid. Steam always launches a game as a descendant of itself
    /// regardless of where the game's exe lives on disk, so this catches non-Steam-game shortcuts
    /// and secondary-library installs that the install-directory path check above misses. Bounded
    /// hop count guards against a cycle or a stale/missing map entry spinning forever.</summary>
    private static bool IsDescendantOfSteam(int pid, IReadOnlyDictionary<int, int> parentPidMap, IReadOnlySet<int> steamRootPids)
    {
        var current = pid;
        for (var hop = 0; hop < MaxAncestryHops; hop++)
        {
            if (!parentPidMap.TryGetValue(current, out var parentPid)) return false;
            if (steamRootPids.Contains(parentPid)) return true;
            current = parentPid;
        }
        return false;
    }

    /// <summary>One-shot snapshot of every running steam.exe/steamwebhelper.exe pid - the set of
    /// "roots" IsDescendantOfSteam walks up towards.</summary>
    private static HashSet<int> GetSteamRootPids()
    {
        var ids = new HashSet<int>();
        foreach (var name in SteamProcessNames)
        {
            foreach (var process in Process.GetProcessesByName(name))
            {
                ids.Add(process.Id);
            }
        }
        return ids;
    }

    /// <summary>Builds the whole system's pid-&gt;parent-pid map in a single WMI query, rather than
    /// one query per ancestry hop per candidate process - the latter was confirmed live (on the
    /// isolated-desktop branch, same underlying cost) to cause multi-second stalls when done
    /// per-process instead.</summary>
    private static Dictionary<int, int> BuildParentPidMap()
    {
        var map = new Dictionary<int, int>();
        try
        {
            using var searcher = new System.Management.ManagementObjectSearcher("SELECT ProcessId, ParentProcessId FROM Win32_Process");
            foreach (System.Management.ManagementObject obj in searcher.Get())
            {
                map[Convert.ToInt32(obj["ProcessId"])] = Convert.ToInt32(obj["ParentProcessId"]);
            }
        }
        catch
        {
            // WMI unavailable/query failed - ancestry check degrades to "no match" for everything;
            // callers still have the name/install-dir checks.
        }
        return map;
    }

    /// <summary>The hwnd FindActiveContentWindow picked on its most recent call (IntPtr.Zero if
    /// none). Exposed so AppOrchestrator can target a real, one-shot ForceForegroundWindow call at
    /// exactly this window on the Big-Picture-to-game transition.</summary>
    public IntPtr LastActiveContentWindow { get; private set; }

    /// <summary>Owning process id of <see cref="LastActiveContentWindow"/> (0 if none) - lets
    /// AppOrchestrator track the game *process* rather than its window, see its _trackedGame.</summary>
    public int LastActiveContentProcessId { get; private set; }

    /// <summary>
    /// Finds whichever visible top-level window belonging to the Steam family is currently the
    /// "real content" - Big Picture's own window while navigating menus, or a launched game's
    /// window once one is running. Launching a game from Big Picture opens a second, separate
    /// top-level window (e.g. class "HL_WIN"), which is why this re-picks every tick. Rather than hardcode per-engine window
    /// classes, this picks the largest visible Steam-family window above a size floor (excludes
    /// small utility/overlay windows like the ~50x50 UAC input indicator) - the actual content
    /// (Big Picture or a game) is always fullscreen-sized. Windows are left exactly where
    /// Steam/the game itself puts them - no repositioning.
    ///
    /// Returns whether the currently active content window belongs to an actual game (true) or to
    /// steam.exe/steamwebhelper.exe itself, i.e. Big Picture (false) - null if no Steam-family
    /// content window was found at all. Used by AppOrchestrator to detect the game-just-closed
    /// transition (true -&gt; false) and kick the virtual gamepad, since Steam Input's own
    /// per-context (game vs. Big Picture) controller binding handoff doesn't always pick a ViGEm
    /// virtual controller back up cleanly on its own.
    /// </summary>
    public bool? FindActiveContentWindow()
    {
        const int minDimension = 400;
        var steamInstallDir = Path.GetDirectoryName(GetSteamExePath());
        var parentPidMap = BuildParentPidMap();
        var steamRootPids = GetSteamRootPids();

        var best = IntPtr.Zero;
        var bestArea = 0L;

        EnumWindowsProc callback = (hWnd, _) =>
        {
            if (!IsWindowVisible(hWnd)) return true;
            GetWindowRect(hWnd, out var rect);
            var width = rect.Right - rect.Left;
            var height = rect.Bottom - rect.Top;
            if (width < minDimension || height < minDimension) return true;

            GetWindowThreadProcessId(hWnd, out var ownerPid);
            try
            {
                var process = Process.GetProcessById((int)ownerPid);
                if (!IsSteamFamilyProcess(process, steamInstallDir, parentPidMap, steamRootPids)) return true;
            }
            catch (ArgumentException) { return true; /* process exited between EnumWindows and this check */ }

            var area = (long)width * height;
            if (area > bestArea)
            {
                bestArea = area;
                best = hWnd;
            }
            return true;
        };

        EnumWindows(callback, IntPtr.Zero);

        LastActiveContentWindow = best;
        LastActiveContentProcessId = 0;
        if (best == IntPtr.Zero) return null;

        GetWindowThreadProcessId(best, out var bestOwnerPid);
        LastActiveContentProcessId = (int)bestOwnerPid;
        try
        {
            var ownerProcess = Process.GetProcessById((int)bestOwnerPid);
            var isGame = Array.IndexOf(SteamProcessNames, ownerProcess.ProcessName) < 0;

            // Diagnostic: logs what this heuristic picks every tick alongside real OS foreground,
            // to catch "best" flipping back to Big Picture while a game is running.
            var classNameSb = new StringBuilder(64);
            GetClassName(best, classNameSb, classNameSb.Capacity);
            var realForeground = GetForegroundWindow();
            GetWindowThreadProcessId(realForeground, out var realForegroundPid);
            string realForegroundProcessName;
            try { realForegroundProcessName = Process.GetProcessById((int)realForegroundPid).ProcessName; }
            catch { realForegroundProcessName = "?"; }
            Logger.Log("SteamLauncher", $"tick: best=0x{best:X} class={classNameSb} process={ownerProcess.ProcessName} isGame={isGame} | realForeground=0x{realForeground:X} process={realForegroundProcessName}");

            // FullscreenOptimizationFix (auto-setting "Disable fullscreen optimizations" on every
            // detected game exe) used to run here - removed: it did not fix the capture problem it
            // was added for (Balatro), and confirmed live it broke a game that captured fine
            // before (Windblown went invisible to capture from its next launch on - the flag pushes
            // it into true exclusive fullscreen, which WGC cannot see). Its registry entries were
            // also cleaned out of HKCU\...\AppCompatFlags\Layers.
            if (isGame)
            {
                // Diagnostic: GetWindowDisplayAffinity reports whether the window itself asked
                // Windows to exclude it from any capture API - WDA_EXCLUDEFROMCAPTURE (0x11). If a
                // game sets this, gfxcapture (or any other capture method) cannot see it at all -
                // no software fix on this end is possible. Logged once per detected exe.
                if (GetWindowDisplayAffinity(best, out var affinity))
                {
                    Logger.Log("SteamLauncher", $"game window display affinity for {ownerProcess.ProcessName} (hwnd={best}): {affinity} ({(affinity == WDA_EXCLUDEFROMCAPTURE ? "EXCLUDED FROM CAPTURE" : "capturable")})");
                }
            }

            return isGame;
        }
        catch (ArgumentException)
        {
            /* process exited between finding the window and here */
            return null;
        }
    }

    /// <summary>
    /// Closes whatever game is currently running under Steam (if any) - called when the client
    /// disconnects, alongside ExitBigPicture. Deliberately does NOT touch
    /// steam.exe/steamwebhelper.exe itself (see IsSteamFamilyProcess for why those are excluded
    /// here) - only an actual game process, identified via IsSteamFamilyProcess's install-directory
    /// path check or its process-ancestry check (the latter needed for non-Steam-game shortcuts and
    /// secondary-library installs - see IsSteamFamilyProcess's doc comment).
    /// </summary>
    public void CloseActiveGame()
    {
        var steamInstallDir = Path.GetDirectoryName(GetSteamExePath());
        var parentPidMap = BuildParentPidMap();
        var steamRootPids = GetSteamRootPids();

        foreach (var process in Process.GetProcesses())
        {
            try
            {
                if (Array.IndexOf(SteamProcessNames, process.ProcessName) >= 0) continue;
                if (!IsSteamFamilyProcess(process, steamInstallDir, parentPidMap, steamRootPids)) continue;

                Logger.Log("SteamLauncher", $"CloseActiveGame: closing {process.ProcessName} (pid={process.Id})");
                process.CloseMainWindow();
                if (!process.WaitForExit(3000)) process.Kill();
            }
            catch
            {
                // process already exited, access denied (different integrity level/bitness), or
                // similar - nothing more we can safely do for this one, move on to the next.
            }
        }
    }

    /// <summary>
    /// A genuine, one-shot real-OS-foreground-focus change, called by AppOrchestrator exactly once
    /// on the confirmed Big-Picture-to-game transition, targeting the exact hwnd that triggered it.
    /// Necessary because ViGEmBus's virtual XInput controller isn't focus-gated, but Steam Input's
    /// own per-context binding keys off *real* OS foreground - and nothing else in this app calls
    /// SetForegroundWindow, so real focus would otherwise stay wherever it was before the game
    /// launched (typically Big Picture's own window, since window creation alone doesn't grant
    /// foreground activation rights under Windows' focus-stealing prevention).
    ///
    /// SetForegroundWindow alone is usually refused by Windows' focus-stealing prevention unless the
    /// calling thread is already "attached" (via AttachThreadInput) to the thread that currently
    /// owns real foreground - this temporarily attaches to borrow that permission, the standard
    /// documented workaround for a background process needing to force a real focus change.
    /// </summary>
    public static void ForceForegroundWindow(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return;

        var foregroundHwnd = GetForegroundWindow();
        var foregroundThreadId = GetWindowThreadProcessId(foregroundHwnd, out _);
        var targetThreadId = GetWindowThreadProcessId(hwnd, out _);
        var currentThreadId = GetCurrentThreadId();

        var attachedToForeground = foregroundThreadId != currentThreadId && AttachThreadInput(currentThreadId, foregroundThreadId, true);
        var attachedToTarget = targetThreadId != currentThreadId && AttachThreadInput(currentThreadId, targetThreadId, true);
        try
        {
            var ok = SetForegroundWindow(hwnd);
            BringWindowToTop(hwnd);
            Logger.Log("SteamLauncher", $"ForceForegroundWindow(hwnd={hwnd}): SetForegroundWindow returned {ok}");
        }
        finally
        {
            if (attachedToForeground) AttachThreadInput(currentThreadId, foregroundThreadId, false);
            if (attachedToTarget) AttachThreadInput(currentThreadId, targetThreadId, false);
        }
    }

    /// <summary>
    /// Sends a real, OS-level Escape keypress via SendInput (not a posted message - see
    /// posted WM_ACTIVATE-family messages were confirmed insufficient for this UI once before). Big Picture's SDL3 UI accepts keyboard as a
    /// first-class navigation input alongside gamepad (Escape = Back).
    ///
    /// Used on the game-closed transition to back Big Picture off the just-closed game's own
    /// detail/store page. Confirmed live via C:\steam\logs\controller_ui.txt: while that page is
    /// showing, Steam Input loads that game's OWN controller config for it instead of Big
    /// Picture's generic navigation config (controller_base/basicui_gamepad.vdf) - harmless for a
    /// real Steam game shipping a proper Steam Input layout, but leaves the gamepad completely
    /// dead on that page for a non-Steam-game shortcut with no configured layout (its config is
    /// just a raw passthrough template - controller_base/templates/controller_xbox360_gamepad_
    /// joystick.vdf - with no menu-navigation binding at all). A real keyboard event sidesteps
    /// Steam Input's per-page controller-config selection entirely, since keyboard is a completely
    /// separate input path from the controller that Steam Input's config-per-appid logic doesn't
    /// touch.
    /// </summary>
    public static void SendBackNavigation()
    {
        var inputs = new[]
        {
            new INPUT { type = InputKeyboard, U = new InputUnion { ki = new KEYBDINPUT { wVk = VkEscape } } },
            new INPUT { type = InputKeyboard, U = new InputUnion { ki = new KEYBDINPUT { wVk = VkEscape, dwFlags = KeyEventFKeyUp } } },
        };
        var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>());
        Logger.Log("SteamLauncher", $"SendBackNavigation: SendInput sent={sent}/{inputs.Length}");
    }

    private static string? GetSteamExePath()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        return key?.GetValue("SteamExe") as string;
    }

    private static IntPtr FindBigPictureWindow()
    {
        IntPtr found = IntPtr.Zero;

        EnumWindowsProc callback = (hWnd, _) =>
        {
            if (!IsWindowVisible(hWnd)) return true;

            var className = new StringBuilder(256);
            GetClassName(hWnd, className, className.Capacity);

            var title = new StringBuilder(256);
            GetWindowText(hWnd, title, title.Capacity);

            var classStr = className.ToString();
            var titleStr = title.ToString();

            var classMatches = Array.IndexOf(CandidateClassNames, classStr) >= 0;

            if (classMatches)
            {
                found = hWnd;
                Logger.Log("SteamLauncher", $"matched hwnd={hWnd} class={classStr} title='{titleStr}'");
                return false; // stop enumeration
            }
            return true;
        };

        EnumWindows(callback, IntPtr.Zero);
        return found;
    }

    // --- Win32 interop ---

    private const uint WM_CLOSE = 0x0010;

    // Values SetWindowDisplayAffinity/GetWindowDisplayAffinity use (WDA_* constants).
    private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool GetWindowDisplayAffinity(IntPtr hWnd, out uint pdwAffinity);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    // --- SendInput (real, OS-level synthetic keyboard events - see SendBackNavigation) ---

    private const uint InputKeyboard = 1;
    private const uint KeyEventFKeyUp = 0x0002;
    private const ushort VkEscape = 0x1B;

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    // Size=32 pinned explicitly: on x64, INPUT's union is sized by its largest member
    // (MOUSEINPUT, 32 bytes with pointer-alignment padding), not KEYBDINPUT (24 bytes) - the
    // only member actually declared here. Without this, the union - and therefore INPUT - comes
    // out 8 bytes too small, cbSize no longer matches user32.dll's own sizeof(INPUT), and
    // SendInput silently rejects the whole call (confirmed live: returned sent=0/2).
    [StructLayout(LayoutKind.Explicit, Size = 32)]
    private struct InputUnion
    {
        [FieldOffset(0)] public KEYBDINPUT ki;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint type;
        public InputUnion U;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
}
