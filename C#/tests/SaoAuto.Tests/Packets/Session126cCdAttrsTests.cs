using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S126c — CD-related player attrs from <c>_process_attr_collection</c>:
/// AttrSkillCd (11750/11751, flat ms), AttrSkillCdPct (11760/11761,
/// /10000 万分比), AttrCdAcceleratePct (11960/11961, /10000),
/// AttrFightResCdSpeed (11980/11981, /10000). Separate from S122's
/// TempAttrCd* (TempAttrCollection ids 100/101/103) — these come from
/// equipment / passives, those come from temporary buffs. Mirrors
/// packet_parser.py 5034–5058.
/// </summary>
public class Session126cCdAttrsTests
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
    public void Decoder_AllCdAttrs_BaseIds()
    {
        var ev = Assert.Single(Dispatch(Inner(
            A(11750, V(2000)),  // SkillCd flat ms
            A(11760, V(1500)),  // SkillCdPct /10000
            A(11960, V(800)),   // CdAcceleratePct /10000
            A(11980, V(1200))   // FightResCdSpeed /10000
        )).OfType<PlayerAttrEvent>());

        Assert.True(ev.HasAttrSkillCd); Assert.Equal(2000, ev.AttrSkillCd);
        Assert.True(ev.HasAttrSkillCdPct); Assert.Equal(1500, ev.AttrSkillCdPct);
        Assert.True(ev.HasAttrCdAcceleratePct); Assert.Equal(800, ev.AttrCdAcceleratePct);
        Assert.True(ev.HasAttrFightResCdSpeed); Assert.Equal(1200, ev.AttrFightResCdSpeed);
    }

    [Fact]
    public void Decoder_TotalIdsAliasBase()
    {
        var ev = Assert.Single(Dispatch(Inner(
            A(11751, V(3333)),  // SkillCd_TOTAL
            A(11761, V(2222)),  // SkillCdPct_TOTAL
            A(11961, V(1111)),  // CdAcceleratePct_TOTAL
            A(11981, V(4444))   // FightResCdSpeed_TOTAL
        )).OfType<PlayerAttrEvent>());

        Assert.Equal(3333, ev.AttrSkillCd);
        Assert.Equal(2222, ev.AttrSkillCdPct);
        Assert.Equal(1111, ev.AttrCdAcceleratePct);
        Assert.Equal(4444, ev.AttrFightResCdSpeed);
    }

    [Fact]
    public void Bridge_MirrorsAllCdAttrs()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            AttrSkillCd = 2000, HasAttrSkillCd = true,
            AttrSkillCdPct = 1500, HasAttrSkillCdPct = true,
            AttrCdAcceleratePct = 800, HasAttrCdAcceleratePct = true,
            AttrFightResCdSpeed = 1200, HasAttrFightResCdSpeed = true,
        });

        var s = state.Snapshot;
        Assert.Equal(2000, s.AttrSkillCd);
        Assert.Equal(1500, s.AttrSkillCdPct);
        Assert.Equal(800, s.AttrCdAcceleratePct);
        Assert.Equal(1200, s.AttrFightResCdSpeed);
    }

    [Fact]
    public void Bridge_SkillCdAndPct_ZeroAcceptedAsBaseline()
    {
        // SkillCd / SkillCdPct / CdAcceleratePct guard with `>= 0` —
        // a buff expiry that drops a +X% CDR back to 0 baseline must
        // land. Pre-seed non-zero, then send zero.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            AttrSkillCd = 2000, AttrSkillCdPct = 1500,
            AttrCdAcceleratePct = 800,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            AttrSkillCd = 0, HasAttrSkillCd = true,
            AttrSkillCdPct = 0, HasAttrSkillCdPct = true,
            AttrCdAcceleratePct = 0, HasAttrCdAcceleratePct = true,
        });

        var s = state.Snapshot;
        Assert.Equal(0, s.AttrSkillCd);
        Assert.Equal(0, s.AttrSkillCdPct);
        Assert.Equal(0, s.AttrCdAcceleratePct);
    }

    [Fact]
    public void Bridge_FightResCdSpeed_ZeroDoesNotStomp()
    {
        // FightResCdSpeed uses `> 0` (Python explicit gate at line 5055)
        // — a transient zero must NOT zero the slot.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid, AttrFightResCdSpeed = 1200,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            AttrFightResCdSpeed = 0, HasAttrFightResCdSpeed = true,
        });

        Assert.Equal(1200, state.Snapshot.AttrFightResCdSpeed);
    }

    [Fact]
    public void Bridge_DistinctFromTempAttrCd()
    {
        // S122 TempAttrCd* and S126c Attr* must NOT share slots — they
        // are independent CDR sources (equipment vs buffs). Set both
        // and verify they coexist.
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            TempAttrCdPct = 500, TempAttrCdFixed = 100, TempAttrCdAccel = 200,
        });
        var bridge = new PacketBridge(state, new PacketParser());

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            AttrSkillCd = 2000, HasAttrSkillCd = true,
            AttrSkillCdPct = 1500, HasAttrSkillCdPct = true,
        });

        var s = state.Snapshot;
        // S122 slots untouched.
        Assert.Equal(500, s.TempAttrCdPct);
        Assert.Equal(100, s.TempAttrCdFixed);
        Assert.Equal(200, s.TempAttrCdAccel);
        // S126c slots written.
        Assert.Equal(2000, s.AttrSkillCd);
        Assert.Equal(1500, s.AttrSkillCdPct);
    }

    [Fact]
    public void Bridge_NoChange_ReturnsFalse()
    {
        var state = new GameStateManager();
        state.Update(s => s with
        {
            SelfUuid = (ulong)PlayerUuid,
            AttrSkillCd = 2000, AttrSkillCdPct = 1500,
        });
        var bridge = new PacketBridge(state, new PacketParser());
        var before = bridge.EventsApplied;

        bridge.Apply(new PlayerAttrEvent(PlayerUuid, 1.0)
        {
            AttrSkillCd = 2000, HasAttrSkillCd = true,
            AttrSkillCdPct = 1500, HasAttrSkillCdPct = true,
        });

        Assert.Equal(before, bridge.EventsApplied);
    }

    [Fact]
    public void DecoderToBridge_EndToEnd()
    {
        var state = new GameStateManager();
        state.Update(s => s with { SelfUuid = (ulong)PlayerUuid });
        var bridge = new PacketBridge(state, new PacketParser());

        var inner = Inner(
            A(11751, V(2000)),
            A(11760, V(1500)),
            A(11961, V(800)),
            A(11981, V(1200))
        );
        var ev = Assert.Single(Dispatch(inner).OfType<PlayerAttrEvent>());
        bridge.Apply(ev);

        var s = state.Snapshot;
        Assert.Equal(2000, s.AttrSkillCd);
        Assert.Equal(1500, s.AttrSkillCdPct);
        Assert.Equal(800, s.AttrCdAcceleratePct);
        Assert.Equal(1200, s.AttrFightResCdSpeed);
    }
}
