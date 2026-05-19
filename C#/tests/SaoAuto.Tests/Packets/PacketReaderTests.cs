using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class PacketReaderTests
{
    [Fact]
    public void ReadBigEndianU16RoundTrips()
    {
        var data = new byte[] { 0x01, 0x02, 0x03, 0x04 };
        Assert.Equal(0x0102, PacketReader.ReadBigEndianU16(data, 0));
        Assert.Equal(0x0304, PacketReader.ReadBigEndianU16(data, 2));
    }

    [Fact]
    public void ReadBigEndianU32MatchesGameFrameLengthPrefix()
    {
        var data = new byte[] { 0x00, 0x00, 0x01, 0x00 };
        Assert.Equal(256u, PacketReader.ReadBigEndianU32(data, 0));
    }

    [Fact]
    public void ReadLittleEndianU32MatchesAoiDirtyEnergyU32()
    {
        // 1234.0 packed LE32 (just an integer here)
        var data = new byte[] { 0xD2, 0x04, 0x00, 0x00 };
        Assert.Equal(1234u, PacketReader.ReadLittleEndianU32(data, 0));
    }

    [Fact]
    public void ReadLittleEndianF32RoundTripsFiniteValues()
    {
        var bytes = BitConverter.GetBytes(0.5f);
        Assert.Equal(0.5f, PacketReader.ReadLittleEndianF32(bytes, 0));
    }

    [Fact]
    public void ReadLittleEndianF64RoundTripsFiniteValues()
    {
        var bytes = BitConverter.GetBytes(3.14159);
        Assert.Equal(3.14159, PacketReader.ReadLittleEndianF64(bytes, 0));
    }

    [Fact]
    public void UnderflowThrowsArgumentException()
    {
        var data = new byte[] { 0x00, 0x01 };
        Assert.Throws<ArgumentException>(() => PacketReader.ReadBigEndianU32(data, 0));
        Assert.Throws<ArgumentException>(() => PacketReader.ReadBigEndianU16(data, 1));
    }
}
