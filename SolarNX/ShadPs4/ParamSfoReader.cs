using System.Buffers.Binary;
using System.Text;

namespace SolarNX.ShadPs4;

/// <summary>
/// DEPRECATED / not currently used: only ShadPs4LibraryScanner (also deprecated/unwired) calls
/// this. Left intact rather than deleted in case the Switch-side library-picker approach is worth
/// reviving - see ShadPs4LibraryChannel's doc comment.
///
/// Minimal from-scratch parser for the PS4 PARAM.SFO format - there's no existing NuGet package
/// for this and the format is small/stable enough not to need one. Byte-verified against a real
/// file (D:\Games\Ps4\CUSA03173\sce_sys\param.sfo, 1760 bytes) during design: a 20-byte header,
/// then an index table (16 bytes/entry), then a key-string table, then a data blob - in that
/// order. Only extracts TITLE and TITLE_ID; the icon comes from the sibling icon0.png file, not
/// from here.
/// </summary>
internal static class ParamSfoReader
{
    private const uint Magic = 0x46535000; // "\0PSF"
    private const ushort FormatUtf8String = 0x0204;
    private const int HeaderSize = 20;
    private const int IndexEntrySize = 16;
    private const int MaxEntries = 64; // defensive cap - real files have a few dozen entries at most

    /// <summary>Returns true only when a non-empty TITLE_ID was found (the only field this feature
    /// strictly needs to launch a game) - a missing/garbled TITLE degrades gracefully rather than
    /// failing the whole read. Any single malformed index entry is skipped, not treated as a reason
    /// to abort the whole file - only a bad magic number or an entry count over the defensive cap
    /// rejects the file outright.</summary>
    public static bool TryRead(byte[] sfoBytes, out string? title, out string? titleId)
    {
        title = null;
        titleId = null;

        if (sfoBytes.Length < HeaderSize) return false;

        var span = sfoBytes.AsSpan();
        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(span[0..4]);
        if (magic != Magic) return false;

        // version at [4..8) is not needed for parsing.
        uint keyTableStart = BinaryPrimitives.ReadUInt32LittleEndian(span[8..12]);
        uint dataTableStart = BinaryPrimitives.ReadUInt32LittleEndian(span[12..16]);
        uint entryCount = BinaryPrimitives.ReadUInt32LittleEndian(span[16..20]);

        if (entryCount > MaxEntries) return false;
        if (keyTableStart > sfoBytes.Length || dataTableStart > sfoBytes.Length) return false;

        for (uint i = 0; i < entryCount; i++)
        {
            int entryOffset = HeaderSize + (int)i * IndexEntrySize;
            if (entryOffset + IndexEntrySize > sfoBytes.Length) break; // truncated table - keep whatever was already found

            ushort keyOffset = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(entryOffset, 2));
            ushort dataFmt = BinaryPrimitives.ReadUInt16LittleEndian(span.Slice(entryOffset + 2, 2));
            uint dataLen = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(entryOffset + 4, 4));
            // dataMaxLen at entryOffset+8 (4 bytes) is not needed - dataLen is the meaningful length.
            uint dataOffset = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(entryOffset + 12, 4));

            long keyStart = keyTableStart + keyOffset;
            if (keyStart < 0 || keyStart >= sfoBytes.Length) continue;

            string key = ReadNulTerminatedAscii(sfoBytes, (int)keyStart);
            if (dataFmt != FormatUtf8String) continue; // skip int32/other formats - only strings matter here

            long valueStart = dataTableStart + dataOffset;
            if (valueStart < 0 || valueStart + dataLen > sfoBytes.Length) continue;

            string value = ReadNulTrimmedUtf8(sfoBytes, (int)valueStart, (int)dataLen);

            if (key == "TITLE") title = value;
            else if (key == "TITLE_ID") titleId = value;
        }

        return !string.IsNullOrEmpty(titleId);
    }

    private static string ReadNulTerminatedAscii(byte[] buffer, int start)
    {
        int end = start;
        while (end < buffer.Length && buffer[end] != 0) end++;
        return Encoding.ASCII.GetString(buffer, start, end - start);
    }

    private static string ReadNulTrimmedUtf8(byte[] buffer, int start, int length)
    {
        int end = start;
        int limit = start + length;
        while (end < limit && buffer[end] != 0) end++;
        return Encoding.UTF8.GetString(buffer, start, end - start);
    }
}
