using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S126 — wire <c>SyncToMeDeltaInfo.BaseDelta.Attrs</c> (player-side
/// path) to a new <see cref="PlayerAttrEvent"/>; bridge mirrors
/// identity + HP + profession into the local player slots. Mirrors
/// Python's <c>_process_attr_collection</c> at packet_parser.py
/// 4894–5050 (identity + HP + profession slice only — combat-stat
/// fields deferred).
/// </summary>
public class Session126PlayerAttrTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;
    private const long MonsterUuid = (123L << 16) | 64;

    private static Attr A(int id, params byte[] raw)
        => new() { Id = id, RawData = ByteString.CopyFrom(raw) };

    // varint helper for small positive ints (one byte covers 0–127).
    private static byte[] V(int v)
    {
        if (v < 0) throw new ArgumentOutOfRangeException(nameof(v));
        var bytes = new List<byte>();
        var u = (uint)v;
        while (u >= 0x80) { bytes.Add((byte)(u | 0x80)); u >>= 7; }
        bytes.Add((byte)u);
        return bytes.ToArray();
    }

    // length-prefixed utf-8 string for AttrType.NAME.
    private static byte[] S(string s)
    {
        var enc = System.Text.Encoding.UTF8.GetBytes(s);
        var len = V(enc.Length);
        var result = new byte[len.Length + enc.Length];
        Buffer.BlockCopy(len, 0, result, 0, len.Length);
        Buffer.BlockCopy(enc, 0, result, len.Length, enc.Length);
        return result;
    }

    private static AoiSyncDelta BaseWithAttrs(long uuid, params Attr[] items)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var i in items) d.Attrs.Attrs.Add(i);
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
    public void Decoder_PlayerAttrs_EmitsIdentityAndHp()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = BaseWithAttrs(PlayerUuid,
                A(1, S("Yui")),
                A(220, V(3)),       // ProfessionId
                A(10000, V(80)),    // Level
                A(10030, V(45000)),  // FightPoint (multi-byte varint)
                A(11310, V(12000)),  // Hp
                A(11320, V(15000))), // MaxHp
        };
        var ev = Assert.Single(Dispatch(inner).OfType<PlayerAttrEvent>());
        Assert.Equal(PlayerUuid, ev.Uuid);
        Assert.True(ev.HasName); Assert.Equal("Yui", ev.Name);
        Assert.True(ev.HasProfessionId); Assert.Equal(3, ev.ProfessionId);
        Assert.True(ev.HasLevel); Assert.Equal(80, ev.Level);
        Assert.True(ev.HasFightPoint); Assert.Equal(45000, ev.FightPoint);
        Assert.True(ev.HasHp); Assert.Equal(12000, ev.Hp);
        Assert.True(ev.HasMaxHp); Assert.Equal(15000, ev.MaxHp);
    }

    [Fact]
    public void Decoder_MonsterUuid_NoEvent()
    {
        // Mirrors S121 gate: only player low-marker BaseDeltas surface
        // PlayerAttrEvent.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = BaseWithAttrs(MonsterUuid, A(11310, V(100))),
        };
        Assert.Empty(Dispatch(inner).OfType<PlayerAttrEvent>());
    }

    [Fact]
    public void Decoder_NoRecognisedAttrs_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = BaseWithAttrs(PlayerUuid, A(99999, V(5))),
        };
        Assert.Empty(Dispatch(inner).OfType<PlayerAttrEvent>());
    }

    [Fact]
    public void Decoder_EmptyAttrs_NoEvent()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = PlayerUuid,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid, Attrs = new AttrCollection() },
        };
        Assert.Empty(Dispatch(inner).OfType<PlayerAttrEvent>());
    }

    [Fact]
    public void Bridge_MirrorsIdentityAndHp()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        var ev = new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Name = "Yui", HasName = true,
            Level = 80, HasLevel = true,
            FightPoint = 45000, HasFightPoint = true,
            Hp = 12000, HasHp = true,
            MaxHp = 15000, HasMaxHp = true,
            ProfessionId = 3, HasProfessionId = true,
        };
        bridge.Apply(ev);

        var s = state.Snapshot;
        Assert.Equal("Yui", s.PlayerName);
        Assert.Equal(80, s.LevelBase);
        Assert.Equal(45000, s.FightPoint);
        Assert.Equal(12000, s.HpCurrent);
        Assert.Equal(15000, s.HpMax);
        Assert.Equal(3, s.ProfessionId);
        Assert.Equal(0.8, s.HpPct, 3);
    }

    [Fact]
    public void Bridge_SelfUuidMismatch_DoesNotWrite()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid, PlayerName = "Original" });
        var bridge = new PacketBridge(state, new PacketParser());

        var otherPlayer = (999L << 16) | 640;
        bridge.Apply(new PlayerAttrEvent(otherPlayer, 1.0)
        {
            Name = "Hacker", HasName = true,
        });

        Assert.Equal("Original", state.Snapshot.PlayerName);
    }

    [Fact]
    public void Bridge_ColdStart_LatchesSelfUuid()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Name = "Yui", HasName = true,
            Level = 80, HasLevel = true,
        });

        Assert.Equal((ulong)PlayerUuid, state.Snapshot.SelfUuid);
        Assert.Equal("Yui", state.Snapshot.PlayerName);
    }

    [Fact]
    public void Bridge_HpZeroIgnored_WhenMaxHpKnown()
    {
        // Python's special case: ignore transient HP=0 unless max_hp is
        // also 0 (cold-start). Pre-seed max_hp + non-zero hp; a delta
        // carrying HasHp=true with Hp=0 must NOT zero the row.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid, HpCurrent = 12000, HpMax = 15000,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Hp = 0, HasHp = true,
        });

        Assert.Equal(12000, state.Snapshot.HpCurrent);
    }

    [Fact]
    public void Bridge_HpZeroAccepted_WhenMaxHpAlsoZero()
    {
        // Cold-start path: both HP and MaxHp absent → HP=0 IS written
        // (matches Python's `if hp > 0 or player.max_hp == 0`).
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Hp = 0, HasHp = true,
        });

        Assert.Equal(0, state.Snapshot.HpCurrent);
        Assert.Equal(0, state.Snapshot.HpMax);
    }

    [Fact]
    public void Bridge_NoChange_ReturnsFalse()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid, PlayerName = "Yui", LevelBase = 80,
        });
        var bridge = new PacketBridge(state, new PacketParser());
        var before = bridge.EventsApplied;

        // Same values → no-op.
        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Name = "Yui", HasName = true,
            Level = 80, HasLevel = true,
        });

        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void Bridge_PartialEvent_OnlyMirrorsPresentFields()
    {
        // Event with only FightPoint set must not stomp the other slots.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid, PlayerName = "Yui", LevelBase = 80,
            HpCurrent = 12000, HpMax = 15000,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            FightPoint = 50000, HasFightPoint = true,
        });

        var s = state.Snapshot;
        Assert.Equal("Yui", s.PlayerName);
        Assert.Equal(80, s.LevelBase);
        Assert.Equal(12000, s.HpCurrent);
        Assert.Equal(15000, s.HpMax);
        Assert.Equal(50000, s.FightPoint);
    }

    [Fact]
    public void Bridge_NamePresentButEmpty_DoesNotStomp()
    {
        // Mirrors Python's `if name: player.name = name` — empty string
        // preserves the existing name (don't blank it on a TempAttr-style
        // delta that re-emits the field with no value).
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid, PlayerName = "Yui" });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            Name = string.Empty, HasName = true,
        });

        Assert.Equal("Yui", state.Snapshot.PlayerName);
    }
}
