using System.Buffers.Binary;
using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class FrameSignatureScannerTests
{
    [Fact]
    public void ScanC3SbNestedFindsSignatureAtCorrectOffset()
    {
        // One nested frame: [4B BE size][payload]; payload contains c3SB at +5..10
        // size includes the 4B header.
        var inner = new byte[16];
        // bytes 5..10 of the payload must be: 00 63 33 53 42 00
        inner[5] = 0x00;
        inner[6] = 0x63;
        inner[7] = 0x33;
        inner[8] = 0x53;
        inner[9] = 0x42;
        inner[10] = 0x00;

        var size = (uint)(4 + inner.Length); // 20
        var frame = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), size);
        Buffer.BlockCopy(inner, 0, frame, 4, inner.Length);

        Assert.True(FrameSignatureScanner.ScanC3SbNested(frame));
    }

    [Fact]
    public void ScanC3SbNestedRejectsRandomBytes()
    {
        var random = Enumerable.Range(0, 64).Select(i => (byte)(i * 7)).ToArray();
        Assert.False(FrameSignatureScanner.ScanC3SbNested(random));
    }

    [Fact]
    public void ScanC3SbNestedShortCircuitsOnInvalidFrameLength()
    {
        // 4B BE size = 1 (below MinValidFrameLength) → bail
        var data = new byte[] { 0x00, 0x00, 0x00, 0x01, 0xFF, 0xFF };
        Assert.False(FrameSignatureScanner.ScanC3SbNested(data));
    }

    [Fact]
    public void FindFrameRealignReturnsOffsetForValidHeader()
    {
        // 8 byte garbage prefix, then a valid header: size=10, type=2
        var data = new byte[8 + 10];
        for (var i = 0; i < 8; i++) data[i] = (byte)(i + 0x10);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(8, 4), 10);
        BinaryPrimitives.WriteUInt16BigEndian(data.AsSpan(12, 2), 2);

        Assert.Equal(8, FrameSignatureScanner.FindFrameRealign(data));
    }

    [Fact]
    public void FindFrameRealignReturnsMinusOneWhenNothingMatches()
    {
        var data = new byte[1024];
        for (var i = 0; i < data.Length; i++) data[i] = 0xFF;
        Assert.Equal(-1, FrameSignatureScanner.FindFrameRealign(data));
    }

    [Fact]
    public void FindFrameRealignSkipsOffsetZeroEvenIfValid()
    {
        // Mirrors Python which starts the scan at i=1 — offset 0 is what
        // the corrupt header is, by definition.
        var data = new byte[12];
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(0, 4), 10);
        BinaryPrimitives.WriteUInt16BigEndian(data.AsSpan(4, 2), 2);
        Assert.Equal(-1, FrameSignatureScanner.FindFrameRealign(data));
    }
}
