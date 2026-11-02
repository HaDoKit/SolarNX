using SolarNX.Capture;
using SolarNX.Input;
using SolarNX.ShadPs4;

namespace SolarNX.UI.Settings;

/// <summary>
/// Replaces the old MainForm's Video/Gamepad/shadPS4/Drivers groups. Audio and the client
/// IP/ports fields are deliberately not here: sample rate/channels are hardcoded to what the
/// Switch client requires (48000Hz/stereo - see Program.cs), gain/debug-record were dropped along
/// with the rest of the audio block, IP is already set on the Home screen, and the 4 network ports
/// only matter for troubleshooting a non-default Switch-side build - shown only when launched with
/// --dev (see Program.cs), never in normal use. Same write-into-Config-and-Save-immediately
/// pattern as the old forms throughout - no Save/Cancel, consistent with the rest of the app.
/// </summary>
public partial class SettingsView : System.Windows.Controls.UserControl
{
    private static readonly (int Width, int Height)[] ResolutionOptions =
        { (640, 360), (854, 480), (960, 540), (1280, 720), (1920, 1080) };

    private static readonly int[] FpsOptions = { 24, 30, 45, 60 };

    private readonly Config _config;
    private readonly AppOrchestrator _orchestrator;

    public event Action? BackRequested;

    public SettingsView(Config config, AppOrchestrator orchestrator, bool devMode)
    {
        InitializeComponent();
        _config = config;
        _orchestrator = orchestrator;

        BackButton.Click += (_, _) => BackRequested?.Invoke();

        InitConnectionSection();
        InitVideoSection();
        InitGamepadSection();
        InitShadPs4Section();
        InitDriversSection();

        PortsCard.Visibility = devMode ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
        if (devMode) InitPortsSection();
    }

    private void InitConnectionSection()
    {
        ConnectionModeCombo.Items.Add("Wi-Fi (локальная сеть)");
        ConnectionModeCombo.Items.Add("Интернет (экспериментально)");
        ConnectionModeCombo.SelectedIndex = _config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet ? 1 : 0;
        ConnectionModeCombo.SelectionChanged += (_, _) =>
        {
            _config.ConnectionMode = ConnectionModeCombo.SelectedIndex == 1 ? SolarNX.Session.ConnectionMode.Internet : SolarNX.Session.ConnectionMode.Wifi;
            if (_config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet && string.IsNullOrWhiteSpace(_config.ConnectionCode))
                _config.ConnectionCode = SolarNX.Session.InternetSession.NewCode();
            _config.Save();
            RefreshConnectionSection();
        };
        NewCodeButton.Click += (_, _) =>
        {
            _config.ConnectionCode = SolarNX.Session.InternetSession.NewCode();
            _config.Save();
            RefreshConnectionSection();
        };
        RefreshConnectionSection();
    }

    private void RefreshConnectionSection()
    {
        var internet = _config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet;
        InternetPanel.Visibility = internet ? System.Windows.Visibility.Visible : System.Windows.Visibility.Collapsed;
        ConnectionCodeBox.Text = _config.ConnectionCode;
    }

    private void InitVideoSection()
    {
        foreach (var r in ResolutionOptions) ResolutionCombo.Items.Add($"{r.Width}x{r.Height}");
        var closestResolution = ResolutionOptions
            .OrderBy(r => Math.Abs(r.Width - _config.TargetWidth) + Math.Abs(r.Height - _config.TargetHeight))
            .First();
        ResolutionCombo.SelectedItem = $"{closestResolution.Width}x{closestResolution.Height}";
        ResolutionCombo.SelectionChanged += (_, _) =>
        {
            var parts = ((string)ResolutionCombo.SelectedItem).Split('x');
            _config.TargetWidth = int.Parse(parts[0]);
            _config.TargetHeight = int.Parse(parts[1]);
            _config.Save();
        };

        foreach (var f in FpsOptions) FpsCombo.Items.Add(f.ToString());
        var closestFps = FpsOptions.OrderBy(f => Math.Abs(f - _config.Fps)).First();
        FpsCombo.SelectedItem = closestFps.ToString();
        FpsCombo.SelectionChanged += (_, _) =>
        {
            _config.Fps = int.Parse((string)FpsCombo.SelectedItem);
            _config.Save();
        };

        BitrateBox.Value = _config.BitrateKbps;
        BitrateBox.IsEnabled = !_config.AdaptiveBitrate;
        BitrateBox.ValueChanged += (_, _) => { _config.BitrateKbps = (int)(BitrateBox.Value ?? _config.BitrateKbps); _config.Save(); };

        AdaptiveBitrateToggle.IsChecked = _config.AdaptiveBitrate;
        AdaptiveBitrateToggle.Checked += (_, _) => SetAdaptiveBitrate(true);
        AdaptiveBitrateToggle.Unchecked += (_, _) => SetAdaptiveBitrate(false);

        InitEncoderRow();
    }

