using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S112 — distinguish a real HP=0 (monster died) from "attr not in this
/// packet" on the SyncNearDeltaInfo path. Adds nullable
/// <c>HasCurHp/HasMaxHp/HasLevel</c> sidecars on
/// <see cref="NearDeltaAttrUpdate"/>: when set, the bridge trusts the
/// flag (so a real zero overrides the existing row); when null (legacy
/// callers from S111 tests), the bridge falls back to the
/// <c>value != 0</c> carry-forward rule.
/// </summary>
public class Session112HpZeroDistinctionTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private static ByteString Varint(int value)
    {
        var buf = new List<byte>(5);
        var v = (uint)value;
        while (v >= 0x80) { buf.Add((byte)(v | 0x80)); v >>= 7; }
        buf.Add((byte)v);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static Attr A(int id, int value) => new() { Id = id, RawData = Varint(value) };

    private static AoiSyncDelta Delta(long uuid, params Attr[] attrs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var a in attrs) d.Attrs.Attrs.Add(a);
        return d;
    }

    // ── Decoder ──

    [Fact]
    public void Decoder_RealZeroHp_SetsHasCurHpTrue()
    {
        // Single 0x00 byte is a valid varint encoding of zero. Python's
        // `_decode_int32_from_raw` returns 0 here too — the "death"
        // signal is the *presence* of the attr, not its value.
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7, A(11310, 0)));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        Assert.Equal(0, u.CurHp);
        Assert.True(u.HasCurHp);
        // Flags for absent attrs stay false (not null) — decoder always
        // populates them explicitly.
        Assert.False(u.HasMaxHp);
        Assert.False(u.HasLevel);
    }

    [Fact]
    public void Decoder_PartialAttrs_AbsentFlagsStayFalse()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7, A(11320, 9_000)));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        Assert.False(u.HasCurHp);
        Assert.True(u.HasMaxHp);
        Assert.Equal(9_000, u.MaxHp);
        Assert.False(u.HasLevel);
    }

    // ── Bridge ──

    [Fact]
    public void Bridge_RealZeroHp_OverridesExistingRow()
    {
        // The death case: row had HP=5000, delta says CurHp=0 with
        // HasCurHp=true. Without S112 the zero would be treated as
        // "attr missing" and carried forward — masking the death.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 5_000, MaxHp = 8_000, Level = 12,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, CurHp: 0, MaxHp: 0, Level: 0)
                {
                    HasCurHp = true,
                },
            },
        });

        var row = state.Snapshot.NearEntities[7];
        Assert.Equal(0, row.CurHp); // real zero wins
        Assert.Equal(8_000, row.MaxHp); // HasMaxHp null, value 0 → carry
        Assert.Equal(12, row.Level);
    }

    [Fact]
    public void Bridge_LegacyCallerNoFlags_FallsBackToNonZeroRule()
    {
        // S111 callers don't set Has*. A zero with no flag means
        // "absent" under the legacy rule — must NOT override the row.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 5_000 },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[] { new NearDeltaAttrUpdate(7, 0, 0, 0) },
        });

        Assert.Equal(5_000, state.Snapshot.NearEntities[7].CurHp);
    }

    [Fact]
    public void Bridge_HasFlagFalse_ExplicitlyAbsent_CarriesForward()
    {
        // Decoder-emitted updates set HasCurHp=false when the attr was
        // absent. Bridge must treat that the same as legacy "0 + null".
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    CurHp = 5_000, MaxHp = 8_000, Level = 12,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, CurHp: 0, MaxHp: 9_000, Level: 0)
                {
                    HasCurHp = false,
                    HasMaxHp = true,
                    HasLevel = false,
                },
            },
        });

        var row = state.Snapshot.NearEntities[7];
        Assert.Equal(5_000, row.CurHp);
        Assert.Equal(9_000, row.MaxHp);
        Assert.Equal(12, row.Level);
    }

    [Fact]
    public void Decoder_To_Bridge_RealZero_EndToEnd()
    {
        // Wire-up: synth a SyncNearDeltaInfo with a real HP=0 attr,
        // dispatch it through the registry, hand the event to the
        // bridge, and verify the death is observed.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 5_000 },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7, A(11310, 0)));
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 2.0,
            e => bridge.Apply(e));

        Assert.Equal(0, state.Snapshot.NearEntities[7].CurHp);
    }
}
