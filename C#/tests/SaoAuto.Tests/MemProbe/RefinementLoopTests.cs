using System.Buffers.Binary;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class RefinementLoopTests
{
    private sealed class FakeSampler : IRefinementTcpSampler
    {
        public long CurrentHp { get; set; }
        public long CurrentMaxHp { get; set; }
        public long CurrentUid { get; set; }
    }

    private static Func<TimeSpan, CancellationToken, Task> NoDelay()
        => (_, _) => Task.CompletedTask;

    private static byte[] PadBytes(int len, byte fill = 0xCC)
    {
        var b = new byte[len];
        for (int i = 0; i < len; i++) b[i] = fill;
        return b;
    }

    private static void WriteI32(byte[] page, int offset, int value)
        => BinaryPrimitives.WriteInt32LittleEndian(page.AsSpan(offset, 4), value);

    private static void WriteU64(byte[] page, int offset, ulong value)
        => BinaryPrimitives.WriteUInt64LittleEndian(page.AsSpan(offset, 8), value);

    [Fact]
    public async Task EmptyHpCandidatesReturnsEmptyStatus()
    {
        using var mem = new FixtureMemorySource();
        var loop = new RefinementLoop(mem, new FakeSampler(), NoDelay());
        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: Array.Empty<ulong>(),
            maxHpCandidates: Array.Empty<ulong>());
        Assert.Equal(RefinementStatus.EmptyCandidates, result.Status);
    }

    [Fact]
    public async Task LockstepKeepsAddressMatchingTcpHp()
    {
        using var mem = new FixtureMemorySource();
        var page = PadBytes(64);
        WriteI32(page, 0, 500);     // real HP at 0x1000
        WriteI32(page, 16, 999);    // decoy at 0x1010 (never matches)
        mem.Map(0x1000, page);

        var sampler = new FakeSampler { CurrentHp = 500 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());
        var survivors = await loop.VerifyLockstepAsync(
            new ulong[] { 0x1000, 0x1010 },
            samples: 10, interval: TimeSpan.Zero, minMatchRatio: 0.5);
        Assert.Single(survivors);
        Assert.Equal(0x1000UL, survivors[0]);
    }

    [Fact]
    public async Task LockstepDropsAllWhenTcpHpZero()
    {
        using var mem = new FixtureMemorySource();
        var page = PadBytes(8);
        WriteI32(page, 0, 100);
        mem.Map(0x2000, page);
        var loop = new RefinementLoop(mem, new FakeSampler { CurrentHp = 0 }, NoDelay());
        var survivors = await loop.VerifyLockstepAsync(
            new ulong[] { 0x2000 }, samples: 5, interval: TimeSpan.Zero, minMatchRatio: 0.5);
        Assert.Empty(survivors);
    }

    [Fact]
    public async Task FullPipelineLocatesHpMaxHpAndUid()
    {
        // Single Player struct at 0x10000:
        //   +0x00  hp=500
        //   +0x04  max_hp=1000
        //   +0x08  uid=0xABCDEF12345678 (8-byte aligned)
        const ulong baseAddr = 0x10000;
        const long hp = 500, maxHp = 1000;
        const ulong uid = 0xABCDEF12345678UL;
        var page = PadBytes(0x100);
        WriteI32(page, 0, (int)hp);
        WriteI32(page, 4, (int)maxHp);
        WriteU64(page, 8, uid);

        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler
        {
            CurrentHp = hp, CurrentMaxHp = maxHp, CurrentUid = (long)uid,
        };
        var loop = new RefinementLoop(mem, sampler, NoDelay());

        var result = await loop.RunAsync(
            uidCandidates: new ulong[] { baseAddr + 8 },
            hpCandidates: new ulong[] { baseAddr },
            maxHpCandidates: new ulong[] { baseAddr + 4 },
            lockstepSamples: 6, lockstepInterval: TimeSpan.Zero);

        Assert.Equal(RefinementStatus.Success, result.Status);
        Assert.Equal(baseAddr, result.HpAddr);
        Assert.Equal(baseAddr + 4, result.MaxHpAddr);
        Assert.Equal(baseAddr + 8, result.UidAddr);
        Assert.Equal(baseAddr & ~0xFUL, result.StructBaseGuess);
    }

    [Fact]
    public async Task PartialResultWhenUidMissing()
    {
        const ulong baseAddr = 0x20000;
        var page = PadBytes(0x40);
        WriteI32(page, 0, 250);
        WriteI32(page, 8, 800);  // delta = +8, valid sibling
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler { CurrentHp = 250, CurrentMaxHp = 800, CurrentUid = 0 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());

        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: new ulong[] { baseAddr },
            maxHpCandidates: new ulong[] { baseAddr + 8 },
            lockstepSamples: 4, lockstepInterval: TimeSpan.Zero);

        Assert.Equal(RefinementStatus.Partial, result.Status);
        Assert.Equal(baseAddr, result.HpAddr);
        Assert.Equal(baseAddr + 8, result.MaxHpAddr);
        Assert.Null(result.UidAddr);
        Assert.Equal(baseAddr & ~0xFUL, result.StructBaseGuess);
    }

    [Fact]
    public async Task LocalMaxHpFallbackTriggersWhenGlobalPairFails()
    {
        // HP at baseAddr+0x200, MaxHP at baseAddr+0x300 (delta=+0x100, NOT a
        // known sibling offset). Global FindPairs misses; ±0x200 local search
        // should hit. HP placed mid-page so the ±0x200 window stays mapped.
        const ulong baseAddr = 0x30000;
        var page = PadBytes(0x500);
        WriteI32(page, 0x200, 333);      // hp
        WriteI32(page, 0x300, 777);      // maxhp
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler { CurrentHp = 333, CurrentMaxHp = 777, CurrentUid = 0 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());

        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: new ulong[] { baseAddr + 0x200 },
            maxHpCandidates: Array.Empty<ulong>(),  // empty global maxhp set
            lockstepSamples: 4, lockstepInterval: TimeSpan.Zero);

        Assert.Equal(RefinementStatus.Partial, result.Status);
        Assert.Equal(baseAddr + 0x200, result.HpAddr);
        Assert.Equal(baseAddr + 0x300, result.MaxHpAddr);
    }

    [Fact]
    public async Task NoLockstepSurvivorsWhenMemoryDoesNotMatchTcp()
    {
        using var mem = new FixtureMemorySource();
        var page = PadBytes(8);
        WriteI32(page, 0, 100);
        mem.Map(0x40000, page);
        var sampler = new FakeSampler { CurrentHp = 999 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());

        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: new ulong[] { 0x40000 },
            maxHpCandidates: Array.Empty<ulong>(),
            lockstepSamples: 5, lockstepInterval: TimeSpan.Zero);

        Assert.Equal(RefinementStatus.NoLockstep, result.Status);
    }

    [Fact]
    public void LocalFindUid64ReturnsAlignedHits()
    {
        const ulong baseAddr = 0x50000;
        const ulong uid = 0x1122334455667788UL;
        var page = PadBytes(0x80);
        WriteU64(page, 0x20, uid);    // aligned
        WriteU64(page, 0x44, uid);    // 4-byte aligned but not 8-byte → reject
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);
        var loop = new RefinementLoop(mem, new FakeSampler(), NoDelay());

        var hits = loop.LocalFindUid64(anchor: baseAddr + 0x40, uidNeedle: uid, radius: 0x40);
        Assert.Single(hits);
        Assert.Equal(baseAddr + 0x20, hits[0]);
    }

    [Fact]
    public async Task NearbyUidUniqueWinsOverLocalSearch()
    {
        // pre-scan UID candidate list contains one address inside the
        // ±0x800 window around HP — should be picked even though the
        // local UID needle search would also find it.
        const ulong baseAddr = 0x60000;
        var page = PadBytes(0x40);
        WriteI32(page, 0, 50);     // hp
        WriteI32(page, 4, 99);     // maxhp at +4
        WriteU64(page, 0x10, 42);  // uid bytes too (just for completeness)
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler { CurrentHp = 50, CurrentMaxHp = 99, CurrentUid = 42 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());
        var result = await loop.RunAsync(
            uidCandidates: new ulong[] { baseAddr + 0x10 },
            hpCandidates: new ulong[] { baseAddr },
            maxHpCandidates: new ulong[] { baseAddr + 4 },
            lockstepSamples: 3, lockstepInterval: TimeSpan.Zero);
        Assert.Equal(RefinementStatus.Success, result.Status);
        Assert.Equal(baseAddr + 0x10UL, result.UidAddr);
    }

    [Fact]
    public async Task AutoRescanSeedsHpCandidatesWhenEmpty()
    {
        // No HP candidates passed; TCP knows HP=777; MemoryScanner finds it.
        const ulong baseAddr = 0x70000;
        var page = PadBytes(0x40);
        WriteI32(page, 0, 777);
        WriteI32(page, 8, 1500);  // maxhp at +8
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler { CurrentHp = 777, CurrentMaxHp = 1500, CurrentUid = 0 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());
        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: Array.Empty<ulong>(),
            maxHpCandidates: new ulong[] { baseAddr + 8 },
            lockstepSamples: 3, lockstepInterval: TimeSpan.Zero,
            autoRescan: true);

        Assert.Equal(RefinementStatus.Partial, result.Status);
        Assert.Equal(baseAddr, result.HpAddr);
        Assert.Equal(baseAddr + 8, result.MaxHpAddr);
    }

    [Fact]
    public async Task AutoRescanFalseStillReturnsEmptyCandidates()
    {
        // Regression check: without autoRescan, empty HP set short-circuits.
        const ulong baseAddr = 0x80000;
        var page = PadBytes(0x10);
        WriteI32(page, 0, 100);
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler { CurrentHp = 100 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());
        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: Array.Empty<ulong>(),
            maxHpCandidates: Array.Empty<ulong>());
        Assert.Equal(RefinementStatus.EmptyCandidates, result.Status);
    }

    [Fact]
    public async Task AutoRescanReseedsMaxHpAfterNarrowEmpties()
    {
        // hpCandidates has the real HP; maxHpCandidates is stale (none match
        // current MaxHP). With autoRescan, MaxHP set is rebuilt by scanning.
        const ulong baseAddr = 0x90000;
        var page = PadBytes(0x40);
        WriteI32(page, 0, 250);
        WriteI32(page, 4, 600);
        using var mem = new FixtureMemorySource();
        mem.Map(baseAddr, page);

        var sampler = new FakeSampler { CurrentHp = 250, CurrentMaxHp = 600, CurrentUid = 0 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());
        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: new ulong[] { baseAddr },
            maxHpCandidates: new ulong[] { 0xDEAD0000 },  // stale — does not contain 600
            lockstepSamples: 3, lockstepInterval: TimeSpan.Zero,
            autoRescan: true);

        Assert.Equal(RefinementStatus.Partial, result.Status);
        Assert.Equal(baseAddr, result.HpAddr);
        Assert.Equal(baseAddr + 4, result.MaxHpAddr);
    }

    [Fact]
    public async Task AutoRescanGivesUpWhenTcpHpZero()
    {
        // autoRescan=true but TCP HP unknown → still EmptyCandidates.
        using var mem = new FixtureMemorySource();
        mem.Map(0xA0000, PadBytes(0x10));
        var sampler = new FakeSampler { CurrentHp = 0 };
        var loop = new RefinementLoop(mem, sampler, NoDelay());
        var result = await loop.RunAsync(
            uidCandidates: Array.Empty<ulong>(),
            hpCandidates: Array.Empty<ulong>(),
            maxHpCandidates: Array.Empty<ulong>(),
            autoRescan: true);
        Assert.Equal(RefinementStatus.EmptyCandidates, result.Status);
    }
}
