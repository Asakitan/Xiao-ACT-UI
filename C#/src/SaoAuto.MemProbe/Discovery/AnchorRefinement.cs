using System.Buffers.Binary;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// Pure-helper port of <c>tools/mem_probe/refine.py</c> — the
/// post-discovery narrowing pass that pairs HP/MaxHP candidates,
/// finds nearby UID anchors, and re-scans an i32 needle inside a
/// blob window. Live-process I/O (TCP source, lockstep verifier,
/// streaming narrow) intentionally lives elsewhere; these helpers
/// stay deterministic so they can be exercised without a running
/// game.
/// </summary>
public static class AnchorRefinement
{
    /// <summary>Common offsets between sibling HP / MaxHP fields in
    /// the .NET object layout the game uses (see Python
    /// <c>_HP_MAXHP_DELTAS</c>). Order matches Python so test
    /// pairings line up bit-for-bit.</summary>
    public static readonly IReadOnlyList<int> HpMaxHpDeltas = new[]
    {
        4, 8, 0x10, 0x18, 0x20,
        -4, -8, -0x10, -0x18, -0x20,
    };

    /// <summary>Default search radius for <see cref="FindNearbyUid"/>
    /// (Python <c>_UID_NEAR_RADIUS = 0x800</c>).</summary>
    public const int UidNearRadius = 0x800;

    public readonly record struct HpMaxHpPair(ulong HpAddr, ulong MaxHpAddr, int Delta);

    /// <summary>Pair every HP candidate with any MaxHp candidate that
    /// sits at one of the well-known sibling offsets. Mirrors Python
    /// <c>_find_pairs</c> — order: HP-major, then deltas in
    /// <see cref="HpMaxHpDeltas"/> order.</summary>
    public static IReadOnlyList<HpMaxHpPair> FindPairs(
        IEnumerable<ulong> hpAddrs, IEnumerable<ulong> maxHpAddrs)
    {
        var sortedMax = maxHpAddrs.ToArray();
        Array.Sort(sortedMax);
        var pairs = new List<HpMaxHpPair>();
        foreach (ulong h in hpAddrs)
        {
            foreach (int d in HpMaxHpDeltas)
            {
                // Match Python ulong arithmetic: cast d through long
                // so negatives wrap before the unsigned add.
                ulong target = unchecked(h + (ulong)(long)d);
                int idx = Array.BinarySearch(sortedMax, target);
                if (idx >= 0) pairs.Add(new HpMaxHpPair(h, target, d));
            }
        }
        return pairs;
    }

    /// <summary>Return UID candidates within <paramref name="radius"/>
    /// bytes of <paramref name="anchor"/>, sorted ascending. Mirrors
    /// Python <c>_find_nearby_uid</c> — uses inclusive lower bound
    /// and inclusive upper bound (Python <c>bisect_right</c>).</summary>
    public static IReadOnlyList<ulong> FindNearbyUid(
        IEnumerable<ulong> uidAddrs, ulong anchor, int radius = UidNearRadius)
    {
        if (radius < 0) throw new ArgumentOutOfRangeException(nameof(radius));
        var sorted = uidAddrs.ToArray();
        Array.Sort(sorted);
        ulong lo = anchor >= (ulong)radius ? anchor - (ulong)radius : 0UL;
        ulong hi = anchor + (ulong)radius;
        // bisect_left for lo, bisect_right for hi → take slice [lo, hi].
        int loIdx = LowerBound(sorted, lo);
        int hiIdx = UpperBound(sorted, hi);
        if (hiIdx <= loIdx) return Array.Empty<ulong>();
        var result = new ulong[hiIdx - loIdx];
        Array.Copy(sorted, loIdx, result, 0, result.Length);
        return result;
    }

    /// <summary>Scan a memory blob for 4-byte-aligned occurrences of
    /// <paramref name="targetI32"/> and return absolute addresses
    /// reconstructed from <paramref name="blobBaseAddr"/>. Mirrors
    /// Python <c>_local_find_i32</c> — caller is responsible for
    /// reading the blob (we stay deterministic and testable).</summary>
    public static IReadOnlyList<ulong> LocalFindI32(
        ReadOnlySpan<byte> blob, ulong blobBaseAddr, int targetI32)
    {
        if (blob.Length < 4) return Array.Empty<ulong>();
        Span<byte> needle = stackalloc byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(needle, targetI32);
        var hits = new List<ulong>();
        // 4-byte aligned scan; mirror Python's `(i & 3) == 0` filter
        // by stepping in 4s instead of post-filtering.
        int last = blob.Length - 4;
        for (int i = 0; i <= last; i += 4)
        {
            if (blob[i] == needle[0]
                && blob[i + 1] == needle[1]
                && blob[i + 2] == needle[2]
                && blob[i + 3] == needle[3])
            {
                hits.Add(blobBaseAddr + (ulong)i);
            }
        }
        return hits;
    }

    /// <summary>Filter <paramref name="addrs"/> down to those whose
    /// 4-byte i32 read still equals <paramref name="value"/>. Mirrors
    /// Python <c>scanner.narrow(... dtype="i32")</c> — the only
    /// dtype the refinement pass ever uses.</summary>
    public static IReadOnlyList<ulong> NarrowI32(
        IMemorySource source, IEnumerable<ulong> addrs, int value)
    {
        ArgumentNullException.ThrowIfNull(source);
        Span<byte> needle = stackalloc byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(needle, value);
        var keep = new List<ulong>();
        foreach (ulong a in addrs)
        {
            var b = source.ReadBytes(a, 4);
            if (b is null || b.Length != 4) continue;
            if (b[0] == needle[0] && b[1] == needle[1]
                && b[2] == needle[2] && b[3] == needle[3])
            {
                keep.Add(a);
            }
        }
        return keep;
    }

    private static int LowerBound(ulong[] arr, ulong v)
    {
        int lo = 0, hi = arr.Length;
        while (lo < hi)
        {
            int mid = (lo + hi) >> 1;
            if (arr[mid] < v) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    private static int UpperBound(ulong[] arr, ulong v)
    {
        int lo = 0, hi = arr.Length;
        while (lo < hi)
        {
            int mid = (lo + hi) >> 1;
            if (arr[mid] <= v) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
}
