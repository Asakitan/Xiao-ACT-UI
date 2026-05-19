using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S124 — wire <c>SyncToMeDeltaInfo.BaseDelta.BuffEffect</c> to a new
/// <see cref="BuffEffectEvent"/>; bridge mutates
/// <see cref="MonsterData"/> rows for HostDeath / ShieldBroken /
/// EnterBreaking. Mirrors Python's <c>_process_buff_effect_sync</c>
/// at packet_parser.py 4789–4825 (filter + per-event branches).
/// </summary>
public class Session124BuffEffectTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long MonsterUuid = (123L << 16) | 64;
    private const long OtherMonsterUuid = (456L << 16) | 64;

    private static BuffEffect BE(int type, int buffUuid = 0, long hostUuid = 0)
        => new() { Type = (EBuffEventType)type, BuffUuid = buffUuid, HostUuid = hostUuid };

    private static AoiSyncDelta Base(long uuid, params BuffEffect[] effects)
    {
        var d = new AoiSyncDelta { Uuid = uuid, BuffEffect = new BuffEffectSync() };
        foreach (var be in effects) d.BuffEffect.BuffEffects.Add(be);
        return d;
    }

    private static List<ParserEvent> Dispatch(AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    [Fact]
    public void Decoder_BossEvent_Emits()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(MonsterUuid, BE(BuffEventType.EnterBreaking, buffUuid: 9001)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<BuffEffectEvent>());
        Assert.Equal(MonsterUuid, ev.TargetUuid);
        var eff = Assert.Single(ev.Effects);
        Assert.Equal(BuffEventType.EnterBreaking, eff.Type);
        Assert.Equal(9001, eff.BuffUuid);
        // be.HostUuid == 0 → falls back to sync uuid.
        Assert.Equal(MonsterUuid, eff.HostUuid);
    }

    [Fact]
    public void Decoder_HostUuidFallback_UsesProvidedHost()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(MonsterUuid, BE(BuffEventType.HostDeath, hostUuid: OtherMonsterUuid)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<BuffEffectEvent>());
        Assert.Equal(OtherMonsterUuid, ev.Effects[0].HostUuid);
    }

    [Fact]
    public void Decoder_FiltersNonBossEventTypes()
    {
        // Event type 999 isn't in BuffEventType.BossEvents → dropped.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(MonsterUuid,
                BE(999),
                BE(BuffEventType.ShieldBroken),
                BE(7)),
        };
        var ev = Assert.Single(Dispatch(inner).OfType<BuffEffectEvent>());
        Assert.Single(ev.Effects);
        Assert.Equal(BuffEventType.ShieldBroken, ev.Effects[0].Type);
    }

    [Fact]
    public void Decoder_AllNonBossTypes_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(MonsterUuid, BE(999), BE(1), BE(2)),
        };
        Assert.Empty(Dispatch(inner).OfType<BuffEffectEvent>());
    }

    [Fact]
    public void Decoder_PlayerLowMarker_NoEvent()
    {
        // Player low-marker → Python's `target_is_monster` predicate is
        // false → branch never invokes _process_buff_effect_sync.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = Base(PlayerUuid, BE(BuffEventType.EnterBreaking)),
        };
        Assert.Empty(Dispatch(inner).OfType<BuffEffectEvent>());
    }

    [Fact]
    public void Decoder_NoBuffEffect_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta { Uuid = MonsterUuid },
        };
        Assert.Empty(Dispatch(inner).OfType<BuffEffectEvent>());
    }

    [Fact]
    public void Bridge_EnterBreaking_ResetsBreakingStageAndExtinction()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap.SetItem(MonsterUuid, new MonsterData
            {
                Uuid = MonsterUuid, BreakingStage = 2, Extinction = 5_000,
                MaxExtinction = 10_000, Hp = 100, MaxHp = 100,
            }),
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new BuffEffectEvent(MonsterUuid,
            new[] { new BuffEffectPayload(BuffEventType.EnterBreaking, 1, MonsterUuid) }, 1.0));

        var md = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(0, md.BreakingStage);
        Assert.Equal(0, md.Extinction);
        // Untouched fields preserved.
        Assert.Equal(10_000, md.MaxExtinction);
        Assert.Equal(100, md.Hp);
    }

    [Fact]
    public void Bridge_ShieldBroken_ClearsShield()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap.SetItem(MonsterUuid, new MonsterData
            {
                Uuid = MonsterUuid, ShieldActive = true, ShieldTotal = 5_000,
                ShieldMaxTotal = 8_000,
            }),
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new BuffEffectEvent(MonsterUuid,
            new[] { new BuffEffectPayload(BuffEventType.ShieldBroken, 1, MonsterUuid) }, 1.0));

        var md = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.False(md.ShieldActive);
        Assert.Equal(0, md.ShieldTotal);
        // ShieldMaxTotal is NOT cleared (Python only zeroes shield_total).
        Assert.Equal(8_000, md.ShieldMaxTotal);
    }

    [Fact]
    public void Bridge_HostDeath_FlipsIsDeadAndZerosHp()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap.SetItem(MonsterUuid, new MonsterData
            {
                Uuid = MonsterUuid, Hp = 1_000, MaxHp = 10_000, IsDead = false,
            }),
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new BuffEffectEvent(MonsterUuid,
            new[] { new BuffEffectPayload(BuffEventType.HostDeath, 1, MonsterUuid) }, 1.0));

        var md = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.True(md.IsDead);
        Assert.Equal(0, md.Hp);
        Assert.Equal(10_000, md.MaxHp);
    }

    [Fact]
    public void Bridge_NonMutatingBossEvent_DoesNotChangeRow()
    {
        // SuperArmorBroken (51) is in the boss-events allow-list (so
        // the decoder emits it) but Python's `_process_buff_effect_sync`
        // has no `elif` arm for it — only the boss-event observer
        // callback fires. The MonsterData row stays intact.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap.SetItem(MonsterUuid, new MonsterData
            {
                Uuid = MonsterUuid, BreakingStage = 2, Hp = 1_000,
                ShieldActive = true, ShieldTotal = 500,
            }),
        });
        var bridge = new PacketBridge(state, new PacketParser());
        var before = bridge.EventsApplied;

        bridge.Apply(new BuffEffectEvent(MonsterUuid,
            new[] { new BuffEffectPayload(BuffEventType.SuperArmorBroken, 1, MonsterUuid) }, 1.0));

        var md = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(2, md.BreakingStage);
        Assert.Equal(1_000, md.Hp);
        Assert.True(md.ShieldActive);
        Assert.Equal(500, md.ShieldTotal);
        // Mutator returns false → EventsApplied unchanged.
        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void Bridge_UnknownMonster_NoOp()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var before = bridge.EventsApplied;

        bridge.Apply(new BuffEffectEvent(MonsterUuid,
            new[] { new BuffEffectPayload(BuffEventType.HostDeath, 1, MonsterUuid) }, 1.0));

        Assert.Empty(state.Snapshot.MonsterDataMap);
        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void Bridge_MultipleEffects_AllApplied()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            MonsterDataMap = s.MonsterDataMap.SetItem(MonsterUuid, new MonsterData
            {
                Uuid = MonsterUuid, BreakingStage = 2, Extinction = 5_000,
                ShieldActive = true, ShieldTotal = 1_000, Hp = 100,
            }),
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new BuffEffectEvent(MonsterUuid, new[]
        {
            new BuffEffectPayload(BuffEventType.EnterBreaking, 1, MonsterUuid),
            new BuffEffectPayload(BuffEventType.ShieldBroken, 2, MonsterUuid),
            new BuffEffectPayload(BuffEventType.HostDeath, 3, MonsterUuid),
        }, 1.0));

        var md = state.Snapshot.MonsterDataMap[MonsterUuid];
        Assert.Equal(0, md.BreakingStage);
        Assert.Equal(0, md.Extinction);
        Assert.False(md.ShieldActive);
        Assert.Equal(0, md.ShieldTotal);
        Assert.True(md.IsDead);
        Assert.Equal(0, md.Hp);
    }
}
