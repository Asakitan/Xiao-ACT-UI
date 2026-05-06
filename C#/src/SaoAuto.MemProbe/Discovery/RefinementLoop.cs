using System.Buffers.Binary;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// One TCP-side observation feeding the refinement state machine.
/// Mirrors the values <c>refine.run</c> reads from
/// <c>_TcpSource.snapshot()</c>; we keep the surface narrow so unit
/// tests can drive deterministic samples without a real packet bridge.
/// </summary>
public interface IRefinementTcpSampler
{
    /// <summary>Latest HP value the TCP side has observed (0 = unknown).</summary>
    long CurrentHp { get; }
    /// <summary>Latest MaxHP value the TCP side has observed (0 = unknown).</summary>
    long CurrentMaxHp { get; }
    /// <summary>Player UID from the TCP handshake (0 = unknown).</summary>
    long CurrentUid { get; }
}

/// <summary>
/// Outcome record returned by <see cref="RefinementLoop.RunAsync"/>.
/// Mirrors the dict that Python <c>refine.run</c> writes back into
/// <c>anchors.json</c> (self_hp_addr, self_max_hp_addr, self_uid_addr,
/// player_struct_base_guess) plus the refined candidate lists for the
/// next pass and a status discriminator the caller can branch on.
/// </summary>
public sealed record RefinementResult(
    RefinementStatus Status,
    ulong? HpAddr,
    ulong? MaxHpAddr,
    ulong? UidAddr,
    ulong? StructBaseGuess,
    IReadOnlyList<ulong> HpRefined,
    IReadOnlyList<ulong> MaxHpRefined);

/// <summary>Discriminator on a <see cref="RefinementResult"/>.</summary>
public enum RefinementStatus
{
    /// <summary>HP+MaxHP+UID all uniquely located.</summary>
    Success,
    /// <summary>HP+MaxHP unique but UID could not be locked (PoC-usable).</summary>
    Partial,
    /// <summary>Lockstep / narrowing converged but no (hp, maxhp) pair survived.</summary>
    NoPair,
    /// <summary>Lockstep verifier rejected every HP candidate.</summary>
    NoLockstep,
    /// <summary>Caller passed an empty HP candidate set.</summary>
    EmptyCandidates,
}

/// <summary>
/// Live state-machine port of <c>tools/mem_probe/refine.py::run</c>.
/// Runs the post-discovery narrowing pass against a provided
/// <see cref="IMemorySource"/> + <see cref="IRefinementTcpSampler"/>:
/// lockstep-verifies HP candidates, narrows MaxHP, finds adjacency
/// pairs, optionally local-searches MaxHP within the HP neighborhood,
/// then locates a UID near each pair. The fresh-rescan branches that
/// Python takes when anchors are stale require the full memory
/// scanner; this loop assumes the caller has run the scanner already
/// and supplies non-empty candidate sets.
/// </summary>
public sealed class RefinementLoop
{
    public const int LocalMaxHpRadius = 0x200;
    public const int LocalUidRadius = 0x2000;

    private readonly IMemorySource _memory;
    private readonly IRefinementTcpSampler _tcp;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;

    public RefinementLoop(
        IMemorySource memory,
        IRefinementTcpSampler tcp,
        Func<TimeSpan, CancellationToken, Task>? delay = null)
    {
        _memory = memory ?? throw new ArgumentNullException(nameof(memory));
        _tcp = tcp ?? throw new ArgumentNullException(nameof(tcp));
        _delay = delay ?? Task.Delay;
    }

