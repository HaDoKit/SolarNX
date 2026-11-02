using Microsoft.Win32;

namespace SolarNX.Input;

/// <summary>
/// Windows tries to auto-launch Xbox Game Bar's "controller connected" startup panel the moment
/// an XInput-compatible controller (which is exactly what ViGEmBus's virtual Xbox 360 controller
/// looks like) shows up. With Game Bar uninstalled, Windows instead shows "Не удается открыть эту
/// ссылку ms-gamebar" (no app registered for that URI), which steals real OS focus away from Big
/// Picture right at the moment the second person needs it to work.
///
/// The Settings-app-level switches (UseNexusForGameBarEnabled, AppCaptureEnabled/GameDVR_Enabled)
/// do NOT stop this: whatever component fires "open ms-gamebar:" on controller arrival (a
/// device-arrival notification handler, not Game Bar deciding to show its own UI) ignores those
/// switches entirely and just asks Windows to resolve the URI. So instead of trying to stop the
/// notification from firing, this registers this app itself as the handler for those two
/// protocols (under HKCU\Software\Classes, which doesn't need admin and takes precedence over
/// HKLM), pointed at a hidden no-op re-launch of this same exe (see Program.cs's
/// "--protocol-stub" argument) - Windows finds a registered app, launches it, it does nothing and
/// exits immediately, no dialog, no visible window.
/// </summary>
internal static class GameBarSuppressor
{
    private static readonly string[] ProtocolsToStub = { "ms-gamebar", "ms-gamebarservices" };

    public static void Disable()
    {
        TrySetValue(@"SOFTWARE\Microsoft\GameBar", "ShowStartupPanel", 0);
        TrySetValue(@"SOFTWARE\Microsoft\GameBar", "UseNexusForGameBarEnabled", 0);
        TrySetValue(@"SOFTWARE\Microsoft\Windows\CurrentVersion\GameDVR", "AppCaptureEnabled", 0);
        TrySetValue(@"System\GameConfigStore", "GameDVR_Enabled", 0);

        foreach (var protocol in ProtocolsToStub)
        {
            RegisterProtocolStub(protocol);
        }
    }

    private static void RegisterProtocolStub(string protocol)
    {
        try
        {
            using var protocolKey = Registry.CurrentUser.CreateSubKey($@"Software\Classes\{protocol}");
            protocolKey.SetValue(string.Empty, $"URL:{protocol} Protocol");
            protocolKey.SetValue("URL Protocol", string.Empty);

            using var commandKey = protocolKey.CreateSubKey(@"shell\open\command");
            var exePath = Environment.ProcessPath ?? AppContext.BaseDirectory + "SolarNX.exe";
            commandKey.SetValue(string.Empty, $"\"{exePath}\" --protocol-stub");
        }
        catch (Exception ex)
        {
            Logger.Log("GameBarSuppressor", $"failed to register protocol stub for {protocol}: {ex}");
        }
    }

    private static void TrySetValue(string subKeyPath, string name, int value)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(subKeyPath);
            key.SetValue(name, value, RegistryValueKind.DWord);
        }
        catch (Exception ex)
        {
            Logger.Log("GameBarSuppressor", $"failed to set {subKeyPath}\\{name}: {ex}");
        }
    }
}
