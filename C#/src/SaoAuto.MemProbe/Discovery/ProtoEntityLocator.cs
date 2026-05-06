using System.Buffers.Binary;

namespace SaoAuto.MemProbe.Discovery;

/// <summary>
/// IL2CPP layout constants for Zproto.Entity / AttrCollection / Attr /
/// ByteString — mirrors the table at the top of
/// <c>mem_probe/proto_entity_locator.py</c>. Treat as a frozen contract:
/// regenerated dump.cs RVAs may shift between game builds, so callers
/// resolve klass pointers via <see cref="ProtoEntityLocator.KlassPtrFor"/>.
/// </summary>
public static class ProtoEntityLayout
{
    public const ulong EntityKlassRva         = 0x96C44D8;
    public const ulong AttrCollectionKlassRva = 0x96A3F08;
    public const ulong AttrKlassRva           = 0x96A36D8;

    public const int EntityUuidOff    = 0x10;
    public const int EntityEntTypeOff = 0x18;
    public const int EntityAttrsOff   = 0x20;

    public const int AttrCollUuidOff  = 0x10;
    public const int AttrCollAttrsOff = 0x18;

    public const int RepFieldArrayOff = 0x10;
    public const int RepFieldCountOff = 0x18;

    public const int ArrayDataOff     = 0x20;

    public const int AttrIdOff        = 0x10;
    public const int AttrRawDataOff   = 0x18;

    public const int ByteStrObjOff    = 0x18;
    public const int ByteStrStartOff  = 0x20;
    public const int ByteStrLengthOff = 0x24;

    public const int AttrHp              = 0x2C2E;
    public const int AttrMaxHp           = 0x2C38;
    public const int AttrMaxExtinction   = 440;
    public const int AttrExtinction      = 441;
    public const int AttrBreakingStage   = 455;
    public const int AttrMonsterSeasonLevel = 462;

    public const int EntTypeChar    = 0;
    public const int EntTypeMonster = 1;

    public const int MaxAttrCount   = 1024;
    public const int MaxRawAttrLen  = 256;

    // Conservative user-mode pointer range used by the Python locator.
    public const ulong PtrLowBound  = 0x10000UL;
    public const ulong PtrHighBound = 0x7FFFFFFFFFFFUL;

    public static bool IsPlausiblePtr(ulong p)
        => p >= PtrLowBound && p <= PtrHighBound;
}

/// <summary>One decoded Entity instance — port of Python <c>EntitySnapshot</c>.</summary>
public sealed record EntitySnapshot(
    ulong Address,
    long Uuid,
    int EntType,
    ulong AttrsCollPtr,
    IReadOnlyDictionary<int, int> Attrs,
    IReadOnlyList<(ulong AttrAddress, int AttrId)> RawAttrs);

/// <summary>
/// Walks Zproto.Entity → AttrCollection → RepeatedField&lt;Attr&gt; → Attr →
/// ByteString → varint payload, all via <see cref="IMemorySource"/>. This is
/// the deterministic CPU half of <c>mem_probe/proto_entity_locator.py</c>;
/// the heap-scan side (<c>find_klass_instances</c>) lives on top of
/// <c>CyMemScan.FindAlignedU64</c> and is intentionally not duplicated here.
/// </summary>
public static class ProtoEntityLocator
{
    /// <summary>Resolve a klass pointer = GameAssembly base + RVA.</summary>
    public static ulong KlassPtrFor(ulong gameAssemblyBase, ulong rva)
        => gameAssemblyBase + rva;

