using System.Buffers.Binary;

namespace SolarNX.Session;

/// <summary>
/// Wire format of the experimental "Internet" mode: everything over one UDP port on the PC
/// (<see cref="InternetSession.Port"/>), and the Switch sends first, so only that one port has to be
/// forwarded on the PC's router. Mirrored by hand in Client/transport/transport.c - change both sides
/// together. Little-endian.
///
/// Every datagram starts with an 8-byte header: u32 magic "SNXU", u8 type, u8 version (1), u16 zero.
///
///   Switch → PC  Hello       u32 nonce, char[16] connection code (ASCII, zero-padded)
///   PC → Switch  Welcome     u32 nonce, SessionConfig (28 bytes)
///   PC → Switch  Reject      u32 nonce, u8 reason (<see cref="RejectReason"/>)
///   Switch → PC  Input       the 224-byte HID packet (Input/HidProtocol.cs), no marker
///   Switch → PC  Bye         session over (the player went back to the menu)
///   PC → Switch  End         session over (the PC ended it - e.g. the game closed)
///   PC → Switch  Video       MPEG-TS bytes, as ffmpeg sent them
///   PC → Switch  Audio       one RTP-like Opus datagram, as AudioCaptureSender sent it
///   PC → Switch  LibOffer    u32 offer id, u64 library hash, u8 button layout, u8[3] zero, u32 layout revision
///   Switch → PC  LibReply    u32 offer id, u8 1 = send the library, u8 button layout, u8[2] zero, u32 layout revision
///   PC → Switch  LibChunk    u64 hash, u32 total size, u32 offset, data
///   Switch → PC  LibAck      u64 hash, u32 bytes received in order so far
///
/// The Switch repeats Hello every second while the app runs: it is both the keepalive that tells
/// the PC where to send and how the session (re)starts. The library payload is the same as on the
/// TCP channel (Library/LibraryWire.cs); it goes go-back-N - the Switch accepts only the chunk at
/// its current offset and acks how far it got.
/// </summary>
internal static class InternetWire
{
    public const uint Magic = 0x55584E53; // "SNXU"
    public const byte Version = 1;
    public const int HeaderSize = 8;
    public const int CodeLength = 16;

    public enum Type : byte
    {
        Hello = 1,
        Welcome = 2,
        Reject = 3,
        Input = 4,
        Bye = 5,
        End = 6,
        Video = 7,
        Audio = 8,
        LibOffer = 9,
        LibReply = 10,
        LibChunk = 11,
        LibAck = 12,
    }

    public enum RejectReason : byte
    {
        WrongCode = 1,
        Busy = 2,
        WrongVersion = 3,
    }

    public static byte[] Packet(Type type, int payloadLength)
    {
        var packet = new byte[HeaderSize + payloadLength];
        BinaryPrimitives.WriteUInt32LittleEndian(packet, Magic);
        packet[4] = (byte)type;
        packet[5] = Version;
        return packet;
    }

    /// <summary>The datagram's type, or null if it isn't one of ours.</summary>
    public static Type? Parse(ReadOnlySpan<byte> datagram, out byte version)
    {
        version = 0;
        if (datagram.Length < HeaderSize || BinaryPrimitives.ReadUInt32LittleEndian(datagram) != Magic) return null;
        version = datagram[5];
        return (Type)datagram[4];
    }
}
