using System.Buffers.Binary;
using System.Net;

namespace SolarNX.Rendezvous;

/// <summary>
/// Wire format between the rendezvous server and the SolarNX PC / Switch - mirrored in
/// SolarNX/Session/RendezvousClient.cs and Client/transport/transport.c (change all three together).
/// Little-endian. The server never sees the session itself: everything it relays is already
/// encrypted end to end (the Internet mode's own crypto); it only learns public addresses and ids.
///
/// Every datagram: u32 magic "SNXR", u8 type, u8 version (1), u16 zero, then:
///   PC → server       Register    host id[8], access key[32] (UTF-8, zero-padded), the PC's local
///                                 (LAN) IPv4[4] + u16 port - optional, older PCs leave it out
///   server → PC       Registered  your public address: IPv4[4], u16 port
///   Switch → server   Lookup      host id[8], access key[32]
///   server → Switch   Peer        u8 status (<see cref="PeerStatus"/>), the PC's public IPv4[4], u16 port, u8 relay allowed
///   server → PC       Introduce   the Switch's public IPv4[4], u16 port - punch towards it now
///   Switch → server   RelayUp     host id[8], access key[32], datagram   (forwarded to the PC)
///   server → PC       RelayDown   the Switch's IPv4[4], u16 port, datagram
///   PC → server       RelayBack   the Switch's IPv4[4], u16 port, datagram (forwarded to that Switch)
///   server → Switch   RelayFrom   datagram
///   any → server      (anything malformed or unauthorized is dropped without an answer, except
///                     Peer statuses, so a scanner learns nothing)
/// Addresses are IPv4 only for now. A Switch looking up from the same public IP as the PC is behind the
/// same router: it gets the PC's local address instead, so the traffic stays on the home network rather
/// than depending on the router turning packets to its own public address back inwards (hairpin NAT,
/// which many routers do badly or not at all).
/// </summary>
public static class RendezvousWire
{
    public const uint Magic = 0x52584E53; // "SNXR"
    public const byte Version = 1;
    public const int HeaderSize = 8;
    public const int HostIdSize = 8;
    public const int KeySize = 32;
    public const int EndpointSize = 6;

    public enum Type : byte
    {
        Register = 1,
        Registered = 2,
        Lookup = 3,
        Peer = 4,
        Introduce = 5,
        RelayUp = 6,
        RelayDown = 7,
        RelayBack = 8,
        RelayFrom = 9,
    }

    public enum PeerStatus : byte
    {
        Ok = 0,
        UnknownHost = 1,
        BadKey = 2,
    }

    public static byte[] Packet(Type type, int payloadLength)
    {
        var packet = new byte[HeaderSize + payloadLength];
        BinaryPrimitives.WriteUInt32LittleEndian(packet, Magic);
        packet[4] = (byte)type;
        packet[5] = Version;
        return packet;
    }

    public static Type? Parse(ReadOnlySpan<byte> datagram)
    {
        if (datagram.Length < HeaderSize || BinaryPrimitives.ReadUInt32LittleEndian(datagram) != Magic || datagram[5] != Version)
            return null;
        return (Type)datagram[4];
    }

    public static void WriteEndpoint(Span<byte> to, IPEndPoint endpoint)
    {
        var address = endpoint.Address.IsIPv4MappedToIPv6 ? endpoint.Address.MapToIPv4() : endpoint.Address;
        address.GetAddressBytes().AsSpan(0, 4).CopyTo(to);
        BinaryPrimitives.WriteUInt16LittleEndian(to[4..], (ushort)endpoint.Port);
    }

    public static IPEndPoint ReadEndpoint(ReadOnlySpan<byte> from) =>
        new(new IPAddress(from[..4]), BinaryPrimitives.ReadUInt16LittleEndian(from[4..]));
}