    private static readonly EncoderKind[] EncoderOptions =
    {
        EncoderKind.Auto, EncoderKind.Nvenc, EncoderKind.Amf, EncoderKind.Qsv, EncoderKind.MediaFoundation, EncoderKind.Software,
    };

    // Probing the encoders spawns ffmpeg once per encoder (cached afterwards), so it runs off the UI
    // thread; until it finishes every option is selectable and the line under the combo says so.
    private void InitEncoderRow()
    {
        foreach (var kind in EncoderOptions)
            EncoderCombo.Items.Add(new System.Windows.Controls.ComboBoxItem { Content = EncoderSelection.DisplayName(kind), Tag = kind });
        EncoderCombo.SelectedIndex = Math.Max(0, Array.IndexOf(EncoderOptions, _config.Encoder));
        EncoderCombo.SelectionChanged += (_, _) =>
        {
            if (EncoderCombo.SelectedItem is not System.Windows.Controls.ComboBoxItem { Tag: EncoderKind kind }) return;
            _config.Encoder = kind;
            _config.Save();
            _ = RefreshEncoderInfoAsync();
        };
        _ = RefreshEncoderInfoAsync();
    }

    // This view is built in Program.Main before the WPF message loop runs, when there is no
    // SynchronizationContext yet - an await would resume on the thread pool, so the result is handed
    // to the UI thread explicitly.
    private async Task RefreshEncoderInfoAsync()
    {
        var preference = _config.Encoder;
        try
        {
            var (gpus, available, resolved) = await Task.Run(() =>
            {
                var gpuList = EncoderSelection.Gpus;
                var availability = EncoderOptions.Where(k => k != EncoderKind.Auto).ToDictionary(k => k, EncoderSelection.IsAvailable);
                return (gpuList, availability, EncoderSelection.Resolve(preference));
            });
            await Dispatcher.InvokeAsync(() => ShowEncoderInfo(preference, gpus, available, resolved));
        }
        catch (Exception ex)
        {
            Logger.Log("encoder", $"encoder check for settings failed: {ex}");
        }
    }

    private void ShowEncoderInfo(EncoderKind preference, IReadOnlyList<GpuInfo> gpus, Dictionary<EncoderKind, bool> available, EncoderKind resolved)
    {
        foreach (System.Windows.Controls.ComboBoxItem item in EncoderCombo.Items)
        {
            if (item.Tag is not EncoderKind kind || kind == EncoderKind.Auto) continue;
            var ok = available[kind];
            item.Content = ok ? EncoderSelection.DisplayName(kind) : $"{EncoderSelection.DisplayName(kind)} — недоступен";
            item.Foreground = ok ? System.Windows.Media.Brushes.White : System.Windows.Media.Brushes.Gray;
        }

        var gpuText = gpus.Count == 0 ? "не найдена" : string.Join(", ", gpus.Select(g => g.Name));
        var fallbackNote = preference != EncoderKind.Auto && resolved != preference ? " (выбранный недоступен)" : "";
        var cpuNote = resolved == EncoderKind.Software ? " Аппаратный кодировщик не найден — видео кодирует процессор, это заметно его нагружает." : "";
        EncoderInfoText.Text = $"Видеокарта: {gpuText}. Будет использоваться: {EncoderSelection.DisplayName(resolved)}{fallbackNote}.{cpuNote} " +
                               "Изменение применится при следующем подключении Switch.";
    }

