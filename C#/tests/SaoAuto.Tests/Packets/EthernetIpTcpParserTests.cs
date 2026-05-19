using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class EthernetIpTcpParserTests
{
    [Fact]
    public void RejectsBuffersTooShortForFullStack()
    {
        Assert.Null(EthernetIpTcpParser.TryParse(new byte[20]));
        Assert.Null(EthernetIpTcpParser.TryParse(new byte[40]));
    }

    [Fact]
    public void RejectsNonIpv4EtherType()
    {
        var frame = new byte[60];
        // dst MAC, src MAC = 12 bytes already zero
        frame[12] = 0x86; frame[13] = 0xDD; // IPv6 ethertype
        Assert.Null(EthernetIpTcpParser.TryParse(frame));
    }

    [Fact]
    public void RejectsNonTcpProtocol()
    {
        var frame = BuildTcpFrame();
        // overwrite IP protocol byte with UDP
        frame[14 + 9] = 17;
        Assert.Null(EthernetIpTcpParser.TryParse(frame));
    }

    [Fact]
    public void DecodesMinimalSyntheticTcpFrame()
    {
        var frame = BuildTcpFrame();
        var parsed = EthernetIpTcpParser.TryParse(frame);
        Assert.NotNull(parsed);
        var f = parsed!.Value;

        // Source IP = 10.0.0.1, Dest IP = 10.0.0.2 from BuildTcpFrame
        Assert.Equal((10u << 24) | (0u << 16) | (0u << 8) | 1u, f.Source.Ipv4);
        Assert.Equal((10u << 24) | (0u << 16) | (0u << 8) | 2u, f.Destination.Ipv4);
        Assert.Equal((ushort)443, f.Source.Port);
        Assert.Equal((ushort)51000, f.Destination.Port);
        Assert.Equal(0x12345678u, f.TcpSequence);
        Assert.Equal(4, f.Payload.Length);
        Assert.Equal(new byte[] { 0xDE, 0xAD, 0xBE, 0xEF }, f.Payload);
    }

    [Fact]
    public void HandlesSingleVlanTag()
    {
        var frame = BuildTcpFrame(includeVlan: true);
        var parsed = EthernetIpTcpParser.TryParse(frame);
        Assert.NotNull(parsed);
        Assert.Equal(4, parsed!.Value.Payload.Length);
    }

    /// <summary>
    /// Build a minimal Ethernet+IPv4+TCP frame:
    /// src=10.0.0.1:443  dst=10.0.0.2:51000  payload=DE AD BE EF
    /// </summary>
    private static byte[] BuildTcpFrame(bool includeVlan = false)
    {
        var ethSize = includeVlan ? 18 : 14;
        var ipHeader = new byte[20];
        var tcpHeader = new byte[20];
        var payload = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF };
        var total = ethSize + ipHeader.Length + tcpHeader.Length + payload.Length;
        var buffer = new byte[total];

        // dst+src MAC = 12 bytes (zero is fine)
        if (includeVlan)
        {
            // VLAN ethertype 0x8100 then TCI then inner ethertype 0x0800
            buffer[12] = 0x81; buffer[13] = 0x00;
            buffer[14] = 0x00; buffer[15] = 0x01;
            buffer[16] = 0x08; buffer[17] = 0x00;
        }
        else
        {
            buffer[12] = 0x08; buffer[13] = 0x00;
        }

        var ipStart = ethSize;
        ipHeader[0] = 0x45; // IPv4, IHL=5
        ipHeader[2] = (byte)((ipHeader.Length + tcpHeader.Length + payload.Length) >> 8);
        ipHeader[3] = (byte)((ipHeader.Length + tcpHeader.Length + payload.Length) & 0xFF);
        ipHeader[8] = 64; // TTL
        ipHeader[9] = 6;  // TCP
        ipHeader[12] = 10; ipHeader[13] = 0; ipHeader[14] = 0; ipHeader[15] = 1; // src 10.0.0.1
        ipHeader[16] = 10; ipHeader[17] = 0; ipHeader[18] = 0; ipHeader[19] = 2; // dst 10.0.0.2
        Buffer.BlockCopy(ipHeader, 0, buffer, ipStart, ipHeader.Length);

        var tcpStart = ipStart + ipHeader.Length;
        tcpHeader[0] = 0x01; tcpHeader[1] = 0xBB; // src port 443
        tcpHeader[2] = 0xC7; tcpHeader[3] = 0x38; // dst port 51000
        tcpHeader[4] = 0x12; tcpHeader[5] = 0x34; tcpHeader[6] = 0x56; tcpHeader[7] = 0x78; // seq
        tcpHeader[12] = 0x50; // data offset = 5 (20 bytes)
        Buffer.BlockCopy(tcpHeader, 0, buffer, tcpStart, tcpHeader.Length);

        Buffer.BlockCopy(payload, 0, buffer, tcpStart + tcpHeader.Length, payload.Length);

        return buffer;
    }
}
