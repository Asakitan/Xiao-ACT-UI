using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S106 — Pin <see cref="SyncToMeDeltaInfoDecoder"/>'s sibling
/// <see cref="ToMeFightResCdEvent"/> emission. The decoder already
/// emits <see cref="ToMeDeltaEvent"/> with skill cooldowns; S106 adds
/// the parallel surface for <c>AoiSyncToMeDelta.FightResCDs</c>
/// (resource cooldowns) so consumers can track fight-resource
/// recharge windows without re-parsing the proto themselves.
///
/// Filter parity with Python <c>_on_sync_to_me_delta</c> at
/// packet_parser.py 3964: only rows with <c>ResId &gt; 0</c> count;
/// when the filtered list is empty no sibling event is emitted.
/// </summary>
public class Session106FightResCdTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private static (List<ParserEvent> all, ToMeFightResCdEvent? fres) Dispatch(
        AoiSyncToMeDelta inner, double ts = 1.0)
    {
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var events = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), ts, events.Add);
        ToMeFightResCdEvent? f = null;
        foreach (var e in events) if (e is ToMeFightResCdEvent x) f = x;
        return (events, f);
    }

    [Fact]
    public void NoFightResCds_NoSiblingEvent()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 100 };
        var (events, fres) = Dispatch(inner);
        Assert.Null(fres);
        // ToMeDeltaEvent (the original) still fires.
        Assert.Contains(events, e => e is ToMeDeltaEvent);
    }

    [Fact]
    public void OneFightResCd_EmitsSiblingWithUuidAndPayload()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 7777 };
        inner.FightResCDs.Add(new FightResCD
        {
            ResId = 50, BeginTime = 1_000, Duration = 3_000, ValidCDTime = 1_500,
        });
        var (_, fres) = Dispatch(inner, ts: 12.5);
        Assert.NotNull(fres);
        Assert.Equal(7777L, fres!.Uuid);
        Assert.Equal(12.5, fres.TimestampSeconds);
        Assert.Single(fres.FightResCds);
        var row = fres.FightResCds[0];
        Assert.Equal(50, row.ResId);
        Assert.Equal(1_000L, row.BeginMs);
        Assert.Equal(3_000, row.DurationMs);
        Assert.Equal(1_500, row.ValidCdTimeMs);
    }

    [Fact]
    public void ResIdZero_FilteredOut()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 1 };
        inner.FightResCDs.Add(new FightResCD { ResId = 0, BeginTime = 1, Duration = 2 });
        var (_, fres) = Dispatch(inner);
        Assert.Null(fres);
    }

    [Fact]
    public void ResIdNegative_FilteredOut()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 1 };
        inner.FightResCDs.Add(new FightResCD { ResId = -3, BeginTime = 1, Duration = 2 });
        var (_, fres) = Dispatch(inner);
        Assert.Null(fres);
    }

    [Fact]
    public void MixedRows_OnlyPositiveResIdsKept()
    {
        var inner = new AoiSyncToMeDelta { Uuid = 9 };
        inner.FightResCDs.Add(new FightResCD { ResId = 10 });
        inner.FightResCDs.Add(new FightResCD { ResId = 0 });   // dropped
        inner.FightResCDs.Add(new FightResCD { ResId = 20 });
        inner.FightResCDs.Add(new FightResCD { ResId = -1 });  // dropped
        var (_, fres) = Dispatch(inner);
        Assert.NotNull(fres);
        Assert.Equal(2, fres!.FightResCds.Count);
        Assert.Equal(10, fres.FightResCds[0].ResId);
        Assert.Equal(20, fres.FightResCds[1].ResId);
    }

    [Fact]
    public void SkillCdsAndFightResCds_BothCoexist()
    {
        // The two surfaces are independent — emitting fight-res cds
        // must NOT replace or absorb the existing skill-cd event.
        var inner = new AoiSyncToMeDelta { Uuid = 42 };
        inner.SyncSkillCDs.Add(new SkillCDInfo { SkillLevelId = 50301 });
        inner.FightResCDs.Add(new FightResCD { ResId = 99, BeginTime = 5 });
        var msg = new SyncToMeDeltaInfo { DeltaInfo = inner };
        var events = new List<ParserEvent>();
        Registry.Dispatch(NotifyMethod.SyncToMeDeltaInfo, msg.ToByteArray(), 1.0, events.Add);

        var skill = events.OfType<ToMeDeltaEvent>().Single();
        var fres = events.OfType<ToMeFightResCdEvent>().Single();
        Assert.Single(skill.SkillCds);
        Assert.Single(fres.FightResCds);
    }

    [Fact]
    public void EmissionOrder_MainEventBeforeSibling()
    {
        // Subscribers that flip on first ToMeDeltaEvent and then read
        // the sibling rely on this ordering. Pin it.
        var inner = new AoiSyncToMeDelta { Uuid = 5 };
        inner.FightResCDs.Add(new FightResCD { ResId = 1 });
        var (events, _) = Dispatch(inner);
        var idxMain = events.FindIndex(e => e is ToMeDeltaEvent);
        var idxSibling = events.FindIndex(e => e is ToMeFightResCdEvent);
        Assert.True(idxMain >= 0);
        Assert.True(idxSibling > idxMain);
    }
}
