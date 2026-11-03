using System.Windows;
using SolarNX.Input;
using Wpf.Ui.Controls;

namespace SolarNX.UI.main;

/// <summary>
/// Home screen: connection/streaming controls ported from the old MainForm (same 3-state
/// RefreshStatus logic/strings), plus the "show screen" live local preview and the button that
/// opens SettingsView.
/// </summary>
public partial class HomeView : System.Windows.Controls.UserControl
{
    private readonly Config _config;
    private readonly AppOrchestrator _orchestrator;
    private readonly ScreenPreviewSource _screenPreview = new();

    public event Action? SettingsRequested;
    public event Action? LibraryRequested;

    public HomeView(Config config, AppOrchestrator orchestrator)
    {
        InitializeComponent();
        _config = config;
        _orchestrator = orchestrator;

        IpBox.Text = _config.ClientIp;
        IpBox.LostFocus += (_, _) => SaveClientIp();
        ToggleStreamingButton.Click += (_, _) => ToggleStreaming();
        SettingsButton.Click += (_, _) => SettingsRequested?.Invoke();
        LibraryButton.Click += (_, _) => LibraryRequested?.Invoke();
        ShowScreenToggle.Checked += (_, _) => StartPreview();
        ShowScreenToggle.Unchecked += (_, _) => StopPreview();
        PreviewImage.Source = _screenPreview.Preview;

        // Covers window-hidden-to-tray too, not just Settings navigation - either way makes this
        // control not visible, and the capture loop has no reason to keep running while it isn't.
        IsVisibleChanged += (_, _) => { if (!IsVisible) StopPreview(); };

        RefreshMode();
        Loc.Changed += RefreshMode;
        _orchestrator.Compatibility.Changed += () => Dispatcher.BeginInvoke(RefreshCompatibility);
        _orchestrator.Updates.Changed += () => Dispatcher.BeginInvoke(RefreshUpdate);
    }

