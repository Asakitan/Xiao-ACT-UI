using System.Buffers.Binary;
using System.Text;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// Full-memory value scanner — port of <c>tools/mem_probe/scanner.py</c>.
/// Walks every region exposed by an <see cref="IMemorySource"/>, finds
/// every aligned occurrence of a needle (i32/u32/i64/u64/f32/f64/utf16),
/// and returns the absolute hit addresses. Mirrors Python's
/// <c>scan</c> + <c>narrow</c> + <c>encode_value</c>; the per-dtype
/// AVX2 fast path that Python delegates to <c>cy_memscan</c> is replaced
/// by .NET 8's SIMD-backed <see cref="MemoryExtensions.IndexOf{T}"/>.
/// </summary>
public static class MemoryScanner
{
    /// <summary>Cap on hits before scanning bails (parity with Python).</summary>
    public const int DefaultMaxHits = 200_000;
    /// <summary>Skip regions larger than this (e.g. IL2CPP metadata segments).</summary>
    public const int DefaultMaxRegionSize = 256 * 1024 * 1024;

    // ─────────────────────── encoders ───────────────────────
    public static byte[] EncodeI32(int v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(b, v);
        return b;
    }

    public static byte[] EncodeU32(uint v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, v);
        return b;
    }

    public static byte[] EncodeI64(long v)
    {
        var b = new byte[8];
        BinaryPrimitives.WriteInt64LittleEndian(b, v);
        return b;
    }

    public static byte[] EncodeU64(ulong v)
    {
        var b = new byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(b, v);
        return b;
    }

    public static byte[] EncodeF32(float v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteSingleLittleEndian(b, v);
        return b;
    }

    public static byte[] EncodeF64(double v)
    {
        var b = new byte[8];
        BinaryPrimitives.WriteDoubleLittleEndian(b, v);
        return b;
    }

    /// <summary>UTF-16-LE bytes WITHOUT a NUL terminator (substring match
    /// is intentionally permissive — mirrors Python's encode path).</summary>
    public static byte[] EncodeUtf16(string v)
    {
        ArgumentNullException.ThrowIfNull(v);
        return Encoding.Unicode.GetBytes(v);
    }

    // ─────────────────────── scan ───────────────────────

    /// <summary>Generic byte-pattern scanner. Caller supplies the encoded
    /// needle + alignment. Skips regions larger than
    /// <paramref name="maxRegionSize"/>; bails when hit count reaches
    /// <paramref name="maxHits"/>.</summary>
    public static IReadOnlyList<ulong> Scan(
        IMemorySource source,
        ReadOnlySpan<byte> needle,
        int align,
        int maxHits = DefaultMaxHits,
        int maxRegionSize = DefaultMaxRegionSize)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (needle.Length == 0) return Array.Empty<ulong>();
        if (align < 1) align = 1;

        var hits = new List<ulong>();
        // Copy needle into an array so the foreach loop (which crosses an
        // await-style boundary semantically) can capture it; the scanner
        // is sync but spans can't escape the call.
        var needleArr = needle.ToArray();
        foreach (var region in source.IterRegions())
        {
            if ((long)region.Size > maxRegionSize) continue;
            if (region.Size == 0) continue;
            var buf = source.ReadBytes(region.Base, (int)region.Size);
            if (buf is null) continue;
            FindAllInChunk(buf, needleArr, align, region.Base, hits, maxHits);
            if (hits.Count >= maxHits) break;
        }
        return hits;
    }

    public static IReadOnlyList<ulong> ScanI32(IMemorySource s, int value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeI32(value), align ?? 4, maxHits, maxRegionSize);

    public static IReadOnlyList<ulong> ScanU32(IMemorySource s, uint value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeU32(value), align ?? 4, maxHits, maxRegionSize);

    public static IReadOnlyList<ulong> ScanI64(IMemorySource s, long value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeI64(value), align ?? 8, maxHits, maxRegionSize);

    public static IReadOnlyList<ulong> ScanU64(IMemorySource s, ulong value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeU64(value), align ?? 8, maxHits, maxRegionSize);

    public static IReadOnlyList<ulong> ScanF32(IMemorySource s, float value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeF32(value), align ?? 4, maxHits, maxRegionSize);

    public static IReadOnlyList<ulong> ScanF64(IMemorySource s, double value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeF64(value), align ?? 8, maxHits, maxRegionSize);

    public static IReadOnlyList<ulong> ScanUtf16(IMemorySource s, string value,
        int? align = null, int maxHits = DefaultMaxHits, int maxRegionSize = DefaultMaxRegionSize)
        => Scan(s, EncodeUtf16(value), align ?? 2, maxHits, maxRegionSize);

    // ─────────────────────── narrow ───────────────────────

    /// <summary>Filter <paramref name="addrs"/> down to those whose bytes
    /// at <c>addr</c> equal <paramref name="needle"/>. Mirrors Python
    /// <c>scanner.narrow</c>.</summary>
    public static IReadOnlyList<ulong> Narrow(
        IMemorySource source, IEnumerable<ulong> addrs, ReadOnlySpan<byte> needle)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentNullException.ThrowIfNull(addrs);
        if (needle.Length == 0) return Array.Empty<ulong>();
        int n = needle.Length;
        // Capture into an array since spans can't be closed over by lambdas.
        var needleArr = needle.ToArray();
        var keep = new List<ulong>();
        foreach (var a in addrs)
        {
            var b = source.ReadBytes(a, n);
            if (b is null || b.Length != n) continue;
            if (b.AsSpan().SequenceEqual(needleArr)) keep.Add(a);
        }
        return keep;
    }

    // ─────────────────────── internal ───────────────────────
    private static void FindAllInChunk(
        ReadOnlySpan<byte> buf,
        ReadOnlySpan<byte> needle,
        int align,
        ulong baseAddr,
        List<ulong> outHits,
        int maxHits)
    {
        int start = 0;
        while (start <= buf.Length - needle.Length)
        {
            int idx = buf[start..].IndexOf(needle);
            if (idx < 0) break;
            int abs = start + idx;
            if (align == 1 || (abs % align) == 0)
            {
                outHits.Add(baseAddr + (ulong)abs);
                if (outHits.Count >= maxHits) return;
            }
            start = abs + 1;
        }
    }
}
