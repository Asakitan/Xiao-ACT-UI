using System.Net;
using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class TcpEndpointTests
{
    [Fact]
    public void KeyMatchesPythonHexFormat()
    {
        var ep = TcpEndpoint.FromIpv4Bytes(192, 168, 1, 1, 443);
        // Python: f'{src_ip.hex()}:{sport}' where src_ip is the 4 raw bytes
        Assert.Equal("c0a80101:443", ep.Key);
    }

    [Fact]
    public void DisplayProducesDottedQuad()
    {
        var ep = TcpEndpoint.FromIpv4Bytes(10, 0, 0, 1, 80);
        Assert.Equal("10.0.0.1:80", ep.Display);
    }

    [Fact]
    public void RoundTripsThroughIPEndPoint()
    {
        var ep = TcpEndpoint.FromIpv4Bytes(8, 8, 8, 8, 53);
        var ip = ep.ToIPEndPoint();
        Assert.Equal(IPAddress.Parse("8.8.8.8"), ip.Address);
        Assert.Equal(53, ip.Port);
    }

    [Fact]
    public void DistinctEndpointsAreNotEqual()
    {
        var a = TcpEndpoint.FromIpv4Bytes(1, 1, 1, 1, 80);
        var b = TcpEndpoint.FromIpv4Bytes(1, 1, 1, 1, 81);
        var c = TcpEndpoint.FromIpv4Bytes(1, 1, 1, 2, 80);
        Assert.NotEqual(a, b);
        Assert.NotEqual(a, c);
    }
}
