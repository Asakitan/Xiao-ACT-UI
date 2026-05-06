using System.Buffers.Binary;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Big-endian / little-endian fixed-width readers ported from
/// <c>_sao_cy_packet.pyx</c> (<c>_read_be16</c>, <c>_read_be32</c>,
/// <c>_read_le32</c>, <c>_read_le64</c>). Game framing is big-endian
/// (<c>[4B-size][payload]</c>), while energy / float dirty values inside
/// AOI deltas are LE32 / LE64. Helpers throw <see cref="ArgumentException"/>
/// on underflow so framing bugs surface loudly during fuzzing.
/// </summary>
public static class PacketReader
{
    public static ushort ReadBigEndianU16(ReadOnlySpan<byte> data, int offset)
    {
        EnsureLength(data, offset, 2);
        return BinaryPrimitives.ReadUInt16BigEndian(data.Slice(offset, 2));
    }

    public static uint ReadBigEndianU32(ReadOnlySpan<byte> data, int offset)
    {
        EnsureLength(data, offset, 4);
        return BinaryPrimitives.ReadUInt32BigEndian(data.Slice(offset, 4));
    }

    public static uint ReadLittleEndianU32(ReadOnlySpan<byte> data, int offset)
    {
        EnsureLength(data, offset, 4);
        return BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(offset, 4));
    }

    public static ulong ReadLittleEndianU64(ReadOnlySpan<byte> data, int offset)
    {
        EnsureLength(data, offset, 8);
        return BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(offset, 8));
    }

    public static float ReadLittleEndianF32(ReadOnlySpan<byte> data, int offset)
    {
        EnsureLength(data, offset, 4);
        return BinaryPrimitives.ReadSingleLittleEndian(data.Slice(offset, 4));
    }

    public static double ReadLittleEndianF64(ReadOnlySpan<byte> data, int offset)
    {
        EnsureLength(data, offset, 8);
        return BinaryPrimitives.ReadDoubleLittleEndian(data.Slice(offset, 8));
    }

    private static void EnsureLength(ReadOnlySpan<byte> data, int offset, int needed)
    {
        if (offset < 0 || offset + needed > data.Length)
        {
            throw new ArgumentException(
                $"PacketReader underflow: need {needed} byte(s) at offset {offset}, buffer length {data.Length}");
        }
    }
}