    /// <summary>"SolarNX X is available", what that means for the Switch app, and a link to the
    /// release page - nothing while this is the newest.</summary>
    private void RefreshUpdate()
    {
        UpdateLabel.Inlines.Clear();
        if (_orchestrator.Updates.Available is not { } info)
        {
            UpdateLabel.Visibility = Visibility.Collapsed;
            return;
        }
        var switchNote = info.Switch switch
        {
            SolarNX.Update.SwitchUpdate.NotNeeded => Loc.T("Update.SwitchNotNeeded"),
            SolarNX.Update.SwitchUpdate.Optional => Loc.T("Update.SwitchOptional"),
            SolarNX.Update.SwitchUpdate.Required => Loc.T("Update.SwitchRequired"),
            _ => Loc.T("Update.SwitchUnknown"),
        };
        UpdateLabel.Inlines.Add(new System.Windows.Documents.Run($"{Loc.F("Home.UpdateAvailable", info.Version)} {switchNote} "));
        var link = new System.Windows.Documents.Hyperlink(new System.Windows.Documents.Run(Loc.T("Home.UpdateOpen")))
        {
            Foreground = System.Windows.Media.Brushes.White,
        };
        link.Click += (_, _) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(info.PageUrl) { UseShellExecute = true });
        UpdateLabel.Inlines.Add(link);
        UpdateLabel.Visibility = Visibility.Visible;
    }

    /// <summary>Says when the Switch's protocol differs from this PC's, and which side to update -
    /// nothing while they match (or nothing was heard from the Switch yet).</summary>
    private void RefreshCompatibility()
    {
        var peer = _orchestrator.Compatibility.Switch;
        var text = SolarNX.Session.SwitchCompatibility.Classify(peer) switch
        {
            SolarNX.Session.Compatibility.SwitchOlder when peer!.IsLegacy => Loc.T("Home.CompatLegacy"),
            SolarNX.Session.Compatibility.SwitchOlder => Loc.F("Home.CompatSwitchOlder", peer!.AppVersion!),
            SolarNX.Session.Compatibility.SwitchNewer => Loc.F("Home.CompatSwitchNewer", peer!.AppVersion!),
            SolarNX.Session.Compatibility.UpdateSwitch => Loc.F("Home.CompatUpdateSwitch", peer!.AppVersion ?? "?"),
            SolarNX.Session.Compatibility.UpdatePc => Loc.F("Home.CompatUpdatePc", peer!.AppVersion ?? "?"),
            _ => null,
        };
        CompatLabel.Text = text ?? "";
        CompatLabel.Visibility = text is null ? Visibility.Collapsed : Visibility.Visible;
    }

    /// <summary>Wi-Fi: the editable client IP. Internet (experimental): the Switch finds this PC
    /// itself, so the same field shows how many Switches are paired, read-only.
    /// Called on start and whenever the settings screen closes.</summary>
    public void RefreshMode()
    {
        var internet = _config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet;
        var paired = SolarNX.Session.PairedDevices.All.Count;
        IpLabel.Text = internet ? Loc.F("Home.InternetLabel", SolarNX.Session.InternetSession.Port) : Loc.T("Home.ClientIp");
        IpBox.Text = internet ? (paired == 0 ? Loc.T("Home.NoPaired") : Loc.F("Home.PairedCount", paired)) : _config.ClientIp;
        IpBox.IsReadOnly = internet;
        RefreshStatus();
        RefreshCompatibility();
        RefreshUpdate();
    }

    private void SaveClientIp()
    {
        if (_config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet) return; // the field shows the pairing state
        var ip = IpBox.Text.Trim();
        if (ip == _config.ClientIp) return;
        _config.ClientIp = ip;
        _config.Save();
    }

    private void ToggleStreaming()
    {
        SaveClientIp();
        if (_orchestrator.IsArmed)
        {
            _orchestrator.Disarm();
        }
        else
        {
            if (!_config.IsValid())
            {
                System.Windows.MessageBox.Show(
                    System.Windows.Window.GetWindow(this),
                    Loc.T("Home.EnterIpFirst"),
                    "SolarNX",
                    System.Windows.MessageBoxButton.OK,
                    System.Windows.MessageBoxImage.Warning);
                return;
            }
            _orchestrator.Arm();
        }
        RefreshStatus();
    }

    /// <summary>Applies the "Start" or "Stop" brushes from HomeView.xaml's resources.</summary>
    private void ApplyToggleButtonStyle(string state)
    {
        var b = ToggleStreamingButton;
        b.Background = (System.Windows.Media.Brush)Resources[state + "Background"];
        b.MouseOverBackground = (System.Windows.Media.Brush)Resources[state + "HoverBackground"];
        b.PressedBackground = (System.Windows.Media.Brush)Resources[state + "PressedBackground"];
        b.Foreground = (System.Windows.Media.Brush)Resources[state + "Foreground"];
        b.PressedForeground = b.Foreground;
        b.BorderBrush = (System.Windows.Media.Brush)Resources[state + "Border"];
        b.MouseOverBorderBrush = b.BorderBrush;
        b.PressedBorderBrush = b.BorderBrush;
    }

    public void RefreshStatus()
    {
        if (!_orchestrator.IsArmed)
        {
            ToggleStreamingButton.Content = Loc.T("Home.Start");
            ApplyToggleButtonStyle("Start");
            StatusLabel.Text = Loc.T("Home.StatusOff");
        }
        else if (_orchestrator.IsConnected)
        {
            ToggleStreamingButton.Content = Loc.T("Home.Stop");
            ApplyToggleButtonStyle("Stop");
            var targetName = _orchestrator.ActiveTarget switch
            {
                AppTarget.BigPicture => "Big Picture (Steam)",
                AppTarget.ShadPs4 => "shadPS4",
                _ => null,
            };
            StatusLabel.Text = targetName is null
                ? Loc.T("Home.StatusStreaming")
                : Loc.F("Home.StatusStreamingTarget", targetName);
        }
        else
        {
            ToggleStreamingButton.Content = Loc.T("Home.Stop");
            ApplyToggleButtonStyle("Stop");
            StatusLabel.Text = _config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet
                ? Loc.F("Home.StatusWaitingInternet", SolarNX.Session.InternetSession.Port)
                : Loc.T("Home.StatusWaiting");
        }

        // "Show screen" only makes sense once a client is actually connected and streaming -
        // before that there is nothing real being transmitted to preview (user's own decision).
        ShowScreenToggle.IsEnabled = _orchestrator.IsConnected;
        if (!_orchestrator.IsConnected && ShowScreenToggle.IsChecked == true)
        {
            ShowScreenToggle.IsChecked = false; // triggers Unchecked -> StopPreview()
        }
    }

    private void StartPreview()
    {
        _screenPreview.Start();
        PreviewImage.Visibility = Visibility.Visible;
    }

    private void StopPreview()
    {
        _screenPreview.Stop();
        PreviewImage.Visibility = Visibility.Collapsed;
    }
}
