using System.Buffers.Binary;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class AnchorDiscoveryEngineTests
{
    [Fact]
    public void ObserveDeduplicatesByUuidAndPrefersLargerMaxHp()
    {
        var eng = new AnchorDiscoveryEngine();
        eng.Observe(new EntityTruth(0xAA, 100, 500, "a"));
        eng.Observe(new EntityTruth(0xAA, 80, 1000, "a")); // larger max → wins
        eng.Observe(new EntityTruth(0xAA, 100, 500, "a")); // smaller max → ignored

        Assert.Equal(1, eng.TruthCount);
        var t = eng.SnapshotTruths().Single();
        Assert.Equal(1000, t.MaxHp);
    }

    [Fact]
    public void ObserveIgnoresZeroUuidAndZeroMax()
    {
        var eng = new AnchorDiscoveryEngine();
        eng.Observe(new EntityTruth(0, 100, 500, ""));
        eng.Observe(new EntityTruth(0xAA, 100, 0, ""));
        Assert.Equal(0, eng.TruthCount);
    }

    [Fact]
    public void ScanFindsHpAtCanonicalRelativeOffsets()
    {
        var eng = new AnchorDiscoveryEngine(minTruths: 2);
        eng.Observe(new EntityTruth(0xCAFEBABEDEADBEEFUL, 7500, 12000, "boss-a"));
        eng.Observe(new EntityTruth(0x1122334455667788UL, 200, 800, "mob-b"));

        // Layout per truth: [uuid|8B][filler|8B][hp|8B][maxHp|8B] = 32 bytes
        // start each at an aligned offset (16) so FindAlignedU64 picks them up.
        var page = new byte[256];
        WriteEntityAt(page, offset: 16, uuid: 0xCAFEBABEDEADBEEFUL, hp: 7500, maxHp: 12000);
        WriteEntityAt(page, offset: 96, uuid: 0x1122334455667788UL, hp: 200, maxHp: 800);

        using var src = new FixtureMemorySource();
        src.Map(0x10_000, page);

        var result = eng.Scan(src);
        Assert.Equal(2, result.TruthsConsidered);
        Assert.Equal(2, result.Hits.Count);
        Assert.True(result.IsConfident);

        var bossHit = result.Hits.Single(h => h.Uuid == 0xCAFEBABEDEADBEEFUL);
        Assert.Equal(16, bossHit.UuidOffset);
        Assert.Equal(7500, bossHit.ObservedHp);
        Assert.Equal(12000, bossHit.ObservedMaxHp);
    }

    [Fact]
    public void ScanReturnsEmptyWhenNotEnoughTruths()
    {
        var eng = new AnchorDiscoveryEngine(minTruths: 3);
        eng.Observe(new EntityTruth(0xAA, 100, 500, ""));
        using var src = new FixtureMemorySource();
        var result = eng.Scan(src);
        Assert.Empty(result.Hits);
        Assert.False(result.IsConfident);
    }

    [Fact]
    public void ClearResetsTruths()
    {
        var eng = new AnchorDiscoveryEngine();
        eng.Observe(new EntityTruth(0xAA, 100, 500, ""));
        Assert.Equal(1, eng.TruthCount);
        eng.Clear();
        Assert.Equal(0, eng.TruthCount);
    }

    private static void WriteEntityAt(byte[] page, int offset, ulong uuid, long hp, long maxHp)
    {
        BinaryPrimitives.WriteUInt64LittleEndian(page.AsSpan(offset, 8), uuid);
        BinaryPrimitives.WriteInt64LittleEndian(page.AsSpan(offset + 0x10, 8), hp);
        BinaryPrimitives.WriteInt64LittleEndian(page.AsSpan(offset + 0x18, 8), maxHp);
    }
}

public class TcpProbeSourceTests
{
    [Fact]
    public void StateChangeProducesSnapshot()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);
        using var probe = new TcpProbeSource(state, bridge);
        probe.Start();

        bridge.Apply(new HealthEvent(8500, 12000, 8500.0 / 12000.0, 0.0));
        var snap = probe.Latest;
        Assert.NotNull(snap);
        Assert.Equal(8500, snap!.Value.Hp);
        Assert.Equal(12000, snap.Value.MaxHp);
        Assert.Contains(8500L, probe.SeenHp);
        Assert.Contains(12000L, probe.SeenMaxHp);
    }

    [Fact]
    public void HpInWindowFindsRecentValue()
    {
        var nowOffset = TimeSpan.Zero;
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);
        using var probe = new TcpProbeSource(
            state, bridge,
            historyCapacity: TimeSpan.FromSeconds(2),
            clock: () => DateTimeOffset.UtcNow + nowOffset);
        probe.Start();

        bridge.Apply(new HealthEvent(7000, 12000, 0.58, 0.0));
        Assert.True(probe.HpInWindow(7000, TimeSpan.FromMilliseconds(500)));

        nowOffset = TimeSpan.FromSeconds(5); // jump time forward beyond window
        Assert.False(probe.HpInWindow(7000, TimeSpan.FromMilliseconds(500)));
    }

    [Fact]
    public void StopUnsubscribes()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);
        using var probe = new TcpProbeSource(state, bridge);
        probe.Start();
        bridge.Apply(new HealthEvent(1, 2, 0.5, 0.0));
        Assert.NotNull(probe.Latest);

        probe.Stop();
        probe.Reset();
        bridge.Apply(new HealthEvent(99, 100, 0.99, 0.0));
        Assert.Null(probe.Latest);
    }

    [Fact]
    public void ResetClearsHistoryAndSeen()
    {
        var state = new GameStateManager();
        var parser = new PacketParser();
        using var bridge = new PacketBridge(state, parser);
        using var probe = new TcpProbeSource(state, bridge);
        probe.Start();
        bridge.Apply(new HealthEvent(1, 2, 0.5, 0.0));
        probe.Reset();
        Assert.Null(probe.Latest);
        Assert.Empty(probe.SeenHp);
        Assert.Empty(probe.SeenMaxHp);
    }
}
