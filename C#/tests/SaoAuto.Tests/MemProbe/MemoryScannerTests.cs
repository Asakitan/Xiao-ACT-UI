using System.Text;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class MemoryScannerTests
{
    private static byte[] Pad(int len, byte fill = 0xCC)
    {
        var b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = fill;
        return b;
    }

    [Fact]
    public void EncodeI32_writes_little_endian()
    {
        Assert.Equal(new byte[] { 0x78, 0x56, 0x34, 0x12 }, MemoryScanner.EncodeI32(0x12345678));
    }

    [Fact]
    public void EncodeI64_negative_value_round_trips_little_endian()
    {
        var b = MemoryScanner.EncodeI64(-1);
        Assert.Equal(Enumerable.Repeat<byte>(0xFF, 8).ToArray(), b);
    }

    [Fact]
    public void EncodeU64_matches_i64_byte_pattern_for_same_bits()
    {
        Assert.Equal(MemoryScanner.EncodeI64(-1), MemoryScanner.EncodeU64(0xFFFFFFFFFFFFFFFFUL));
    }

    [Fact]
    public void EncodeUtf16_no_nul_terminator()
    {
        var b = MemoryScanner.EncodeUtf16("AB");
        Assert.Equal(new byte[] { 0x41, 0x00, 0x42, 0x00 }, b);
    }

    [Fact]
    public void EncodeF32_round_trips()
    {
        var b = MemoryScanner.EncodeF32(1.5f);
        Assert.Equal(BitConverter.GetBytes(1.5f), b);
    }

    [Fact]
    public void ScanI32_finds_aligned_hits_only()
    {
        // Place value 0x12345678 at offset 0 (aligned) and offset 5 (unaligned)
        // and at offset 16 (aligned). Default i32 alignment = 4 → offset 5 rejected.
        var page = Pad(0x40);
        var needle = MemoryScanner.EncodeI32(0x12345678);
        needle.CopyTo(page, 0);
        needle.CopyTo(page, 5);
        needle.CopyTo(page, 16);

        using var mem = new FixtureMemorySource();
        mem.Map(0x10000, page);

        var hits = MemoryScanner.ScanI32(mem, 0x12345678);
        Assert.Equal(new ulong[] { 0x10000, 0x10010 }, hits);
    }

    [Fact]
    public void ScanI32_align_1_picks_unaligned_hits()
    {
        var page = Pad(0x40);
        var needle = MemoryScanner.EncodeI32(unchecked((int)0xDEADBEEF));
        needle.CopyTo(page, 5);
        using var mem = new FixtureMemorySource();
        mem.Map(0x20000, page);

        var hits = MemoryScanner.ScanI32(mem, unchecked((int)0xDEADBEEF), align: 1);
        Assert.Single(hits);
        Assert.Equal(0x20005UL, hits[0]);
    }

    [Fact]
    public void Scan_skips_regions_larger_than_max_region_size()
    {
        // Create a "huge" region so that maxRegionSize check trips.
        var page = Pad(64);
        MemoryScanner.EncodeI32(7).CopyTo(page, 0);
        using var mem = new FixtureMemorySource();
        mem.Map(0x30000, page);

        // maxRegionSize=32 → page (size 64) is skipped → no hits.
        var hits = MemoryScanner.ScanI32(mem, 7, maxRegionSize: 32);
        Assert.Empty(hits);
    }

    [Fact]
    public void Scan_caps_at_max_hits()
    {
        // Pack 10 aligned hits, ask for max 3.
        var page = new byte[40];
        var needle = MemoryScanner.EncodeI32(99);
        for (int i = 0; i < 40; i += 4) needle.CopyTo(page, i);
        using var mem = new FixtureMemorySource();
        mem.Map(0x40000, page);

        var hits = MemoryScanner.ScanI32(mem, 99, maxHits: 3);
        Assert.Equal(3, hits.Count);
    }

    [Fact]
    public void ScanU64_finds_8_byte_aligned_hits()
    {
        var page = Pad(0x40);
        MemoryScanner.EncodeU64(0xABCDEF01_23456789UL).CopyTo(page, 0);
        MemoryScanner.EncodeU64(0xABCDEF01_23456789UL).CopyTo(page, 0x18);
        using var mem = new FixtureMemorySource();
        mem.Map(0x50000, page);

        var hits = MemoryScanner.ScanU64(mem, 0xABCDEF01_23456789UL);
        Assert.Equal(new ulong[] { 0x50000, 0x50018 }, hits);
    }

    [Fact]
    public void ScanUtf16_finds_substring_without_terminator()
    {
        var bytes = Encoding.Unicode.GetBytes("hello world!");
        var page = new byte[bytes.Length + 8];
        bytes.CopyTo(page, 4);
        using var mem = new FixtureMemorySource();
        mem.Map(0x60000, page);

        var hits = MemoryScanner.ScanUtf16(mem, "world");
        Assert.Single(hits);
        // "world" starts at offset 4 + len("hello ")*2 = 4 + 12 = 16
        Assert.Equal(0x60010UL, hits[0]);
    }

    [Fact]
    public void Narrow_keeps_only_addresses_with_matching_bytes()
    {
        var page = Pad(0x20);
        MemoryScanner.EncodeI32(42).CopyTo(page, 0);
        MemoryScanner.EncodeI32(43).CopyTo(page, 8);
        using var mem = new FixtureMemorySource();
        mem.Map(0x70000, page);

        var kept = MemoryScanner.Narrow(mem,
            new ulong[] { 0x70000, 0x70008, 0x70010 },
            MemoryScanner.EncodeI32(42));
        Assert.Equal(new ulong[] { 0x70000 }, kept);
    }

    [Fact]
    public void Narrow_drops_unreadable_addresses()
    {
        var page = Pad(8);
        MemoryScanner.EncodeI32(7).CopyTo(page, 0);
        using var mem = new FixtureMemorySource();
        mem.Map(0x80000, page);
        var kept = MemoryScanner.Narrow(mem,
            new ulong[] { 0x80000, 0xDEADBEEFUL },
            MemoryScanner.EncodeI32(7));
        Assert.Equal(new ulong[] { 0x80000 }, kept);
    }
}
