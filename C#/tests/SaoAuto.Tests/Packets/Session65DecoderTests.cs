using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S65 — alias decoder + 3 near-entity skeleton decoders. Decode-boundary
/// only; bridge wiring deferred. Tests verify the proto unwrap + emit
/// shape / drop conditions.
/// </summary>
public class Session65DecoderTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    // ── SyncClientUseSkillWorld alias ──

    [Fact]
    public void SyncClientUseSkillWorld_emitsSameShapeAsRegularUseSkill()
    {
        var msg = new SyncClientUseSkill
        {
            SkillTargetUuid = 0x0123_4567_89AB_CDEFL,
            SkillLevelId = 50301,
        };
        SkillUseEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncClientUseSkillWorld, msg.ToByteArray(), 5.0,
            ev => { if (ev is SkillUseEvent s) captured = s; });
        Assert.NotNull(captured);
        Assert.Equal(0x0123_4567_89AB_CDEFL, captured!.TargetUuid);
        Assert.Equal(50301, captured.SkillLevelId);
        Assert.Equal(5.0, captured.TimestampSeconds);
    }

    [Fact]
    public void SyncClientUseSkillWorld_isRegisteredUnderItsOwnId()
    {
        Assert.True(Registry.TryGet(NotifyMethod.SyncClientUseSkillWorld, out var d));
        Assert.NotNull(d);
        Assert.Equal(NotifyMethod.SyncClientUseSkillWorld, d.MethodId);
    }

    // ── SyncNearEntities ──

    [Fact]
    public void SyncNearEntities_passesAppearAndDisappearLists()
    {
        var msg = new SyncNearEntities();
        msg.Appear.Add(new Entity { Uuid = 100, EntType = EEntityType.Entchar });
        msg.Appear.Add(new Entity { Uuid = 200, EntType = EEntityType.Entmonster });
        msg.Disappear.Add(new DisappearEntity { Uuid = 999, Type = EDisappearType.Edisappeardead });
        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            ev => { if (ev is NearEntitiesEvent n) captured = n; });
        Assert.NotNull(captured);
        Assert.Equal(2, captured!.Appear.Count);
        var disappeared = Assert.Single(captured.Disappear);
        Assert.Contains(captured.Appear, a => a.Uuid == 100 && a.EntityType == (int)EEntityType.Entchar);
        Assert.Contains(captured.Appear, a => a.Uuid == 200 && a.EntityType == (int)EEntityType.Entmonster);
        Assert.Equal(999, disappeared.Uuid);
    }

    [Fact]
    public void SyncNearEntities_dropsWhenBothListsEmpty()
    {
        var msg = new SyncNearEntities();
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    // ── SyncNearDeltaInfo ──

    [Fact]
    public void SyncNearDeltaInfo_collectsDeltaUuids()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(new AoiSyncDelta { Uuid = 11 });
        msg.DeltaInfos.Add(new AoiSyncDelta { Uuid = 22 });
        msg.DeltaInfos.Add(new AoiSyncDelta { Uuid = 33 });
        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 0.5,
            ev => { if (ev is NearDeltaEvent n) captured = n; });
        Assert.NotNull(captured);
        Assert.Equal(new long[] { 11, 22, 33 }, captured!.DeltaUuids);
    }

    [Fact]
    public void SyncNearDeltaInfo_dropsWhenEmpty()
    {
        var msg = new SyncNearDeltaInfo();
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    // ── SyncToMeDeltaInfo ──

    [Fact]
    public void SyncToMeDeltaInfo_emitsUuidAndCounts()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 4242 };
        inner.SyncHateIds.Add(1);
        inner.SyncHateIds.Add(2);
        inner.SyncSkillCDs.Add(new SkillCDInfo());
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        ToMeDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 2.0,
            ev => { if (ev is ToMeDeltaEvent t) captured = t; });
        Assert.NotNull(captured);
        Assert.Equal(4242, captured!.Uuid);
        Assert.Equal(2, captured.HateIdCount);
        Assert.Single(captured.SkillCds);
    }

    [Fact]
    public void SyncToMeDeltaInfo_dropsWhenDeltaInfoMissing()
    {
        var msg = new SyncToMeDeltaInfo();
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }
}
