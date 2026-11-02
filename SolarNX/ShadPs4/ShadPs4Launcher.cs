using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace SolarNX.ShadPs4;

/// <summary>
/// Launches shadPS4 (https://shadps4.net) - a portable, installer-less PS4 emulator - on the
/// normal interactive desktop, exactly like SteamLauncher does for Steam/Big Picture but without
/// any of that class's Steam-Input-specific machinery (no focus-hook DLL injection, no DWM/game-
/// window detection, no AttachThreadInput focus-stealing dance). shadPS4 is an ordinary single
/// top-level window, so a plain launch/focus/close is all v1 needs; whether it keeps accepting the
/// virtual gamepad while unfocused is unverified and should be checked live before relying on it.
/// </summary>
public sealed class ShadPs4Launcher
{
    // TitleIds are always short alphanumeric strings (e.g. "CUSA03173") - this is validated
    // defense-in-depth given titleId ultimately arrives over the network (see
    // ShadPs4LibraryChannel/LibraryProtocol), even though the wire format already caps its length.
    // Anything failing this is treated as "no titleId" (falls back to the generic launch below)
    // rather than ever reaching ProcessStartInfo.Arguments unvalidated.
    private static readonly Regex TitleIdPattern = new(@"^[A-Za-z0-9]{1,16}$", RegexOptions.Compiled);

    /// <summary>
    /// Launches shadPS4 if it isn't already running, or brings its window to the front if it is.
    /// Returns false (without launching anything) if <paramref name="exePath"/> is empty or the
    /// file doesn't exist - shadPS4 has no registry/install-path auto-detect (it's a portable zip,
    /// see the class doc comment), so an unset/wrong path is an expected, user-fixable state rather
    /// than an error to throw on.
    ///
    /// <paramref name="titleId"/> is currently always passed as null by AppOrchestrator - the
    /// generic launch below now uses shadPS4's own `--big-picture` mode (its native,
    /// gamepad-navigable game list) instead of our own Switch-side picker screen, so the user picks
    /// the game inside shadPS4 itself rather than on the Switch beforehand. The titleId-driven
    /// direct-launch path (deprecated, not deleted - see ShadPs4LibraryChannel/LibraryProtocol/
    /// library.c on the Switch, also deprecated/unwired) is kept intact below in case it's worth
    /// reviving: when non-null, it launches straight into that specific game via CLI (confirmed
    /// live: `shadPS4.exe CUSA03173` boots directly into the game, bypassing shadPS4's own GUI).
    /// If shadPS4 is already running, a titleId can't be injected into the existing window - this
    /// just refocuses it exactly like the title-less path always has.
    /// </summary>
    public Task<bool> LaunchOrFocusAsync(string exePath, string? titleId, CancellationToken ct)
    {
        if (string.IsNullOrWhiteSpace(exePath) || !File.Exists(exePath))
        {
            Logger.Log("ShadPs4Launcher", $"LaunchOrFocusAsync: exePath not configured or missing ('{exePath}')");
            return Task.FromResult(false);
        }

        if (CandidateProcessNames(exePath).Any(name => Process.GetProcessesByName(name).Length > 0))
        {
            Focus(exePath);
            return Task.FromResult(true);
        }

        if (!string.IsNullOrEmpty(titleId))
        {
            if (TitleIdPattern.IsMatch(titleId))
            {
                var directExe = ResolveRealShadPs4Exe(exePath);
                if (directExe is not null)
                {
                    // shadPS4QtLauncher.exe (the Qt-based GUI wrapper, currently checked first by
                    // KnownExeNames) has a known upstream bug where it doesn't forward CLI args to
                    // the real emulator (shadps4-emu/shadPS4#3756) - direct-title launches must
                    // target the real shadPS4.exe specifically, resolved as a sibling of whatever's
                    // configured if that itself isn't already shadPS4.exe.
                    //
                    // --fullscreen true is passed explicitly rather than relying on config.toml's/
                    // config.json's own Fullscreen setting - confirmed live that a bare CLI boot
                    // doesn't honor it the way a normal GUI-driven launch does. This was suspected of
                    // breaking the capture pipeline's gfxcapture (exclusive fullscreen vs. Windows
                    // Graphics Capture) and reverted once, but the actual root cause of that live
                    // video breakage turned out to be unrelated (a DualShock4 output-report queue
                    // never being drained, fixed in VirtualGamepadHub.PlugInDs4/DrainDs4OutputReports)
                    // - confirmed by the user that fullscreen itself wasn't the culprit.
                    Process.Start(new ProcessStartInfo(directExe)
                    {
                        Arguments = $"{titleId} --fullscreen true",
                        UseShellExecute = true,
                        WorkingDirectory = Path.GetDirectoryName(directExe)!,
                    });
                    Logger.Log("ShadPs4Launcher", $"launched shadPS4 directly into title '{titleId}' ({directExe})");
                    return Task.FromResult(true);
                }

                Logger.Log("ShadPs4Launcher", $"titleId '{titleId}' given but no sibling shadPS4.exe found next to '{exePath}' - falling back to generic launch");
            }
            else
            {
                Logger.Log("ShadPs4Launcher", $"titleId '{titleId}' failed validation - falling back to generic launch");
            }
        }

        // --big-picture (confirmed live: shows a "shadPS4 Big Picture Mode" window - shadPS4's own
        // gamepad-navigable game list) needs the real shadPS4.exe specifically, same reason as the
        // titleId path above - the Qt launcher doesn't forward CLI args. Falls back to whatever
        // exePath itself is (unchanged, no arguments) if no sibling shadPS4.exe can be found, rather
        // than failing the whole launch over one missing flag.
        //
        // WorkingDirectory is set to the exe's own folder to match how a user normally
        // double-clicks it directly - shadPS4 is a portable app (no installer), though contrary to
        // what an earlier version of this comment assumed, its actual user config/state lives in
        // %APPDATA%\shadPS4\config.toml or config.json (confirmed live), not in a folder created
        // next to the exe.
        var bigPictureDirectExe = ResolveRealShadPs4Exe(exePath);
        var bigPictureExe = bigPictureDirectExe ?? exePath;
        Process.Start(new ProcessStartInfo(bigPictureExe)
        {
            // Only pass the flag when we know bigPictureExe is genuinely shadPS4.exe (resolution
            // succeeded) - if it failed and we fell back to launching exePath as-is, exePath might
            // be the Qt launcher, which would silently ignore/mishandle this argument.
            Arguments = bigPictureDirectExe is not null ? "--big-picture" : "",
            UseShellExecute = true,
            WorkingDirectory = Path.GetDirectoryName(bigPictureExe)!,
        });
        Logger.Log("ShadPs4Launcher", $"launched shadPS4 ({bigPictureExe}{(bigPictureDirectExe is not null ? " --big-picture" : "")})");
        return Task.FromResult(true);
    }

