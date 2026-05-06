using System.Buffers.Binary;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>Ground-truth observation feeding <see cref="SceneDiscovery"/>.
/// Mirrors the Python <c>SceneTruth</c> dataclass.
/// <c>SceneUuid</c> is the only required field (int64, unique per dungeon
/// instance); the rest narrow the body match when present.</summary>
public sealed record SceneTruth(
    long SceneUuid,
    int DungeonId = 0,
    int SceneId = 0,
    int Layer = 0,
    int Difficulty = 0)
{
    public bool IsUsable => SceneUuid > 0;
}

/// <summary>Outcome of <see cref="SceneDiscovery.Discover"/>. Matches the
/// dict that Python <c>scene_discovery.discover_scene_klass</c> emits into
/// <c>anchors.json</c> under <c>smart_locator.anchors.scene_manager</c>.</summary>
public sealed record SceneDiscoveryResult(
    ulong ObjAddr,
    ulong KlassPtr,
    int SceneUuidOff,
    int DungeonIdOff,
    int SceneIdOff,
    int LayerOff,
    int Score,
    int CandidateCount);

/// <summary>
/// Port of <c>tools/mem_probe/scene_discovery.py</c>. Locates the
/// SceneInfo / SceneManager object purely from a TCP-derived
/// <c>(scene_uuid, dungeon_id)</c> pair — no IL2CPP base address needed.
///
/// Algorithm:
///   1. <see cref="MemoryScanner.ScanI64"/> the whole heap for
///      <c>scene_uuid</c> (8-byte aligned).
///   2. For each hit, walk a small set of candidate field offsets
///      (<see cref="DefaultUidOffTries"/>) backwards to a presumed object
///      base; read 8 bytes there as <c>klass_ptr</c>.
///   3. If a GameAssembly module range is supplied, drop candidates
///      whose <c>klass_ptr</c> is outside that range.
///   4. Read a <see cref="DefaultBodyBytes"/>-byte body once, scan it for
///      the optional ground-truth fields (dungeon_id / scene_id /
///      difficulty / layer) at any 4-byte aligned slot.
///   5. Score by how many of those secondary fields hit; tie-break by
///      proximity to <c>0x10</c> (CharSerialize.SceneData layout).
/// </summary>
public static class SceneDiscovery
{
    public const int DefaultBodyBytes = 0x180;
    public const int MaxRegionSize = 256 * 1024 * 1024;

    /// <summary>Default field offsets we probe backwards from a
    /// scene_uuid hit. Mirrors Python <c>DEFAULT_UID_OFF_TRIES</c>.</summary>
    public static readonly int[] DefaultUidOffTries =
        { 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 };

    /// <summary>Run the discovery pipeline.</summary>
    /// <param name="memory">Process memory source.</param>
    /// <param name="truth">Must have <see cref="SceneTruth.SceneUuid"/> &gt; 0.</param>
    /// <param name="gaBase">GameAssembly.dll base; 0 to skip klass_ptr range check.</param>
    /// <param name="gaEnd">GameAssembly.dll base + size; 0 to skip.</param>
    /// <param name="bodyBytes">How many bytes to read per candidate body.</param>
    /// <param name="uidOffTries">Override the candidate offset list.</param>
    /// <param name="rescanMaxHits">Cap on the heap scan.</param>
    /// <returns>Best-scoring candidate, or <c>null</c> if none survived.</returns>
    public static SceneDiscoveryResult? Discover(
        IMemorySource memory,
        SceneTruth truth,
        ulong gaBase = 0,
        ulong gaEnd = 0,
        int bodyBytes = DefaultBodyBytes,
        IReadOnlyList<int>? uidOffTries = null,
        int rescanMaxHits = MemoryScanner.DefaultMaxHits)
    {
        ArgumentNullException.ThrowIfNull(memory);
        ArgumentNullException.ThrowIfNull(truth);
        if (!truth.IsUsable) return null;

        var tries = uidOffTries ?? DefaultUidOffTries;
        var hits = MemoryScanner.ScanI64(
            memory, truth.SceneUuid,
            maxHits: rescanMaxHits,
            maxRegionSize: MaxRegionSize);
        if (hits.Count == 0) return null;

        var candidates = new List<Candidate>();
        foreach (var uidAddr in hits)
        {
            foreach (var k1 in tries)
            {
                if (uidAddr < (ulong)k1) continue;
                ulong objBase = uidAddr - (ulong)k1;
                if (objBase < 0x10000UL) continue;

                var klassBlob = memory.ReadBytes(objBase, 8);
                if (klassBlob is null) continue;
                ulong klassPtr = BinaryPrimitives.ReadUInt64LittleEndian(klassBlob);
                if (gaEnd != 0 && (klassPtr < gaBase || klassPtr >= gaEnd)) continue;

                var body = memory.ReadBytes(objBase, bodyBytes);
                if (body is null || body.Length < 0x40) continue;

                int dungeonOff = truth.DungeonId != 0 ? FirstI32Offset(body, truth.DungeonId) : -1;
                int sceneIdOff = truth.SceneId != 0 ? FirstI32Offset(body, truth.SceneId) : -1;
                int diffOff = truth.Difficulty != 0 ? FirstI32Offset(body, truth.Difficulty) : -1;
                int layerOff = truth.Layer != 0 ? FirstI32Offset(body, truth.Layer) : -1;

                int score = (dungeonOff >= 0 ? 1 : 0)
                    + (sceneIdOff >= 0 ? 1 : 0)
                    + (diffOff >= 0 ? 1 : 0)
                    + (layerOff >= 0 ? 1 : 0);

                candidates.Add(new Candidate(
                    objBase, klassPtr, k1,
                    dungeonOff, sceneIdOff, layerOff,
                    score));
            }
        }

        if (candidates.Count == 0) return null;

        candidates.Sort((a, b) =>
        {
            int byScore = b.Score.CompareTo(a.Score);
            if (byScore != 0) return byScore;
            return Math.Abs(a.SceneUuidOff - 0x10).CompareTo(Math.Abs(b.SceneUuidOff - 0x10));
        });
        var best = candidates[0];

        return new SceneDiscoveryResult(
            ObjAddr: best.ObjAddr,
            KlassPtr: best.KlassPtr,
            SceneUuidOff: best.SceneUuidOff,
            DungeonIdOff: best.DungeonIdOff,
            SceneIdOff: best.SceneIdOff,
            LayerOff: best.LayerOff,
            Score: best.Score,
            CandidateCount: candidates.Count);
    }

    /// <summary>Return the first 4-byte-aligned offset in
    /// <paramref name="body"/> where the i32 equals
    /// <paramref name="value"/>, or -1 if absent. Mirrors the Python
    /// helper that returned the full list and the caller picked [0].</summary>
    public static int FirstI32Offset(ReadOnlySpan<byte> body, int value)
    {
        Span<byte> needle = stackalloc byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(needle, value);
        int last = body.Length - 4;
        for (int i = 0; i <= last; i += 4)
        {
            if (body[i] == needle[0] && body[i + 1] == needle[1]
                && body[i + 2] == needle[2] && body[i + 3] == needle[3])
            {
                return i;
            }
        }
        return -1;
    }

    private readonly record struct Candidate(
        ulong ObjAddr,
        ulong KlassPtr,
        int SceneUuidOff,
        int DungeonIdOff,
        int SceneIdOff,
        int LayerOff,
        int Score);
}
