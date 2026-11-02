using System.Windows;
using SolarNX.Input;
using Wpf.Ui.Controls;

namespace SolarNX.UI.main;

/// <summary>
/// Home screen: connection/streaming controls ported from the old MainForm (same 3-state
/// RefreshStatus logic/strings), plus the "Показать экран" live local preview and the button that
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
    }

    /// <summary>Wi-Fi: the editable client IP. Internet (experimental): the Switch finds this PC
    /// itself, so the same field shows how many Switches are paired, read-only.
    /// Called on start and whenever the settings screen closes.</summary>
    public void RefreshMode()
    {
        var internet = _config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet;
        var paired = SolarNX.Session.PairedDevices.All.Count;
        IpLabel.Text = internet ? $"Интернет, UDP {SolarNX.Session.InternetSession.Port} (эксперимент)" : "IP клиента";
        IpBox.Text = internet ? (paired == 0 ? "Нет привязанных Switch — см. Настройки" : $"Привязано Switch: {paired}") : _config.ClientIp;
        IpBox.IsReadOnly = internet;
        RefreshStatus();
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
                    "Сначала укажите IP клиента.",
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
            ToggleStreamingButton.Content = "▶  Включить трансляцию";
            ApplyToggleButtonStyle("Start");
            StatusLabel.Text = "Статус: выключено";
        }
        else if (_orchestrator.IsConnected)
        {
            ToggleStreamingButton.Content = "■  Выключить трансляцию";
            ApplyToggleButtonStyle("Stop");
            var targetName = _orchestrator.ActiveTarget switch
            {
                AppTarget.BigPicture => "Big Picture (Steam)",
                AppTarget.ShadPs4 => "shadPS4",
                _ => null,
            };
            StatusLabel.Text = targetName is null
                ? "Статус: клиент подключён, идёт трансляция"
                : $"Статус: клиент подключён, идёт трансляция — {targetName}";
        }
        else
        {
            ToggleStreamingButton.Content = "■  Выключить трансляцию";
            ApplyToggleButtonStyle("Stop");
            StatusLabel.Text = _config.ConnectionMode == SolarNX.Session.ConnectionMode.Internet
                ? $"Статус: ожидание Switch через интернет (UDP {SolarNX.Session.InternetSession.Port})…"
                : "Статус: ожидание подключения клиента…";
        }

        // "Показать экран" only makes sense once a client is actually connected and streaming -
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
