using System.Collections.Concurrent;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.State;

public class GameStateManagerTests
{
    [Fact]
    public void DefaultSnapshotMatchesPythonInitialDefaults()
    {
        var manager = new GameStateManager();
        var s = manager.Snapshot;

        Assert.Equal(string.Empty, s.PlayerName);
        Assert.Equal(0, s.LevelBase);
        Assert.Equal(1.0, s.HpPct);
        Assert.Equal(1.0, s.StaminaPct);
        Assert.Equal(1.0, s.BossHpEstPct);
        Assert.Equal(-1, s.BossBreakingStage);
        Assert.Equal(BossHpSource.None, s.BossHpSource);
        Assert.False(s.RecognitionOk);
    }

    [Fact]
    public void PartialUpdatePreservesUnrelatedFields()
    {
        var manager = new GameStateManager();
        manager.Update(s => s with { PlayerName = "咲", LevelBase = 60, LevelExtra = 12 });
        manager.Update(s => s with { HpCurrent = 100, HpMax = 200, HpPct = 0.5 });

        var s = manager.Snapshot;
        Assert.Equal("咲", s.PlayerName);
        Assert.Equal(60, s.LevelBase);
        Assert.Equal(12, s.LevelExtra);
        Assert.Equal(100, s.HpCurrent);
        Assert.Equal(200, s.HpMax);
        Assert.Equal(0.5, s.HpPct);
        Assert.Equal("60(+12)", s.LevelText);
        Assert.Equal("100/200", s.HpText);
    }

    [Fact]
    public void SubscribersReceiveImmutableSnapshotsAfterEachUpdate()
    {
        var manager = new GameStateManager();
        var received = new List<GameState>();
        using var sub = manager.Subscribe(received.Add);

        manager.Update(s => s with { LevelBase = 60 });
        manager.Update(s => s with { LevelBase = 61 });

        Assert.Equal(2, received.Count);
        Assert.Equal(60, received[0].LevelBase);
        Assert.Equal(61, received[1].LevelBase);
        // Records have value equality but each Update produces a new instance.
        Assert.NotSame(received[0], received[1]);
    }

    [Fact]
    public void DisposingSubscriptionStopsCallbacks()
    {
        var manager = new GameStateManager();
        var count = 0;
        var sub = manager.Subscribe(_ => Interlocked.Increment(ref count));

        manager.Update(s => s with { LevelBase = 10 });
        Assert.Equal(1, count);

        sub.Dispose();
        manager.Update(s => s with { LevelBase = 11 });
        Assert.Equal(1, count);
    }

    [Fact]
    public async Task SnapshotIsThreadSafeUnderConcurrentReadersAndWriters()
    {
        var manager = new GameStateManager();
        var stop = false;
        var readerStarted = new ManualResetEventSlim(false);
        var reader = Task.Run(() =>
        {
            var seen = 0;
            readerStarted.Set();
            while (!Volatile.Read(ref stop))
            {
                var snap = manager.Snapshot;
                if (snap.LevelBase >= 0) seen++;
            }
            return seen;
        });

        // Wait until the reader thread is actually scheduled before issuing writes,
        // otherwise on a fast machine the writes can finish before the reader executes
        // a single Snapshot read.
        readerStarted.Wait(TimeSpan.FromSeconds(2));

        for (var i = 0; i < 1000; i++)
        {
            var lv = i;
            manager.Update(s => s with { LevelBase = lv });
        }
        Volatile.Write(ref stop, true);
        var observed = await reader;

        Assert.Equal(999, manager.Snapshot.LevelBase);
        Assert.True(observed > 0, "reader should have observed at least one snapshot");
    }

    [Fact]
    public void DispatcherHookMarshalsSubscriberCallbacks()
    {
        var queue = new ConcurrentQueue<Action>();
        var manager = new GameStateManager(dispatch: queue.Enqueue);
        var received = 0;
        using var sub = manager.Subscribe(_ => received++);

        manager.Update(s => s with { LevelBase = 5 });

        Assert.Equal(0, received); // dispatcher held the callback
        Assert.True(queue.TryDequeue(out var pending));
        pending!();
        Assert.Equal(1, received);
    }

    [Fact]
    public void SubscriberExceptionDoesNotBlockOtherListeners()
    {
        var manager = new GameStateManager();
        using var bad = manager.Subscribe(_ => throw new InvalidOperationException("boom"));
        var goodCount = 0;
        using var good = manager.Subscribe(_ => goodCount++);

        manager.Update(s => s with { LevelBase = 1 });

        Assert.Equal(1, goodCount);
    }
}
