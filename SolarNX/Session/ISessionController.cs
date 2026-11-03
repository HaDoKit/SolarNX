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

    /// <summary>A control message from the Switch (Session/ControlWire.cs), Hello included. Raised
    /// on the network thread.</summary>
    event Action<ControlWire.ClientMessage, byte[]>? ControlReceived;

    /// <summary>The Switch's version as it announced it for the current connection - null until it
    /// did, and for a Switch that never does (0.2.4).</summary>
    PeerVersion? SessionPeer { get; }

    void Start();
    void Stop();

    /// <summary>Ends the current session (if any) without disarming - the controller keeps waiting
    /// for the next one.</summary>
    void ForceDisconnect();

    /// <summary>Sends a control message to the Switch if the current one understands them (it
    /// announced protocol 1.1+); otherwise drops it and returns false. Any thread, never blocks.</summary>
    bool SendControl(ControlWire.HostMessage type, ReadOnlySpan<byte> payload);
}
