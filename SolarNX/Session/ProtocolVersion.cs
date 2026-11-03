using System.Buffers.Binary;
using System.Reflection;
using System.Text;

namespace SolarNX.Session;

/// <summary>
/// The PC ↔ Switch wire protocol as "major.minor", from ../PROTOCOL (stamped into the assembly by
/// SolarNX.csproj; the Switch gets the same file through Client/Makefile). Separate from the app
/// version on purpose: a release that changes nothing on the wire leaves it alone, and the Switch
/// app can stay on its older version.
///
/// - Major: bumped by a change the other side can't live with (video framing, the HID packet,
///   SessionConfig...). Different majors refuse to stream; both sides say which one to update.
/// - Minor: bumped by an addition an older peer can safely ignore (a new control message, a new
///   field appended to a handshake). The newer side then simply doesn't use what the older one
///   lacks - <see cref="Supports"/> on the lower of the two minors.
///
/// 1.0 is what 0.2.4 speaks without saying so (it predates this number); 1.1 added the version
/// exchange itself and the control messages (Session/ControlWire.cs).
/// </summary>
public readonly record struct ProtocolVersion(ushort Major, ushort Minor)
{
    /// <summary>The version an older client/host that doesn't announce one is assumed to speak.</summary>
    public static readonly ProtocolVersion Legacy = new(1, 0);

    public static ProtocolVersion Current { get; } = Parse(
        typeof(ProtocolVersion).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "SolarNXProtocol")?.Value) ?? Legacy;

    /// <summary>Control messages, rumble, debug mode, the version exchange.</summary>
    public static readonly ProtocolVersion ControlMessages = new(1, 1);

    public static ProtocolVersion? Parse(string? text)
    {
        var parts = text?.Trim().Split('.');
        return parts is { Length: 2 } && ushort.TryParse(parts[0], out var major) && ushort.TryParse(parts[1], out var minor)
            ? new ProtocolVersion(major, minor)
            : null;
    }

    /// <summary>True if a session where the peer speaks <paramref name="peer"/> can use a feature
    /// introduced in <paramref name="feature"/> - the same major, and both minors at least that far.</summary>
    public static bool Supports(ProtocolVersion peer, ProtocolVersion feature) =>
        peer.Major == feature.Major && Current.Major == feature.Major
        && peer.Minor >= feature.Minor && Current.Minor >= feature.Minor;

    public override string ToString() => $"{Major}.{Minor}";
}

/// <summary>What the other side said about itself: its protocol and app version. Null
/// <see cref="AppVersion"/> = an older build that doesn't say (0.2.4 or earlier).</summary>
public sealed record PeerVersion(ProtocolVersion Protocol, string? AppVersion)
{
    public static readonly PeerVersion Legacy = new(ProtocolVersion.Legacy, null);

    public bool IsLegacy => AppVersion is null;

    public bool Compatible => Protocol.Major == ProtocolVersion.Current.Major;

    /// <summary>Wire form shared by every handshake that carries it (library header/reply, Internet
    /// Hello/Welcome/Reject, the Hello control message): u16 major, u16 minor, char[24] app version
    /// (UTF-8, zero-padded) - 28 bytes, little-endian. Frozen: an old peer must still be able to read
    /// a newer one's version to say "update".</summary>
    public const int WireSize = 28;
    private const int AppVersionSize = 24;

    public static byte[] Own()
    {
        var bytes = new byte[WireSize];
        BinaryPrimitives.WriteUInt16LittleEndian(bytes, ProtocolVersion.Current.Major);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(2), ProtocolVersion.Current.Minor);
        var version = Encoding.UTF8.GetBytes(SolarNX.AppVersion.Current);
        version.AsSpan(0, Math.Min(version.Length, AppVersionSize - 1)).CopyTo(bytes.AsSpan(4));
        return bytes;
    }

    public static PeerVersion Read(ReadOnlySpan<byte> wire)
    {
        var major = BinaryPrimitives.ReadUInt16LittleEndian(wire);
        var minor = BinaryPrimitives.ReadUInt16LittleEndian(wire[2..]);
        var text = wire.Slice(4, AppVersionSize);
        var end = text.IndexOf((byte)0);
        var version = Encoding.UTF8.GetString(end < 0 ? text : text[..end]).Trim();
        return new PeerVersion(new ProtocolVersion(major, minor), version.Length == 0 ? "?" : version);
    }

    public override string ToString() => IsLegacy ? $"legacy (protocol {Protocol})" : $"{AppVersion} (protocol {Protocol})";
}
