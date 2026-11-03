using System.Buffers.Binary;

namespace SolarNX.Session;

/// <summary>
/// Control messages during a session, both directions (protocol 1.1, see
/// <see cref="ProtocolVersion"/>). Mirrored by hand in Client/control/control.h - change both sides
/// together.
///
/// Carried on the session's own channel so they share its fate and ordering:
///   Wi-Fi, PC → Switch:   after the 28-byte SessionConfig on the HID TCP connection, frames of
///                         u16 length (type + payload), u8 type, payload. A 0.2.4 Switch never reads
///                         past the SessionConfig, so it never sees them.
///   Wi-Fi, Switch → PC:   8×0xFE marker, u16 length, u8 type, payload - between HID packets (marker
///                         8×0xFF). <see cref="SolarNX.Input.HidFrameReader"/> tells them apart; a
///                         0.2.4 host skips the frame as noise while it looks for the next 0xFF marker.
///   Internet:             sealed <see cref="InternetWire.Type.Control"/> /
///                         <see cref="InternetWire.Type.ClientControl"/>, payload u8 type + payload.
///
/// Each side opens a session with Hello; anything else is sent only after the peer's Hello arrived -
/// that is the whole negotiation, and why a peer that never says Hello (0.2.4) only ever gets the
/// first message. Little-endian.
/// </summary>
public static class ControlWire
{
    public const int MaxPayload = 1024;

    /// <summary>The Switch → PC frame marker (HID packets use 8×0xFF).</summary>
    public static readonly byte[] ClientMarker = [0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE];

    public enum HostMessage : byte
    {
        /// <summary><see cref="PeerVersion.WireSize"/> bytes: this PC's protocol and app version.</summary>
        Hello = 1,

        /// <summary>8 bytes: per player 1-4, u8 large motor, u8 small motor (0-255, XInput meaning:
        /// large = low frequency, small = high frequency). Sent when a value changes; the Switch keeps
        /// the last levels until the next message and stops on its own when the session ends.</summary>
        Rumble = 2,

        /// <summary>4 bytes: u8 flags (<see cref="OptionFlags"/>), u8[3] zero. Sent when the Switch
        /// said Hello and whenever one of them changes in the settings.</summary>
        Options = 3,
    }

    [Flags]
    public enum OptionFlags : byte
    {
        None = 0,
        /// <summary>Mark the Switch's own input (HID) packets with DSCP EF.</summary>
        TrafficPriority = 0x01,
    }

    public enum ClientMessage : byte
    {
        /// <summary><see cref="PeerVersion.WireSize"/> bytes: the Switch's protocol and app version.</summary>
        Hello = 1,
    }

    /// <summary>A PC → Switch frame for the TCP stream: u16 length, u8 type, payload.</summary>
    public static byte[] TcpFrame(HostMessage type, ReadOnlySpan<byte> payload)
    {
        var frame = new byte[3 + payload.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(frame, (ushort)(1 + payload.Length));
        frame[2] = (byte)type;
        payload.CopyTo(frame.AsSpan(3));
        return frame;
    }

    /// <summary>The Internet-mode sealed payload: u8 type, payload.</summary>
    public static byte[] DatagramPayload(HostMessage type, ReadOnlySpan<byte> payload)
    {
        var data = new byte[1 + payload.Length];
        data[0] = (byte)type;
        payload.CopyTo(data.AsSpan(1));
        return data;
    }
}
