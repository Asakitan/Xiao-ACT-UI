using System.Buffers.Binary;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class HardUnpackTests
{
    private static byte[] Pad(int len, byte fill = 0xCC)
    {
        var b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = fill;
        return b;
    }

    private static void WriteI32(byte[] page, int off, int value)
        => BinaryPrimitives.WriteInt32LittleEndian(page.AsSpan(off, 4), value);

    private static void WriteU64(byte[] page, int off, ulong value)
        => BinaryPrimitives.WriteUInt64LittleEndian(page.AsSpan(off, 8), value);

    private static void WriteF32(byte[] page, int off, float value)
        => BinaryPrimitives.WriteSingleLittleEndian(page.AsSpan(off, 4), value);

    [Fact]
    public void BuildNeedles_intEmitsI32AndI64_dropsZero()
    {
        var fields = new[]
        {
            new StatField("p", "hp", 535339, true),
            new StatField("p", "zero", 0, true),
        };
        var ns = HardUnpack.BuildNeedles(fields);
        Assert.True(ns.U32.ContainsKey(535339u));
        Assert.True(ns.U64.ContainsKey(535339UL));
        Assert.Single(ns.U32[535339u]);
        Assert.Equal("i32", ns.U32[535339u][0].Encoding);
        Assert.Equal("i64", ns.U64[535339UL][0].Encoding);
    }

    [Fact]
    public void BuildNeedles_intInF32OfIntRange_emitsExtraNeedle()
    {
        var fields = new[] { new StatField("w", "haste", 5400, true) };
        var ns = HardUnpack.BuildNeedles(fields);
        uint f32 = BitConverter.SingleToUInt32Bits(5400f);
        Assert.True(ns.U32.ContainsKey(f32));
        Assert.Contains(ns.U32[f32], e => e.Encoding == "f32_of_int");
        Assert.Contains(ns.U32[5400u], e => e.Encoding == "i32");
    }

    [Fact]
    public void BuildNeedles_floatEmitsF32AndF64()
    {
        var fields = new[] { new StatField("p", "crit", 0.2095, false) };
        var ns = HardUnpack.BuildNeedles(fields);
        uint f32 = BitConverter.SingleToUInt32Bits(0.2095f);
        ulong f64 = BitConverter.DoubleToUInt64Bits(0.2095);
        Assert.Contains(ns.U32[f32], e => e.Encoding == "f32");
        Assert.Contains(ns.U64[f64], e => e.Encoding == "f64");
    }

    [Fact]
    public void ScanRegions_findsI32AndI64Hits()
    {
        // Place hp=535339 as i32 at +0 and as i64 at +0x18. The i32 scan
        // ALSO catches the low half of the i64 (same bit pattern), so
        // expect 3 hits total (i32@0, i32@0x18, i64@0x18). Pad with 0xCC
        // bytes so no other slot accidentally matches.
        var page = Pad(0x40);
        WriteI32(page, 0, 535339);
        WriteU64(page, 0x18, 535339UL);
        using var mem = new FixtureMemorySource();
        mem.Map(0x10000, page);

        var ns = HardUnpack.BuildNeedles(new[] { new StatField("p", "hp", 535339, true) });
        var hits = HardUnpack.ScanRegions(mem, ns);
        Assert.Contains(hits, h => h.Addr == 0x10000UL && h.Encoding == "i32");
        Assert.Contains(hits, h => h.Addr == 0x10018UL && h.Encoding == "i64");
        Assert.Contains(hits, h => h.Addr == 0x10018UL && h.Encoding == "i32");
        Assert.Equal(3, hits.Count);
    }

    [Fact]
    public void ScanRegions_skipsModulePages()
    {
        var page = Pad(0x40);
        WriteI32(page, 0, 9999);
        using var mem = new FixtureMemorySource();
        mem.Map(0x20000, page);
        bool InModule(ulong addr) => addr == 0x20000UL;
        var ns = HardUnpack.BuildNeedles(new[] { new StatField("p", "x", 9999, true) });
        var hits = HardUnpack.ScanRegions(mem, ns, inModule: InModule);
        Assert.Empty(hits);
    }

    [Fact]
    public void ClusterHits_emitsClusterWhenWindowHasMinDistinctFields()
    {
        var hits = new[]
        {
            new ValueHit(0x1000, "p.a", 1, "i32"),
            new ValueHit(0x1010, "p.b", 2, "i32"),
            new ValueHit(0x1100, "p.c", 3, "i32"),  // within 0x400 of first
            new ValueHit(0x4000, "p.d", 4, "i32"),  // far away, alone
        };
        var clusters = HardUnpack.ClusterHits(hits, window: 0x400, minDistinct: 3);
        Assert.Single(clusters);
        Assert.Equal(0x1000UL, clusters[0].SpanLo);
        Assert.Equal(0x1100UL, clusters[0].SpanHi);
        Assert.Equal(3, clusters[0].DistinctFieldCount);
    }

    [Fact]
    public void ClusterHits_skipsBelowMinDistinct()
    {
        // 3 hits but only 2 distinct field names → no cluster.
        var hits = new[]
        {
            new ValueHit(0x1000, "p.a", 1, "i32"),
            new ValueHit(0x1010, "p.a", 1, "i64"),
            new ValueHit(0x1020, "p.b", 2, "i32"),
        };
        var clusters = HardUnpack.ClusterHits(hits, window: 0x400, minDistinct: 3);
        Assert.Empty(clusters);
    }

    [Fact]
    public void ClusterHits_sortedByDistinctFieldsDescending()
    {
        var hits = new[]
        {
            // Cluster A: 3 distinct
            new ValueHit(0x1000, "p.a", 1, "i32"),
            new ValueHit(0x1010, "p.b", 2, "i32"),
            new ValueHit(0x1020, "p.c", 3, "i32"),
            // Cluster B: 4 distinct, far away
            new ValueHit(0x9000, "p.d", 4, "i32"),
            new ValueHit(0x9010, "p.e", 5, "i32"),
            new ValueHit(0x9020, "p.f", 6, "i32"),
            new ValueHit(0x9030, "p.g", 7, "i32"),
        };
        var clusters = HardUnpack.ClusterHits(hits);
        Assert.Equal(2, clusters.Count);
        Assert.Equal(4, clusters[0].DistinctFieldCount);
        Assert.Equal(0x9000UL, clusters[0].SpanLo);
    }

    [Fact]
    public void WalkBackToKlass_findsGaPointers_nearestFirst()
    {
        // Place two GA-pointing slots before addr=0x20100. Page must
        // span [regionStart..aligned] so FixtureMemorySource doesn't
        // partial-read; with maxWalk=0x100 that's [0x20000..0x20100].
        const ulong gaBase = 0x7FF0000000UL;
        const ulong gaEnd = 0x7FF1000000UL;
        var page = Pad(0x200);
        WriteU64(page, 0x80, 0x7FF000ABCDUL);   // farther
        WriteU64(page, 0xF8, 0x7FF000DEADUL);   // closer to 0x100
        using var mem = new FixtureMemorySource();
        mem.Map(0x20000, page);
        var hits = HardUnpack.WalkBackToKlass(
            mem, addr: 0x20100, gaBase: gaBase, gaEnd: gaEnd, maxWalk: 0x100);
        Assert.Equal(2, hits.Count);
        // Nearest by |obj_addr - 0x20100| → 0x200F8 first.
        Assert.Equal(0x200F8UL, hits[0].ObjAddr);
        Assert.Equal(0x7FF000DEADUL, hits[0].KlassPtr);
        Assert.Equal(0x20080UL, hits[1].ObjAddr);
    }

    [Fact]
    public void WalkBackToKlass_skipsOutOfGaRange()
    {
        const ulong gaBase = 0x7FF0000000UL;
        const ulong gaEnd = 0x7FF1000000UL;
        // Page covers full walk window so empty result is from filtering,
        // not from a partial-read failure.
        var page = Pad(0x100);
        WriteU64(page, 0x40, 0xDEAD0000UL);  // outside GA
        using var mem = new FixtureMemorySource();
        mem.Map(0x30000, page);
        var hits = HardUnpack.WalkBackToKlass(
            mem, addr: 0x300F0, gaBase: gaBase, gaEnd: gaEnd, maxWalk: 0xF0);
        Assert.Empty(hits);
    }

    [Fact]
    public void WalkBackToKlass_inModulePredWhenGaEndZero()
    {
        // addr=0x400F0, maxWalk=0xF0 → regionStart=0x40000 (matches page).
        var page = Pad(0x100);
        WriteU64(page, 0x40, 0xCAFEBABEUL);
        using var mem = new FixtureMemorySource();
        mem.Map(0x40000, page);
        bool InMod(ulong v) => v == 0xCAFEBABEUL;
        var hits = HardUnpack.WalkBackToKlass(
            mem, addr: 0x400F0, gaBase: 0, gaEnd: 0, inModule: InMod, maxWalk: 0xF0);
        Assert.Single(hits);
        Assert.Equal(0xCAFEBABEUL, hits[0].KlassPtr);
    }

    [Fact]
    public void WalkBackToKlass_capsAtMaxResults()
    {
        const ulong gaBase = 0x7FF0000000UL;
        const ulong gaEnd = 0x7FF1000000UL;
        // addr=0x500F0, maxWalk=0xF0 → regionStart=0x50000 (matches page).
        var page = Pad(0x100);
        // 16 GA-pointers, every 8 bytes (uses bytes 0..0x80)
        for (int i = 0; i < 16; i++) WriteU64(page, i * 8, gaBase + (ulong)i);
        using var mem = new FixtureMemorySource();
        mem.Map(0x50000, page);
        var hits = HardUnpack.WalkBackToKlass(
            mem, addr: 0x500F0, gaBase: gaBase, gaEnd: gaEnd, maxWalk: 0xF0);
        Assert.Equal(8, hits.Count);
    }

    [Fact]
    public void DiagnoseNeighborhood_reportsUserPointersTagged()
    {
        var page = Pad(0x100);
        WriteU64(page, 0x40, 0x123456UL);     // user-space heap
        WriteU64(page, 0x48, 0x7FF000ABCDUL); // user-space "module"
        WriteU64(page, 0x50, 0x100UL);        // too low — skipped
        using var mem = new FixtureMemorySource();
        mem.Map(0x60000, page);
        bool InMod(ulong v) => v >= 0x7FF0000000UL;
        var pts = HardUnpack.DiagnoseNeighborhood(
            mem, addr: 0x60050, inModule: InMod, before: 0x40, after: 0x40);
        Assert.Contains(pts, p => p.Value == 0x123456UL && p.Kind == "heap");
        Assert.Contains(pts, p => p.Value == 0x7FF000ABCDUL && p.Kind == "module");
        Assert.DoesNotContain(pts, p => p.Value == 0x100UL);
    }

    [Fact]
    public void NullArgs_throw()
    {
        using var mem = new FixtureMemorySource();
        Assert.Throws<ArgumentNullException>(() => HardUnpack.BuildNeedles(null!));
        Assert.Throws<ArgumentNullException>(
            () => HardUnpack.ScanRegions(null!, HardUnpack.BuildNeedles(Array.Empty<StatField>())));
        Assert.Throws<ArgumentNullException>(
            () => HardUnpack.ScanRegions(mem, null!));
        Assert.Throws<ArgumentNullException>(() => HardUnpack.ClusterHits(null!));
        Assert.Throws<ArgumentNullException>(() => HardUnpack.WalkBackToKlass(null!, 0, 0, 0));
        Assert.Throws<ArgumentNullException>(() => HardUnpack.DiagnoseNeighborhood(null!, 0, _ => true));
        Assert.Throws<ArgumentNullException>(() => HardUnpack.DiagnoseNeighborhood(mem, 0, null!));
    }
}
