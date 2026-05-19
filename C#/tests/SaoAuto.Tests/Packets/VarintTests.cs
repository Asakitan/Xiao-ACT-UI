using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class VarintTests
{
    [Theory]
    [InlineData(new byte[] { 0x00 }, 0UL, 1)]
    [InlineData(new byte[] { 0x01 }, 1UL, 1)]
    [InlineData(new byte[] { 0x7F }, 127UL, 1)]
    [InlineData(new byte[] { 0x80, 0x01 }, 128UL, 2)]
    [InlineData(new byte[] { 0xAC, 0x02 }, 300UL, 2)]
    [InlineData(new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, 0xFFFFFFFFUL, 5)]
    public void ReadUInt64MatchesProtobufWireFormat(byte[] data, ulong expected, int expectedBytes)
    {
        var actual = Varint.ReadUInt64(data, out var bytesRead);
        Assert.Equal(expected, actual);
        Assert.Equal(expectedBytes, bytesRead);
    }

    [Fact]
    public void ReadUInt64HandlesAllOnesTenByteEncoding()
    {
        var data = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
        var actual = Varint.ReadUInt64(data, out var bytesRead);
        Assert.Equal(ulong.MaxValue, actual);
        Assert.Equal(10, bytesRead);
    }

    [Fact]
    public void ReadInt32MasksOverWideToLow32BitsBeforeSignExtension()
    {
        // Live AOI streams emit -1 as the 10-byte all-ones varint encoding of UInt64.MaxValue.
        // Python's varint_to_int32 masks to 0xFFFFFFFF first → -1 (0xFFFFFFFF reinterpreted).
        var data = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
        var value = Varint.ReadInt32(data, out _);
        Assert.Equal(-1, value);
    }

    [Fact]
    public void ToInt32MatchesPythonOverWideMaskRule()
    {
        // 0x1_0000_0001 → low 32 bits is 1 → +1 (NOT a wrap-around to a large negative)
        Assert.Equal(1, Varint.ToInt32(0x1_0000_0001UL));
        // 0xFFFFFFFF (low) → -1
        Assert.Equal(-1, Varint.ToInt32(0xFFFFFFFFUL));
        // 0x7FFFFFFF (low) → +0x7FFFFFFF
        Assert.Equal(0x7FFFFFFF, Varint.ToInt32(0x7FFFFFFFUL));
        // 0x80000000 (low) → -0x80000000 (Int32.MinValue)
        Assert.Equal(int.MinValue, Varint.ToInt32(0x80000000UL));
    }

    [Fact]
    public void ToInt64ReturnsTwosComplement()
    {
        Assert.Equal(-1L, Varint.ToInt64(ulong.MaxValue));
        Assert.Equal(0L, Varint.ToInt64(0));
        Assert.Equal(long.MaxValue, Varint.ToInt64(0x7FFFFFFFFFFFFFFFUL));
        Assert.Equal(long.MinValue, Varint.ToInt64(0x8000000000000000UL));
    }

    [Fact]
    public void DecodeStringRespectsLengthPrefix()
    {
        // [0x05] [u t f - 8] but the test ASCII case suffices: [0x05] [h e l l o]
        var data = new byte[] { 0x05, (byte)'h', (byte)'e', (byte)'l', (byte)'l', (byte)'o' };
        Assert.Equal("hello", Varint.DecodeString(data));
    }

    [Fact]
    public void DecodeStringFallsBackToFullBufferOnInvalidLength()
    {
        // The leading 0xFF ... lengths past the buffer end → fall back to whole-buffer UTF-8.
        var data = new byte[] { 0xFF, (byte)'a', (byte)'b' };
        var result = Varint.DecodeString(data);
        Assert.Contains("ab", result);
    }

    [Fact]
    public void DecodeStringHandlesUtf8Cjk()
    {
        // "咲" UTF-8 = 0xE5 0x92 0xB2 (3 bytes), prefixed by length 0x03
        var data = new byte[] { 0x03, 0xE5, 0x92, 0xB2 };
        Assert.Equal("咲", Varint.DecodeString(data));
    }

    [Fact]
    public void DecodeStringEmptyBufferReturnsEmpty()
    {
        Assert.Equal(string.Empty, Varint.DecodeString(ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void DecodeStringSingleZeroByteFallsBackToWholeBuffer()
    {
        // Matches Python: a leading 0x00 length means "len > 0 and end <= length" is false,
        // so we fall through to bytes(raw).decode('utf-8') which yields "\0".
        Assert.Equal("\0", Varint.DecodeString(new byte[] { 0x00 }));
    }
}
