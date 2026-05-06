using System.Threading.Channels;

namespace SaoAuto.Overlay.Rendering;

/// <summary>
/// Worker-lane abstraction ported from <c>overlay_render_worker.py</c>.
/// Sticky least-loaded lane assignment: a renderer's first job picks the
/// lane with fewest pending jobs; subsequent jobs from the same renderer
/// reuse that lane unless <see cref="AssignmentRequest.PreferIsolation"/>
/// is set, in which case the renderer gets its own dedicated lane.
/// </summary>
public sealed class AsyncFrameWorker : IDisposable
{
    private readonly Lane[] _lanes;
    private readonly Dictionary<string, int> _stickyLanes = new(StringComparer.Ordinal);
    private readonly object _gate = new();

    public AsyncFrameWorker(int laneCount = 2)
    {
        if (laneCount < 1) throw new ArgumentOutOfRangeException(nameof(laneCount));
        _lanes = Enumerable.Range(0, laneCount).Select(i => new Lane(i)).ToArray();
    }

    public int LaneCount => _lanes.Length;

    /// <summary>Pick a lane for <paramref name="request"/> using sticky least-loaded rules.</summary>
    public int AssignLane(AssignmentRequest request)
    {
        if (string.IsNullOrEmpty(request.RendererId))
        {
            throw new ArgumentException("RendererId required", nameof(request));
        }
        lock (_gate)
        {
            if (request.PreferIsolation)
            {
                // Pick the least-loaded lane and pin it.
                var idx = LeastLoadedIndex();
                _stickyLanes[request.RendererId] = idx;
                return idx;
            }
            if (_stickyLanes.TryGetValue(request.RendererId, out var pinned))
            {
                return pinned;
            }
            var picked = LeastLoadedIndex();
            _stickyLanes[request.RendererId] = picked;
            return picked;
        }
    }

    public void EnqueueWork(int laneIndex)
    {
        lock (_gate) _lanes[laneIndex].PendingCount++;
    }

    public void CompleteWork(int laneIndex)
    {
        lock (_gate)
        {
            ref var lane = ref _lanes[laneIndex];
            if (lane.PendingCount > 0) lane.PendingCount--;
        }
    }

    public IReadOnlyList<int> LaneLoads()
    {
        lock (_gate)
        {
            return _lanes.Select(l => l.PendingCount).ToArray();
        }
    }

    private int LeastLoadedIndex()
    {
        var min = int.MaxValue;
        var picked = 0;
        for (var i = 0; i < _lanes.Length; i++)
        {
            if (_lanes[i].PendingCount < min)
            {
                min = _lanes[i].PendingCount;
                picked = i;
            }
        }
        return picked;
    }

    public void Dispose() { /* lanes are pure data today; live workers land in 7b */ }

    private struct Lane
    {
        public int Id;
        public int PendingCount;
        public Lane(int id) { Id = id; PendingCount = 0; }
    }
}

public readonly record struct AssignmentRequest(string RendererId, bool PreferIsolation);