    // AdaptiveBitrateController scales the actually-running bitrate on its own between a fixed
    // floor and BitrateKbps when this is on - the manual field becomes a ceiling rather than a
    // fixed value, so it's disabled here rather than left editable but silently ignored (mirrors
    // the old MainForm behavior exactly).
    private void SetAdaptiveBitrate(bool enabled)
    {
        _config.AdaptiveBitrate = enabled;
        BitrateBox.IsEnabled = !enabled;
        _config.Save();
    }

    // Set while the toggle is moved to show a change made on the Switch - that must not be saved
    // back as a new PC-side change.
    private bool _showingSwitchLayout;

    private void InitGamepadSection()
    {
        SwitchLayoutToggle.IsChecked = _config.SwitchButtonLayout;
        SwitchLayoutToggle.Checked += (_, _) => SetSwitchLayout(true);
        SwitchLayoutToggle.Unchecked += (_, _) => SetSwitchLayout(false);
        _orchestrator.ButtonLayoutChanged += () => Dispatcher.BeginInvoke(() =>
        {
            _showingSwitchLayout = true;
            SwitchLayoutToggle.IsChecked = _orchestrator.SwapAbxy;
            _showingSwitchLayout = false;
        });
    }

    private void SetSwitchLayout(bool enabled)
    {
        if (_showingSwitchLayout) return;
        _orchestrator.SetSwapAbxy(enabled);
    }

    private void InitShadPs4Section()
    {
        // Safe to run unconditionally here (no guard flag needed) - SettingsView is constructed
        // exactly once for the process's lifetime (see MainWindow), matching the old MainForm's
        // "auto-detect only the first time the group is built" behavior.
        if (string.IsNullOrEmpty(_config.ShadPs4ExePath))
        {
            var detected = ShadPs4Launcher.TryAutoDetect();
            if (detected is not null)
            {
                Logger.Log("SettingsView", $"shadPS4 auto-detected at {detected}");
                _config.ShadPs4ExePath = detected;
                _config.Save();
            }
        }
        ShadPs4PathBox.Text = _config.ShadPs4ExePath;

        BrowseShadPs4Button.Click += (_, _) => BrowseForShadPs4Exe();
        DownloadShadPs4Link.Click += (_, _) => ShadPs4Launcher.OpenDownloadPage();
    }

    private void BrowseForShadPs4Exe()
    {
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "shadPS4.exe|shadPS4.exe|Исполняемые файлы (*.exe)|*.exe",
            Title = "Укажите shadPS4.exe",
        };
        if (dialog.ShowDialog(System.Windows.Window.GetWindow(this)) != true) return;

        _config.ShadPs4ExePath = dialog.FileName;
        _config.Save();
        ShadPs4PathBox.Text = dialog.FileName;
    }

    private void InitDriversSection()
    {
        DriversButton.Click += (_, _) => ViGEmBusInstaller.RunInstaller();
    }

    private void InitPortsSection()
    {
        VideoPortBox.Value = _config.VideoPort;
        VideoPortBox.ValueChanged += (_, _) => { _config.VideoPort = (int)(VideoPortBox.Value ?? _config.VideoPort); _config.Save(); };

        HidPortBox.Value = _config.HidPort;
        HidPortBox.ValueChanged += (_, _) => { _config.HidPort = (int)(HidPortBox.Value ?? _config.HidPort); _config.Save(); };

        AudioPortBox.Value = _config.AudioPort;
        AudioPortBox.ValueChanged += (_, _) => { _config.AudioPort = (int)(AudioPortBox.Value ?? _config.AudioPort); _config.Save(); };

        LibraryPortBox.Value = _config.LibraryPort;
        LibraryPortBox.ValueChanged += (_, _) => { _config.LibraryPort = (int)(LibraryPortBox.Value ?? _config.LibraryPort); _config.Save(); };
    }
}
