using System.Buffers.Binary;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Discovery;

namespace SaoAuto.Tests.MemProbe;

public class ProtoEntityLocatorTests
{
    private static byte[] Block(int size) => new byte[size];

    private static void WriteU64(byte[] buf, int off, ulong v)
        => BinaryPrimitives.WriteUInt64LittleEndian(buf.AsSpan(off, 8), v);
    private static void WriteI64(byte[] buf, int off, long v)
        => BinaryPrimitives.WriteInt64LittleEndian(buf.AsSpan(off, 8), v);
    private static void WriteI32(byte[] buf, int off, int v)
        => BinaryPrimitives.WriteInt32LittleEndian(buf.AsSpan(off, 4), v);

    [Fact]
    public void DecodeVarintI32MatchesProtobufEncoding()
    {
        // value 42 → single byte 0x2A
        Assert.Equal(42, ProtoEntityLocator.DecodeVarintI32(new byte[] { 0x2A }));
        // value 300 → 0xAC 0x02
        Assert.Equal(300, ProtoEntityLocator.DecodeVarintI32(new byte[] { 0xAC, 0x02 }));
        // value 0
        Assert.Equal(0, ProtoEntityLocator.DecodeVarintI32(new byte[] { 0x00 }));
    }

    [Fact]
    public void DecodeVarintI32SignExtendsHighBit()
    {
        // -1 as 32-bit varint → ten 0xFFs (low) but 0x..FFFFFFFF wraps to -1.
        // Quick path: encode 0xFFFFFFFF in 5-byte form: FF FF FF FF 0F.
        var raw = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F };
        Assert.Equal(-1, ProtoEntityLocator.DecodeVarintI32(raw));
    }

    [Fact]
    public void DecodeVarintI32CapsAtTenBytes()
    {
        var raw = new byte[16];
        for (int i = 0; i < raw.Length; i++) raw[i] = 0xFF; // never terminates
        // Should not loop forever, returns whatever fits in low 32 bits.
        var v = ProtoEntityLocator.DecodeVarintI32(raw);
        Assert.Equal(-1, v);
    }

    [Fact]
    public void IsPlausiblePtrRejectsNullAndKernelRange()
    {
        Assert.False(ProtoEntityLayout.IsPlausiblePtr(0));
        Assert.False(ProtoEntityLayout.IsPlausiblePtr(0xFFFF));
        Assert.True(ProtoEntityLayout.IsPlausiblePtr(0x10000));
        Assert.True(ProtoEntityLayout.IsPlausiblePtr(0x7FFFFFFFFFFFUL));
        Assert.False(ProtoEntityLayout.IsPlausiblePtr(0x800000000000UL));
    }

    [Fact]
    public void ReadEntityFullDecodesTwoAttrsThroughByteStringChain()
    {
        const ulong entityAddr  = 0x100_000UL;
        const ulong collAddr    = 0x200_000UL;
        const ulong repFldAddr  = 0x300_000UL;
        const ulong arrObjAddr  = 0x400_000UL;
        const ulong attr1Addr   = 0x500_000UL;
        const ulong attr2Addr   = 0x510_000UL;
        const ulong bs1Addr     = 0x600_000UL;
        const ulong bs2Addr     = 0x610_000UL;
        const ulong arr1Addr    = 0x700_000UL;
        const ulong arr2Addr    = 0x710_000UL;

        // Entity body
        var entity = Block(0x40);
        WriteI64(entity, ProtoEntityLayout.EntityUuidOff, 0x1234_5678_ABCDL);
        WriteI32(entity, ProtoEntityLayout.EntityEntTypeOff, ProtoEntityLayout.EntTypeMonster);
        WriteU64(entity, ProtoEntityLayout.EntityAttrsOff, collAddr);

        // AttrCollection body
        var coll = Block(0x40);
        WriteU64(coll, ProtoEntityLayout.AttrCollAttrsOff, repFldAddr);

        // RepeatedField<Attr>
        var rf = Block(0x40);
        WriteU64(rf, ProtoEntityLayout.RepFieldArrayOff, arrObjAddr);
        WriteI32(rf, ProtoEntityLayout.RepFieldCountOff, 2);

        // Attr[] object: header + 2 ptrs
        var arrObj = Block(ProtoEntityLayout.ArrayDataOff + 2 * 8);
        WriteU64(arrObj, ProtoEntityLayout.ArrayDataOff + 0, attr1Addr);
        WriteU64(arrObj, ProtoEntityLayout.ArrayDataOff + 8, attr2Addr);

        // Attr 1 (HP = 42)
        var attr1 = Block(0x40);
        WriteI32(attr1, ProtoEntityLayout.AttrIdOff, ProtoEntityLayout.AttrHp);
        WriteU64(attr1, ProtoEntityLayout.AttrRawDataOff, bs1Addr);

        // Attr 2 (MaxHp = 300)
        var attr2 = Block(0x40);
        WriteI32(attr2, ProtoEntityLayout.AttrIdOff, ProtoEntityLayout.AttrMaxHp);
        WriteU64(attr2, ProtoEntityLayout.AttrRawDataOff, bs2Addr);

        // ByteString 1 → arr1, start=0, length=1
        var bs1 = Block(0x40);
        WriteU64(bs1, ProtoEntityLayout.ByteStrObjOff, arr1Addr);
        WriteI32(bs1, ProtoEntityLayout.ByteStrStartOff, 0);
        WriteI32(bs1, ProtoEntityLayout.ByteStrLengthOff, 1);

        // ByteString 2 → arr2, start=0, length=2
        var bs2 = Block(0x40);
        WriteU64(bs2, ProtoEntityLayout.ByteStrObjOff, arr2Addr);
        WriteI32(bs2, ProtoEntityLayout.ByteStrStartOff, 0);
        WriteI32(bs2, ProtoEntityLayout.ByteStrLengthOff, 2);

        // byte[] 1 — varint 42
        var arr1 = Block(ProtoEntityLayout.ArrayDataOff + 4);
        arr1[ProtoEntityLayout.ArrayDataOff + 0] = 0x2A;

        // byte[] 2 — varint 300 = 0xAC 0x02
        var arr2 = Block(ProtoEntityLayout.ArrayDataOff + 4);
        arr2[ProtoEntityLayout.ArrayDataOff + 0] = 0xAC;
        arr2[ProtoEntityLayout.ArrayDataOff + 1] = 0x02;

        using var pm = new FixtureMemorySource();
        pm.Map(entityAddr,  entity);
        pm.Map(collAddr,    coll);
        pm.Map(repFldAddr,  rf);
        pm.Map(arrObjAddr,  arrObj);
        pm.Map(attr1Addr,   attr1);
        pm.Map(attr2Addr,   attr2);
        pm.Map(bs1Addr,     bs1);
        pm.Map(bs2Addr,     bs2);
        pm.Map(arr1Addr,    arr1);
        pm.Map(arr2Addr,    arr2);

        var snap = ProtoEntityLocator.ReadEntityFull(pm, entityAddr);
        Assert.NotNull(snap);
        Assert.Equal(0x1234_5678_ABCDL, snap!.Uuid);
        Assert.Equal(ProtoEntityLayout.EntTypeMonster, snap.EntType);
        Assert.Equal(collAddr, snap.AttrsCollPtr);
        Assert.Equal(2, snap.RawAttrs.Count);
        Assert.Equal(42,  snap.Attrs[ProtoEntityLayout.AttrHp]);
        Assert.Equal(300, snap.Attrs[ProtoEntityLayout.AttrMaxHp]);
    }

    [Fact]
    public void ReadEntityFullReturnsNullWhenHeaderUnreadable()
    {
        using var pm = new FixtureMemorySource();
        var snap = ProtoEntityLocator.ReadEntityFull(pm, 0x123_000UL);
        Assert.Null(snap);
    }

    [Fact]
    public void ReadEntityFullReturnsHeaderOnlyWhenAttrsCollNull()
    {
        const ulong entityAddr = 0x100_000UL;
        var entity = Block(0x40);
        WriteI64(entity, ProtoEntityLayout.EntityUuidOff, 99);
        WriteI32(entity, ProtoEntityLayout.EntityEntTypeOff, ProtoEntityLayout.EntTypeChar);
        WriteU64(entity, ProtoEntityLayout.EntityAttrsOff, 0);

        using var pm = new FixtureMemorySource();
        pm.Map(entityAddr, entity);

        var snap = ProtoEntityLocator.ReadEntityFull(pm, entityAddr);
        Assert.NotNull(snap);
        Assert.Equal(99, snap!.Uuid);
        Assert.Empty(snap.Attrs);
        Assert.Empty(snap.RawAttrs);
    }

    [Fact]
    public void ReadEntityFullSkipsDecodeWhenDecodeAttrsFalse()
    {
        // Even with a dangling attrs_coll ptr, decodeAttrs=false must not touch it.
        const ulong entityAddr = 0x100_000UL;
        var entity = Block(0x40);
        WriteI64(entity, ProtoEntityLayout.EntityUuidOff, 7);
        WriteU64(entity, ProtoEntityLayout.EntityAttrsOff, 0xDEAD_BEEFUL);

        using var pm = new FixtureMemorySource();
        pm.Map(entityAddr, entity);

        var snap = ProtoEntityLocator.ReadEntityFull(pm, entityAddr, decodeAttrs: false);
        Assert.NotNull(snap);
        Assert.Equal(0xDEAD_BEEFUL, snap!.AttrsCollPtr);
        Assert.Empty(snap.Attrs);
    }

    [Fact]
    public void AttrFilterRestrictsDecodedAttrsButRawAttrsRecordsAll()
    {
        const ulong entityAddr = 0x100_000UL;
        const ulong collAddr   = 0x200_000UL;
        const ulong repFldAddr = 0x300_000UL;
        const ulong arrObjAddr = 0x400_000UL;
        const ulong attr1Addr  = 0x500_000UL;
        const ulong bs1Addr    = 0x600_000UL;
        const ulong arr1Addr   = 0x700_000UL;

        var entity = Block(0x40);
        WriteU64(entity, ProtoEntityLayout.EntityAttrsOff, collAddr);
        WriteI64(entity, ProtoEntityLayout.EntityUuidOff, 1);

        var coll = Block(0x40);
        WriteU64(coll, ProtoEntityLayout.AttrCollAttrsOff, repFldAddr);

        var rf = Block(0x40);
        WriteU64(rf, ProtoEntityLayout.RepFieldArrayOff, arrObjAddr);
        WriteI32(rf, ProtoEntityLayout.RepFieldCountOff, 1);

        var arrObj = Block(ProtoEntityLayout.ArrayDataOff + 8);
        WriteU64(arrObj, ProtoEntityLayout.ArrayDataOff, attr1Addr);

        var attr1 = Block(0x40);
        // Use an ID that's NOT in the filter
        WriteI32(attr1, ProtoEntityLayout.AttrIdOff, 0x1111);
        WriteU64(attr1, ProtoEntityLayout.AttrRawDataOff, bs1Addr);

        var bs1 = Block(0x40);
        WriteU64(bs1, ProtoEntityLayout.ByteStrObjOff, arr1Addr);
        WriteI32(bs1, ProtoEntityLayout.ByteStrLengthOff, 1);

        var arr1 = Block(ProtoEntityLayout.ArrayDataOff + 4);
        arr1[ProtoEntityLayout.ArrayDataOff] = 0x2A;

        using var pm = new FixtureMemorySource();
        pm.Map(entityAddr, entity);
        pm.Map(collAddr,   coll);
        pm.Map(repFldAddr, rf);
        pm.Map(arrObjAddr, arrObj);
        pm.Map(attr1Addr,  attr1);
        pm.Map(bs1Addr,    bs1);
        pm.Map(arr1Addr,   arr1);

        var filter = new HashSet<int> { ProtoEntityLayout.AttrHp };
        var snap = ProtoEntityLocator.ReadEntityFull(pm, entityAddr, attrFilter: filter);

        Assert.NotNull(snap);
        Assert.Single(snap!.RawAttrs);
        Assert.Equal(0x1111, snap.RawAttrs[0].AttrId);
        Assert.Empty(snap.Attrs); // filter excluded the only present attr
    }

    [Fact]
    public void ReadRepeatedFieldAttrsRejectsImplausibleCount()
    {
        const ulong collAddr   = 0x200_000UL;
        const ulong repFldAddr = 0x300_000UL;
        const ulong arrObjAddr = 0x400_000UL;

        var coll = Block(0x40);
        WriteU64(coll, ProtoEntityLayout.AttrCollAttrsOff, repFldAddr);

        var rf = Block(0x40);
        WriteU64(rf, ProtoEntityLayout.RepFieldArrayOff, arrObjAddr);
        WriteI32(rf, ProtoEntityLayout.RepFieldCountOff, 9999); // > MaxAttrCount

        using var pm = new FixtureMemorySource();
        pm.Map(collAddr,   coll);
        pm.Map(repFldAddr, rf);

        var ptrs = ProtoEntityLocator.ReadRepeatedFieldAttrs(pm, collAddr);
        Assert.Empty(ptrs);
    }

    [Fact]
    public void KlassPtrForReturnsBasePlusRva()
    {
        Assert.Equal(0x1_0000_0000UL + 0x123,
            ProtoEntityLocator.KlassPtrFor(0x1_0000_0000UL, 0x123UL));
    }
}
