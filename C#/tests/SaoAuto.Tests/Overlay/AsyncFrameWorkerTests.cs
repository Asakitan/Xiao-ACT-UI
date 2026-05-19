using SaoAuto.Overlay.Rendering;

namespace SaoAuto.Tests.Overlay;

public class AsyncFrameWorkerTests
{
    [Fact]
    public void StickyAssignmentReusesSameLane()
    {
        var worker = new AsyncFrameWorker(laneCount: 2);
        var first = worker.AssignLane(new AssignmentRequest("hp", PreferIsolation: false));
        var again = worker.AssignLane(new AssignmentRequest("hp", PreferIsolation: false));
        Assert.Equal(first, again);
    }

    [Fact]
    public void LeastLoadedLaneIsPickedForNewRenderers()
    {
        var worker = new AsyncFrameWorker(laneCount: 2);
        worker.AssignLane(new AssignmentRequest("a", false));
        worker.EnqueueWork(0);
        worker.EnqueueWork(0);
        // Lane 0 has 2 jobs, lane 1 has 0 → new renderer goes to lane 1.
        var lane = worker.AssignLane(new AssignmentRequest("b", false));
        Assert.Equal(1, lane);
    }

    [Fact]
    public void IsolationRebalancesEvenForExistingRenderer()
    {
        var worker = new AsyncFrameWorker(laneCount: 3);
        var initial = worker.AssignLane(new AssignmentRequest("burst", false));
        worker.EnqueueWork(initial);
        // Force isolation — should pick the least-loaded lane (not necessarily `initial`).
        var isolated = worker.AssignLane(new AssignmentRequest("burst", true));
        Assert.NotEqual(initial, isolated);
    }

    [Fact]
    public void CompleteWorkDecrementsPending()
    {
        var worker = new AsyncFrameWorker(laneCount: 1);
        worker.EnqueueWork(0);
        worker.EnqueueWork(0);
        worker.CompleteWork(0);
        Assert.Equal(new[] { 1 }, worker.LaneLoads());
    }
}
