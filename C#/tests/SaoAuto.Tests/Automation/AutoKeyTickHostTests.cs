using System.Collections.Immutable;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S137 — AutoKeyTickHost gate matrix. Synchronous TickOnce() facts
/// pin the parity gates from Python's <c>AutoKeyEngine._tick</c>:
/// disabled → idle, no recognition → idle, dead → idle (with PauseOnDeath),
/// background → idle (with RequireForeground), then fires when all
/// gates pass.
/// </summary>
public class AutoKeyTickHostTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public AutoKeyTickHostTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s137-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private sealed class Recorder : IKeyDispatcher
    {
        public readonly List<KeyStroke> Strokes = new();
        public void Dispatch(KeyStroke stroke) => Strokes.Add(stroke);
    }

    private static AutoKeyProfileSpecRecord SimpleProfile(int tickMs = 50, bool requireForeground = false, bool pauseOnDeath = true)
        => new(
            Id: "p1",
            SchemaVersion: 1,
            ProfileName: "Test",
            Description: "",
            ProfessionId: 0,
            ProfessionName: "",
            Source: "local",
            RemoteId: null,
            CreatedAt: "2026-01-01T00:00:00Z",
            UpdatedAt: "2026-01-01T00:00:00Z",
            AuthorSnapshot: AuthorSnapshot.Empty,
            Engine: new AutoKeyEngineConfig(TickMs: tickMs, RequireForeground: requireForeground, PauseOnDeath: pauseOnDeath),
            Actions: ImmutableArray.Create(new AutoKeyActionSpec(
                Id: "a1",
                Label: "Cast",
                Enabled: true,
                SlotIndex: 1,
                Key: "1",
                PressMode: "tap",
                PressCount: 1,
                PressIntervalMs: 0,
                HoldMs: 0,
                ReadyDelayMs: 0,
                MinRearmMs: 0,
                PostDelayMs: 0,
                Conditions: ImmutableArray<AutoKeyCondition>.Empty)));

    private void SeedConfig(AutoKeyConfig cfg)
    {
        var s = new SettingsManager(_path);
        AutoKeyConfigLoader.Save(s, cfg);
        s.Save();
    }

    private static GameState ReadyState() => new()
    {
        RecognitionOk = true,
        HpCurrent = 100,
        HpMax = 100,
        HpPct = 1.0,
        SkillSlots = ImmutableArray.Create(new SkillSlot
        {
            Index = 1,
            State = SkillSlotState.Ready,
            Active = true,
            CooldownPct = 0.0,
            RemainingMs = 0,
        }),
    };

    private AutoKeyTickHost MakeHost(GameStateManager states, Recorder rec, Func<bool>? fg = null, Func<DateTimeOffset>? clock = null)
    {
        var settings = new SettingsManager(_path);
        var runtime = new AutoKeySpecRuntime(rec);
        return new AutoKeyTickHost(settings, states, runtime, fg, clock);
    }

    [Fact]
    public void DisabledConfigYieldsIdle()
    {
        SeedConfig(new AutoKeyConfig(Enabled: false, ServerUrl: "", ActiveProfileId: "", Profiles: ImmutableArray<AutoKeyProfileSpecRecord>.Empty));
        var states = new GameStateManager();
        var rec = new Recorder();
        using var host = MakeHost(states, rec);

        host.TickOnce();

        Assert.False(host.LastActive);
        Assert.Equal("disabled", host.LastReason);
        Assert.Empty(rec.Strokes);
    }

    [Fact]
    public void NoRecognitionAndNoPacketSkipsTick()
    {
        var p = SimpleProfile();
        SeedConfig(new AutoKeyConfig(Enabled: true, ServerUrl: "", ActiveProfileId: "p1", Profiles: ImmutableArray.Create(p)));
        var states = new GameStateManager();
        // default state: RecognitionOk=false, PacketActive=false
        var rec = new Recorder();
        using var host = MakeHost(states, rec);

        host.TickOnce();

        Assert.True(host.LastActive);
        Assert.Equal("recognition-off", host.LastReason);
        Assert.Empty(rec.Strokes);
    }

    [Fact]
    public void DeadPlayerWithPauseOnDeathSkipsTick()
    {
        var p = SimpleProfile(pauseOnDeath: true);
        SeedConfig(new AutoKeyConfig(Enabled: true, ServerUrl: "", ActiveProfileId: "p1", Profiles: ImmutableArray.Create(p)));
        var states = new GameStateManager();
        states.Update(_ => new GameState { RecognitionOk = true, HpCurrent = 0, HpMax = 100, HpPct = 0.0 });
        var rec = new Recorder();
        using var host = MakeHost(states, rec);

        host.TickOnce();

        Assert.Equal("dead", host.LastReason);
        Assert.Empty(rec.Strokes);
    }

    [Fact]
    public void BackgroundWithRequireForegroundSkipsTick()
    {
        var p = SimpleProfile(requireForeground: true);
        SeedConfig(new AutoKeyConfig(Enabled: true, ServerUrl: "", ActiveProfileId: "p1", Profiles: ImmutableArray.Create(p)));
        var states = new GameStateManager();
        states.Update(_ => ReadyState());
        var rec = new Recorder();
        using var host = MakeHost(states, rec, fg: () => false);

        host.TickOnce();

        Assert.Equal("background", host.LastReason);
        Assert.Empty(rec.Strokes);
    }

    [Fact]
    public void FiresActionWhenAllGatesPass()
    {
        var p = SimpleProfile(requireForeground: false);
        SeedConfig(new AutoKeyConfig(Enabled: true, ServerUrl: "", ActiveProfileId: "p1", Profiles: ImmutableArray.Create(p)));
        var states = new GameStateManager();
        states.Update(_ => ReadyState());
        var rec = new Recorder();
        using var host = MakeHost(states, rec);

        host.TickOnce();

        Assert.Equal("fired", host.LastReason);
        Assert.Equal("a1", host.LastFiredActionId);
        Assert.Equal("p1", host.LastActiveProfileId);
        Assert.NotEmpty(rec.Strokes);
    }

    [Fact]
    public void TickIntervalComesFromActiveProfile()
    {
        var p = SimpleProfile(tickMs: 123);
        SeedConfig(new AutoKeyConfig(Enabled: true, ServerUrl: "", ActiveProfileId: "p1", Profiles: ImmutableArray.Create(p)));
        var states = new GameStateManager();
        states.Update(_ => ReadyState());
        var rec = new Recorder();
        using var host = MakeHost(states, rec);

        var ms = host.TickOnce();

        Assert.Equal(123, ms);
    }

    [Fact]
    public void DisabledConfigStillReturnsDefaultTickWhenNoProfile()
    {
        SeedConfig(new AutoKeyConfig(Enabled: false, ServerUrl: "", ActiveProfileId: "", Profiles: ImmutableArray<AutoKeyProfileSpecRecord>.Empty));
        var states = new GameStateManager();
        var rec = new Recorder();
        using var host = MakeHost(states, rec);

        var ms = host.TickOnce();

        Assert.Equal(50, ms);
    }

    [Fact]
    public async Task StartAsyncPumpsTicksUntilStopped()
    {
        var p = SimpleProfile(tickMs: 10);
        SeedConfig(new AutoKeyConfig(Enabled: true, ServerUrl: "", ActiveProfileId: "p1", Profiles: ImmutableArray.Create(p)));
        var states = new GameStateManager();
        states.Update(_ => ReadyState());
        var rec = new Recorder();
        var host = MakeHost(states, rec);

        await host.StartAsync(CancellationToken.None);
        await Task.Delay(80);
        await host.StopAsync();
        await host.DisposeAsync();

        Assert.True(host.TickCount >= 2, $"expected >= 2 ticks, got {host.TickCount}");
    }
}
