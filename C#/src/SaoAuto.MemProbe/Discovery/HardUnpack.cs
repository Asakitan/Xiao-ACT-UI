using System.Buffers.Binary;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>One ground-truth field that <see cref="HardUnpack"/> turns
/// into one or more bit-pattern needles. Mirrors a single dict entry
/// in Python <c>PLAYER_STATS</c> / <c>WEAPON_STATS</c> / etc.
/// String values (e.g. <c>name_tag</c>) are filtered upstream.</summary>
public sealed record StatField(string Group, string Name, double Value, bool IsInteger)
{
    public string FullName => $"{Group}.{Name}";
}

/// <summary>One needle expansion of a <see cref="StatField"/>: same
/// underlying field, different bit-pattern interpretation.</summary>
public sealed record NeedleEntry(string FullName, string Encoding, double OriginalValue);

/// <summary>The grouped output of <see cref="HardUnpack.BuildNeedles"/>.
/// Both maps key by the bit pattern; the value is every (field, encoding,
/// original) triple that resolves to that pattern. One pattern can map to
/// many fields when distinct values share the same low bytes.</summary>
public sealed record NeedleSet(
    IReadOnlyDictionary<uint, IReadOnlyList<NeedleEntry>> U32,
    IReadOnlyDictionary<ulong, IReadOnlyList<NeedleEntry>> U64);

/// <summary>One memory-side hit produced by <see cref="HardUnpack.ScanRegions"/>.</summary>
public sealed record ValueHit(ulong Addr, string FieldName, double Value, string Encoding);

/// <summary>A cluster of <see cref="ValueHit"/>s within
/// <see cref="HardUnpack.DefaultClusterWindow"/> bytes that contains at
/// least <see cref="HardUnpack.DefaultMinDistinctFields"/> distinct
/// field names. The high cluster IS the struct.</summary>
public sealed record ValueCluster(ulong SpanLo, ulong SpanHi, IReadOnlyList<ValueHit> Hits)
{
    public int DistinctFieldCount => Hits.Select(h => h.FieldName).Distinct().Count();
}

/// <summary>One (obj_base, klass_ptr) candidate produced by
/// <see cref="HardUnpack.WalkBackToKlass"/>.</summary>
public sealed record OwnerCandidate(ulong ObjAddr, ulong KlassPtr);

/// <summary>One pointer-shaped u64 found in
/// <see cref="HardUnpack.DiagnoseNeighborhood"/> output.</summary>
public sealed record NeighborhoodPointer(long RelOff, ulong Value, string Kind);

/// <summary>
/// Port of <c>tools/mem_probe/hard_unpack.py</c> — value-cluster
/// IL2CPP discovery. Given a set of KNOWN field values (typically
/// scraped from in-game UI screenshots), this finds spatial clusters
/// where many distinct values co-locate within ~1 KB. A high-density
/// cluster IS a struct holding those fields; walk back ≤256 KB to the
/// nearest GameAssembly-pointing 8-byte slot to recover the IL2CPP
/// klass.
///
/// Only the pure, deterministic library pieces are ported here:
/// needle building, region scan, clustering, walk-back, neighborhood
/// dump. The Python CLI orchestration (hardcoded stats dicts,
/// `anchors.json` persistence, console logging) is held for whoever
/// wires this into the live discovery pass — those are caller
/// concerns.
/// </summary>
public static class HardUnpack
{
    public const int MaxRegionSize = 256 * 1024 * 1024;
    public const int DefaultClusterWindow = 0x400;
    public const int DefaultMinDistinctFields = 3;
    public const int DefaultWalkBack = 0x40000;
    public const int DefaultWalkBackResults = 8;

