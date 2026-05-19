using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// Per-decoder facts for the S63 batch:
/// SyncDungeonData / SyncDungeonDirtyData.
/// </summary>
public class Session63DecoderTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    [Fact]
    public void SyncDungeonData_passesThroughSceneAndDifficulty()
    {
        var msg = new SyncDungeonData
        {
            VData = new DungeonSyncData
            {
                SceneUuid = 0x0BAD_F00D_DEAD_BEEFL,
                DungeonSceneInfo = new DungeonSceneInfo { Difficulty = 3 },
            },
        };
        DungeonDataEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncDungeonData, msg.ToByteArray(), 1.0,
            ev => { if (ev is DungeonDataEvent d) captured = d; });
        Assert.NotNull(captured);
        Assert.Equal(0x0BAD_F00D_DEAD_BEEFL, captured!.SceneUuid);
        Assert.Equal(3, captured.DungeonDifficulty);
        Assert.Empty(captured.Targets);
    }

    [Fact]
    public void SyncDungeonData_collectsTargetMapEntries()
    {
        var dt = new DungeonTarget();
        dt.TargetData[1] = new DungeonTargetData { TargetId = 100, Nums = 5, Complete = 0 };
        dt.TargetData[2] = new DungeonTargetData { TargetId = 200, Nums = 10, Complete = 1 };
        var msg = new SyncDungeonData
        {
            VData = new DungeonSyncData
            {
                SceneUuid = 7001,
                Target = dt,
            },
        };
        DungeonDataEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncDungeonData, msg.ToByteArray(), 2.5,
            ev => { if (ev is DungeonDataEvent d) captured = d; });
        Assert.NotNull(captured);
        Assert.Equal(2, captured!.Targets.Count);
        Assert.Contains(captured.Targets, t => t.TargetId == 100 && t.Nums == 5 && t.Complete == 0);
        Assert.Contains(captured.Targets, t => t.TargetId == 200 && t.Nums == 10 && t.Complete == 1);
    }

    [Fact]
    public void SyncDungeonData_dropsWhenVDataMissing()
    {
        var msg = new SyncDungeonData();  // VData unset
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncDungeonData, msg.ToByteArray(), 0.0, _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void SyncDungeonData_difficultyZeroWhenInfoMissing()
    {
        var msg = new SyncDungeonData
        {
            VData = new DungeonSyncData { SceneUuid = 1 },
        };
        DungeonDataEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncDungeonData, msg.ToByteArray(), 0.0,
            ev => { if (ev is DungeonDataEvent d) captured = d; });
        Assert.NotNull(captured);
        Assert.Equal(0, captured!.DungeonDifficulty);
    }

    [Fact]
    public void SyncDungeonDirtyData_dropsEmptyBuffer()
    {
        var msg = new SyncDungeonDirtyData
        {
            VData = new BufferStream { Buffer = ByteString.Empty },
        };
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncDungeonDirtyData, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void SyncDungeonDirtyData_dropsWhenVDataMissing()
    {
        var msg = new SyncDungeonDirtyData();
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncDungeonDirtyData, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void SyncDungeonDirtyData_swallowsBadInnerBuffer()
    {
        // Buffer that's non-empty but wrong shape — ParseDungeonDirtyBuffer
        // throws InvalidDataException; decoder must swallow.
        var msg = new SyncDungeonDirtyData
        {
            VData = new BufferStream { Buffer = ByteString.CopyFrom(new byte[] { 0xAA, 0xBB, 0xCC, 0xDD }) },
        };
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncDungeonDirtyData, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }
}
