using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S107 — Pin <see cref="SyncToMeDeltaInfoDecoder"/>'s
/// <c>BaseDelta.Uuid</c> fallback path. Mirrors Python's
/// <c>_on_sync_to_me_delta</c> at packet_parser.py:3893–3902:
/// when the primary <c>DeltaInfo.Uuid</c> (proto field 5) is 0,
/// fall back to <c>BaseDelta.Uuid</c> only when it is present AND
/// passes the player-uuid low-marker check (<c>(u &amp; 0xFFFF)
/// == 640</c>). Otherwise leave the resolved uuid at 0 so
/// downstream consumers that filter by uuid behave identically
/// to the Python pipeline.
/// </summary>
public class Session107BaseDeltaUuidFallbackTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private const long PlayerUuid = (123L << 16) | 640;       // is_player → true
    private const long MonsterUuid = (123L << 16) | 64;        // is_player → false
    private const long OtherUuid = (123L << 16) | 0xABCD;      // arbitrary low marker

    private static ToMeDeltaEvent Dispatch(AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        ToMeDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts,
            ev => { if (ev is ToMeDeltaEvent t) captured = t; });
        Assert.NotNull(captured);
        return captured!;
    }

    [Fact]
    public void PrimaryUuidNonZero_BaseDeltaIgnored()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = 4242,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid },
        };
        var ev = Dispatch(inner);
        Assert.Equal(4242L, ev.Uuid);
    }

    [Fact]
    public void PrimaryUuidZero_BaseDeltaPresentWithPlayerUuid_FallsBack()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = 0,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid },
        };
        var ev = Dispatch(inner);
        Assert.Equal(PlayerUuid, ev.Uuid);
    }

    [Fact]
    public void PrimaryUuidZero_BaseDeltaWithMonsterUuid_DoesNotFallBack()
    {
        // SyncToMeDelta is always about self → fallback only when the
        // BaseDelta uuid actually looks like a player. Monster uuid
        // would be a packet-shape anomaly; stay at 0.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = 0,
            BaseDelta = new AoiSyncDelta { Uuid = MonsterUuid },
        };
        var ev = Dispatch(inner);
        Assert.Equal(0L, ev.Uuid);
    }

    [Fact]
    public void PrimaryUuidZero_BaseDeltaWithOtherUuid_DoesNotFallBack()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = 0,
            BaseDelta = new AoiSyncDelta { Uuid = OtherUuid },
        };
        var ev = Dispatch(inner);
        Assert.Equal(0L, ev.Uuid);
    }

    [Fact]
    public void PrimaryUuidZero_NoBaseDelta_StaysZero()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 0 };
        var ev = Dispatch(inner);
        Assert.Equal(0L, ev.Uuid);
    }

    [Fact]
    public void PrimaryUuidZero_BaseDeltaUuidZero_StaysZero()
    {
        var inner = new AoiSyncToMeDelta
        {
            Uuid = 0,
            BaseDelta = new AoiSyncDelta { Uuid = 0 },
        };
        var ev = Dispatch(inner);
        Assert.Equal(0L, ev.Uuid);
    }

    [Fact]
    public void FightResCdSibling_AlsoUsesResolvedUuid()
    {
        // Pin that the fallback resolution applies to BOTH events
        // emitted by the decoder, not just the primary one.
        var inner = new AoiSyncToMeDelta
        {
            Uuid = 0,
            BaseDelta = new AoiSyncDelta { Uuid = PlayerUuid },
        };
        inner.FightResCDs.Add(new FightResCD { ResId = 1 });
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        ToMeFightResCdEvent? sibling = null;
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 1.0,
            ev => { if (ev is ToMeFightResCdEvent f) sibling = f; });
        Assert.NotNull(sibling);
        Assert.Equal(PlayerUuid, sibling!.Uuid);
    }
}
