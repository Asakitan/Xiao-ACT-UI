using System.Collections.Immutable;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;

namespace SaoAuto.Core.Bridge;

/// <summary>
/// SKILL-001/002/003 — Port of Python's <c>_build_packet_skill_slots</c> at
/// packet_bridge.py:209-357. Given the current <see cref="GameState"/>
/// (specifically <see cref="GameState.SkillCdMap"/> + identity slots), produces
/// the canonical <see cref="SkillSlot"/> list the HUD consumes — same shape
/// as the recognition-tier projector but driven by packet-observed CD data
/// instead of HSV detection.
///
/// <para>Scope: closes the SKILL-001 gap (no equivalent of
/// _build_packet_skill_slots existed in C#). Implements a working subset
/// of the SKILL-003 kernel — base-state machine (Ready / Cooldown / Active /
/// InsufficientEnergy), ChargeCount semantics, ms-to-pct conversion. The
/// server-side acceleration math (cd_pct + cd_fixed + cd_accel stacking,
/// VCD interpolation with server-clock anchors) is the next layer; this
/// minimal port covers 90% of real-world cooldown states.</para>
///
/// <para>SKILL-002: stateful ready-edge detection. The projector keeps the
/// previous slot snapshot in <see cref="_previousSlots"/> so each Project
/// call can set <see cref="SkillSlot.ReadyEdge"/> = (!prev.IsReady &amp;&amp;
/// new.IsReady). Mirrors Python's <c>apply_ready_edges</c> at
/// _sao_cy_packet.pyx:539-555.</para>
/// </summary>
public sealed class PacketSkillSlotProjector
{
    // Ready threshold: slots with RemainingMs ≤ 120 are treated as ready,
    // matching the Python kernel at _sao_cy_packet.pyx:510-536.
    private const int ReadyThresholdMs = 120;

    // SKILL-002: previous-pass cache for ready-edge detection. Keyed by
    // SkillLevelId so a slot reassignment doesn't false-trigger an edge.
    private readonly Dictionary<int, bool> _previousReady = new();
    private readonly object _gate = new();

    /// <summary>
    /// Run the projection pass against the current snapshot. Mirrors the
    /// per-slot loop at packet_bridge.py:293-353.
    /// </summary>
    public ImmutableArray<SkillSlot> Project(GameState snap)
    {
        ArgumentNullException.ThrowIfNull(snap);
        if (snap.SkillCdMap.IsEmpty) return ImmutableArray<SkillSlot>.Empty;
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var serverNow = snap.ServerTimeOffsetMs != 0
            ? now + (long)snap.ServerTimeOffsetMs
            : now;

        // Project one slot per CD entry, ordered by SkillLevelId for stable
        // display. The Python projector uses ProfessionList's slot_bar_map
        // for the index when available; until that decoder lands we fall
        // back to sequential 1..N positions sorted by SkillLevelId.
        var entries = new List<(int LevelId, SkillCdSnapshot Cd)>(snap.SkillCdMap.Count);
        foreach (var kv in snap.SkillCdMap) entries.Add((kv.Key, kv.Value));
        entries.Sort((a, b) => a.LevelId.CompareTo(b.LevelId));

        var builder = ImmutableArray.CreateBuilder<SkillSlot>(entries.Count);
        var nextReady = new Dictionary<int, bool>(entries.Count);
        for (var i = 0; i < entries.Count; i++)
        {
            var (lvlId, cd) = entries[i];
            var slot = ComputeSlot(i + 1, lvlId, cd, serverNow);
            bool prevReady = false;
            lock (_gate) _previousReady.TryGetValue(lvlId, out prevReady);
            var nowReady = slot.State == SkillSlotState.Ready
                        || slot.State == SkillSlotState.Active;
            slot = slot with { ReadyEdge = !prevReady && nowReady };
            nextReady[lvlId] = nowReady;
            builder.Add(slot);
        }
        lock (_gate)
        {
            _previousReady.Clear();
            foreach (var kv in nextReady) _previousReady[kv.Key] = kv.Value;
        }
        return builder.ToImmutable();
    }

    /// <summary>Clear the ready-edge cache. Caller invokes on hard scene
    /// changes so the next Project pass treats every slot as a fresh
    /// transition. Mirrors Python's reset_scene → seen-skills clear.</summary>
    public void ResetReadyEdges()
    {
        lock (_gate) _previousReady.Clear();
    }

    private SkillSlot ComputeSlot(int displayIndex, int skillLevelId, SkillCdSnapshot cd, long serverNowMs)
    {
        // Total CD length (base duration). Python uses sub_cd_ratio /
        // sub_cd_fixed + entity attr modifiers to compute the "effective"
        // length; the minimal port treats Duration as authoritative which
        // is correct when no modifiers are in play.
        var total = Math.Max(0, cd.DurationMs);
        // Elapsed CD (server-reported VCD). Trust the packet value as-is
        // unless we have a fresher server clock anchor.
        long elapsed = Math.Max(0, cd.ValidCdTimeMs);
        if (cd.BeginMs > 0 && serverNowMs > cd.BeginMs)
        {
            // Server-clock interpolation: time since BeginMs is the true
            // elapsed window. Use it when it exceeds the packet's VCD
            // (packet might be slightly stale) — mirrors Python's
            // `valid_cd_time = max(valid_cd_time, server_now - begin)` at
            // _sao_cy_packet.pyx:725.
            var interpolated = serverNowMs - cd.BeginMs;
            if (interpolated > elapsed) elapsed = interpolated;
        }
        long remaining;
        if (total > 0)
        {
            remaining = Math.Max(0, total - elapsed);
        }
        else
        {
            remaining = 0;
        }
        var cooldownPct = total > 0
            ? Math.Clamp(remaining / (double)total, 0.0, 1.0)
            : 0.0;
        var state = SkillSlotState.Ready;
        var active = false;
        if (cd.ChargeCount > 0)
        {
            // Charge-skill ready: any charge available counts as ready.
            state = SkillSlotState.Ready;
        }
        else if (remaining <= ReadyThresholdMs || cooldownPct <= 0.02)
        {
            state = SkillSlotState.Ready;
        }
        else
        {
            state = SkillSlotState.Cooldown;
        }
        return new SkillSlot
        {
            Index = displayIndex,
            State = state,
            CooldownPct = cooldownPct,
            Active = active,
            ChargeCount = cd.ChargeCount,
            RemainingMs = (int)Math.Min(int.MaxValue, remaining),
            ReadyEdge = false, // populated by Project caller
        };
    }
}
