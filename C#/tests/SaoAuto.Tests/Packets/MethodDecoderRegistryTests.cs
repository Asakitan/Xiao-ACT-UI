using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

public class MethodDecoderRegistryTests
{
    [Fact]
    public void DefaultRegistryRegistersAllPortedMethods()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        Assert.Contains(NotifyMethod.SyncServerTime, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.NotifyReviveUser, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.NotifyClientKickOff, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.NotifyAllMemberReady, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.NotifyCaptainReady, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.NotifyStartPlayingDungeon, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.EnterScene, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.EnterMatchResultNtf, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.EnterGame, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.NotifyBuffChange, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncClientUseSkill, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncServerSkillEnd, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncServerSkillStageEnd, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.QteBegin, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncDungeonData, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncDungeonDirtyData, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncClientUseSkillWorld, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncNearEntities, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncNearDeltaInfo, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncToMeDeltaInfo, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncContainerData, reg.RegisteredMethodIds);
        Assert.Contains(NotifyMethod.SyncContainerDirtyData, reg.RegisteredMethodIds);
        Assert.True(reg.Count >= 22);
    }

    [Fact]
    public void EnterSceneAndMatchResultEmitMarkerEvents()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        EnterSceneEvent? scene = null;
        MatchResultEvent? match = null;
        Assert.True(reg.Dispatch(NotifyMethod.EnterScene, ReadOnlySpan<byte>.Empty, 1.0,
            ev => { if (ev is EnterSceneEvent e) scene = e; }));
        Assert.True(reg.Dispatch(NotifyMethod.EnterMatchResultNtf, ReadOnlySpan<byte>.Empty, 2.0,
            ev => { if (ev is MatchResultEvent m) match = m; }));
        Assert.NotNull(scene);
        Assert.Equal(1.0, scene!.TimestampSeconds);
        Assert.NotNull(match);
        Assert.Equal(2.0, match!.TimestampSeconds);
    }

    [Fact]
    public void DispatchReturnsFalseForUnregistered()
    {
        var reg = new MethodDecoderRegistry();
        var fired = false;
        var ok = reg.Dispatch(0xDEAD, ReadOnlySpan<byte>.Empty, 0.0, _ => fired = true);
        Assert.False(ok);
        Assert.False(fired);
    }

    [Fact]
    public void SyncServerTimeRoundTripsThroughDecoder()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        var msg = new SyncServerTime { ClientMilliseconds = 1_000, ServerMilliseconds = 1_500_000_000_000L };
        var body = msg.ToByteArray();

        ServerTimeEvent? captured = null;
        var ok = reg.Dispatch(NotifyMethod.SyncServerTime, body, 1.5, ev =>
        {
            if (ev is ServerTimeEvent ste) captured = ste;
        });

        Assert.True(ok);
        Assert.NotNull(captured);
        Assert.Equal(1_500_000_000_000UL, captured!.ServerTimeMs);
        Assert.Equal(1.5, captured.TimestampSeconds);
    }

    [Fact]
    public void SyncServerTimeWithBothZeroEmitsNothing()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        var body = new SyncServerTime().ToByteArray();
        var fired = false;
        reg.Dispatch(NotifyMethod.SyncServerTime, body, 0.0, _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void NotifyReviveUserParsesUuid()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        var msg = new NotifyReviveUser { VActorUuid = 0x0123_4567_89AB_CDEFL };
        ReviveEvent? captured = null;
        reg.Dispatch(NotifyMethod.NotifyReviveUser, msg.ToByteArray(), 7.5, ev =>
        {
            if (ev is ReviveEvent re) captured = re;
        });
        Assert.NotNull(captured);
        Assert.Equal(0x0123_4567_89AB_CDEFUL, captured!.Uuid);
        Assert.Equal(7.5, captured.TimestampSeconds);
    }

    [Fact]
    public void MarkerDecodersEmitMarkerEvents()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        ParserEvent? captured = null;
        reg.Dispatch(NotifyMethod.NotifyClientKickOff, ReadOnlySpan<byte>.Empty, 1.0, e => captured = e);
        Assert.IsType<KickOffEvent>(captured);

        captured = null;
        reg.Dispatch(NotifyMethod.NotifyAllMemberReady, ReadOnlySpan<byte>.Empty, 1.0, e => captured = e);
        Assert.IsType<AllMemberReadyEvent>(captured);

        captured = null;
        reg.Dispatch(NotifyMethod.NotifyCaptainReady, ReadOnlySpan<byte>.Empty, 1.0, e => captured = e);
        Assert.IsType<CaptainReadyEvent>(captured);
    }

    [Fact]
    public void DungeonStartReadsVarintFromPayload()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        // Hand-rolled "field 1, varint, value=42" payload: 0x08 0x2A.
        var body = new byte[] { 0x08, 0x2A };
        DungeonStartEvent? captured = null;
        reg.Dispatch(NotifyMethod.NotifyStartPlayingDungeon, body, 0.0, ev =>
        {
            if (ev is DungeonStartEvent de) captured = de;
        });
        Assert.NotNull(captured);
        Assert.Equal(42, captured!.DungeonId);
    }

    [Fact]
    public void DispatchSwallowsDecoderExceptions()
    {
        var reg = new MethodDecoderRegistry();
        reg.Register(new ThrowingDecoder());
        // Should NOT throw out of Dispatch.
        var ok = reg.Dispatch(0x1234, new byte[] { 1, 2 }, 0.0, _ => { });
        Assert.True(ok);
    }

    [Fact]
    public void MalformedProtoBodyReturnsCleanly()
    {
        var reg = MethodDecoderRegistry.BuildDefault();
        var fired = false;
        var bogus = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF };
        var ok = reg.Dispatch(NotifyMethod.SyncServerTime, bogus, 0.0, _ => fired = true);
        Assert.True(ok);
        Assert.False(fired);
    }

    private sealed class ThrowingDecoder : IMethodDecoder
    {
        public int MethodId => 0x1234;
        public void Decode(ReadOnlySpan<byte> body, double ts, Action<ParserEvent> emit)
            => throw new InvalidOperationException("boom");
    }
}
