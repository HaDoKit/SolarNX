using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;

namespace SolarNX.Capture;

/// <summary>
/// Reed-Solomon erasure code over GF(2^8) - the parity for the video stream's forward error
/// correction (see <see cref="VideoSender"/>). The Switch rebuilds lost shards with the same code
/// (Client/common/rs/rs.c), so the field (polynomial 0x11D) and the coefficient formula must stay
/// byte-for-byte identical to that file.
///
/// Parity shard p = sum over k of C(p, k) * data shard k, with the Cauchy coefficient
/// C(p, k) = 1 / ((255 - p) xor k). Every square submatrix of a Cauchy matrix is invertible, so any
/// dataCount of a block's dataCount + parityCount shards rebuild it - up to parityCount lost shards
/// per block, wherever they are.
/// </summary>
public static class ReedSolomon
{
    public const int MaxShards = 255;
    public const int MaxParity = 64;

    private static readonly byte[] Exp = new byte[512];
    private static readonly byte[] Log = new byte[256];
    private static readonly byte[] Inv = new byte[256];
    // Mul[a * 256 + b] = a * b - one 64KB table, so the per-byte inner loop is a single lookup.
    private static readonly byte[] Mul = new byte[256 * 256];
    // Nibble[c * 32 ..+16] = c * i and [c * 32 + 16 ..+16] = c * (i << 4) for i = 0..15: c * x is
    // then two 16-entry lookups (low and high nibble of x) xor'ed - which SSSE3/AVX2 do for 16/32
    // bytes per instruction (pshufb). ~10x faster than the byte loop, which matters for keyframes:
    // their parity is computed before the frame's first datagram goes out.
    private static readonly byte[] Nibble = new byte[256 * 32];

    static ReedSolomon()
    {
        var x = 1;
        for (var i = 0; i < 255; i++)
        {
            Exp[i] = (byte)x;
            Log[x] = (byte)i;
            x <<= 1;
            if ((x & 0x100) != 0) x ^= 0x11D;
        }
        for (var i = 255; i < 512; i++) Exp[i] = Exp[i - 255];

        for (var a = 1; a < 256; a++) Inv[a] = Exp[255 - Log[a]];

        for (var a = 1; a < 256; a++)
            for (var b = 1; b < 256; b++)
                Mul[a * 256 + b] = Exp[Log[a] + Log[b]];

        for (var c = 0; c < 256; c++)
            for (var i = 0; i < 16; i++)
            {
                Nibble[c * 32 + i] = Mul[c * 256 + i];
                Nibble[c * 32 + 16 + i] = Mul[c * 256 + (i << 4)];
            }
    }

    private static byte Coefficient(int parity, int data) => Inv[(byte)((255 - parity) ^ data)];

    /// <summary>Fills <paramref name="parity"/> (parityCount shards back to back) from
    /// <paramref name="data"/> (dataCount shards back to back), every shard
    /// <paramref name="shardSize"/> bytes.</summary>
    public static void Encode(ReadOnlySpan<byte> data, int dataCount, Span<byte> parity, int parityCount, int shardSize)
    {
        if (dataCount <= 0 || parityCount < 0 || parityCount > MaxParity || dataCount + parityCount > MaxShards)
            throw new ArgumentOutOfRangeException(nameof(dataCount), $"block of {dataCount}+{parityCount} shards");
        if (data.Length < dataCount * shardSize || parity.Length < parityCount * shardSize)
            throw new ArgumentException("shard buffers are smaller than the block");

        parity[..(parityCount * shardSize)].Clear();
        for (var p = 0; p < parityCount; p++)
        {
            var target = parity.Slice(p * shardSize, shardSize);
            for (var k = 0; k < dataCount; k++)
                MulAdd(target, data.Slice(k * shardSize, shardSize), Coefficient(p, k));
        }
    }

    // target ^= c * source
    private static void MulAdd(Span<byte> target, ReadOnlySpan<byte> source, byte c)
    {
        var i = 0;
        ref var dst = ref MemoryMarshal.GetReference(target);
        ref var src = ref MemoryMarshal.GetReference(source);
        ref var tables = ref Nibble[c * 32];
        if (Avx2.IsSupported)
        {
            var lo = Vector256.Create(Vector128.LoadUnsafe(ref tables));
            var hi = Vector256.Create(Vector128.LoadUnsafe(ref tables, 16));
            var mask = Vector256.Create((byte)0x0F);
            for (; i + 32 <= target.Length; i += 32)
            {
                var x = Vector256.LoadUnsafe(ref src, (nuint)i);
                var product = Avx2.Shuffle(lo, x & mask) ^ Avx2.Shuffle(hi, Vector256.ShiftRightLogical(x.AsUInt16(), 4).AsByte() & mask);
                (Vector256.LoadUnsafe(ref dst, (nuint)i) ^ product).StoreUnsafe(ref dst, (nuint)i);
            }
        }
        else if (Ssse3.IsSupported)
        {
            var lo = Vector128.LoadUnsafe(ref tables);
            var hi = Vector128.LoadUnsafe(ref tables, 16);
            var mask = Vector128.Create((byte)0x0F);
            for (; i + 16 <= target.Length; i += 16)
            {
                var x = Vector128.LoadUnsafe(ref src, (nuint)i);
                var product = Ssse3.Shuffle(lo, x & mask) ^ Ssse3.Shuffle(hi, Vector128.ShiftRightLogical(x.AsUInt16(), 4).AsByte() & mask);
                (Vector128.LoadUnsafe(ref dst, (nuint)i) ^ product).StoreUnsafe(ref dst, (nuint)i);
            }
        }

        var row = Mul.AsSpan(c * 256, 256);
        for (; i < target.Length; i++)
            Unsafe.Add(ref dst, i) ^= row[Unsafe.Add(ref src, i)];
    }
}