    /// <summary>
    /// Decode a protobuf varint payload to signed int32. Caps at 10 bytes
    /// (the standard varint envelope), matches the Python reference exactly
    /// including the 32-bit sign-extend wrap.
    /// </summary>
    public static int DecodeVarintI32(ReadOnlySpan<byte> raw)
    {
        ulong val = 0;
        int shift = 0;
        for (int i = 0; i < raw.Length && i < 10; i++)
        {
            byte b = raw[i];
            val |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        uint v32 = (uint)(val & 0xFFFFFFFFUL);
        return unchecked((int)v32);
    }

    /// <summary>
    /// Read AttrCollection*.Attrs (a RepeatedField&lt;Attr&gt;) and return the
    /// list of Attr* pointers. Returns an empty list on any read failure
    /// (touching unmapped or torn pages is normal during scanning).
    /// </summary>
    public static IReadOnlyList<ulong> ReadRepeatedFieldAttrs(IMemorySource pm, ulong attrsCollPtr)
    {
        var collBlob = pm.ReadBytes(attrsCollPtr, 0x40);
        if (collBlob is null || collBlob.Length < ProtoEntityLayout.AttrCollAttrsOff + 8)
            return Array.Empty<ulong>();

        ulong repFieldPtr = BinaryPrimitives.ReadUInt64LittleEndian(
            collBlob.AsSpan(ProtoEntityLayout.AttrCollAttrsOff, 8));
        if (!ProtoEntityLayout.IsPlausiblePtr(repFieldPtr))
            return Array.Empty<ulong>();

        var rfBlob = pm.ReadBytes(repFieldPtr, 0x40);
        if (rfBlob is null || rfBlob.Length < ProtoEntityLayout.RepFieldCountOff + 4)
            return Array.Empty<ulong>();

        ulong arrayPtr = BinaryPrimitives.ReadUInt64LittleEndian(
            rfBlob.AsSpan(ProtoEntityLayout.RepFieldArrayOff, 8));
        int count = BinaryPrimitives.ReadInt32LittleEndian(
            rfBlob.AsSpan(ProtoEntityLayout.RepFieldCountOff, 4));
        if (count <= 0 || count > ProtoEntityLayout.MaxAttrCount
            || !ProtoEntityLayout.IsPlausiblePtr(arrayPtr))
            return Array.Empty<ulong>();

        int wantBytes = ProtoEntityLayout.ArrayDataOff + count * 8;
        int readSize = Math.Min(wantBytes, 0x4000);
        var arrBlob = pm.ReadBytes(arrayPtr, readSize);
        if (arrBlob is null || arrBlob.Length < ProtoEntityLayout.ArrayDataOff)
            return Array.Empty<ulong>();

        var result = new List<ulong>(count);
        for (int i = 0; i < count; i++)
        {
            int off = ProtoEntityLayout.ArrayDataOff + i * 8;
            if (off + 8 > arrBlob.Length) break;
            ulong p = BinaryPrimitives.ReadUInt64LittleEndian(arrBlob.AsSpan(off, 8));
            if (ProtoEntityLayout.IsPlausiblePtr(p))
                result.Add(p);
        }
        return result;
    }

    /// <summary>
    /// Read Attr.{Id, RawData bytes}. Returns null on outright read failure;
    /// returns (id, empty) when the id is readable but the ByteString chain
    /// is torn / pool-resident (matches the Python "(attr_id, b'')" path).
    /// </summary>
    public static (int Id, byte[] Raw)? ReadAttrIdAndRawBytes(IMemorySource pm, ulong attrPtr)
    {
        var blob = pm.ReadBytes(attrPtr, ProtoEntityLayout.AttrRawDataOff + 8);
        if (blob is null || blob.Length < ProtoEntityLayout.AttrRawDataOff + 8)
            return null;

        int attrId = BinaryPrimitives.ReadInt32LittleEndian(
            blob.AsSpan(ProtoEntityLayout.AttrIdOff, 4));
        ulong bsPtr = BinaryPrimitives.ReadUInt64LittleEndian(
            blob.AsSpan(ProtoEntityLayout.AttrRawDataOff, 8));
        if (!ProtoEntityLayout.IsPlausiblePtr(bsPtr))
            return (attrId, Array.Empty<byte>());

        var bsBlob = pm.ReadBytes(bsPtr, ProtoEntityLayout.ByteStrLengthOff + 4);
        if (bsBlob is null || bsBlob.Length < ProtoEntityLayout.ByteStrLengthOff + 4)
            return (attrId, Array.Empty<byte>());

        ulong arrPtr = BinaryPrimitives.ReadUInt64LittleEndian(
            bsBlob.AsSpan(ProtoEntityLayout.ByteStrObjOff, 8));
        int start = BinaryPrimitives.ReadInt32LittleEndian(
            bsBlob.AsSpan(ProtoEntityLayout.ByteStrStartOff, 4));
        int length = BinaryPrimitives.ReadInt32LittleEndian(
            bsBlob.AsSpan(ProtoEntityLayout.ByteStrLengthOff, 4));
        if (length < 0 || length > ProtoEntityLayout.MaxRawAttrLen
            || !ProtoEntityLayout.IsPlausiblePtr(arrPtr))
            return (attrId, Array.Empty<byte>());

        ulong dataAddr = arrPtr + (ulong)ProtoEntityLayout.ArrayDataOff + (ulong)Math.Max(0, start);
        var raw = pm.ReadBytes(dataAddr, length);
        return (attrId, raw ?? Array.Empty<byte>());
    }

    /// <summary>
    /// Read a full Entity at <paramref name="entityAddr"/>. Returns null when
    /// the header itself can't be read; otherwise returns a snapshot. With
    /// <paramref name="decodeAttrs"/>=false only header fields are populated.
    /// <paramref name="attrFilter"/>, when non-null, restricts varint decode
    /// to those attribute ids (raw_attrs still records every id).
    /// </summary>
    public static EntitySnapshot? ReadEntityFull(
        IMemorySource pm,
        ulong entityAddr,
        bool decodeAttrs = true,
        IReadOnlySet<int>? attrFilter = null)
    {
        var body = pm.ReadBytes(entityAddr, 0x40);
        if (body is null || body.Length < ProtoEntityLayout.EntityAttrsOff + 8)
            return null;

        long uuid = BinaryPrimitives.ReadInt64LittleEndian(
            body.AsSpan(ProtoEntityLayout.EntityUuidOff, 8));
        int entType = BinaryPrimitives.ReadInt32LittleEndian(
            body.AsSpan(ProtoEntityLayout.EntityEntTypeOff, 4));
        ulong attrsColl = BinaryPrimitives.ReadUInt64LittleEndian(
            body.AsSpan(ProtoEntityLayout.EntityAttrsOff, 8));

        var attrs = new Dictionary<int, int>();
        var rawAttrs = new List<(ulong, int)>();

        if (decodeAttrs && attrsColl != 0 && ProtoEntityLayout.IsPlausiblePtr(attrsColl))
        {
            var attrPtrs = ReadRepeatedFieldAttrs(pm, attrsColl);
            foreach (var attrPtr in attrPtrs)
            {
                var pair = ReadAttrIdAndRawBytes(pm, attrPtr);
                if (pair is null) continue;
                var (attrId, raw) = pair.Value;
                rawAttrs.Add((attrPtr, attrId));
                if (attrFilter is not null && !attrFilter.Contains(attrId)) continue;
                if (raw.Length == 0) continue;
                attrs[attrId] = DecodeVarintI32(raw);
            }
        }

        return new EntitySnapshot(entityAddr, uuid, entType, attrsColl, attrs, rawAttrs);
    }
}
