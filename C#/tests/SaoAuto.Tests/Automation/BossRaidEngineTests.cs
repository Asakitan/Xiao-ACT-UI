using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class BossRaidEngineTests
{
    [Fact]
    public void StartFiresPhaseEnteredForFirstPhase()
    {
        var engine = new BossRaidEngine();
        var phases = new List<RaidPhase>();
        engine.PhaseEntered += phases.Add;

        engine.Start(new[]
        {
            new RaidPhase(0, "P1", 60),
            new RaidPhase(1, "P2", 60),
        }, enrageSeconds: 180);

        Assert.True(engine.Running);
        Assert.Single(phases);
        Assert.Equal("P1", phases[0].Name);
        Assert.Equal("P1", engine.CurrentPhase!.Name);
    }

    [Fact]
    public void NextPhaseAdvances()
    {
        var engine = new BossRaidEngine();
        var phases = new List<RaidPhase>();
        engine.PhaseEntered += phases.Add;
        engine.Start(new[] { new RaidPhase(0, "P1", 60), new RaidPhase(1, "P2", 60) });

        engine.NextPhase();

        Assert.Equal(2, phases.Count);
        Assert.Equal("P2", phases[1].Name);
    }

    [Fact]
    public void NextPhasePastEndStopsAndFiresEnded()
    {
        var engine = new BossRaidEngine();
        var ended = false;
        engine.RaidEnded += () => ended = true;
        engine.Start(new[] { new RaidPhase(0, "P1", 60) });
        engine.NextPhase();

        Assert.False(engine.Running);
        Assert.True(ended);
    }

    [Fact]
    public void EnrageRemainingCountsDown()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var engine = new BossRaidEngine(() => clock.Now);
        engine.Start(new[] { new RaidPhase(0, "P1", 60) }, enrageSeconds: 180);

        Assert.Equal(180, engine.EnrageRemainingSeconds, 1);
        clock.Advance(TimeSpan.FromSeconds(60));
        Assert.Equal(120, engine.EnrageRemainingSeconds, 1);
        clock.Advance(TimeSpan.FromSeconds(200));
        Assert.Equal(0, engine.EnrageRemainingSeconds);
    }

    [Fact]
    public void StopFiresEndedOnce()
    {
        var engine = new BossRaidEngine();
        var endedCount = 0;
        engine.RaidEnded += () => endedCount++;
        engine.Start(new[] { new RaidPhase(0, "P1", 60) });
        engine.Stop();
        engine.Stop();
        Assert.Equal(1, endedCount);
    }

    [Fact]
    public void SetEntityRolePromotesOneBossAndDemotesPreviousBoss()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "P1", 60) });
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false, "First");
        engine.OnDamageEvent(0xDEF, 50, true, true, false, false, false, "Second");

        Assert.True(engine.SetEntityRole(0xDEF, "boss"));

        Assert.Equal(0xDEF, engine.BossUuid);
        var entities = engine.Entities;
        Assert.Equal("enemy", entities.Single(e => e.Uuid == 0xABC).Role);
        Assert.Equal("boss", entities.Single(e => e.Uuid == 0xDEF).Role);
    }

    [Fact]
    public void SetEntityRoleDemotingCurrentBossClearsManualPin()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "P1", 60) });
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false, "First");

        Assert.True(engine.SetEntityRole(0xABC, "enemy"));

        Assert.Equal(0, engine.BossUuid);
        Assert.Equal("enemy", engine.Entities.Single().Role);
        engine.OnDamageEvent(0xABC, 50, true, true, false, false, false, "First");
        Assert.Equal(0xABC, engine.BossUuid);
        Assert.Equal("boss", engine.Entities.Single().Role);
    }

    [Fact]
    public void SetEntityRoleRejectsUnknownOrInvalidInput()
    {
        var engine = new BossRaidEngine();
        engine.Start(new[] { new RaidPhase(0, "P1", 60) });
        engine.OnDamageEvent(0xABC, 100, true, true, false, false, false, "First");

        Assert.False(engine.SetEntityRole(0xDEF, "boss"));
        Assert.False(engine.SetEntityRole(0xABC, "healer"));
        Assert.False(engine.SetEntityRole(0, "boss"));
        Assert.Equal(0xABC, engine.BossUuid);
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; }
        public TestClock(DateTimeOffset s) => Now = s;
        public void Advance(TimeSpan d) => Now = Now.Add(d);
    }
}

public class SoundConcurrencyGuardTests
{
    [Fact]
    public void FirstPlaySucceeds()
    {
        var guard = new SoundConcurrencyGuard();
        Assert.True(guard.TryBeginPlayback("alert"));
    }

    [Fact]
    public void ConcurrentPlayOfSameClipFails()
    {
        var guard = new SoundConcurrencyGuard();
        Assert.True(guard.TryBeginPlayback("alert"));
        Assert.False(guard.TryBeginPlayback("alert"));
    }

    [Fact]
    public void RapidRepeatRejectedByMinInterval()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var guard = new SoundConcurrencyGuard(TimeSpan.FromMilliseconds(100), () => clock.Now);
        Assert.True(guard.TryBeginPlayback("alert"));
        guard.EndPlayback("alert");
        clock.Advance(TimeSpan.FromMilliseconds(50));
        Assert.False(guard.TryBeginPlayback("alert"));
        clock.Advance(TimeSpan.FromMilliseconds(100));
        Assert.True(guard.TryBeginPlayback("alert"));
    }

    [Fact]
    public void DistinctClipsAreIndependent()
    {
        var guard = new SoundConcurrencyGuard();
        Assert.True(guard.TryBeginPlayback("a"));
        Assert.True(guard.TryBeginPlayback("b"));
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; }
        public TestClock(DateTimeOffset s) => Now = s;
        public void Advance(TimeSpan d) => Now = Now.Add(d);
    }
}
