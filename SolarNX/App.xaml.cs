namespace SolarNX;

/// <summary>
/// No StartupUri on purpose - Program.cs's Main must build Config/AppOrchestrator/the tray icon
/// before any window exists (arming/streaming state has to be ready before MainWindow can bind to
/// it), so Program.cs constructs this, calls InitializeComponent() to merge the WPF-UI theme/
/// control dictionaries, and drives Run(mainWindow) itself instead of letting WPF auto-launch a
/// window on startup.
/// </summary>
public partial class App : System.Windows.Application
{
    public App()
    {
        InitializeComponent();
        // Gold accent (the eclipse rim, see tools/branding/make_branding.py) instead of the Windows
        // accent colour - toggles, primary buttons and links follow the app's own style.
        Wpf.Ui.Appearance.ApplicationAccentColorManager.Apply(
            System.Windows.Media.Color.FromRgb(0xFF, 0xC0, 0x64), Wpf.Ui.Appearance.ApplicationTheme.Dark);
    }
}
