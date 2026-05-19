using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class IpFragmentCacheTests
{
    [Fact]
    public void TwoOrderedFragmentsReassembleIntoFullPayload()
    {
        var cache = new IpFragmentCache();
        var first = Enumerable.Range(0, 16).Select(i => (byte)i).ToArray();
        var second = Enumerable.Range(16, 16).Select(i => (byte)i).ToArray();

        // First fragment: offset=0, MF=true
        var midResult = cache.Feed(ipId: 1, srcIp: 0x0A000001, dstIp: 0x0A000002,
            protocol: 6, fragmentOffset: 0, moreFragments: true, payload: first);
        Assert.Null(midResult);

        // Second fragment: offset=2 (i.e. 16 bytes / 8), MF=false
        var done = cache.Feed(ipId: 1, srcIp: 0x0A000001, dstIp: 0x0A000002,
            protocol: 6, fragmentOffset: 2, moreFragments: false, payload: second);

        Assert.NotNull(done);
        var expected = first.Concat(second).ToArray();
        Assert.Equal(expected, done);
        Assert.Equal(0, cache.PendingCount);
    }

    [Fact]
    public void OutOfOrderFragmentsStillReassemble()
    {
        var cache = new IpFragmentCache();
        var first = new byte[8];  for (var i = 0; i < 8; i++) first[i] = (byte)(0xA0 + i);
        var second = new byte[8]; for (var i = 0; i < 8; i++) second[i] = (byte)(0xB0 + i);

        // Last fragment first (offset=1, MF=false), then offset=0 with MF=true.
        var first1 = cache.Feed(ipId: 9, srcIp: 1, dstIp: 2,
            protocol: 6, fragmentOffset: 1, moreFragments: false, payload: second);
        Assert.Null(first1);

        var done = cache.Feed(ipId: 9, srcIp: 1, dstIp: 2,
            protocol: 6, fragmentOffset: 0, moreFragments: true, payload: first);

        Assert.NotNull(done);
        Assert.Equal(first.Concat(second).ToArray(), done);
    }

    [Fact]
    public void ExpiresFragmentsAfterTimeout()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var cache = new IpFragmentCache(() => clock.Now);

        var partial = new byte[8];
        cache.Feed(ipId: 5, srcIp: 1, dstIp: 2, protocol: 6,
            fragmentOffset: 0, moreFragments: true, payload: partial);
        Assert.Equal(1, cache.PendingCount);

        // Advance > 30s and feed an unrelated fragment → expiry sweep.
        clock.Advance(TimeSpan.FromSeconds(31));
        cache.Feed(ipId: 6, srcIp: 1, dstIp: 2, protocol: 6,
            fragmentOffset: 0, moreFragments: true, payload: partial);

        Assert.Equal(1, cache.PendingCount); // only the new one survives
    }

    [Fact]
    public void DistinctIpIdsRouteToSeparateBuckets()
    {
        var cache = new IpFragmentCache();
        var data = new byte[8];
        cache.Feed(ipId: 1, srcIp: 1, dstIp: 2, protocol: 6, fragmentOffset: 0, moreFragments: true, payload: data);
        cache.Feed(ipId: 2, srcIp: 1, dstIp: 2, protocol: 6, fragmentOffset: 0, moreFragments: true, payload: data);
        Assert.Equal(2, cache.PendingCount);
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; }
        public TestClock(DateTimeOffset start) => Now = start;
        public void Advance(TimeSpan delta) => Now = Now.Add(delta);
    }
}
