using System.Windows;
using SolarNX.ShadPs4;
using SolarNX.UI.Library;
using SolarNX.UI.Settings;

namespace SolarNX.UI.main;

/// <summary>
/// Thin shell: hosts the animated background plus three views (HomeView/SettingsView/LibraryView),
/// built once and toggled via Visibility rather than Frame/Page navigation - a fixed few-screen
/// app has no use for a navigation back-stack, and building SettingsView once (instead of per-open)
/// is also what makes SettingsView's shadPS4 auto-detect run exactly once, matching the old
/// MainForm's behavior, with no extra guard flag needed.
/// </summary>
public partial class MainWindow : Wpf.Ui.Controls.FluentWindow
{
    private readonly AppOrchestrator _orchestrator;
    private readonly NotifyIcon _trayIcon;
    private readonly HomeView _homeView;
    private readonly SettingsView _settingsView;
    private readonly LibraryView _libraryView;

    /// <summary>Only the tray "Выход" handler and a real Windows session-ending event set this to
    /// true - everywhere else, closing the window hides it to the tray instead (see Closing below),
    /// matching the old WinForms MainForm's close-to-tray behavior.</summary>
    public bool AllowClose { get; set; }

    public MainWindow(Config config, AppOrchestrator orchestrator, NotifyIcon trayIcon, bool devMode)
    {
        // Roughly a quarter of the screen, not full-screen, on launch - WindowStartupLocation=
        // CenterScreen (in XAML) centers using whatever size is set by the time the window is
        // shown, so this has to happen before that, not after.
        Width = SystemParameters.PrimaryScreenWidth * 0.25;
        Height = SystemParameters.PrimaryScreenHeight * 0.25;

        InitializeComponent();
        // Only the drawn title bar shows the version - Window.Title stays "SolarNX" because
        // Program.ActivateExistingInstance finds the running window by that exact title.
        AppTitleBar.Title = $"SolarNX {AppVersion.Current}";
        _orchestrator = orchestrator;
        _trayIcon = trayIcon;

        _homeView = new HomeView(config, orchestrator);
        _settingsView = new SettingsView(config, orchestrator, devMode) { Visibility = Visibility.Collapsed };
        _homeView.SettingsRequested += () =>
        {
            _homeView.Visibility = Visibility.Collapsed;
            _settingsView.Visibility = Visibility.Visible;
        };
        _settingsView.BackRequested += () =>
        {
            _settingsView.Visibility = Visibility.Collapsed;
            _homeView.Visibility = Visibility.Visible;
            _homeView.RefreshMode(); // the connection mode may have changed
        };
        _libraryView = new LibraryView(orchestrator.Library) { Visibility = Visibility.Collapsed };
        _homeView.LibraryRequested += () =>
        {
            _homeView.Visibility = Visibility.Collapsed;
            _libraryView.Visibility = Visibility.Visible;
        };
        _libraryView.BackRequested += () =>
        {
            _libraryView.Visibility = Visibility.Collapsed;
            _homeView.Visibility = Visibility.Visible;
        };
        ViewHost.Children.Add(_homeView);
        ViewHost.Children.Add(_settingsView);
        ViewHost.Children.Add(_libraryView);

        // Both fire on a thread-pool thread, not the UI thread (see AppOrchestrator's own doc
        // comments) - marshal via the Dispatcher, same as the old MainForm did via BeginInvoke.
        _orchestrator.ConnectionChanged += () => Dispatcher.BeginInvoke(RefreshTrayAndHome);
        _orchestrator.ShadPs4NotConfigured += () => Dispatcher.BeginInvoke(PromptShadPs4NotConfigured);
        RefreshTrayAndHome();

        Closing += (_, e) =>
        {
            if (AllowClose) return;
            e.Cancel = true;
            Hide();
        };
        StateChanged += (_, _) =>
        {
            if (WindowState == WindowState.Minimized) Hide();
        };
    }

    public void RestoreFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    private void RefreshTrayAndHome()
    {
        _homeView.RefreshStatus();
        _trayIcon.Text = _orchestrator.IsArmed
            ? Loc.T(_orchestrator.IsConnected ? "Tray.Connected" : "Tray.Waiting")
            : Loc.T("Tray.Off");
    }

    private void PromptShadPs4NotConfigured()
    {
        var result = System.Windows.MessageBox.Show(
            this,
            Loc.T("Main.ShadPs4Missing"),
            "SolarNX",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        if (result == MessageBoxResult.Yes) ShadPs4Launcher.OpenDownloadPage();
    }
}
