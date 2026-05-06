using System.Buffers.Binary;
using System.Runtime.InteropServices;

namespace SaoAuto.MemProbe;

/// <summary>
/// AOB + aligned-needle scanner ported from
/// <c>mem_probe/_sao_cy_memscan.pyx</c>. Operates on a managed
/// <see cref="ReadOnlySpan{T}"/> so unit tests can drive it with a
/// synthetic byte map; the live process-memory page walker reads pages
/// via <c>ReadProcessMemory</c> and feeds them in.
/// </summary>
public static class CyMemScan
{
    public static int FindPattern(ReadOnlySpan<byte> haystack, IReadOnlyList<byte?> pattern, int start = 0)
    {
        if (pattern.Count == 0) return -1;
        if (start < 0) start = 0;
        var end = haystack.Length - pattern.Count;
        for (var i = start; i <= end; i++)
        {
            if (Matches(haystack, i, pattern)) return i;
        }
        return -1;
    }

    public static List<int> FindAllPatterns(ReadOnlySpan<byte> haystack, IReadOnlyList<byte?> pattern, int start = 0)
    {
        var result = new List<int>();
        if (pattern.Count == 0) return result;
        if (start < 0) start = 0;
        var end = haystack.Length - pattern.Count;
        for (var i = start; i <= end; i++)
        {
            if (Matches(haystack, i, pattern)) result.Add(i);
        }
        return result;
    }

    public static byte?[] ParsePattern(string pattern)
    {
        if (string.IsNullOrEmpty(pattern)) return Array.Empty<byte?>();
        var tokens = pattern.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        var result = new byte?[tokens.Length];
        for (var i = 0; i < tokens.Length; i++)
        {
            var t = tokens[i];
            if (t == "??" || t == "?")
            {
                result[i] = null;
            }
            else
            {
                if (!byte.TryParse(t, System.Globalization.NumberStyles.HexNumber,
                    System.Globalization.CultureInfo.InvariantCulture, out var b))
                {
                    throw new ArgumentException($"Invalid hex byte at token {i}: '{t}'");
                }
                result[i] = b;
            }
        }
        return result;
    }

    private static bool Matches(ReadOnlySpan<byte> haystack, int offset, IReadOnlyList<byte?> pattern)
    {
        for (var j = 0; j < pattern.Count; j++)
        {
            var p = pattern[j];
            if (p.HasValue && haystack[offset + j] != p.Value) return false;
        }
        return true;
    }

    /// <summary>
    /// Scan a buffer for every 8-byte-aligned occurrence of <paramref name="needle"/>.
    /// Mirrors <c>_sao_cy_memscan.find_aligned_u64</c> (scalar fallback path).
    /// </summary>
    public static List<int> FindAlignedU64(ReadOnlySpan<byte> haystack, ulong needle, int maxHits = 1_000_000)
    {
        var hits = new List<int>();
        var u64 = MemoryMarshal.Cast<byte, ulong>(haystack);
        for (var i = 0; i < u64.Length; i++)
        {
            if (u64[i] == needle)
            {
                hits.Add(i * 8);
                if (hits.Count >= maxHits) break;
            }
        }
        return hits;
    }

    /// <summary>4-byte-aligned u32 needle scan.</summary>
    public static List<int> FindAlignedU32(ReadOnlySpan<byte> haystack, uint needle, int maxHits = 1_000_000)
    {
        var hits = new List<int>();
        var u32 = MemoryMarshal.Cast<byte, uint>(haystack);
        for (var i = 0; i < u32.Length; i++)
        {
            if (u32[i] == needle)
            {
                hits.Add(i * 4);
                if (hits.Count >= maxHits) break;
            }
        }
        return hits;
    }

    /// <summary>
    /// Scan for any of <paramref name="needles"/>, 8-byte aligned. Returns
    /// (offset, matchedNeedle) tuples. Matches Python
    /// <c>find_aligned_u64_in_set</c>.
    /// </summary>
    public static List<(int Offset, ulong Needle)> FindAlignedU64InSet(
        ReadOnlySpan<byte> haystack, IReadOnlyCollection<ulong> needles, int maxHits = 1_000_000)
    {
        var result = new List<(int, ulong)>();
        if (needles.Count == 0) return result;
        var set = new HashSet<ulong>(needles);
        ulong lo = ulong.MaxValue, hi = 0;
        foreach (var n in set) { if (n < lo) lo = n; if (n > hi) hi = n; }
        var u64 = MemoryMarshal.Cast<byte, ulong>(haystack);
        for (var i = 0; i < u64.Length; i++)
        {
            var v = u64[i];
            if (v < lo || v > hi) continue;
            if (set.Contains(v))
            {
                result.Add((i * 8, v));
                if (result.Count >= maxHits) break;
            }
        }
        return result;
    }

    /// <summary>
    /// Read multiple little-endian fields from one buffer in one pass.
    /// Mirrors Python <c>_cy.unpack_struct_fields(buf, [(off, width), ...])</c>.
    /// Width must be 1/2/4/8. Out-of-range or unsupported widths yield 0.
    /// </summary>
    public static ulong[] UnpackStructFields(ReadOnlySpan<byte> buf, IReadOnlyList<(int Off, int Width)> specs)
    {
        var result = new ulong[specs.Count];
        for (var i = 0; i < specs.Count; i++)
        {
            var (off, width) = specs[i];
            if (off < 0 || off + width > buf.Length) { result[i] = 0; continue; }
            result[i] = width switch
            {
                1 => buf[off],
                2 => BinaryPrimitives.ReadUInt16LittleEndian(buf.Slice(off, 2)),
                4 => BinaryPrimitives.ReadUInt32LittleEndian(buf.Slice(off, 4)),
                8 => BinaryPrimitives.ReadUInt64LittleEndian(buf.Slice(off, 8)),
                _ => 0UL,
            };
        }
        return result;
    }
}
