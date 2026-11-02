using System.Buffers.Binary;

namespace SolarNX.Session;

/// <summary>
/// Wire format of the experimental "Internet" mode: everything over one UDP port on the PC
/// (<see cref="InternetSession.Port"/>), and the Switch sends first, so only that one port has to be
/// forwarded on the PC's router. Mirrored by hand in Client/transport/transport.c - change both sides
/// together. Little-endian. Cryptography: <see cref="InternetCrypto"/>.
///
/// Every datagram starts with an 8-byte header: u32 magic "SNXU", u8 type, u8 version (2), u16 zero.
///
/// Pairing (once per Switch, only while pairing is open on the PC) - numeric comparison: X25519 keys,
/// the PC commits to its nonce before it sees the Switch's, both screens show the same 6-digit code
/// and the player confirms it on the PC:
///   Switch → PC  PairStart   device id[16], X25519 public[32], name[32] (UTF-8, zero-padded)
///   PC → Switch  PairKey     X25519 public[32], commit[32] = SHA256(pcPublic || switchPublic || pcNonce)
///   Switch → PC  PairNonceS  device id[16], switch nonce[16]
///   PC → Switch  PairNonceP  pc nonce[16]
///   PC → Switch  PairResult  u8 accepted; if accepted: host id[8], proof[32] (InternetCrypto.PairProof)
///
/// Session (in clear only these two and Reject):
///   Switch → PC  Hello       device id[16], switch nonce[16]
///   PC → Switch  Welcome     pc nonce[16], host id[8], then sealed SessionConfig (counter 1) with the
///                            header + pc nonce + host id as associated data
///   PC → Switch  Reject      u8 reason (<see cref="RejectReason"/>)
///
/// Everything else is sealed with the session keys (header as associated data, counter, AES-CTR, tag):
///   Switch → PC  Input       the 224-byte HID packet (Input/HidProtocol.cs)
///   Switch → PC  Bye         session over (the player went back to the menu)
///   PC → Switch  End         session over (the PC ended it - e.g. the game closed)
///   PC → Switch  Video       one video datagram (header + shard), as VideoSender sent it
///   PC → Switch  Audio       one RTP-like Opus datagram, as AudioCaptureSender sent it
///   PC → Switch  LibOffer    u32 offer id, u64 library hash, u8 button layout, u8[3] zero, u32 layout revision
///   Switch → PC  LibReply    u32 offer id, u8 1 = send the library, u8 button layout, u8[2] zero, u32 layout revision
///   PC → Switch  LibChunk    u64 hash, u32 total size, u32 offset, data
///   Switch → PC  LibAck      u64 hash, u32 bytes received in order so far
///   Switch → PC  Keepalive   (empty) once a second while connected
///   PC → Switch  KeepaliveAck
///
/// The Switch says Hello every second until it gets a Welcome, then Keepalive every second; three
/// seconds without anything valid from the PC send it back to Hello with a fresh nonce (new keys).
/// The library payload is the same as on the TCP channel (Library/LibraryWire.cs), sent go-back-N.
/// </summary>
internal static class InternetWire
{
    public const uint Magic = 0x55584E53; // "SNXU"
    public const byte Version = 2;
    public const int HeaderSize = 8;
    public const int DeviceIdSize = 16;
    public const int HostIdSize = 8;
    public const int NameSize = 32;

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
        PairStart = 13,
        PairKey = 14,
        PairNonceS = 15,
        PairNonceP = 16,
        PairResult = 17,
        Keepalive = 18,
        KeepaliveAck = 19,
        /// <summary>Empty, sent towards a Switch the rendezvous server introduced - opens this PC's
        /// router for the Switch's packets (hole punching). Ignored on arrival.</summary>
        Punch = 20,
    }

    public enum RejectReason : byte
    {
        NotPaired = 1,
        Busy = 2,
        WrongVersion = 3,
        PairingClosed = 4,
    }

    public static byte[] Header(Type type)
    {
        var header = new byte[HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(header, Magic);
        header[4] = (byte)type;
        header[5] = Version;
        return header;
    }

    /// <summary>A packet sent in clear: header + payload.</summary>
    public static byte[] Plain(Type type, ReadOnlySpan<byte> payload)
    {
        var packet = new byte[HeaderSize + payload.Length];
        Header(type).CopyTo(packet, 0);
        payload.CopyTo(packet.AsSpan(HeaderSize));
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