    /// <summary>Run the full refinement pipeline. See class summary.
    /// When <paramref name="autoRescan"/> is true and a candidate set is
    /// empty, the loop falls back to <see cref="MemoryScanner"/> seeded
    /// by the current TCP HP / MaxHP — the parity port of Python
    /// <c>refine.run</c>'s <c>stale_anchors</c> + <c>retry</c> branches.</summary>
    public async Task<RefinementResult> RunAsync(
        IReadOnlyList<ulong> uidCandidates,
        IReadOnlyList<ulong> hpCandidates,
        IReadOnlyList<ulong> maxHpCandidates,
        int lockstepSamples = 25,
        TimeSpan? lockstepInterval = null,
        double minMatchRatio = 0.5,
        bool autoRescan = false,
        int rescanMaxHits = MemoryScanner.DefaultMaxHits,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(uidCandidates);
        ArgumentNullException.ThrowIfNull(hpCandidates);
        ArgumentNullException.ThrowIfNull(maxHpCandidates);

        // ── Step 0 (auto-rescan): seed HP candidates from current TCP HP ──
        if (hpCandidates.Count == 0 && autoRescan)
        {
            long seedHp = _tcp.CurrentHp;
            if (seedHp > 0)
            {
                hpCandidates = MemoryScanner.ScanI32(
                    _memory, (int)seedHp, maxHits: rescanMaxHits);
            }
        }
        if (hpCandidates.Count == 0)
        {
            return new RefinementResult(
                RefinementStatus.EmptyCandidates,
                null, null, null, null,
                Array.Empty<ulong>(), Array.Empty<ulong>());
        }

        var interval = lockstepInterval ?? TimeSpan.FromMilliseconds(100);

        // ── Step 1: lockstep verify HP candidates ──
        var hpRefined = await VerifyLockstepAsync(
            hpCandidates, lockstepSamples, interval, minMatchRatio, ct);
        if (hpRefined.Count == 0)
        {
            return new RefinementResult(
                RefinementStatus.NoLockstep,
                null, null, null, null,
                Array.Empty<ulong>(), Array.Empty<ulong>());
        }

        // ── Step 2: narrow MaxHP by current TCP value (skip if TCP unknown) ──
        var curMax = _tcp.CurrentMaxHp;
        IReadOnlyList<ulong> mhRefined = curMax > 0
            ? AnchorRefinement.NarrowI32(_memory, maxHpCandidates, (int)curMax)
            : maxHpCandidates;
        // Auto-rescan: if narrow eliminated all MaxHP candidates (anchors
        // stale) and TCP knows the current MaxHP, scan the whole address
        // space for it. Mirrors Python's "mh_hits 与当前 MaxHP 不匹配" branch.
        if (autoRescan && mhRefined.Count == 0 && curMax > 0)
        {
            mhRefined = MemoryScanner.ScanI32(
                _memory, (int)curMax, maxHits: rescanMaxHits);
        }

        // ── Step 3: global adjacency pairing ──
        var pairs = AnchorRefinement.FindPairs(hpRefined, mhRefined);

        // ── Step 3.5: HP unique but no global pair → local ±0x200 search ──
        if (pairs.Count == 0 && hpRefined.Count <= 3 && curMax > 0)
        {
            var localPairs = new List<AnchorRefinement.HpMaxHpPair>();
            foreach (var h in hpRefined)
            {
                ulong baseAddr = h >= LocalMaxHpRadius ? h - LocalMaxHpRadius : 0UL;
                int blobSize = LocalMaxHpRadius * 2;
                var blob = _memory.ReadBytes(baseAddr, blobSize);
                if (blob is null) continue;
                var local = AnchorRefinement.LocalFindI32(blob, baseAddr, (int)curMax);
                foreach (var lm in local)
                {
                    int delta = (int)((long)lm - (long)h);
                    localPairs.Add(new AnchorRefinement.HpMaxHpPair(h, lm, delta));
                }
            }
            pairs = localPairs;
        }

        if (pairs.Count == 0)
        {
            return new RefinementResult(
                RefinementStatus.NoPair,
                null, null, null, null,
                hpRefined, mhRefined);
        }

        // ── Step 4: find UID near the first viable pair ──
        long gtUid = _tcp.CurrentUid;
        AnchorRefinement.HpMaxHpPair? chosenPair = null;
        ulong? chosenUid = null;
        foreach (var pair in pairs)
        {
            var nearby = AnchorRefinement.FindNearbyUid(
                uidCandidates, pair.HpAddr, AnchorRefinement.UidNearRadius);
            ulong? cand = null;
            if (nearby.Count == 1)
            {
                cand = nearby[0];
            }
            else if (gtUid != 0)
            {
                // Local 8-byte-aligned search for the UID needle.
                var localUids = LocalFindUid64(pair.HpAddr, (ulong)gtUid, LocalUidRadius);
                if (localUids.Count == 1)
                {
                    cand = localUids[0];
                }
                else if (localUids.Count > 1)
                {
                    cand = NearestUid(localUids, pair.HpAddr);
                }
            }
            if (cand is not null)
            {
                chosenPair = pair;
                chosenUid = cand;
                break;
            }
        }

        if (chosenPair is { } cp && chosenUid is { } cu)
        {
            ulong baseGuess = MinAddr(cp.HpAddr, cp.MaxHpAddr, cu) & ~0xFUL;
            return new RefinementResult(
                RefinementStatus.Success,
                cp.HpAddr, cp.MaxHpAddr, cu, baseGuess,
                hpRefined, mhRefined);
        }

        // ── Partial: HP/MaxHP unique pair but UID not locked ──
        if (pairs.Count == 1)
        {
            var p = pairs[0];
            ulong baseGuess = (p.HpAddr < p.MaxHpAddr ? p.HpAddr : p.MaxHpAddr) & ~0xFUL;
            return new RefinementResult(
                RefinementStatus.Partial,
                p.HpAddr, p.MaxHpAddr, null, baseGuess,
                hpRefined, mhRefined);
        }

        return new RefinementResult(
            RefinementStatus.NoPair,
            null, null, null, null,
            hpRefined, mhRefined);
    }

