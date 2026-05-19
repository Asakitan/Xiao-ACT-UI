using System.Buffers.Binary;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class SceneDiscoveryTests
{
    private static byte[] Pad(int len, byte fill = 0xCC)
    {
        var b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = fill;
        return b;
    }

    private static void WriteI32(byte[] page, int off, int value)
        => BinaryPrimitives.WriteInt32LittleEndian(page.AsSpan(off, 4), value);

    private static void WriteI64(byte[] page, int off, long value)
        => BinaryPrimitives.WriteInt64LittleEndian(page.AsSpan(off, 8), value);

    private static void WriteU64(byte[] page, int off, ulong value)
        => BinaryPrimitives.WriteUInt64LittleEndian(page.AsSpan(off, 8), value);

    [Fact]
    public void NullArgs_throw()
    {
        using var mem = new FixtureMemorySource();
        Assert.Throws<ArgumentNullException>(
            () => SceneDiscovery.Discover(null!, new SceneTruth(1)));
        Assert.Throws<ArgumentNullException>(
            () => SceneDiscovery.Discover(mem, null!));
    }

    [Fact]
    public void UnusableTruth_returnsNull()
    {
        using var mem = new FixtureMemorySource();
        Assert.Null(SceneDiscovery.Discover(mem, new SceneTruth(0)));
        Assert.Null(SceneDiscovery.Discover(mem, new SceneTruth(-5)));
    }

    [Fact]
    public void NoHits_returnsNull()
    {
        using var mem = new FixtureMemorySource();
        mem.Map(0x10000, Pad(0x100));
        Assert.Null(SceneDiscovery.Discover(mem, new SceneTruth(0xDEADBEEFL)));
    }

    [Fact]
    public void FirstI32Offset_findsAlignedHit_returnsMinusOneOnMiss()
    {
        var page = Pad(0x40);
        WriteI32(page, 8, 1234);
        Assert.Equal(8, SceneDiscovery.FirstI32Offset(page, 1234));
        Assert.Equal(-1, SceneDiscovery.FirstI32Offset(page, 9999));
    }

    [Fact]
    public void Discover_pickedSceneUuidOff_andBodyFields()
    {
        // SceneInfo at baseAddr:
        //   +0x00  klass_ptr   (inside fake GA range)
        //   +0x10  scene_uuid  (i64)
        //   +0x20  dungeon_id  (i32)
        //   +0x28  scene_id    (i32)
        //   +0x2C  difficulty  (i32)
        //   +0x30  layer       (i32)
        const ulong baseAddr = 0x40000;
        const ulong klassPtr = 0x7FF000ABC0UL;
        const long uuid = 0x1122334455667788L;
        const int dungeonId = 7001, sceneId = 200, diff = 3, layer = 1;

        var page = Pad(0x200);
        WriteU64(page, 0x00, klassPtr);
        WriteI64(page, 0x10, uuid);
        WriteI32(page, 0x20, dungeonId);
        WriteI32(page, 0x28, sceneId);
        WriteI32(page, 0x2C, diff);
        WriteI32(page, 0x30, layer);

        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var result = SceneDiscovery.Discover(mem,
            new SceneTruth(uuid, dungeonId, sceneId, layer, diff),
            gaBase: 0x7FF0000000UL, gaEnd: 0x7FF1000000UL);

        Assert.NotNull(result);
        Assert.Equal(baseAddr, result!.ObjAddr);
        Assert.Equal(klassPtr, result.KlassPtr);
        Assert.Equal(0x10, result.SceneUuidOff);
        Assert.Equal(0x20, result.DungeonIdOff);
        Assert.Equal(0x28, result.SceneIdOff);
        Assert.Equal(0x30, result.LayerOff);
        Assert.Equal(4, result.Score);
    }

    [Fact]
    public void Discover_filtersByGameAssemblyRange()
    {
        const ulong baseAddr = 0x50000;
        // klass_ptr deliberately OUTSIDE the GA range we'll pass in
        const ulong klassPtr = 0xDEAD0000UL;
        const long uuid = 0x42424242L;

        var page = Pad(0x200);
        WriteU64(page, 0x00, klassPtr);
        WriteI64(page, 0x10, uuid);
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var result = SceneDiscovery.Discover(mem,
            new SceneTruth(uuid, 1, 2),
            gaBase: 0x7FF0000000UL, gaEnd: 0x7FF1000000UL);
        Assert.Null(result);
    }

    [Fact]
    public void Discover_skipsGaCheckWhenRangeZero()
    {
        // Same layout as above but no GA range supplied → klass_ptr can be anything.
        const ulong baseAddr = 0x60000;
        const ulong klassPtr = 0xDEAD0000UL;
        const long uuid = 0x77777777L;

        var page = Pad(0x200);
        WriteU64(page, 0x00, klassPtr);
        WriteI64(page, 0x10, uuid);
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var result = SceneDiscovery.Discover(mem, new SceneTruth(uuid));
        Assert.NotNull(result);
        Assert.Equal(baseAddr, result!.ObjAddr);
        Assert.Equal(0x10, result.SceneUuidOff);
        Assert.Equal(0, result.Score);  // no secondary truths supplied
    }

    [Fact]
    public void Discover_tieBreakPrefersOff0x10()
    {
        // Two valid candidates (same score=0): one at obj_base where
        // scene_uuid is at off 0x10, another at off 0x30. Tie-break by
        // |off - 0x10| should pick the first.
        // Layout: page has uuid written at TWO different positions, so
        // each scan hit triggers a different (uid_addr - k1) match.
        const ulong baseAddr = 0x70000;
        const long uuid = 0xABCD1234L;
        var page = Pad(0x200);
        // fake klass at obj_base candidate A: page+0x000
        WriteU64(page, 0x00, 0x7FF0CCCC00UL);
        WriteI64(page, 0x10, uuid);  // → matches k1=0x10 starting at base+0
        // fake klass at obj_base candidate B: page+0x80
        WriteU64(page, 0x80, 0x7FF0CCCC11UL);
        WriteI64(page, 0xB0, uuid);  // → matches k1=0x30 starting at base+0x80

        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var result = SceneDiscovery.Discover(mem,
            new SceneTruth(uuid),
            gaBase: 0x7FF0000000UL, gaEnd: 0x7FF1000000UL);
        Assert.NotNull(result);
        Assert.Equal(0x10, result!.SceneUuidOff);
        Assert.Equal(baseAddr, result.ObjAddr);
        Assert.True(result.CandidateCount >= 2);
    }

    [Fact]
    public void Discover_dungeonIdMissingMarkedNegative()
    {
        // Truth has dungeon_id=0 → dungeon_id_off must be -1 in result.
        const ulong baseAddr = 0x80000;
        const long uuid = 0x99999999L;
        var page = Pad(0x200);
        WriteU64(page, 0x00, 0x7FF000AAAAUL);
        WriteI64(page, 0x10, uuid);
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var result = SceneDiscovery.Discover(mem,
            new SceneTruth(uuid),  // no dungeon/scene/layer
            gaBase: 0x7FF0000000UL, gaEnd: 0x7FF1000000UL);
        Assert.NotNull(result);
        Assert.Equal(-1, result!.DungeonIdOff);
        Assert.Equal(-1, result.SceneIdOff);
        Assert.Equal(-1, result.LayerOff);
    }

    [Fact]
    public void Discover_rejectsObjBaseUnder0x10000()
    {
        // uid hit at very low address → uid - k1 lands < 0x10000 → drop.
        var page = Pad(0x100);
        WriteI64(page, 0x10, 0x5A5AL);
        using var mem = new FixtureMemorySource();
        mem.Map(0x100, page);  // base under 0x10000

        var result = SceneDiscovery.Discover(mem, new SceneTruth(0x5A5AL));
        Assert.Null(result);
    }
}
