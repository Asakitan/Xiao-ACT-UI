using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using Google.Protobuf;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.App.Startup;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.App;

/// <summary>
/// S132 — packet lifecycle wiring. Pins the App-layer live packet host:
/// best-effort enable/disable, source-factory failures swallowed, and
/// end-to-end `IPacketSource -> TcpReassembler -> PacketParser ->
/// PacketBridge -> GameState` flow for a registry-only notify method.
/// </summary>
public class Session132PacketLifecycleTests
{
    [Fact]
    public void NullSettings_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            PacketLifecycle.Start(null!, new GameStateManager(), NullLogger.Instance, CancellationToken.None));
    }

    [Fact]
    public void DisabledBySettings_LifecycleInactive_WithoutInvokingFactory()
    {
        using var dir = new TempDir();
        var path = System.IO.Path.Combine(dir.Path, "settings.json");
        File.WriteAllText(path, """
        {
          "data_source": "vision",
          "mem_data_source": "memory",
          "data_source_map": {
            "hp": "vision",
            "level": "vision",
            "identity": "vision",
            "skills": "vision",
            "stamina": "vision"
          }
        }
        """);
        var settings = new SettingsManager(path);

        using var lifecycle = PacketLifecycle.Start(
            settings,
            new GameStateManager(),
            NullLogger.Instance,
            CancellationToken.None,
            sourceFactory: () => throw new InvalidOperationException("should not run"));

        Assert.False(lifecycle.IsActive);
        Assert.Null(lifecycle.DpsSnapshotProvider);
    }

    [Fact]
    public void SourceFactoryThrows_LifecycleInactive_NoThrow()
    {
        using var dir = new TempDir();
        var path = System.IO.Path.Combine(dir.Path, "settings.json");
        File.WriteAllText(path, """{ "data_source": "mixed" }""");
        var settings = new SettingsManager(path);

        using var lifecycle = PacketLifecycle.Start(
            settings,
            new GameStateManager(),
            NullLogger.Instance,
            CancellationToken.None,
            sourceFactory: () => throw new InvalidOperationException("boom"));

        Assert.False(lifecycle.IsActive);
    }

    [Fact]
    public void RuntimeProcessesRegistryOnlyNotifyIntoState()
    {
        using var dir = new TempDir();
        var path = System.IO.Path.Combine(dir.Path, "settings.json");
        File.WriteAllText(path, """{ "data_source": "mixed" }""");
        var settings = new SettingsManager(path);
        var states = new GameStateManager();

        var msg = new SyncContainerData
        {
            VData = new CharSerialize
            {
                CharId = 8888L,
                CharBase = new CharBaseInfo { Name = "Asuna", FightPoint = 99000 },
                RoleLevel = new RoleLevel { Level = 75 },
                Attr = new UserFightAttr { CurHp = 5000, MaxHp = 5000, OriginEnergy = 100f },
            },
        };
        var rawFrame = BuildEthIpTcpFrame(
            seq: 1000,
            payload: BuildNotifyGameFrame(NotifyMethod.SyncContainerData, msg.ToByteArray()));
        var source = new FakePacketSource(new[]
        {
            new RawFrame(DateTimeOffset.UtcNow, rawFrame),
        });

        using var lifecycle = PacketLifecycle.Start(
            settings,
            states,
            NullLogger.Instance,
            CancellationToken.None,
            sourceFactory: () => source);

        Assert.True(lifecycle.IsActive);
        Assert.NotNull(lifecycle.DpsSnapshotProvider);
        Assert.True(SpinWait.SpinUntil(() => states.Snapshot.PlayerName == "Asuna", TimeSpan.FromSeconds(1)));

        var snap = states.Snapshot;
        Assert.Equal("Asuna", snap.PlayerName);
        Assert.Equal(75, snap.LevelBase);
        Assert.Equal(99000, snap.FightPoint);
        Assert.Equal(5000, snap.HpCurrent);
        Assert.Equal(5000, snap.HpMax);
    }

    private static byte[] BuildNotifyGameFrame(int methodId, byte[] body)
    {
        var notifyPayload = new byte[16 + body.Length];
        BinaryPrimitives.WriteUInt64BigEndian(notifyPayload.AsSpan(0, 8), PacketCodes.ServiceUuidC3Sb);
        BinaryPrimitives.WriteUInt32BigEndian(notifyPayload.AsSpan(12, 4), (uint)methodId);
        Buffer.BlockCopy(body, 0, notifyPayload, 16, body.Length);

        var size = 4 + 2 + notifyPayload.Length;
        var frame = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)size);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), (ushort)MessageType.Notify);
        Buffer.BlockCopy(notifyPayload, 0, frame, 6, notifyPayload.Length);
        return frame;
    }

    private static byte[] BuildEthIpTcpFrame(uint seq, byte[] payload)
    {
        const int eth = 14;
        const int ipLen = 20;
        const int tcpLen = 20;
        var total = eth + ipLen + tcpLen + payload.Length;
        var frame = new byte[total];

        frame[12] = 0x08;
        frame[13] = 0x00;

        var ipStart = eth;
        frame[ipStart] = 0x45;
        var ipTotal = ipLen + tcpLen + payload.Length;
        frame[ipStart + 2] = (byte)(ipTotal >> 8);
        frame[ipStart + 3] = (byte)(ipTotal & 0xFF);
        frame[ipStart + 8] = 64;
        frame[ipStart + 9] = 6;
        frame[ipStart + 12] = 10; frame[ipStart + 13] = 0;
        frame[ipStart + 14] = 0;  frame[ipStart + 15] = 1;
        frame[ipStart + 16] = 10; frame[ipStart + 17] = 0;
        frame[ipStart + 18] = 0;  frame[ipStart + 19] = 2;

        var tcpStart = eth + ipLen;
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(tcpStart, 2), 443);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(tcpStart + 2, 2), 51000);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(tcpStart + 4, 4), seq);
        frame[tcpStart + 12] = 0x50;

        Buffer.BlockCopy(payload, 0, frame, eth + ipLen + tcpLen, payload.Length);
        return frame;
    }

    private sealed class FakePacketSource : IPacketSource
    {
        private readonly IReadOnlyList<RawFrame> _frames;

        public FakePacketSource(IReadOnlyList<RawFrame> frames)
        {
            _frames = frames;
        }

        public async IAsyncEnumerable<RawFrame> ReadAsync(
            [EnumeratorCancellation] CancellationToken cancellationToken = default)
        {
            foreach (var frame in _frames)
            {
                cancellationToken.ThrowIfCancellationRequested();
                yield return frame;
                await Task.Yield();
            }
        }

        public void Dispose()
        {
        }
    }

    private sealed class TempDir : IDisposable
    {
        public TempDir()
        {
            Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"saoauto-tests-{Guid.NewGuid():N}");
            Directory.CreateDirectory(Path);
        }

        public string Path { get; }

        public void Dispose()
        {
            try { Directory.Delete(Path, recursive: true); } catch { /* swallow */ }
        }
    }
}
