using SaoAuto.MemProbe;

namespace SaoAuto.Tests.MemProbe;

public class CyMemScanAlignedTests
{
    [Fact]
    public void FindAlignedU64FindsAllHits()
    {
        var buf = new byte[64];
        BitConverter.GetBytes(0xDEADBEEFCAFEBABEUL).CopyTo(buf, 0);
        BitConverter.GetBytes(0xDEADBEEFCAFEBABEUL).CopyTo(buf, 24);  // 8-aligned
        BitConverter.GetBytes(0x1111UL).CopyTo(buf, 8);
        var hits = CyMemScan.FindAlignedU64(buf, 0xDEADBEEFCAFEBABEUL);
        Assert.Equal(new[] { 0, 24 }, hits);
    }

    [Fact]
    public void FindAlignedU64InSetMatchesAny()
    {
        var buf = new byte[32];
        BitConverter.GetBytes(0xAAAAUL).CopyTo(buf, 0);
        BitConverter.GetBytes(0xBBBBUL).CopyTo(buf, 16);
        var hits = CyMemScan.FindAlignedU64InSet(buf, new[] { 0xAAAAUL, 0xBBBBUL, 0xCCCCUL });
        Assert.Equal(2, hits.Count);
        Assert.Contains((0, 0xAAAAUL), hits);
        Assert.Contains((16, 0xBBBBUL), hits);
    }

    [Fact]
    public void UnpackStructFieldsReadsAllWidths()
    {
        var buf = new byte[16];
        buf[0] = 0xAB;
        BitConverter.GetBytes((ushort)0x1234).CopyTo(buf, 2);
        BitConverter.GetBytes((uint)0xCAFEBABE).CopyTo(buf, 4);
        BitConverter.GetBytes(0xDEADBEEFUL).CopyTo(buf, 8);
        var vals = CyMemScan.UnpackStructFields(buf,
            new[] { (0, 1), (2, 2), (4, 4), (8, 8), (100, 4) });
        Assert.Equal(0xABUL, vals[0]);
        Assert.Equal(0x1234UL, vals[1]);
        Assert.Equal(0xCAFEBABEUL, vals[2]);
        Assert.Equal(0xDEADBEEFUL, vals[3]);
        Assert.Equal(0UL, vals[4]); // out of range
    }
}
