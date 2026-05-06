using System.Net;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Read-only view of a TCP endpoint key used for server identification.
/// Matches Python's <c>addr = f'{src_ip.hex()}:{sport}'</c> where <c>src_ip</c>
/// is the IPv4 source address as raw bytes. The C# port keeps the IPv4 bytes
/// + port pair so we can trivially round-trip to <see cref="IPEndPoint"/>
/// without re-parsing strings.
/// </summary>
public readonly record struct TcpEndpoint(uint Ipv4, ushort Port)
{
    /// <summary>Construct from IPv4 octets in network byte order.</summary>
    public static TcpEndpoint FromIpv4Bytes(byte a, byte b, byte c, byte d, ushort port)
    {
        var packed = ((uint)a << 24) | ((uint)b << 16) | ((uint)c << 8) | d;
        return new TcpEndpoint(packed, port);
    }

    public byte[] ToIpv4Bytes() => new[]
    {
        (byte)(Ipv4 >> 24),
        (byte)(Ipv4 >> 16),
        (byte)(Ipv4 >> 8),
        (byte)Ipv4,
    };

    /// <summary>String key matching Python's <c>'{src_ip.hex()}:{sport}'</c>.</summary>
    public string Key => $"{Ipv4:x8}:{Port}";

    /// <summary>Human-readable form, e.g. <c>192.168.1.1:443</c>.</summary>
    public string Display =>
        $"{(byte)(Ipv4 >> 24)}.{(byte)(Ipv4 >> 16)}.{(byte)(Ipv4 >> 8)}.{(byte)Ipv4}:{Port}";

    public IPEndPoint ToIPEndPoint() =>
        new(new IPAddress(ToIpv4Bytes()), Port);
}
