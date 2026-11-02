using SolarNX.Input;

namespace SolarNX.Session;

/// <summary>The connection to the Switch that drives the app's lifecycle - the Wi-Fi
/// <see cref="SessionController"/> (TCP, the PC dials the Switch) or the experimental
/// <see cref="InternetSession"/> (one UDP port, the Switch dials the PC).</summary>
public interface ISessionController : IAsyncDisposable
{
    /// <summary>The first input packet of a session - its AppTarget picks what to launch.</summary>
    event Action<HidPacket>? SessionStarted;
    event Action? SessionStopped;
    event Action<HidPacket>? PacketReceived;

    void Start();
    void Stop();

    /// <summary>Ends the current session (if any) without disarming - the controller keeps waiting
    /// for the next one.</summary>
    void ForceDisconnect();
}
