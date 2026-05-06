namespace SaoAuto.Core.Packets;

/// <summary>
/// Minimal Ethernet → IPv4 → TCP header decoder, ported from
/// <c>packet_capture.parse_eth_ip_tcp</c> (which delegates to
/// <c>_sao_cy_packet.parse_eth_ip_tcp</c>). Only TCP-over-IPv4 is recognized;
/// other protocols return <c>null</c>.
/// </summary>
public static class EthernetIpTcpParser
{
    private const int EthernetHeaderLength = 14;
    private const ushort EtherTypeIpv4 = 0x0800;
    private const ushort EtherTypeVlan = 0x8100;
    private const byte ProtocolTcp = 6;

    public static EthIpTcpFrame? TryParse(ReadOnlySpan<byte> raw)
    {
        if (raw.Length < EthernetHeaderLength + 20 + 20) return null;

        var pos = 12;
        var etherType = (ushort)((raw[pos] << 8) | raw[pos + 1]);
        pos += 2;

        // Single 802.1Q VLAN tag — accept once, skip 4 bytes.
        if (etherType == EtherTypeVlan)
        {
            if (raw.Length < pos + 4) return null;
            etherType = (ushort)((raw[pos + 2] << 8) | raw[pos + 3]);
            pos += 4;
        }

        if (etherType != EtherTypeIpv4) return null;

        var ipStart = pos;
        if (raw.Length < ipStart + 20) return null;

        var versionIhl = raw[ipStart];
        var version = versionIhl >> 4;
        var ihl = versionIhl & 0x0F;
        if (version != 4 || ihl < 5) return null;

        var ipHeaderLength = ihl * 4;
        var totalLength = (raw[ipStart + 2] << 8) | raw[ipStart + 3];
        if (totalLength < ipHeaderLength) return null;
        if (raw.Length < ipStart + totalLength) return null;

        var ipId = (ushort)((raw[ipStart + 4] << 8) | raw[ipStart + 5]);
        var flagsFragment = (raw[ipStart + 6] << 8) | raw[ipStart + 7];
        var moreFragments = (flagsFragment & 0x2000) != 0;
        var fragmentOffset = flagsFragment & 0x1FFF;

        var protocol = raw[ipStart + 9];
        if (protocol != ProtocolTcp) return null;

        var srcIp = PacketReader.ReadBigEndianU32(raw, ipStart + 12);
        var dstIp = PacketReader.ReadBigEndianU32(raw, ipStart + 16);

        var tcpStart = ipStart + ipHeaderLength;
        if (raw.Length < tcpStart + 20) return null;

        var srcPort = PacketReader.ReadBigEndianU16(raw, tcpStart);
        var dstPort = PacketReader.ReadBigEndianU16(raw, tcpStart + 2);
        var seq = PacketReader.ReadBigEndianU32(raw, tcpStart + 4);
        var dataOffsetWords = (raw[tcpStart + 12] >> 4) & 0x0F;
        var tcpHeaderLength = dataOffsetWords * 4;
        if (tcpHeaderLength < 20) return null;

        var payloadStart = tcpStart + tcpHeaderLength;
        var payloadEnd = ipStart + totalLength;
        if (payloadStart > payloadEnd || payloadEnd > raw.Length) return null;

        return new EthIpTcpFrame(
            new TcpEndpoint(srcIp, srcPort),
            new TcpEndpoint(dstIp, dstPort),
            seq,
            ipId,
            (ushort)fragmentOffset,
            moreFragments,
            raw.Slice(payloadStart, payloadEnd - payloadStart).ToArray());
    }
}

/// <summary>Decoded Ethernet/IPv4/TCP frame — payload is a defensive copy.</summary>
public readonly record struct EthIpTcpFrame(
    TcpEndpoint Source,
    TcpEndpoint Destination,
    uint TcpSequence,
    ushort IpId,
    ushort FragmentOffset,
    bool MoreFragments,
    byte[] Payload);