    /// <summary>Translate a list of typed truth fields into the two
    /// keyed needle maps. Mirrors Python <c>_build_needles</c>:
    ///   - integers: emit i32 (when 0 &lt; v &lt; 2^32) AND i64
    ///     (when 0 &lt; v &lt; 2^63), AND f32_of_int when the integer
    ///     fits 24 bits and the f32 bit pattern differs.
    ///   - floats: emit f32 AND f64.
    /// Values ≤ 0 are dropped exactly like the Python truthiness check.
    /// </summary>
    public static NeedleSet BuildNeedles(IEnumerable<StatField> fields)
    {
        ArgumentNullException.ThrowIfNull(fields);
        var u32 = new Dictionary<uint, List<NeedleEntry>>();
        var u64 = new Dictionary<ulong, List<NeedleEntry>>();

        void Add32(uint pat, NeedleEntry e)
        {
            if (!u32.TryGetValue(pat, out var list)) u32[pat] = list = new List<NeedleEntry>();
            list.Add(e);
        }
        void Add64(ulong pat, NeedleEntry e)
        {
            if (!u64.TryGetValue(pat, out var list)) u64[pat] = list = new List<NeedleEntry>();
            list.Add(e);
        }

        foreach (var f in fields)
        {
            if (f.Value == 0) continue;
            string full = f.FullName;
            if (f.IsInteger)
            {
                long iv = (long)f.Value;
                if (iv > 0 && iv < (1L << 32))
                {
                    Add32((uint)(iv & 0xFFFFFFFFL),
                        new NeedleEntry(full, "i32", f.Value));
                }
                if (iv > 0)
                {
                    Add64((ulong)iv, new NeedleEntry(full, "i64", f.Value));
                }
                if (iv > 0 && iv < (1L << 24))
                {
                    uint f32 = BitConverter.SingleToUInt32Bits((float)iv);
                    if (f32 != (uint)(iv & 0xFFFFFFFFL))
                    {
                        Add32(f32, new NeedleEntry(full, "f32_of_int", f.Value));
                    }
                }
            }
            else
            {
                uint f32 = BitConverter.SingleToUInt32Bits((float)f.Value);
                Add32(f32, new NeedleEntry(full, "f32", f.Value));
                ulong f64 = BitConverter.DoubleToUInt64Bits(f.Value);
                Add64(f64, new NeedleEntry(full, "f64", f.Value));
            }
        }

        return new NeedleSet(
            u32.ToDictionary(kv => kv.Key,
                kv => (IReadOnlyList<NeedleEntry>)kv.Value),
            u64.ToDictionary(kv => kv.Key,
                kv => (IReadOnlyList<NeedleEntry>)kv.Value));
    }

    /// <summary>Scan every readable private region against
    /// <paramref name="needles"/>; emit one <see cref="ValueHit"/> per
    /// (matched bit-pattern × field) pair. Regions that fall inside any
    /// loaded module (per <paramref name="inModule"/>) are skipped to
    /// avoid code/static pages. Mirrors Python <c>scan_all_values</c>
    /// minus the worker pool.</summary>
    public static IReadOnlyList<ValueHit> ScanRegions(
        IMemorySource memory,
        NeedleSet needles,
        Func<ulong, bool>? inModule = null,
        int maxRegionSize = MaxRegionSize)
    {
        ArgumentNullException.ThrowIfNull(memory);
        ArgumentNullException.ThrowIfNull(needles);
        var hits = new List<ValueHit>();
        var u32Keys = needles.U32.Keys.ToArray();
        var u64Keys = needles.U64.Keys.ToArray();
        foreach (var region in memory.IterRegions(onlyReadable: true, onlyPrivate: true))
        {
            if ((long)region.Size > maxRegionSize) continue;
            if (inModule is not null && inModule(region.Base)) continue;
            var buf = memory.ReadBytes(region.Base, (int)region.Size);
            if (buf is null) continue;
            // u64 multi-needle in one pass
            if (u64Keys.Length > 0)
            {
                var found = CyMemScan.FindAlignedU64InSet(buf, u64Keys);
                foreach (var (off, val) in found)
                {
                    foreach (var entry in needles.U64[val])
                    {
                        hits.Add(new ValueHit(
                            region.Base + (ulong)off, entry.FullName,
                            entry.OriginalValue, entry.Encoding));
                    }
                }
            }
            // u32 single-needle loop
            foreach (var n in u32Keys)
            {
                var found = CyMemScan.FindAlignedU32(buf, n);
                if (found.Count == 0) continue;
                foreach (var off in found)
                {
                    foreach (var entry in needles.U32[n])
                    {
                        hits.Add(new ValueHit(
                            region.Base + (ulong)off, entry.FullName,
                            entry.OriginalValue, entry.Encoding));
                    }
                }
            }
        }
        return hits;
    }

    /// <summary>Sweep <paramref name="hits"/> sorted by address; emit a
    /// <see cref="ValueCluster"/> whenever a window of
    /// <paramref name="window"/> bytes contains
    /// <paramref name="minDistinct"/>+ distinct field names. After each
    /// emit the cursor advances past the window — mirrors Python's
    /// "advance past this window to avoid double-counting" guard.
    /// Result is sorted descending by distinct-field count.</summary>
    public static IReadOnlyList<ValueCluster> ClusterHits(
        IEnumerable<ValueHit> hits,
        int window = DefaultClusterWindow,
        int minDistinct = DefaultMinDistinctFields)
    {
        ArgumentNullException.ThrowIfNull(hits);
        var sorted = hits.OrderBy(h => h.Addr).ToList();
        var clusters = new List<ValueCluster>();
        int i = 0, n = sorted.Count;
        while (i < n)
        {
            int j = i;
            while (j < n && sorted[j].Addr - sorted[i].Addr <= (ulong)window) j++;
            var slice = sorted.GetRange(i, j - i);
            int distinct = slice.Select(h => h.FieldName).Distinct().Count();
            if (distinct >= minDistinct)
            {
                clusters.Add(new ValueCluster(
                    SpanLo: slice[0].Addr,
                    SpanHi: slice[^1].Addr,
                    Hits: slice));
                i = j;
            }
            else
            {
                i++;
            }
        }
        clusters.Sort((a, b) => b.DistinctFieldCount.CompareTo(a.DistinctFieldCount));
        return clusters;
    }

