using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class FixturePacketSourceTests
{
    [Fact]
    public async Task RoundTripsFramesThroughInMemoryStream()
    {
        var frame1 = new RawFrame(DateTimeOffset.FromUnixTimeMilliseconds(1_700_000_000_000), new byte[] { 0xAA, 0xBB });
        var frame2 = new RawFrame(DateTimeOffset.FromUnixTimeMilliseconds(1_700_000_000_500), new byte[] { 0x01, 0x02, 0x03 });

        using var memory = new MemoryStream();
        FixturePacketSource.WriteFixture(memory, new[] { frame1, frame2 });
        memory.Position = 0;

        using var source = new FixturePacketSource(memory, ownsStream: false);
        var collected = new List<RawFrame>();
        await foreach (var frame in source.ReadAsync())
        {
            collected.Add(frame);
        }

        Assert.Equal(2, collected.Count);
        Assert.Equal(frame1.Timestamp, collected[0].Timestamp);
        Assert.Equal(frame1.Bytes.ToArray(), collected[0].Bytes.ToArray());
        Assert.Equal(frame2.Timestamp, collected[1].Timestamp);
        Assert.Equal(frame2.Bytes.ToArray(), collected[1].Bytes.ToArray());
    }

    [Fact]
    public async Task ThrowsOnBadMagic()
    {
        using var memory = new MemoryStream(new byte[] { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x01 });
        using var source = new FixturePacketSource(memory, ownsStream: false);

        await Assert.ThrowsAsync<InvalidDataException>(async () =>
        {
            await foreach (var _ in source.ReadAsync()) { }
        });
    }

    [Fact]
    public async Task EmptyFixtureYieldsZeroFrames()
    {
        using var memory = new MemoryStream();
        FixturePacketSource.WriteFixture(memory, Array.Empty<RawFrame>());
        memory.Position = 0;

        using var source = new FixturePacketSource(memory, ownsStream: false);
        var count = 0;
        await foreach (var _ in source.ReadAsync()) count++;
        Assert.Equal(0, count);
    }
}
