namespace SolarNX.Session;

/// <summary>How the Switch's protocol relates to this PC's (see <see cref="ProtocolVersion"/>).</summary>
public enum Compatibility
{
    /// <summary>The Switch hasn't been heard from yet.</summary>
    Unknown,
    /// <summary>Same protocol - every feature works, whatever the two app versions.</summary>
    Same,
    /// <summary>Same major, older minor on the Switch: works, without what it lacks.</summary>
    SwitchOlder,
    /// <summary>Same major, newer minor on the Switch: works, without what this PC lacks.</summary>
    SwitchNewer,
    /// <summary>Different major, the Switch's is older: no streaming until the Switch app is updated.</summary>
    UpdateSwitch,
    /// <summary>Different major, the Switch's is newer: no streaming until SolarNX on the PC is updated.</summary>
    UpdatePc,
}

/// <summary>
/// What the PC last learned about the Switch's version - from the library handshake (Wi-Fi, also
/// while the Switch sits in its menu), the Internet-mode Hello, or the Hello control message of a
/// session. A different major blocks sessions on this side (<see cref="Blocks"/>); the Switch blocks
/// on its own side too, so whichever of the two is older can't sneak a session in.
/// </summary>
public sealed class SwitchCompatibility
{
    private readonly object _lock = new();
    private PeerVersion? _switch;

    /// <summary>Raised (on a background thread) when the Switch's reported version changes.</summary>
    public event Action? Changed;

    public PeerVersion? Switch
    {
        get { lock (_lock) return _switch; }
    }

    public Compatibility State => Classify(Switch);

    /// <summary>True when the known Switch speaks another major protocol - no session then.</summary>
    public bool Blocks => State is Compatibility.UpdateSwitch or Compatibility.UpdatePc;

    public void Report(PeerVersion peer)
    {
        lock (_lock)
        {
            if (_switch == peer) return;
            _switch = peer;
        }
        Logger.Log("Compatibility", $"Switch: {peer}, this PC: {AppVersion.Current} (protocol {ProtocolVersion.Current}) - {Classify(peer)}");
        Changed?.Invoke();
    }

    public static Compatibility Classify(PeerVersion? peer)
    {
        if (peer is null) return Compatibility.Unknown;
        var own = ProtocolVersion.Current;
        if (peer.Protocol.Major != own.Major)
            return peer.Protocol.Major < own.Major ? Compatibility.UpdateSwitch : Compatibility.UpdatePc;
        if (peer.Protocol.Minor == own.Minor) return Compatibility.Same;
        return peer.Protocol.Minor < own.Minor ? Compatibility.SwitchOlder : Compatibility.SwitchNewer;
    }
}
