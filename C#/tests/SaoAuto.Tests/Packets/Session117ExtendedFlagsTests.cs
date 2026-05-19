using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S117 — extend <c>ExtractMonsterCoreAttrs</c> with the remaining
/// monster flag set: IS_LOCK_STUNNED (445), STOP_BREAKING_TICKING (453),
/// STATE (11), DEAD_TYPE (78), DEAD_TIME (206), FIRST_ATTACK (456),
/// HATED_CHAR_ID (471), HATED_CHAR_NAME (473). Mirrors the corresponding
/// branches in Python's <c>_process_monster_attr_collection</c>.
/// HatedCharName uses first-non-empty-wins on the delta path so an empty-
/// string delta cannot blank a previously-observed aggro target.
/// </summary>
public class Session117ExtendedFlagsTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private static ByteString Varint(long value)
    {
        var buf = new List<byte>(10);
        var v = unchecked((ulong)value);
        while (v >= 0x80) { buf.Add((byte)(v | 0x80)); v >>= 7; }
        buf.Add((byte)v);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static ByteString LenString(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s);
        var buf = new List<byte>(bytes.Length + 5);
        var len = (uint)bytes.Length;
        while (len >= 0x80) { buf.Add((byte)(len | 0x80)); len >>= 7; }
        buf.Add((byte)len);
        buf.AddRange(bytes);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static Attr A(int id, ByteString raw) => new() { Id = id, RawData = raw };

    private static AoiSyncDelta Delta(long uuid, params Attr[] attrs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var a in attrs) d.Attrs.Attrs.Add(a);
        return d;
    }

    [Fact]
    public void Decoder_UnpacksExtendedFlagsOnDelta()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7,
            A(11, Varint(3)),                     // STATE = 3
            A(78, Varint(2)),                     // DEAD_TYPE = 2
            A(206, Varint(123_456)),              // DEAD_TIME = 123456
            A(445, Varint(1)),                    // IS_LOCK_STUNNED = true
            A(453, Varint(1)),                    // STOP_BREAKING_TICKING = true
            A(456, Varint(1)),                    // FIRST_ATTACK = true
            A(471, Varint(0x1_00000042L)),        // HATED_CHAR_ID — wide long
            A(473, LenString("阿尔薇娜"))));      // HATED_CHAR_NAME — utf-8

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        var a = u.Attrs;
        Assert.True(a.HasState); Assert.Equal(3, a.State);
        Assert.True(a.HasDeadType); Assert.Equal(2, a.DeadType);
        Assert.True(a.HasDeadTime); Assert.Equal(123_456, a.DeadTime);
        Assert.True(a.HasIsLockStunned); Assert.True(a.IsLockStunned);
        Assert.True(a.HasStopBreakingTicking); Assert.True(a.StopBreakingTicking);
        Assert.True(a.HasFirstAttack); Assert.True(a.FirstAttack);
        Assert.True(a.HasHatedCharId); Assert.Equal(0x1_00000042L, a.HatedCharId);
        Assert.True(a.HasHatedCharName); Assert.Equal("阿尔薇娜", a.HatedCharName);
    }

    [Fact]
    public void Decoder_UnpacksExtendedFlagsOnAppearance()
    {
        var msg = new SyncNearEntities();
        msg.Appear.Add(new Entity
        {
            Uuid = 7,
            EntType = EEntityType.Entmonster,
            Attrs = new AttrCollection
            {
                Attrs =
                {
                    A(11310, Varint(50_000)),
                    A(11, Varint(5)),
                    A(445, Varint(1)),
                    A(471, Varint(0x42L)),
                    A(473, LenString("Boss")),
                },
            },
        });

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        Assert.NotNull(captured);
        var app = Assert.Single(captured!.Appear);
        Assert.True(app.Attrs.HasState); Assert.Equal(5, app.Attrs.State);
        Assert.True(app.Attrs.HasIsLockStunned); Assert.True(app.Attrs.IsLockStunned);
        Assert.True(app.Attrs.HasHatedCharId); Assert.Equal(0x42L, app.Attrs.HatedCharId);
        Assert.True(app.Attrs.HasHatedCharName); Assert.Equal("Boss", app.Attrs.HatedCharName);
    }

    [Fact]
    public void Bridge_AppearanceSeedsExtendedFlags()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var attrs = new MonsterCoreAttrs
        {
            CurHp = 50_000, HasCurHp = true,
            IsLockStunned = true, HasIsLockStunned = true,
            State = 7, HasState = true,
            FirstAttack = true, HasFirstAttack = true,
            HatedCharId = 0xABCDL, HasHatedCharId = true,
            HatedCharName = "Hero", HasHatedCharName = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 50_000,
                    Attrs = attrs,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.True(mon.IsLockStunned);
        Assert.Equal(7, mon.State);
        Assert.True(mon.FirstAttack);
        Assert.Equal(0xABCDL, mon.HatedCharId);
        Assert.Equal("Hero", mon.HatedCharName);
    }

    [Fact]
    public void Bridge_DeltaWritesExtendedFlags()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.False(state.Snapshot.MonsterDataMap[7].IsLockStunned);

        var attrs = new MonsterCoreAttrs
        {
            IsLockStunned = true, HasIsLockStunned = true,
            StopBreakingTicking = true, HasStopBreakingTicking = true,
            State = 9, HasState = true,
            DeadType = 1, HasDeadType = true,
            DeadTime = 9999, HasDeadTime = true,
            HatedCharId = 12345L, HasHatedCharId = true,
            HatedCharName = "P1", HasHatedCharName = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = attrs } },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.True(mon.IsLockStunned);
        Assert.True(mon.StopBreakingTicking);
        Assert.Equal(9, mon.State);
        Assert.Equal(1, mon.DeadType);
        Assert.Equal(9999, mon.DeadTime);
        Assert.Equal(12345L, mon.HatedCharId);
        Assert.Equal("P1", mon.HatedCharName);
    }

    [Fact]
    public void Bridge_DeltaIsLockStunnedFalseOverridesSticky()
    {
        // HasIsLockStunned=true with value=false must override prior true
        // (real packet ending the lock, not "attr missing"). Same pattern
        // as S115 InOverdrive false test.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            IsLockStunned = true, HasIsLockStunned = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 1,
                    Attrs = seed,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.True(state.Snapshot.MonsterDataMap[7].IsLockStunned);

        var ended = new MonsterCoreAttrs
        {
            IsLockStunned = false, HasIsLockStunned = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = ended } },
        });

        Assert.False(state.Snapshot.MonsterDataMap[7].IsLockStunned);
    }

    [Fact]
    public void Bridge_DeltaWithoutExtendedFlagsCarriesForward()
    {
        // HP-only delta must NOT blank IsLockStunned / State / HatedCharName.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            IsLockStunned = true, HasIsLockStunned = true,
            StopBreakingTicking = true, HasStopBreakingTicking = true,
            State = 4, HasState = true,
            FirstAttack = true, HasFirstAttack = true,
            HatedCharId = 999L, HasHatedCharId = true,
            HatedCharName = "Aggro", HasHatedCharName = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 100,
                    Attrs = seed,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, 50, 0, 0) { HasCurHp = true },
            },
        });

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal(50, mon.Hp);
        Assert.True(mon.IsLockStunned);
        Assert.True(mon.StopBreakingTicking);
        Assert.Equal(4, mon.State);
        Assert.True(mon.FirstAttack);
        Assert.Equal(999L, mon.HatedCharId);
        Assert.Equal("Aggro", mon.HatedCharName);
    }

    [Fact]
    public void Bridge_DeltaEmptyHatedCharNamePreservesAggro()
    {
        // Python first-non-empty-wins: a delta carrying HasHatedCharName=true
        // but value="" must NOT overwrite an existing aggro display.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        var seed = new MonsterCoreAttrs
        {
            HatedCharName = "Hero", HasHatedCharName = true,
        };
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 1,
                    Attrs = seed,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.Equal("Hero", state.Snapshot.MonsterDataMap[7].HatedCharName);

        var blank = new MonsterCoreAttrs
        {
            HatedCharName = "", HasHatedCharName = true,
        };
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) { Attrs = blank } },
        });

        Assert.Equal("Hero", state.Snapshot.MonsterDataMap[7].HatedCharName);
    }
}
