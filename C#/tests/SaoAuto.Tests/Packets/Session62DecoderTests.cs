using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// Per-decoder facts for the S62 batch:
/// EnterGame / NotifyBuffChange / SyncClientUseSkill /
/// SyncServerSkillEnd / SyncServerSkillStageEnd / QteBegin.
/// </summary>
public class Session62DecoderTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    [Fact]
    public void EnterGame_emitsEvent_whenUidPresent()
    {
        // Hand-rolled "field 1, varint, value=0x1234" payload: 0x08 then varint.
        // 0x1234 = 4660 → varint = 0xB4 0x24.
        var body = new byte[] { 0x08, 0xB4, 0x24 };
        EnterGameEvent? captured = null;
        Registry.Dispatch(NotifyMethod.EnterGame, body, 1.25,
            ev => { if (ev is EnterGameEvent e) captured = e; });
        Assert.NotNull(captured);
        Assert.Equal(0x1234UL, captured!.SelfUuid);
        Assert.Equal(1.25, captured.TimestampSeconds);
    }

    [Fact]
    public void EnterGame_skipsWhenUidZero()
    {
        // field 1, varint, value=0 → 0x08 0x00.
        var body = new byte[] { 0x08, 0x00 };
        var fired = false;
        Registry.Dispatch(NotifyMethod.EnterGame, body, 0.0, _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void EnterGame_skipsEmptyBody()
    {
        var fired = false;
        Registry.Dispatch(NotifyMethod.EnterGame, ReadOnlySpan<byte>.Empty, 0.0, _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void NotifyBuffChange_emitsOldNewIds()
    {
        var msg = new NotifyBuffChange { OldBuffId = 1001, NewBuffId = 2002 };
        BuffChangeEvent? captured = null;
        Registry.Dispatch(NotifyMethod.NotifyBuffChange, msg.ToByteArray(), 3.5,
            ev => { if (ev is BuffChangeEvent b) captured = b; });
        Assert.NotNull(captured);
        Assert.Equal(1001, captured!.OldBuffId);
        Assert.Equal(2002, captured.NewBuffId);
        Assert.Equal(3.5, captured.TimestampSeconds);
    }

    [Fact]
    public void SyncClientUseSkill_emitsTargetAndLevel()
    {
        var msg = new SyncClientUseSkill
        {
            SkillTargetUuid = 0x0123_4567_89AB_CDEFL,
            SkillLevelId = 50301,
        };
        SkillUseEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncClientUseSkill, msg.ToByteArray(), 4.0,
            ev => { if (ev is SkillUseEvent s) captured = s; });
        Assert.NotNull(captured);
        Assert.Equal(0x0123_4567_89AB_CDEFL, captured!.TargetUuid);
        Assert.Equal(50301, captured.SkillLevelId);
    }

    [Fact]
    public void SyncServerSkillEnd_emitsSkillUuid()
    {
        var msg = new SyncServerSkillEnd { SkillUuid = 9999 };
        SkillEndEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncServerSkillEnd, msg.ToByteArray(), 5.0,
            ev => { if (ev is SkillEndEvent s) captured = s; });
        Assert.NotNull(captured);
        Assert.Equal(9999, captured!.SkillUuid);
    }

    [Fact]
    public void SyncServerSkillStageEnd_unwrapsInnerInfo()
    {
        var msg = new SyncServerSkillStageEnd
        {
            SkillStageEndInfo = new ServerSkillStageEnd
            {
                SkillUuid = 4242,
                StageId = 1,
                NewStageId = 2,
                ConditionId = 7,
            },
        };
        SkillStageEndEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncServerSkillStageEnd, msg.ToByteArray(), 6.0,
            ev => { if (ev is SkillStageEndEvent s) captured = s; });
        Assert.NotNull(captured);
        Assert.Equal(4242, captured!.SkillUuid);
        Assert.Equal(1u, captured.StageId);
        Assert.Equal(2u, captured.NewStageId);
        Assert.Equal(7u, captured.ConditionId);
    }

    [Fact]
    public void SyncServerSkillStageEnd_skipsWhenInnerMissing()
    {
        var msg = new SyncServerSkillStageEnd();  // SkillStageEndInfo = null
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncServerSkillStageEnd, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void QteBegin_readsIdAndType()
    {
        // field 1 varint=10 (0x0A) + field 2 varint=3 → 0x08 0x0A 0x10 0x03.
        var body = new byte[] { 0x08, 0x0A, 0x10, 0x03 };
        QteBeginEvent? captured = null;
        Registry.Dispatch(NotifyMethod.QteBegin, body, 7.5,
            ev => { if (ev is QteBeginEvent q) captured = q; });
        Assert.NotNull(captured);
        Assert.Equal(10, captured!.QteId);
        Assert.Equal(3, captured.QteType);
    }

    [Fact]
    public void QteBegin_emptyBodyEmitsZeros()
    {
        QteBeginEvent? captured = null;
        Registry.Dispatch(NotifyMethod.QteBegin, ReadOnlySpan<byte>.Empty, 0.0,
            ev => { if (ev is QteBeginEvent q) captured = q; });
        Assert.NotNull(captured);
        Assert.Equal(0, captured!.QteId);
        Assert.Equal(0, captured.QteType);
    }

    [Fact]
    public void MalformedProtoSilentlyDrops()
    {
        var bogus = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF };
        var fired = false;
        Registry.Dispatch(NotifyMethod.NotifyBuffChange, bogus, 0.0, _ => fired = true);
        Assert.False(fired);
    }
}