    /// <summary>Resolves the real shadPS4.exe to use for a direct-title CLI launch: exePath itself
    /// if it's already shadPS4.exe, otherwise a sibling shadPS4.exe next to it (covers the common
    /// case where the configured exePath is shadPS4QtLauncher.exe). Returns null if no such exe can
    /// be found - the caller falls back to the generic launch rather than failing outright.</summary>
    private static string? ResolveRealShadPs4Exe(string exePath)
    {
        if (string.Equals(Path.GetFileName(exePath), "shadPS4.exe", StringComparison.OrdinalIgnoreCase))
            return exePath;

        var directory = Path.GetDirectoryName(exePath);
        if (directory is null) return null;

        var candidate = Path.Combine(directory, "shadPS4.exe");
        return File.Exists(candidate) ? candidate : null;
    }

    /// <summary>The configured exe's own process name, plus "shadPS4" - the real emulator's actual
    /// process name, which can differ from the configured exePath whenever a titleId launch ran
    /// (that path always launches shadPS4.exe directly regardless of what's configured - see
    /// ResolveRealShadPs4Exe). Checking both means an already-running process is found/focused
    /// correctly no matter which of the two ways it was most recently launched.</summary>
    private static IEnumerable<string> CandidateProcessNames(string exePath)
    {
        var configuredName = Path.GetFileNameWithoutExtension(exePath);
        yield return configuredName;
        if (!string.Equals(configuredName, "shadPS4", StringComparison.OrdinalIgnoreCase))
            yield return "shadPS4";
    }

    /// <summary>Brings shadPS4's window to the foreground if it's already running. No
    /// AttachThreadInput dance (unlike SteamLauncher.ForceForegroundWindow) - accepted v1
    /// simplification; Windows' focus-stealing prevention may occasionally ignore this.</summary>
    public void Focus(string exePath)
    {
        var hwnd = FindMainWindow(exePath);
        if (hwnd == IntPtr.Zero) return;

        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    }

