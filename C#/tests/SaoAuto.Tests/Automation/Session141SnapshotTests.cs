using System.Collections.Immutable;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S141 — pins the atomic immutable snapshot helper on
/// <see cref="AutoKeySpecRuntime.Snapshot"/>. Verifies that the
/// returned dictionaries are independent of the live ones (mutation
/// of subsequent ticks doesn't bleed into prior snapshots) and that
/// a concurrent tick loop never produces a torn snapshot.
/// </summary>
public class Session141SnapshotTests
{
    private sealed class Recorder : IKeyDispatcher
    {
        public void Dispatch(KeyStroke s) { }
    }

    private static DateTimeOffset T(int ms = 0)
        => new DateTimeOffset(2026, 5, 20, 12, 0, 0, TimeSpan.Zero).AddMilliseconds(ms);

    private static SlotReadiness Ready() => new("ready", false, 0, 0, 0.0);

    private static AutoKeyActionSpec Act(string id, int slot = 1, int minRearm = 1000)
        => new(
            Id: id, Label: id, Enabled: true, SlotIndex: slot,
            Key: "1", PressMode: "tap", PressCount: 1, PressIntervalMs: 0,
            HoldMs: 0, ReadyDelayMs: 0, MinRearmMs: minRearm, PostDelayMs: 0,
            Conditions: ImmutableArray<AutoKeyCondition>.Empty);

    private static AutoKeyProfileSpecRecord Profile(params AutoKeyActionSpec[] actions)
        => new("p1", 1, "T", "", 0, "", "local", null, "", "",
               AuthorSnapshot.Empty,
               new AutoKeyEngineConfig(50, false, true),
               actions.ToImmutableArray());

    private static AutoKeySpecContext Ctx(SlotReadiness slot, int slotIndex = 1, DateTimeOffset? now = null)
        => AutoKeySpecContext.Empty with
        {
            Slots = ImmutableDictionary<int, SlotReadiness>.Empty.SetItem(slotIndex, slot),
            Now = now ?? T(0),
        };

    [Fact]
    public void EmptyStaticIsTrulyEmpty()
    {
        var s = AutoKeyRuntimeSnapshot.Empty;
        Assert.Empty(s.BlockReasons);
        Assert.Empty(s.CooldownRemainingMs);
        Assert.Equal(0, s.FireCount);
    }

    [Fact]
    public void SnapshotCapturesCurrentTickState()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var prof = Profile(Act("a", minRearm: 2_000), Act("b", minRearm: 0));
        rt.Tick(prof, Ctx(Ready(), now: T(0)));
        rt.Tick(prof, Ctx(Ready(), now: T(100)));
        var snap = rt.Snapshot();
        Assert.Equal("cooldown", snap.BlockReasons["a"]);
        Assert.True(snap.CooldownRemainingMs.ContainsKey("a"));
        Assert.Equal(2, snap.FireCount);
    }

    [Fact]
    public void SnapshotIsIndependentOfSubsequentMutation()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var prof = Profile(Act("a", minRearm: 2_000));
        rt.Tick(prof, Ctx(Ready(), now: T(0)));
        var snap = rt.Snapshot();
        rt.InvalidateProfileState();
        rt.Tick(Profile(Act("z", minRearm: 0)), Ctx(Ready(), now: T(5_000)));
        Assert.Contains("a", snap.BlockReasons.Keys);
        Assert.DoesNotContain("z", snap.BlockReasons.Keys);
    }

    [Fact]
    public async Task ConcurrentTicksAndSnapshotsNeverTear()
    {
        var rt = new AutoKeySpecRuntime(new Recorder());
        var prof = Profile(Act("a", minRearm: 5_000), Act("b", minRearm: 5_000), Act("c", minRearm: 5_000));
        var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(200));
        var ticker = Task.Run(() =>
        {
            var start = T(0);
            var i = 0;
            while (!cts.IsCancellationRequested)
            {
                rt.Tick(prof, Ctx(Ready(), now: start.AddMilliseconds(i++)));
            }
        });
        var sampler = Task.Run(() =>
        {
            var ok = 0;
            while (!cts.IsCancellationRequested)
            {
                var s = rt.Snapshot();
                // every snapshot must either be empty (pre-first-tick) or
                // contain exactly the profile's actions — never a partial set.
                if (s.BlockReasons.Count is 0 or 3) ok++;
                else throw new Xunit.Sdk.XunitException($"torn snapshot: {s.BlockReasons.Count} entries");
            }
            return ok;
        });
        await Task.WhenAll(ticker, sampler);
        Assert.True((await sampler) > 0);
    }
}
