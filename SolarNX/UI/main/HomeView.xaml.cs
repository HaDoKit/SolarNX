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

        RefreshStatus();
    }

    private void SaveClientIp()
    {
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

    public void RefreshStatus()
    {
        if (!_orchestrator.IsArmed)
        {
            ToggleStreamingButton.Content = "▶  Включить трансляцию";
            ToggleStreamingButton.Appearance = ControlAppearance.Success;
            StatusLabel.Text = "Статус: выключено";
        }
        else if (_orchestrator.IsConnected)
        {
            ToggleStreamingButton.Content = "■  Выключить трансляцию";
            ToggleStreamingButton.Appearance = ControlAppearance.Danger;
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
            ToggleStreamingButton.Appearance = ControlAppearance.Danger;
            StatusLabel.Text = "Статус: ожидание подключения клиента…";
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
