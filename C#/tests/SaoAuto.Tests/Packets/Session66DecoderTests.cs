using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S66 — SyncContainerData skeleton decoder. Top-level identity + HP
/// only; deeper attribute table mutation deferred. Tests verify proto
/// unwrap + drop conditions.
/// </summary>
public class Session66DecoderTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private static SyncContainerData BuildMessage(
        long charId = 1234567890L,
        string name = "Asuna",
        int level = 50,
        int fightPoint = 12345,
        long curHp = 4200,
        long maxHp = 8800,
        float energy = 75f)
    {
        return new SyncContainerData
        {
            VData = new CharSerialize
            {
                CharId = charId,
                CharBase = new CharBaseInfo { Name = name, FightPoint = fightPoint },
                Attr = new UserFightAttr { CurHp = curHp, MaxHp = maxHp, OriginEnergy = energy },
                RoleLevel = new RoleLevel { Level = level },
            },
        };
    }

    [Fact]
    public void SyncContainerData_emitsAllTopLevelFields()
    {
        var msg = BuildMessage();
        ContainerSyncEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncContainerData, msg.ToByteArray(), 9.0,
            ev => { if (ev is ContainerSyncEvent c) captured = c; });
        Assert.NotNull(captured);
        Assert.Equal(1234567890L, captured!.CharId);
        Assert.Equal("Asuna", captured.Name);
        Assert.Equal(50, captured.Level);
        Assert.Equal(12345, captured.FightPoint);
        Assert.Equal(4200L, captured.CurHp);
        Assert.Equal(8800L, captured.MaxHp);
        Assert.Equal(75f, captured.Energy);
        Assert.Equal(9.0, captured.TimestampSeconds);
    }

    [Fact]
    public void SyncContainerData_dropsWhenVDataMissing()
    {
        var msg = new SyncContainerData();
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncContainerData, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void SyncContainerData_dropsWhenCharIdNonPositive()
    {
        var msg = BuildMessage(charId: 0);
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncContainerData, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void SyncContainerData_handlesMissingCharBaseAndAttr()
    {
        var msg = new SyncContainerData
        {
            VData = new CharSerialize { CharId = 42L },
        };
        ContainerSyncEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncContainerData, msg.ToByteArray(), 1.0,
            ev => { if (ev is ContainerSyncEvent c) captured = c; });
        Assert.NotNull(captured);
        Assert.Equal(42L, captured!.CharId);
        Assert.Equal(string.Empty, captured.Name);
        Assert.Equal(0, captured.Level);
        Assert.Equal(0, captured.FightPoint);
        Assert.Equal(0L, captured.CurHp);
        Assert.Equal(0L, captured.MaxHp);
    }

    [Fact]
    public void SyncContainerData_malformedBodyDropsCleanly()
    {
        var bogus = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF };
        var fired = false;
        var ok = Registry.Dispatch(NotifyMethod.SyncContainerData, bogus, 0.0,
            _ => fired = true);
        Assert.True(ok);
        Assert.False(fired);
    }

    [Fact]
    public void SyncContainerData_isRegisteredUnderItsId()
    {
        Assert.True(Registry.TryGet(NotifyMethod.SyncContainerData, out var d));
        Assert.NotNull(d);
        Assert.Equal(NotifyMethod.SyncContainerData, d.MethodId);
    }
}