    /// <summary>Scan back from <paramref name="addr"/> in 8-byte steps
    /// up to <paramref name="maxWalk"/> bytes; for each slot, treat the
    /// u64 as a candidate <c>klass_ptr</c>. If
    /// <paramref name="gaEnd"/> != 0, require klass_ptr to live in
    /// [<paramref name="gaBase"/>, <paramref name="gaEnd"/>); otherwise
    /// require <paramref name="inModule"/>(klass_ptr). Returns the
    /// 8 closest candidates (Python default), nearest-first by
    /// |obj_addr - addr|.</summary>
    public static IReadOnlyList<OwnerCandidate> WalkBackToKlass(
        IMemorySource memory,
        ulong addr,
        ulong gaBase,
        ulong gaEnd,
        Func<ulong, bool>? inModule = null,
        int maxWalk = DefaultWalkBack,
        int maxResults = DefaultWalkBackResults)
    {
        ArgumentNullException.ThrowIfNull(memory);
        ulong aligned = addr - (addr % 8);
        ulong regionStart = aligned > (ulong)maxWalk
            ? aligned - (ulong)maxWalk
            : 0x10000UL;
        if (regionStart < 0x10000UL) regionStart = 0x10000UL;
        if (aligned <= regionStart) return Array.Empty<OwnerCandidate>();
        int regionSize = (int)(aligned - regionStart);
        if (regionSize < 8) return Array.Empty<OwnerCandidate>();
        var buf = memory.ReadBytes(regionStart, regionSize);
        if (buf is null) return Array.Empty<OwnerCandidate>();
        int slots = buf.Length / 8;
        var hits = new List<OwnerCandidate>();
        for (int i = 0; i < slots; i++)
        {
            ulong kp = BinaryPrimitives.ReadUInt64LittleEndian(buf.AsSpan(i * 8, 8));
            if (gaEnd != 0)
            {
                if (kp < gaBase || kp >= gaEnd) continue;
            }
            else
            {
                if (inModule is null || !inModule(kp)) continue;
            }
            hits.Add(new OwnerCandidate(regionStart + (ulong)(i * 8), kp));
        }
        hits.Sort((a, b) =>
        {
            ulong da = a.ObjAddr <= addr ? addr - a.ObjAddr : a.ObjAddr - addr;
            ulong db = b.ObjAddr <= addr ? addr - b.ObjAddr : b.ObjAddr - addr;
            return da.CompareTo(db);
        });
        if (hits.Count > maxResults) hits.RemoveRange(maxResults, hits.Count - maxResults);
        return hits;
    }

    /// <summary>Dump every 8-byte aligned slot in a small window around
    /// <paramref name="addr"/> that looks like a user-space pointer
    /// (0x10000 ≤ v ≤ 0x7FFFFFFFFFFF). Tags each as "module" or "heap"
    /// using <paramref name="inModule"/>. Helpful when
    /// <see cref="WalkBackToKlass"/> finds no GA candidate and the
    /// caller wants to see the surrounding pointer structure.</summary>
    public static IReadOnlyList<NeighborhoodPointer> DiagnoseNeighborhood(
        IMemorySource memory,
        ulong addr,
        Func<ulong, bool> inModule,
        int before = 0x100,
        int after = 0x40)
    {
        ArgumentNullException.ThrowIfNull(memory);
        ArgumentNullException.ThrowIfNull(inModule);
        ulong aligned = addr - (addr % 8);
        ulong regionStart = aligned > (ulong)before
            ? aligned - (ulong)before
            : 0x10000UL;
        if (regionStart < 0x10000UL) regionStart = 0x10000UL;
        ulong regionEnd = aligned + (ulong)after;
        if (regionEnd <= regionStart) return Array.Empty<NeighborhoodPointer>();
        int size = (int)(regionEnd - regionStart);
        var buf = memory.ReadBytes(regionStart, size);
        if (buf is null) return Array.Empty<NeighborhoodPointer>();
        int slots = buf.Length / 8;
        var pts = new List<NeighborhoodPointer>();
        for (int i = 0; i < slots; i++)
        {
            ulong v = BinaryPrimitives.ReadUInt64LittleEndian(buf.AsSpan(i * 8, 8));
            if (v < 0x10000UL || v > 0x7FFFFFFFFFFFUL) continue;
            long rel = (long)(regionStart + (ulong)(i * 8)) - (long)addr;
            string kind = inModule(v) ? "module" : "heap";
            pts.Add(new NeighborhoodPointer(rel, v, kind));
        }
        return pts;
    }
}