    /// <summary>Per-candidate score over <paramref name="samples"/>
    /// TCP/memory readings. Mirrors Python <c>_verify_lockstep</c>
    /// returning the addresses with score &gt;= valid * minMatchRatio.</summary>
    public async Task<IReadOnlyList<ulong>> VerifyLockstepAsync(
        IReadOnlyList<ulong> candidates,
        int samples,
        TimeSpan interval,
        double minMatchRatio,
        CancellationToken ct = default)
    {
        if (candidates.Count == 0) return Array.Empty<ulong>();
        var score = new Dictionary<ulong, int>(candidates.Count);
        foreach (var a in candidates) score[a] = 0;
        int valid = 0;
        for (int k = 0; k < samples; k++)
        {
            ct.ThrowIfCancellationRequested();
            long tcp = _tcp.CurrentHp;
            if (tcp <= 0)
            {
                await _delay(interval, ct).ConfigureAwait(false);
                continue;
            }
            valid++;
            foreach (var a in candidates)
            {
                int? v = _memory.ReadI32(a);
                if (v == (int)tcp) score[a]++;
            }
            await _delay(interval, ct).ConfigureAwait(false);
        }
        if (valid == 0) return Array.Empty<ulong>();
        int threshold = Math.Max(2, (int)(valid * minMatchRatio));
        var survivors = score.Where(kv => kv.Value >= threshold)
            .OrderByDescending(kv => kv.Value)
            .Select(kv => kv.Key)
            .ToArray();
        return survivors;
    }

    /// <summary>Read ±<paramref name="radius"/> bytes around
    /// <paramref name="anchor"/> and return all 8-byte-aligned offsets
    /// holding <paramref name="uidNeedle"/> as a little-endian u64.
    /// Mirrors the Step-5 local UID search inside Python <c>refine.run</c>.</summary>
    public IReadOnlyList<ulong> LocalFindUid64(ulong anchor, ulong uidNeedle, int radius)
    {
        ulong baseAddr = anchor >= (ulong)radius ? anchor - (ulong)radius : 0UL;
        int blobSize = radius * 2;
        var blob = _memory.ReadBytes(baseAddr, blobSize);
        if (blob is null || blob.Length < 8) return Array.Empty<ulong>();
        Span<byte> needle = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(needle, uidNeedle);
        var hits = new List<ulong>();
        int last = blob.Length - 8;
        for (int i = 0; i <= last; i += 8)
        {
            if (blob[i] == needle[0] && blob[i + 1] == needle[1]
                && blob[i + 2] == needle[2] && blob[i + 3] == needle[3]
                && blob[i + 4] == needle[4] && blob[i + 5] == needle[5]
                && blob[i + 6] == needle[6] && blob[i + 7] == needle[7])
            {
                hits.Add(baseAddr + (ulong)i);
            }
        }
        return hits;
    }

    private static ulong NearestUid(IReadOnlyList<ulong> uids, ulong anchor)
    {
        ulong best = uids[0];
        ulong bestDist = AbsDist(best, anchor);
        for (int i = 1; i < uids.Count; i++)
        {
            ulong d = AbsDist(uids[i], anchor);
            if (d < bestDist) { best = uids[i]; bestDist = d; }
        }
        return best;
    }

    private static ulong AbsDist(ulong a, ulong b) => a >= b ? a - b : b - a;

    private static ulong MinAddr(ulong a, ulong b, ulong c)
    {
        ulong m = a < b ? a : b;
        return m < c ? m : c;
    }
}
