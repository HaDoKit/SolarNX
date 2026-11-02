using System.Runtime.InteropServices;
using SolarNX.UI.main;

namespace SolarNX;

internal static class Program
{
    private const int SwRestore = 9;

    [STAThread]
    private static void Main(string[] args)
    {
        // Windows invokes this exe again (with this argument) when it resolves the ms-gamebar/
        // ms-gamebarservices protocol stubs registered by GameBarSuppressor - do absolutely nothing
        // and exit immediately, no window, no tray icon, nothing for the first user to notice.
        if (Array.IndexOf(args, "--protocol-stub") >= 0) return;

        // Only one instance may run at a time - a second copy would fight the first over the same
        // ffmpeg/virtual-gamepad/tray-icon resources. The mutex name is process-wide but session-local
        // (no "Global\" prefix), matching how this app is actually used: one user, one desktop session.
        using var singleInstanceMutex = new Mutex(true, "Local\\SolarNX-SingleInstance", out var isFirstInstance);
        if (!isFirstInstance)
        {
            ActivateExistingInstance();
            return;
        }

        ApplicationConfiguration.Initialize();

        // Hidden switch, not a Config field - the ports section it reveals in Settings is a
        // developer/troubleshooting tool (matching non-default Switch-side ports), not something
        // regular use ever needs, so it doesn't persist and isn't discoverable in the UI itself.
        var devMode = Array.IndexOf(args, "--dev") >= 0;

        // Always 48000 - the patched Switch client's audio.c decode/resample path is compiled in
        // around that exact rate (see AudioCaptureSender/SettingsView) - overriding here means a
        // stale non-default value from an old config.json can't silently break audio.
        Logger.Log("App", $"SolarNX {AppVersion.Current}");
        var config = Config.Load();
        config.AudioSampleRate = 48000;
        var orchestrator = new AppOrchestrator(config);

        using var appIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application;

        using var trayIcon = new NotifyIcon
        {
            Icon = appIcon,
            Text = "SolarNX",
            Visible = true,
        };

        // WPF app/window, not WinForms - App.xaml merges the WPF-UI theme/control dictionaries
        // that MainWindow's controls render against. No StartupUri on App.xaml, so nothing shows
        // until Run(mainWindow) below - config/orchestrator/tray must exist first (MainWindow binds
        // to them in its constructor).
        var app = new App();
        app.InitializeComponent();
        app.ShutdownMode = System.Windows.ShutdownMode.OnExplicitShutdown;
        Loc.Apply(config.Language);

        var mainWindow = new MainWindow(config, orchestrator, trayIcon, devMode);

        var menu = new ContextMenuStrip();
        var openItem = menu.Items.Add(Loc.T("Tray.Open"), null, (_, _) => mainWindow.RestoreFromTray());
        menu.Items.Add(new ToolStripSeparator());
        var exitItem = menu.Items.Add(Loc.T("Tray.Exit"), null, (_, _) =>
        {
            mainWindow.AllowClose = true;
            mainWindow.Close();
            app.Shutdown();
        });
        Loc.Changed += () =>
        {
            openItem.Text = Loc.T("Tray.Open");
            exitItem.Text = Loc.T("Tray.Exit");
        };
        trayIcon.ContextMenuStrip = menu;
        trayIcon.DoubleClick += (_, _) => mainWindow.RestoreFromTray();

        app.Exit += (_, _) => orchestrator.Dispose();
        // A real Windows logoff/shutdown must actually close the window instead of Hide()-ing it
        // to tray the way a user clicking the close button does - see MainWindow.AllowClose.
        app.SessionEnding += (_, _) => mainWindow.AllowClose = true;

        app.Run(mainWindow);
    }

    /// <summary>Best-effort: brings the already-running instance's window to the front by title.
    /// Works even while that window is hidden in the tray (FindWindow isn't limited to visible
    /// windows). SetForegroundWindow can be silently ignored by Windows' foreground-lock rules for
    /// a background process stealing focus - if that happens the existing instance simply stays
    /// wherever it was (tray), which is still correct: there's still exactly one process running.</summary>
    private static void ActivateExistingInstance()
    {
        var hwnd = FindWindow(null, "SolarNX");
        if (hwnd == IntPtr.Zero) return;

        ShowWindow(hwnd, SwRestore);
        SetForegroundWindow(hwnd);
    }

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr FindWindow(string? lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);
}
