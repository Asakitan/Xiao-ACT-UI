using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using System.Collections.Immutable;

namespace SaoAuto.Core.Bridge;

/// <summary>
/// Per-event-type mutator table — applies a <see cref="ParserEvent"/> to a
/// <see cref="GameState"/> via <see cref="GameStateManager.Update"/>.
/// Mirrors the per-handler `_apply_*` methods in
/// <c>sao_auto/packet_bridge.py</c>; one method per event keeps the
/// switch short and per-event tests trivial.
///
/// Each mutator returns true when it actually changed something. The
/// canonical "did this event matter" signal is used by
/// <see cref="PacketBridge.EventsApplied"/> for stats / smokes.
/// </summary>
public static class StateMutators
{
    public static bool Apply(GameStateManager state, ParserEvent ev)
    {
        return ev switch
        {
            IdentityEvent id => ApplyIdentity(state, id),
            HealthEvent hp => ApplyHealth(state, hp),
            StaminaEvent st => ApplyStamina(state, st),
            ServerTimeEvent srv => ApplyServerTime(state, srv),
            ReviveEvent rv => ApplyRevive(state, rv),
            KickOffEvent _ => ApplyKickOff(state),
            EnterGameEvent enter => ApplyEnterGame(state, enter),
            CombatStateEvent cs => ApplyCombatState(state, cs),
            DungeonStartEvent _ => ApplyDungeonStart(state),
            BuffSnapshotEvent buff => ApplyBuffs(state, buff),
            BossHpEvent boss => ApplyBossHp(state, boss),
            DpsEvent dps => ApplyDps(state, dps),
            IdentityAlertEvent alert => ApplyIdentityAlert(state, alert),
            // Marker events that don't mutate state on their own — caller
            // can still observe via Subscribe → no-op here.
            AllMemberReadyEvent _ or CaptainReadyEvent _ => false,
            _ => false,
        };
    }

    private static bool ApplyIdentity(GameStateManager state, IdentityEvent id)
    {
        state.Update(s => s with
        {
            PlayerName = id.PlayerName,
            PlayerId = id.PlayerId,
            LevelBase = id.LevelBase,
            LevelExtra = id.LevelExtra,
            SeasonExp = id.SeasonExp,
            FightPoint = id.FightPoint,
            ProfessionId = id.ProfessionId,
            ProfessionName = id.ProfessionName,
            PacketActive = true,
        });
        return true;
    }

    private static bool ApplyHealth(GameStateManager state, HealthEvent hp)
    {
        state.Update(s => s with
        {
            HpCurrent = hp.HpCurrent,
            HpMax = hp.HpMax,
            HpPct = hp.HpPct,
            PacketActive = true,
        });
        return true;
    }

    private static bool ApplyStamina(GameStateManager state, StaminaEvent st)
    {
        state.Update(s => s with
        {
            StaminaCurrent = (int)Math.Round(st.StaminaCurrent),
            StaminaMax = (int)Math.Min(int.MaxValue, st.StaminaMax),
            StaminaPct = st.StaminaPct,
            StaminaOffline = st.Offline,
        });
        return true;
    }

    private static bool ApplyServerTime(GameStateManager state, ServerTimeEvent srv)
    {
        state.Update(s => s with { ServerTimeOffsetMs = srv.OffsetMs });
        return true;
    }

    private static bool ApplyRevive(GameStateManager state, ReviveEvent _)
    {
        state.Update(s => s with { InCombat = false, HpPct = 1.0 });
        return true;
    }

    private static bool ApplyKickOff(GameStateManager state)
    {
        state.Update(s => s with
        {
            PacketActive = false,
            InCombat = false,
            ErrorMsg = "session ended (kicked)",
        });
        return true;
    }

    private static bool ApplyEnterGame(GameStateManager state, EnterGameEvent _)
    {
        state.Update(s => s with { PacketActive = true, ErrorMsg = string.Empty });
        return true;
    }

    private static bool ApplyCombatState(GameStateManager state, CombatStateEvent cs)
    {
        state.Update(s => s with { InCombat = cs.InCombat });
        return true;
    }

    private static bool ApplyDungeonStart(GameStateManager state)
    {
        state.Update(s => s with { InCombat = false });
        return true;
    }

    private static bool ApplyBuffs(GameStateManager state, BuffSnapshotEvent buff)
    {
        var entries = buff.Buffs.Select(b => new BuffEntry
        {
            Id = b.Id,
            Uuid = b.Uuid,
            BeginMs = b.BeginMs,
            DurationMs = b.DurationMs,
            Layer = b.Layer,
            Count = b.Count,
            Name = b.Name,
        }).ToImmutableArray();

        state.Update(s => s with
        {
            SelfBuffs = entries,
            ServerTimeOffsetMs = buff.ServerTimeOffsetMs,
        });
        return true;
    }

    private static bool ApplyBossHp(GameStateManager state, BossHpEvent boss)
    {
        state.Update(s => s with
        {
            BossCurrentHp = (int)Math.Min(int.MaxValue, boss.CurrentHp),
            BossTotalHp = (int)Math.Min(int.MaxValue, boss.MaxHp),
            BossHpEstPct = boss.EstimatedPct,
            BossShieldActive = boss.ShieldActive,
            BossShieldPct = boss.ShieldPct,
            BossBreakingStage = boss.BreakingStage,
            BossInOverdrive = boss.InOverdrive,
            BossInvincible = boss.Invincible,
            BossHpSource = BossHpSource.Packet,
        });
        return true;
    }

    private static bool ApplyDps(GameStateManager state, DpsEvent dps)
    {
        state.Update(s => s with
        {
            BossTotalDamage = (int)Math.Min(int.MaxValue, dps.TotalDamage),
            BossDps = (int)Math.Min(int.MaxValue, dps.Dps),
        });
        return true;
    }

    private static bool ApplyIdentityAlert(GameStateManager state, IdentityAlertEvent alert)
    {
        state.Update(s => s with
        {
            IdentityAlertSerial = alert.Serial,
            IdentityAlertTitle = alert.Title,
            IdentityAlertMessage = alert.Message,
        });
        return true;
    }
}
