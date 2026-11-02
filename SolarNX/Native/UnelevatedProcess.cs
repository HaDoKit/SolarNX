using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace SolarNX.Native;

/// <summary>
/// Starts a process as the ordinary (non-elevated) desktop user although SolarNX itself runs as
/// administrator (app.manifest: requireAdministrator) - anything started the usual way inherits
/// that, which a game rarely needs and which anti-cheats, the Steam overlay and drag-and-drop from
/// Explorer often dislike. The token is borrowed from the shell (Explorer), the same way Windows
/// starts a program on a double-click; nothing stays open afterwards.
/// </summary>
internal static class UnelevatedProcess
{
    private const int ErrorElevationRequired = 740;

    /// <summary>The new process's id. Throws Win32Exception on failure; NativeErrorCode 740
    /// (<see cref="IsElevationRequired"/>) means the exe's own manifest demands administrator.</summary>
    public static int Start(string exePath, string arguments, string? workingDirectory, bool hideWindow = false)
    {
        var shell = GetShellWindow();
        if (shell == IntPtr.Zero) throw new Win32Exception("no desktop shell (Explorer isn't running)");
        GetWindowThreadProcessId(shell, out var shellPid);

        var shellProcess = IntPtr.Zero;
        var shellToken = IntPtr.Zero;
        var token = IntPtr.Zero;
        try
        {
            shellProcess = OpenProcess(ProcessQueryInformation, false, shellPid);
            if (shellProcess == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (!OpenProcessToken(shellProcess, TokenDuplicate, out shellToken)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (!DuplicateTokenEx(shellToken, PrimaryTokenAccess, IntPtr.Zero, SecurityImpersonation, TokenPrimary, out token))
                throw new Win32Exception(Marshal.GetLastWin32Error());

            var startup = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
            // A buffer, not a string: the function may write into the command line.
            var commandLine = new StringBuilder($"\"{exePath}\" {arguments}".TrimEnd());
            var flags = CreateUnicodeEnvironment | (hideWindow ? CreateNoWindow : 0);
            if (!CreateProcessWithTokenW(token, 0, exePath, commandLine, flags, IntPtr.Zero,
                    string.IsNullOrEmpty(workingDirectory) ? null : workingDirectory, ref startup, out var info))
                throw new Win32Exception(Marshal.GetLastWin32Error());

            CloseHandle(info.hThread);
            CloseHandle(info.hProcess); // followed by pid via TrackedProcess, never by a held handle
            return info.dwProcessId;
        }
        finally
        {
            if (token != IntPtr.Zero) CloseHandle(token);
            if (shellToken != IntPtr.Zero) CloseHandle(shellToken);
            if (shellProcess != IntPtr.Zero) CloseHandle(shellProcess);
        }
    }

    public static bool IsElevationRequired(Exception ex) =>
        ex is Win32Exception { NativeErrorCode: ErrorElevationRequired };

    private const uint ProcessQueryInformation = 0x0400;
    private const uint TokenDuplicate = 0x0002;
    // TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID
    private const uint PrimaryTokenAccess = 0x0001 | 0x0002 | 0x0008 | 0x0080 | 0x0100;
    private const int SecurityImpersonation = 2;
    private const int TokenPrimary = 1;
    private const uint CreateUnicodeEnvironment = 0x00000400;
    private const uint CreateNoWindow = 0x08000000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb;
        public string? lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetShellWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool DuplicateTokenEx(IntPtr existingToken, uint access, IntPtr tokenAttributes,
        int impersonationLevel, int tokenType, out IntPtr newToken);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcessWithTokenW(IntPtr token, uint logonFlags, string? applicationName,
        StringBuilder commandLine, uint creationFlags, IntPtr environment, string? currentDirectory,
        ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}
