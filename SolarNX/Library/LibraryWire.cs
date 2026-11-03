using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace SolarNX.Library;

/// <summary>
/// Wire format of the library channel (TCP, Config.LibraryPort, the Switch listens and the host
/// dials - same direction as HID). Mirrored by hand in Client/library/library.c - change both
/// sides together. All integers little-endian.
///
/// One exchange per connection (version 3, protocol 1.1):
///   host → Switch   header, 56 bytes: u32 magic "ONXL" (0x4C584E4F), u32 version (3),
///                   version[28] (Session/ProtocolVersion.cs PeerVersion: protocol + app version),
///                   u64 content hash, u8 button layout (1 = A/B X/Y by label), u8[3] zero,
///                   u32 layout revision, u32 extra length (bytes after the header; 0 today)
///   Switch → host   reply, 40 bytes: u8: 1 = "send it", 0 = "I already have this hash" (always 0 on
///                   another major protocol), u8 button layout, u8[2] zero, u32 layout revision - the
///                   Switch's, after syncing with the host's - then version[28], u32 extra length (0)
///   host → Switch   (only if 1) payload: u32 count, then per entry:
///                     u32 id, u16 nameLength, UTF-8 name, u32 coverLength, JPEG bytes (0 = no cover)
/// then the host closes. The hash covers the whole payload, so any edit (name, cover, add, remove)
/// produces a new hash and the Switch re-downloads; an unchanged library costs one tiny round trip.
///
/// Frozen across protocol majors: the header's first 36 bytes, the reply's bytes 8..35 and the
/// "fixed size + extra length" rule - so an old side can always read a newer one's version and say
/// "update" instead of failing silently. A newer minor appends fields as extra bytes.
///
/// Version 2 (0.2.4 and earlier, protocol 1.0) is the same exchange without the versions: a 24-byte
/// header (magic, 2, hash, layout, revision) and an 8-byte reply. A 0.2.4 Switch drops a version-3
/// header unanswered, and the host then talks version 2 to it (LibraryPusher).
///
/// The button layout rides along because this is the only channel open while the Switch is in its
/// menu - where its own layout toggle lives. Both sides keep the layout plus a revision bumped on
/// every change; the higher revision wins, a tie goes to the host (see Client/settings/settings.c).
/// </summary>
internal static class LibraryWire
{
    public const uint Magic = 0x4C584E4F; // "ONXL"
    public const uint Version = 3;
    public const uint LegacyVersion = 2;
    public const int HeaderSize = 56;
    public const int ReplySize = 40;
    public const int LegacyHeaderSize = 24;
    public const int LegacyReplySize = 8;

    // Client-side caps (LIBRARY_MAX_ENTRIES / LIBRARY_MAX_NAME_BYTES in library.h) - enforced here
    // too so the host never sends something the Switch would reject halfway through.
    public const int MaxEntries = 256;
    public const int MaxNameBytes = 255;

    public static (byte[] Payload, ulong Hash) BuildPayload(IReadOnlyList<GameEntry> games)
    {
        using var stream = new MemoryStream();
        Span<byte> u32 = stackalloc byte[4];
        Span<byte> u16 = stackalloc byte[2];

        var included = games.Take(MaxEntries).ToList();
        if (included.Count < games.Count)
            Logger.Log("LibraryWire", $"library has {games.Count} entries, only the first {MaxEntries} are sent");

        BinaryPrimitives.WriteUInt32LittleEndian(u32, (uint)included.Count);
        stream.Write(u32);

        foreach (var game in included)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(u32, game.Id);
            stream.Write(u32);

            var name = TruncateUtf8(string.IsNullOrWhiteSpace(game.Name) ? $"#{game.Id}" : game.Name.Trim(), MaxNameBytes);
            BinaryPrimitives.WriteUInt16LittleEndian(u16, (ushort)name.Length);
            stream.Write(u16);
            stream.Write(name);

            var cover = CoverImage.ToSwitchJpeg(GameLibrary.CoverPath(game), game.CoverCrop) ?? Array.Empty<byte>();
            BinaryPrimitives.WriteUInt32LittleEndian(u32, (uint)cover.Length);
            stream.Write(u32);
            stream.Write(cover);
        }

        var payload = stream.ToArray();
        var hash = BinaryPrimitives.ReadUInt64LittleEndian(SHA256.HashData(payload));
        return (payload, hash);
    }

    public static byte[] BuildHeader(ulong hash, bool swapAbxy, uint layoutRevision, bool legacy = false)
    {
        if (legacy)
        {
            var old = new byte[LegacyHeaderSize];
            BinaryPrimitives.WriteUInt32LittleEndian(old.AsSpan(0), Magic);
            BinaryPrimitives.WriteUInt32LittleEndian(old.AsSpan(4), LegacyVersion);
            BinaryPrimitives.WriteUInt64LittleEndian(old.AsSpan(8), hash);
            old[16] = swapAbxy ? (byte)1 : (byte)0;
            BinaryPrimitives.WriteUInt32LittleEndian(old.AsSpan(20), layoutRevision);
            return old;
        }

        var header = new byte[HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(0), Magic);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4), Version);
        Session.PeerVersion.Own().CopyTo(header.AsSpan(8));
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(36), hash);
        header[44] = swapAbxy ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(48), layoutRevision);
        // header[52..56]: extra length, 0
        return header;
    }

    public readonly record struct Reply(bool NeedsPayload, bool SwapAbxy, uint LayoutRevision);

    /// <summary>Either reply version - the first 8 bytes are the same in both.</summary>
    public static Reply ParseReply(ReadOnlySpan<byte> reply) =>
        new(reply[0] == 1, reply[1] != 0, BinaryPrimitives.ReadUInt32LittleEndian(reply[4..]));

    /// <summary>The Switch's version and the extra bytes that follow a version-3 reply.</summary>
    public static (Session.PeerVersion Version, uint ExtraLength) ParseReplyVersion(ReadOnlySpan<byte> reply) =>
        (Session.PeerVersion.Read(reply.Slice(8, Session.PeerVersion.WireSize)), BinaryPrimitives.ReadUInt32LittleEndian(reply[36..]));

    // Cuts on a character boundary - never leaves half a UTF-8 sequence for the Switch to render.
    private static byte[] TruncateUtf8(string text, int maxBytes)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        if (bytes.Length <= maxBytes) return bytes;
        var length = maxBytes;
        while (length > 0 && (bytes[length] & 0xC0) == 0x80) length--;
        return bytes.AsSpan(0, length).ToArray();
    }
}
