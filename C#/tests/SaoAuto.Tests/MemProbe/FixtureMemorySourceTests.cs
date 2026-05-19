using SaoAuto.MemProbe;

namespace SaoAuto.Tests.MemProbe;

public class FixtureMemorySourceTests
{
    [Fact]
    public void MapAndReadRoundTrip()
    {
        using var pm = new FixtureMemorySource();
        var data = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44 };
        pm.Map(0x10000, data);
        Assert.Equal(0xEFBEADDEu, pm.ReadU32(0x10000));
        Assert.Equal(0x4433_2211_EFBE_ADDEUL, pm.ReadU64(0x10000));
        Assert.Equal(data, pm.ReadBytes(0x10000, 8));
    }

    [Fact]
    public void ReadOutsideMappedReturnsNull()
    {
        using var pm = new FixtureMemorySource();
        pm.Map(0x10000, new byte[16]);
        Assert.Null(pm.ReadU32(0x20000));
        Assert.Null(pm.ReadBytes(0x10000, 32));   // partial -> null
    }

    [Fact]
    public void IterRegionsHonoursPrivateFilter()
    {
        using var pm = new FixtureMemorySource();
        pm.Map(0x10000, new byte[16], type: MemoryRegion.MEM_IMAGE);
        pm.Map(0x20000, new byte[16]);  // default = MEM_PRIVATE
        var rs = pm.IterRegions(onlyPrivate: true).ToList();
        Assert.Single(rs);
        Assert.Equal(0x20000UL, rs[0].Base);
    }
}
