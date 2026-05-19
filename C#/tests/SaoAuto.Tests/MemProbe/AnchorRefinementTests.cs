using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class AnchorRefinementTests
{
    [Fact]
    public void HpMaxHpDeltas_match_python_order()
    {
        Assert.Equal(
            new[] { 4, 8, 0x10, 0x18, 0x20, -4, -8, -0x10, -0x18, -0x20 },
            AnchorRefinement.HpMaxHpDeltas);
    }

    [Fact]
    public void FindPairs_emits_one_pair_per_matching_delta()
    {
        var hp = new ulong[] { 0x1000, 0x2000 };
        // 0x1000 + 4 → match; 0x2000 + 0x18 → match
        var max = new ulong[] { 0x1004, 0x2018, 0x3000 };
        var pairs = AnchorRefinement.FindPairs(hp, max);
        Assert.Equal(2, pairs.Count);
        Assert.Contains(new AnchorRefinement.HpMaxHpPair(0x1000, 0x1004, 4), pairs);
        Assert.Contains(new AnchorRefinement.HpMaxHpPair(0x2000, 0x2018, 0x18), pairs);
    }

    [Fact]
    public void FindPairs_handles_negative_deltas()
    {
        // hp at 0x2000, max at 0x1FF0 → delta -0x10
        var pairs = AnchorRefinement.FindPairs(
            new ulong[] { 0x2000 }, new ulong[] { 0x1FF0 });
        Assert.Single(pairs);
        Assert.Equal(-0x10, pairs[0].Delta);
        Assert.Equal((ulong)0x1FF0, pairs[0].MaxHpAddr);
    }

    [Fact]
    public void FindPairs_emits_multiple_pairs_for_one_hp_when_layout_ambiguous()
    {
        // Both +4 and +8 happen to land on a maxhp candidate
        var pairs = AnchorRefinement.FindPairs(
            new ulong[] { 0x1000 }, new ulong[] { 0x1004, 0x1008 });
        Assert.Equal(2, pairs.Count);
    }

    [Fact]
    public void FindPairs_empty_inputs_return_empty()
    {
        Assert.Empty(AnchorRefinement.FindPairs(Array.Empty<ulong>(), Array.Empty<ulong>()));
        Assert.Empty(AnchorRefinement.FindPairs(new ulong[] { 0x1000 }, Array.Empty<ulong>()));
    }

    [Fact]
    public void FindNearbyUid_inclusive_bounds_around_anchor()
    {
        // anchor 0x10000, default radius 0x800 → window [0xF800, 0x10800]
        var uids = new ulong[] { 0xF000, 0xF800, 0x10000, 0x10800, 0x10801 };
        var hits = AnchorRefinement.FindNearbyUid(uids, anchor: 0x10000);
        Assert.Equal(new ulong[] { 0xF800, 0x10000, 0x10800 }, hits);
    }

    [Fact]
    public void FindNearbyUid_clamps_low_addr_at_zero()
    {
        var hits = AnchorRefinement.FindNearbyUid(
            new ulong[] { 0x0, 0x100 }, anchor: 0x50, radius: 0x200);
        Assert.Equal(new ulong[] { 0x0, 0x100 }, hits);
    }

    [Fact]
    public void FindNearbyUid_returns_sorted_even_when_input_unsorted()
    {
        var hits = AnchorRefinement.FindNearbyUid(
            new ulong[] { 0x10500, 0x10100, 0x10300 }, anchor: 0x10000, radius: 0x800);
        Assert.Equal(new ulong[] { 0x10100, 0x10300, 0x10500 }, hits);
    }

    [Fact]
    public void FindNearbyUid_negative_radius_throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            AnchorRefinement.FindNearbyUid(Array.Empty<ulong>(), 0, radius: -1));
    }

    [Fact]
    public void LocalFindI32_returns_aligned_hits_only()
    {
        // blob = 4 bytes value 0xDEAD, then a misaligned 0xDEAD at offset 5
        // Python only emits the aligned hit at offset 0.
        Span<byte> blob = stackalloc byte[]
        {
            0xAD, 0xDE, 0x00, 0x00,            // i32=0xDEAD at offset 0
            0xFF,
            0xAD, 0xDE, 0x00, 0x00,            // misaligned (offset 5)
            0xAD, 0xDE, 0x00, 0x00,            // aligned at offset 9? no, offset 9 not div by 4
            0xAD, 0xDE, 0x00, 0x00,            // offset 13
        };
        var hits = AnchorRefinement.LocalFindI32(blob, blobBaseAddr: 0x1000, targetI32: 0xDEAD);
        Assert.Equal(new ulong[] { 0x1000 }, hits);
    }

    [Fact]
    public void LocalFindI32_emits_absolute_addresses()
    {
        Span<byte> blob = stackalloc byte[]
        {
            0x00, 0x00, 0x00, 0x00,
            0x2A, 0x00, 0x00, 0x00,  // i32=42 at offset 4
            0x00, 0x00, 0x00, 0x00,
            0x2A, 0x00, 0x00, 0x00,  // i32=42 at offset 12
        };
        var hits = AnchorRefinement.LocalFindI32(blob, 0x20000, 42);
        Assert.Equal(new ulong[] { 0x20004, 0x2000C }, hits);
    }

    [Fact]
    public void LocalFindI32_short_blob_returns_empty()
    {
        Assert.Empty(AnchorRefinement.LocalFindI32(stackalloc byte[3], 0, 0));
    }

    [Fact]
    public void NarrowI32_keeps_only_addrs_whose_value_matches()
    {
        using var src = new FixtureMemorySource();
        // 16-byte page at 0x10000 with i32=999 at offset 0, i32=42 at offset 4,
        // i32=999 at offset 8, i32=42 at offset 12.
        src.Map(0x10000, new byte[]
        {
            0xE7, 0x03, 0x00, 0x00,
            0x2A, 0x00, 0x00, 0x00,
            0xE7, 0x03, 0x00, 0x00,
            0x2A, 0x00, 0x00, 0x00,
        });

        var keep = AnchorRefinement.NarrowI32(
            src, new ulong[] { 0x10000, 0x10004, 0x10008, 0x1000C, 0x99999 }, value: 42);
        Assert.Equal(new ulong[] { 0x10004, 0x1000C }, keep);
    }

    [Fact]
    public void NarrowI32_silently_skips_unreadable_addrs()
    {
        using var src = new FixtureMemorySource();
        // No mappings → every read returns null → empty result, no throw.
        var keep = AnchorRefinement.NarrowI32(src, new ulong[] { 0x1, 0x2, 0x3 }, 0);
        Assert.Empty(keep);
    }
}
