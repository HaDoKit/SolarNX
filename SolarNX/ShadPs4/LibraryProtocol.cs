using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text;

namespace SolarNX.ShadPs4;

/// <summary>
/// DEPRECATED / not currently used: only ShadPs4LibraryChannel (also deprecated/unwired) calls
/// this. Left intact rather than deleted in case the Switch-side library-picker approach is worth
/// reviving - see ShadPs4LibraryChannel's doc comment.
///
/// Wire format for the shadPS4 game-library picker channel - a separate TCP port from the
/// existing HID one (SolarNX Client/SolarNX/source/network.h mirrors this exactly on the Switch
/// side). That channel is fixed-size/fixed-rate/one-way; this one needs variable-length data
/// (titles, icons) and a request/response shape, so it gets its own manually-serialized framing
/// (field-by-field via BinaryPrimitives, not a raw struct cast - same defensive style HidPacket
/// already uses) instead of extending JoyPkg.
///
/// Every constant here must stay numerically identical to network.h's LIBRARY_* definitions - no
/// shared build between the two repos, a mismatch fails at runtime, not compile time (same class
/// of risk as the JoyPkg/HidPacket discrepancy already on record).
/// </summary>
public static class LibraryProtocol
{
    public const int Port = 19003;
    private static readonly byte[] Magic = { (byte)'O', (byte)'N', (byte)'X', (byte)'L' };
    public const byte ProtocolVersion = 1;
    public const byte MsgTypeList = 1;
    public const byte MsgTypeSelection = 2;
    public const byte SelectionStatusSelected = 0;
    public const byte SelectionStatusCancelled = 1;
    public const int MaxGames = 64;
    public const int MaxTitleIdWireLen = 16;
    public const int MaxTitleWireLen = 512;
    public const int MaxIconBytes = 4 * 1024 * 1024;
    public const int SelectionMessageSize = 24;

    /// <summary>Builds the full LIST message: a 12-byte header (magic, version, msgType, 2 reserved
    /// bytes, gameCount) followed by one variable-length record per game (titleIdLen:u16,
    /// titleLen:u16, iconLen:u32, then the raw un-terminated bytes for each in that order). Caps at
    /// MaxGames as defense in depth - ShadPs4LibraryScanner already caps there too.</summary>
    public static byte[] BuildListMessage(IReadOnlyList<ShadPs4GameEntry> games)
    {
        var capped = games.Count > MaxGames ? games.Take(MaxGames).ToList() : games;

        using var stream = new MemoryStream();

        Span<byte> headerBuf = stackalloc byte[12];
        Magic.CopyTo(headerBuf);
        headerBuf[4] = ProtocolVersion;
        headerBuf[5] = MsgTypeList;
        headerBuf[6] = 0;
        headerBuf[7] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(headerBuf[8..], (uint)capped.Count);
        stream.Write(headerBuf);

        Span<byte> subHeader = stackalloc byte[8]; // hoisted out of the loop below - stackalloc in
                                                    // a loop grows the frame per iteration instead
                                                    // of reusing the same slot (CA2014)
        foreach (var game in capped)
        {
            var titleIdBytes = Encoding.UTF8.GetBytes(game.TitleId);
            if (titleIdBytes.Length > MaxTitleIdWireLen) titleIdBytes = titleIdBytes[..MaxTitleIdWireLen];

            var titleBytes = Encoding.UTF8.GetBytes(game.Title);
            if (titleBytes.Length > MaxTitleWireLen) titleBytes = titleBytes[..MaxTitleWireLen];

            var iconBytes = game.IconPngBytes;
            if (iconBytes is not null && iconBytes.Length > MaxIconBytes)
            {
                Logger.Log("LibraryProtocol", $"icon for '{game.TitleId}' exceeds MaxIconBytes ({MaxIconBytes}), omitting it");
                iconBytes = null;
            }

            BinaryPrimitives.WriteUInt16LittleEndian(subHeader[..2], (ushort)titleIdBytes.Length);
            BinaryPrimitives.WriteUInt16LittleEndian(subHeader[2..4], (ushort)titleBytes.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(subHeader[4..8], (uint)(iconBytes?.Length ?? 0));
            stream.Write(subHeader);

            stream.Write(titleIdBytes);
            stream.Write(titleBytes);
            if (iconBytes is not null) stream.Write(iconBytes);
        }

        return stream.ToArray();
    }

    /// <summary>Reads the 24-byte selection response off an already-connected stream, bounded by
    /// timeout. Every failure mode (timeout, connection closed, magic/version mismatch, explicit
    /// CANCELLED status, malformed length) resolves to null - matches the existing "null/false
    /// means fall back to generic launch" contract used elsewhere in this app (e.g.
    /// ShadPs4Launcher.LaunchOrFocusAsync's own return value), never throws out to the caller.</summary>
    public static async Task<string?> ReadSelectionResponseAsync(NetworkStream stream, TimeSpan timeout, CancellationToken ct)
    {
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(timeout);

        try
        {
            var buffer = new byte[SelectionMessageSize];
            await ReadExactAsync(stream, buffer, cts.Token);

            if (buffer[0] != Magic[0] || buffer[1] != Magic[1] || buffer[2] != Magic[2] || buffer[3] != Magic[3])
            {
                Logger.Log("LibraryProtocol", "selection response magic mismatch - protocol desync or unexpected sender");
                return null;
            }
            if (buffer[4] != ProtocolVersion || buffer[5] != MsgTypeSelection)
            {
                Logger.Log("LibraryProtocol", $"unexpected selection response version={buffer[4]} msgType={buffer[5]}");
                return null;
            }
            if (buffer[6] != SelectionStatusSelected) return null; // explicit CANCELLED or any non-zero status

            int idLen = buffer[7];
            if (idLen > MaxTitleIdWireLen)
            {
                Logger.Log("LibraryProtocol", $"selection response titleIdLen {idLen} exceeds max {MaxTitleIdWireLen}");
                return null;
            }

            return Encoding.UTF8.GetString(buffer, 8, idLen);
        }
        catch (OperationCanceledException)
        {
            Logger.Log("LibraryProtocol", "timed out waiting for selection response");
            return null;
        }
        catch (IOException ex)
        {
            Logger.Log("LibraryProtocol", $"I/O error waiting for selection response: {ex.Message}");
            return null;
        }
    }

    private static async Task ReadExactAsync(NetworkStream stream, byte[] buffer, CancellationToken ct)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int read = await stream.ReadAsync(buffer.AsMemory(offset, buffer.Length - offset), ct);
            if (read == 0) throw new IOException("connection closed before the selection response was fully received");
            offset += read;
        }
    }
}
