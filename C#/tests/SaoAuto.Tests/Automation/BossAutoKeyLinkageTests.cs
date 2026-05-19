using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class BossAutoKeyLinkageTests
{
    [Fact]
    public void RuleMatchingPhaseFiresKeystroke()
    {
        var engine = new BossRaidEngine();
        var dispatcher = new RecordingDispatcher();
        using var link = new BossAutoKeyLinkage(engine, dispatcher);
        link.SetRules(new[]
        {
            new BossAutoKeyLinkage.Rule("p2", p => p.Name == "P2", new KeyStroke(0x31, AutoKeyModifiers.None), 100),
        });

        engine.Start(new[] { new RaidPhase(0, "P1", 30), new RaidPhase(1, "P2", 30) });
        engine.NextPhase();

        Assert.Single(dispatcher.Strokes);
        Assert.Equal(0x31, dispatcher.Strokes[0].VirtualKey);
        Assert.Equal(1, link.FireCount);
    }

    [Fact]
    public void NonMatchingPhaseDoesNotFire()
    {
        var engine = new BossRaidEngine();
        var dispatcher = new RecordingDispatcher();
        using var link = new BossAutoKeyLinkage(engine, dispatcher);
        link.SetRules(new[]
        {
            new BossAutoKeyLinkage.Rule("p3", p => p.Name == "P3", new KeyStroke(0x31, AutoKeyModifiers.None), 100),
        });
        engine.Start(new[] { new RaidPhase(0, "P1", 30) });
        Assert.Empty(dispatcher.Strokes);
    }

    [Fact]
    public void CooldownBlocksRepeatPhaseTransitions()
    {
        var now = DateTimeOffset.UtcNow;
        var engine = new BossRaidEngine(() => now);
        var dispatcher = new RecordingDispatcher();
        using var link = new BossAutoKeyLinkage(engine, dispatcher, clock: () => now);
        link.SetRules(new[]
        {
            new BossAutoKeyLinkage.Rule("any", _ => true, new KeyStroke(0x31, AutoKeyModifiers.None), 1000),
        });

        engine.Start(new[]
        {
            new RaidPhase(0, "P1", 30),
            new RaidPhase(1, "P2", 30),
            new RaidPhase(2, "P3", 30),
        });
        engine.NextPhase();
        Assert.Single(dispatcher.Strokes);

        now = now.AddSeconds(2);
        engine.NextPhase();
        Assert.Equal(2, dispatcher.Strokes.Count);
    }

    [Fact]
    public void DisposeUnsubscribesFromEngine()
    {
        var engine = new BossRaidEngine();
        var dispatcher = new RecordingDispatcher();
        var link = new BossAutoKeyLinkage(engine, dispatcher);
        link.SetRules(new[]
        {
            new BossAutoKeyLinkage.Rule("any", _ => true, new KeyStroke(0x31, AutoKeyModifiers.None), 1),
        });
        link.Dispose();
        engine.Start(new[] { new RaidPhase(0, "P1", 30) });
        Assert.Empty(dispatcher.Strokes);
    }

    [Fact]
    public void OnPhaseFirstMatchWinsAndReturnsRuleId()
    {
        var engine = new BossRaidEngine();
        var dispatcher = new RecordingDispatcher();
        using var link = new BossAutoKeyLinkage(engine, dispatcher);
        link.SetRules(new[]
        {
            new BossAutoKeyLinkage.Rule("a", _ => true, new KeyStroke(0x31, AutoKeyModifiers.None), 1),
            new BossAutoKeyLinkage.Rule("b", _ => true, new KeyStroke(0x32, AutoKeyModifiers.None), 1),
        });
        Assert.Equal("a", link.OnPhase(new RaidPhase(0, "P1", 30)));
        Assert.Single(dispatcher.Strokes);
    }

    private sealed class RecordingDispatcher : IKeyDispatcher
    {
        public List<KeyStroke> Strokes { get; } = new();
        public void Dispatch(KeyStroke stroke) => Strokes.Add(stroke);
    }
}
