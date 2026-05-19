using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S126b — extends <see cref="PlayerAttrEvent"/> + bridge mirror with
/// the 13 player combat-stat slots (Attack/MAttack/Defense/MDefense,
/// CritRate/CritDamage, Attack/Cast/ChargeSpeedPct, HealPower,
/// DamInc/MDamInc/BossDamInc). BASE id and TOTAL id share an
/// accumulator (last-write-wins on the foreach — matches Python's
/// `elif attr_id in (BASE, TOTAL):` at packet_parser.py 5060–5111).
/// Raw stats use `> 0` guards, percent stats use `>= 0` (so a buff
/// expiry can drop the slot back to baseline).
/// </summary>
public class Session126bCombatStatsTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;

    private static Attr A(int id, params byte[] raw)
        => new() { Id = id, RawData = ByteString.CopyFrom(raw) };

    private static byte[] V(int v)
    {
        if (v < 0) throw new ArgumentOutOfRangeException(nameof(v));
        var bytes = new List<byte>();
        var u = (uint)v;
        while (u >= 0x80) { bytes.Add((byte)(u | 0x80)); u >>= 7; }
        bytes.Add((byte)u);
        return bytes.ToArray();
    }

    private static List<ParserEvent> Dispatch(AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var captured = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts,
            captured.Add);
        return captured;
    }

    private static AoiSyncToMeDelta Inner(params Attr[] items)
    {
        var d = new AoiSyncDelta { Uuid = PlayerUuid, Attrs = new AttrCollection() };
        foreach (var i in items) d.Attrs.Attrs.Add(i);
        return new AoiSyncToMeDelta { Uuid = PlayerUuid, BaseDelta = d };
    }

    [Fact]
    public void Decoder_AllStats_BaseIds()
    {
        var ev = Assert.Single(Dispatch(Inner(
            A(11330, V(5000)),  // Attack
            A(11340, V(4000)),  // MagicAttack
            A(11350, V(3000)),  // Defense
            A(11360, V(2000)),  // MagicDefense
            A(11110, V(1500)),  // CritRate
            A(12510, V(20000)), // CritDamage
            A(11720, V(800)),   // AttackSpeedPct
            A(11730, V(700)),   // CastSpeedPct
            A(11740, V(600)),   // ChargeSpeedPct
            A(11790, V(1000)),  // HealPower
            A(12550, V(900)),   // DamInc
            A(12570, V(850)),   // MDamInc
            A(12630, V(750))    // BossDamInc
        )).OfType<PlayerAttrEvent>());

        Assert.True(ev.HasAttack); Assert.Equal(5000, ev.Attack);
        Assert.True(ev.HasMagicAttack); Assert.Equal(4000, ev.MagicAttack);
        Assert.True(ev.HasDefense); Assert.Equal(3000, ev.Defense);
        Assert.True(ev.HasMagicDefense); Assert.Equal(2000, ev.MagicDefense);
        Assert.True(ev.HasCritRate); Assert.Equal(1500, ev.CritRate);
        Assert.True(ev.HasCritDamage); Assert.Equal(20000, ev.CritDamage);
        Assert.True(ev.HasAttackSpeedPct); Assert.Equal(800, ev.AttackSpeedPct);
        Assert.True(ev.HasCastSpeedPct); Assert.Equal(700, ev.CastSpeedPct);
        Assert.True(ev.HasChargeSpeedPct); Assert.Equal(600, ev.ChargeSpeedPct);
        Assert.True(ev.HasHealPower); Assert.Equal(1000, ev.HealPower);
        Assert.True(ev.HasDamInc); Assert.Equal(900, ev.DamInc);
        Assert.True(ev.HasMDamInc); Assert.Equal(850, ev.MDamInc);
        Assert.True(ev.HasBossDamInc); Assert.Equal(750, ev.BossDamInc);
    }

    [Fact]
    public void Decoder_TotalIdsAliasBase()
    {
        // TOTAL id (e.g. 11331) writes into the same Attack slot as BASE
        // (11330). Send only TOTAL — Has* still flips, value lands.
        var ev = Assert.Single(Dispatch(Inner(
            A(11331, V(7777))
        )).OfType<PlayerAttrEvent>());
        Assert.True(ev.HasAttack);
        Assert.Equal(7777, ev.Attack);
    }

    [Fact]
    public void Decoder_BaseThenTotal_LastWriteWins()
    {
        // Python's `elif attr_id in (BASE, TOTAL):` is foreach-order
        // last-write-wins. C# foreach mirrors proto wire order.
        var ev = Assert.Single(Dispatch(Inner(
            A(11330, V(100)),
            A(11331, V(200))
        )).OfType<PlayerAttrEvent>());
        Assert.Equal(200, ev.Attack);
    }

    [Fact]
    public void Bridge_MirrorsAllCombatStats()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Attack = 5000, HasAttack = true,
            MagicAttack = 4000, HasMagicAttack = true,
            Defense = 3000, HasDefense = true,
            MagicDefense = 2000, HasMagicDefense = true,
            CritRate = 1500, HasCritRate = true,
            CritDamage = 20000, HasCritDamage = true,
            AttackSpeedPct = 800, HasAttackSpeedPct = true,
            CastSpeedPct = 700, HasCastSpeedPct = true,
            ChargeSpeedPct = 600, HasChargeSpeedPct = true,
            HealPower = 1000, HasHealPower = true,
            DamInc = 900, HasDamInc = true,
            MDamInc = 850, HasMDamInc = true,
            BossDamInc = 750, HasBossDamInc = true,
        });

        var s = state.Snapshot;
        Assert.Equal(5000, s.Attack);
        Assert.Equal(4000, s.MagicAttack);
        Assert.Equal(3000, s.Defense);
        Assert.Equal(2000, s.MagicDefense);
        Assert.Equal(1500, s.CritRate);
        Assert.Equal(20000, s.CritDamage);
        Assert.Equal(800, s.AttackSpeedPct);
        Assert.Equal(700, s.CastSpeedPct);
        Assert.Equal(600, s.ChargeSpeedPct);
        Assert.Equal(1000, s.HealPower);
        Assert.Equal(900, s.DamInc);
        Assert.Equal(850, s.MDamInc);
        Assert.Equal(750, s.BossDamInc);
    }

    [Fact]
    public void Bridge_RawStatsZero_DoesNotStomp()
    {
        // Attack/MAttack/Defense/MDefense use `> 0` — a transient zero
        // (e.g. mid-respawn delta) must NOT zero the slot.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            Attack = 5000, MagicAttack = 4000,
            Defense = 3000, MagicDefense = 2000,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Attack = 0, HasAttack = true,
            MagicAttack = 0, HasMagicAttack = true,
            Defense = 0, HasDefense = true,
            MagicDefense = 0, HasMagicDefense = true,
        });

        var s = state.Snapshot;
        Assert.Equal(5000, s.Attack);
        Assert.Equal(4000, s.MagicAttack);
        Assert.Equal(3000, s.Defense);
        Assert.Equal(2000, s.MagicDefense);
    }

    [Fact]
    public void Bridge_PercentStatsZero_AcceptedAsBaseline()
    {
        // Percent stats use `>= 0` — buff expiry that drops a +20% to 0
        // baseline must land. Pre-seed non-zero, then send zero.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            CritRate = 2000, AttackSpeedPct = 500,
            DamInc = 1000, BossDamInc = 800,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            CritRate = 0, HasCritRate = true,
            AttackSpeedPct = 0, HasAttackSpeedPct = true,
            DamInc = 0, HasDamInc = true,
            BossDamInc = 0, HasBossDamInc = true,
        });

        var s = state.Snapshot;
        Assert.Equal(0, s.CritRate);
        Assert.Equal(0, s.AttackSpeedPct);
        Assert.Equal(0, s.DamInc);
        Assert.Equal(0, s.BossDamInc);
    }

    [Fact]
    public void Bridge_PartialEvent_OnlyMirrorsPresentSlots()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            Attack = 5000, Defense = 3000, CritRate = 1500,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            CritRate = 1800, HasCritRate = true,
        });

        var s = state.Snapshot;
        Assert.Equal(5000, s.Attack);    // untouched
        Assert.Equal(3000, s.Defense);   // untouched
        Assert.Equal(1800, s.CritRate);  // updated
    }

    [Fact]
    public void Bridge_SelfUuidMismatch_DoesNotWrite()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid, Attack = 5000 });
        var bridge = new PacketBridge(state, new PacketParser());

        var otherPlayer = (999L << 16) | 640;
        bridge.Apply(new PlayerAttrEvent(otherPlayer, 1.0)
        {
            Attack = 9999, HasAttack = true,
        });

        Assert.Equal(5000, state.Snapshot.Attack);
    }

    [Fact]
    public void Bridge_NoChange_ReturnsFalse()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid, Attack = 5000, CritRate = 1500,
        });
        var bridge = new PacketBridge(state, new PacketParser());
        var before = bridge.EventsApplied;

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Attack = 5000, HasAttack = true,
            CritRate = 1500, HasCritRate = true,
        });

        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void DecoderToBridge_EndToEnd()
    {
        // Wire path: SyncToMeDeltaInfo → PlayerAttrEvent → bridge mirror.
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        var inner = Inner(
            A(11330, V(5000)),
            A(11110, V(1500)),
            A(12630, V(750))
        );
        var ev = Assert.Single(Dispatch(inner).OfType<PlayerAttrEvent>());
        bridge.Apply(ev);

        var s = state.Snapshot;
        Assert.Equal(5000, s.Attack);
        Assert.Equal(1500, s.CritRate);
        Assert.Equal(750, s.BossDamInc);
    }
}
