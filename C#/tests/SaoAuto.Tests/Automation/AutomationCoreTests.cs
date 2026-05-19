using System.Collections.Immutable;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Automation;

public class AutomationCoreTests
{
    private static SettingsManager NewSettings() =>
        new(Path.Combine(Path.GetTempPath(), $"sao_auto_{Guid.NewGuid():N}.json"));

    [Fact]
    public void Toggles_FlipAndRaiseStateChanged()
    {
        using var core = new AutomationCore(new GameStateManager(), NewSettings(), new NullSoundPlayer());
        int events = 0;
        core.StateChanged += _ => events++;

        Assert.True(core.CaptureEnabled);
        Assert.False(core.AutoEnabled);
        Assert.True(core.SoundEnabled);

        core.ToggleCapture(); Assert.False(core.CaptureEnabled);
        core.ToggleAuto();    Assert.True(core.AutoEnabled);
        core.ToggleSound();   Assert.False(core.SoundEnabled);
        Assert.Equal(3, events);
    }

    [Fact]
    public void StartStop_AreIdempotent()
    {
        using var core = new AutomationCore(new GameStateManager(), NewSettings(), new NullSoundPlayer());
        core.Start(); core.Start();   // second is no-op
        core.Stop();  core.Stop();    // second is no-op
    }

    [Fact]
    public void AutoKeyTrigger_FiresOnHpBelow_WhenAutoEnabled()
    {
        var state = new GameStateManager();
        var keys = new List<KeyStroke>();
        using var core = new AutomationCore(state, NewSettings(),
            sound: new NullSoundPlayer(),
            keyEmitter: k => keys.Add(k));
        var profile = new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("hp", new HpBelowTrigger(0.5),
                new KeyStroke(0x71 /*F2*/, AutoKeyModifiers.None), CooldownMs: 100, Priority: 1)));
        core.LoadProfile(profile);
        core.Start();

        // Auto disabled → no fire even on low HP
        state.Update(s => s with { HpPct = 0.1 });
        Assert.Empty(keys);

        core.ToggleAuto();
        state.Update(s => s with { HpPct = 0.1 });
        Assert.Single(keys);
        Assert.Equal(0x71, keys[0].VirtualKey);
    }

    [Fact]
    public void AutoKeyCooldown_BlocksRepeatedFires()
    {
        var state = new GameStateManager();
        var keys = new List<KeyStroke>();
        using var core = new AutomationCore(state, NewSettings(),
            sound: new NullSoundPlayer(),
            keyEmitter: k => keys.Add(k));
        core.LoadProfile(new AutoKeyProfile("p", ImmutableArray.Create(
            new AutoKeyAction("burst", new BurstReadyTrigger(),
                new KeyStroke(0x52, AutoKeyModifiers.None), CooldownMs: 60_000, Priority: 1))));
        core.Start();
        core.ToggleAuto();

        state.Update(s => s with { BurstReady = true });
        state.Update(s => s with { BurstReady = true });
        state.Update(s => s with { BurstReady = true });

        Assert.Single(keys);  // cooldown gates the rest
    }

    [Fact]
    public void NullSoundPlayer_RecordsWhenEnabled()
    {
        var sp = new NullSoundPlayer();
        sp.Play("a.wav"); sp.Play("b.wav");
        sp.Enabled = false;
        sp.Play("c.wav");
        Assert.Equal(new[] { "a.wav", "b.wav" }, sp.Played);
    }
}
