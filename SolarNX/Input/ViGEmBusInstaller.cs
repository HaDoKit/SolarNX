using System.Diagnostics;
using System.Windows.Forms;

namespace SolarNX.Input;

/// <summary>
/// Launches the bundled ViGEmBus driver installer (github.com/nefarius/ViGEmBus releases) - the
/// virtual-controller-bus driver VirtualGamepadHub's Nefarius.ViGEm.Client library needs installed
/// to create virtual Xbox 360 controllers at all. Only ever triggered manually from the main
/// window's "reinstall driver" button - there's no way to detect a "half-broken" driver install
/// from here, so this is a user-initiated repair action, not an automatic first-run prompt.
/// </summary>
public static class ViGEmBusInstaller
{
    public static void RunInstaller()
    {
        var installerPath = Path.Combine(AppContext.BaseDirectory, "vigembus-setup.exe");
        if (!File.Exists(installerPath))
        {
            Logger.Log("ViGEmBusInstaller", $"installer not found at {installerPath}");
            MessageBox.Show($"Установщик не найден: {installerPath}", "SolarNX", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo(installerPath) { UseShellExecute = true });
            Logger.Log("ViGEmBusInstaller", "installer launched (user must click through it themselves)");
        }
        catch (Exception ex)
        {
            Logger.Log("ViGEmBusInstaller", $"failed to launch installer: {ex}");
            MessageBox.Show($"Не удалось запустить установщик: {ex.Message}", "SolarNX", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