    /// <summary>
    /// Polls for shadPS4's main window, up to timeout. Much simpler than
    /// SteamLauncher.WaitForWindowAsync's EnumWindows/class-name matching - the exact process we
    /// launched is already known, so there's no Big-Picture-vs-game window ambiguity to resolve.
    /// </summary>
    public async Task<IntPtr> WaitForWindowAsync(string exePath, TimeSpan timeout, CancellationToken ct)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            var hwnd = FindMainWindow(exePath);
            if (hwnd != IntPtr.Zero) return hwnd;
            await Task.Delay(300, ct);
        }
        Logger.Log("ShadPs4Launcher", "WaitForWindowAsync: timed out without finding shadPS4 window");
        return IntPtr.Zero;
    }

    /// <summary>
    /// Closes shadPS4's window - called when the client disconnects. Unlike Steam (where Steam
    /// itself stays running and only Big Picture is exited), shadPS4 has no persistent background
    /// process to preserve - it's one ordinary window, so closing it is simply "close the app."
    /// </summary>
    public void Close(string exePath)
    {
        var hwnd = FindMainWindow(exePath);
        if (hwnd == IntPtr.Zero)
        {
            Logger.Log("ShadPs4Launcher", "Close: no shadPS4 window found, nothing to close");
            return;
        }

        PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        Logger.Log("ShadPs4Launcher", $"Close: WM_CLOSE posted to hwnd={hwnd}");
    }

    /// <summary>Opens shadPS4's official download page in the default browser - used when
    /// AppOrchestrator reports the exe path isn't configured/found (see MainForm's
    /// ShadPs4NotConfigured handler).</summary>
    public static void OpenDownloadPage()
    {
        Process.Start(new ProcessStartInfo("https://shadps4.net/downloads/") { UseShellExecute = true });
    }

    /// <summary>Filenames shadPS4's official portable zip has shipped its executable under -
    /// "shadPS4QTLauncher.exe" is the current Qt-based launcher; "shadPS4.exe" covers older/renamed
    /// builds. Checked in this order wherever a candidate folder is scanned.</summary>
    private static readonly string[] KnownExeNames = { "shadPS4QTLauncher.exe", "shadPS4.exe" };

    /// <summary>
    /// Best-effort scan for an already-extracted shadPS4 install, since there's no registry entry
    /// to consult (see class doc comment) - checks a handful of common places a user would have
    /// extracted the portable zip to (Desktop, Downloads, Program Files, drive roots), plus any
    /// top-level folder at those locations whose name contains "shadps4". Not exhaustive by design -
    /// a portable app can end up literally anywhere, so this only ever saves the common case; the
    /// manual "Обзор" picker in MainForm remains the reliable fallback. Returns null if nothing
    /// matched.
    /// </summary>
    public static string? TryAutoDetect()
    {
        var roots = new List<string>
        {
            Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"),
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
        };

        try
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType == DriveType.Fixed && drive.IsReady)
                {
                    roots.Add(drive.RootDirectory.FullName);
                }
            }
        }
        catch (IOException)
        {
            // Best-effort - a drive enumeration failure just means fewer roots get checked.
        }

        foreach (var root in roots)
        {
            if (string.IsNullOrEmpty(root) || !Directory.Exists(root)) continue;

            // The root itself might already be the extracted folder (e.g. the user extracted
            // straight into a drive root or Downloads with no wrapping subfolder).
            foreach (var exeName in KnownExeNames)
            {
                var direct = Path.Combine(root, exeName);
                if (File.Exists(direct)) return direct;
            }

            IEnumerable<string> subdirs;
            try
            {
                subdirs = Directory.EnumerateDirectories(root);
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException)
            {
                continue;
            }

            foreach (var dir in subdirs)
            {
                if (Path.GetFileName(dir).IndexOf("shadps4", StringComparison.OrdinalIgnoreCase) < 0) continue;

                foreach (var exeName in KnownExeNames)
                {
                    var candidate = Path.Combine(dir, exeName);
                    if (File.Exists(candidate)) return candidate;
                }
            }
        }

        return null;
    }

    private static IntPtr FindMainWindow(string exePath)
    {
        foreach (var processName in CandidateProcessNames(exePath))
        {
            foreach (var process in Process.GetProcessesByName(processName))
            {
                if (process.MainWindowHandle != IntPtr.Zero) return process.MainWindowHandle;
            }
        }
        return IntPtr.Zero;
    }

    // --- Win32 interop ---

    private const uint WM_CLOSE = 0x0010;
    private const int SW_RESTORE = 9;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
