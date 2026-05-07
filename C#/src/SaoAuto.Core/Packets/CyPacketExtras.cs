using System.Buffers.Binary;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Remaining helpers from <c>_sao_cy_packet.pyx</c> that did not land
/// during Phase A. Together with <see cref="Varint"/>, <see cref="PacketReader"/>,
/// <see cref="StaminaDecoder"/>, <see cref="EthernetIpTcpParser"/>, and
/// <see cref="FrameSignatureScanner"/>, this class makes the Cython
/// extension unnecessary at runtime.
/// </summary>
public static class CyPacketExtras
{
    /// <summary>Mirrors <c>_LEVEL_EXTRA_SOURCE_PRIORITY</c>.</summary>
    private static readonly Dictionary<string, int> LevelExtraPriority = new(StringComparer.Ordinal)
    {
        ["deep_sleep"]      = 200,
        ["season_attr"]     = 100,
        ["season_attr_lv"]  = 100,
        ["season_medal"]    = 50,
        ["monster_hunt"]    = 10,
        ["battlepass"]      = 5,
        ["battlepass_data"] = 3,
    };

    public static int LevelExtraSourcePriority(string? source)
    {
        if (string.IsNullOrEmpty(source)) return 0;
        return LevelExtraPriority.TryGetValue(source, out var pri) ? pri : 0;
    }

    /// <summary>Mirrors <c>attrs_match_monster_hint</c> — any-of intersection.</summary>
    public static bool AttrsMatchMonsterHint(IEnumerable<int>? attrIds, ISet<int>? hintSet)
    {
        if (attrIds is null || hintSet is null || hintSet.Count == 0) return false;
        foreach (var x in attrIds)
        {
            if (hintSet.Contains(x)) return true;
        }
        return false;
    }

    /// <summary>Mirrors <c>raw_varint_to_int32</c> — fail-fast version with no fallback.</summary>
    public static int RawVarintToInt32(ReadOnlySpan<byte> raw)
    {
        if (raw.IsEmpty) return 0;
        ulong val = 0;
        var shift = 0;
        for (var i = 0; i < raw.Length && i < 10; i++)
        {
            var b = raw[i];
            val |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return Varint.ToInt32(val);
    }

    /// <summary>Mirrors <c>decode_int32_from_raw</c>.</summary>
    public static int DecodeInt32FromRaw(ReadOnlySpan<byte> raw)
    {
        if (raw.IsEmpty) return 0;
        var val = Varint.ReadUInt64(raw, out _);
        return Varint.ToInt32(val);
    }

    /// <summary>Mirrors <c>decode_float32_from_raw</c>; returns null on short data.</summary>
    public static float? DecodeFloat32FromRaw(ReadOnlySpan<byte> raw)
    {
        if (raw.Length < 4) return null;
        return BinaryPrimitives.ReadSingleLittleEndian(raw[..4]);
    }

    /// <summary>
    /// Mirrors Cython <c>decode_string_from_raw</c>: parses a leading
    /// varint length, then reads that many UTF-8 bytes. Falls back to
    /// decoding the entire payload as UTF-8 when the leading length is
    /// invalid or absent (matches the legacy Python helper). Used by
    /// AttrCollection unpack for string-typed monster attrs (NAME id=1).
    /// </summary>
    public static string DecodeStringFromRaw(ReadOnlySpan<byte> raw)
    {
        if (raw.IsEmpty) return string.Empty;
        try
        {
            var len = (int)Varint.ReadUInt64(raw, out var consumed);
            if (consumed > 0 && len >= 0 && consumed + len <= raw.Length)
            {
                return len == 0
                    ? string.Empty
                    : System.Text.Encoding.UTF8.GetString(raw.Slice(consumed, len));
            }
        }
        catch
        {
            // fall through to whole-payload decode
        }
        try
        {
            return System.Text.Encoding.UTF8.GetString(raw);
        }
        catch
        {
            return string.Empty;
        }
    }

    /// <summary>
    /// S116 — port of Python <c>_decode_shield_list</c> at packet_parser.py
    /// 4742. Sums (value, max_value) over each <c>ShieldInfo</c> submessage
    /// (uuid=1, shield_type=2, value=3, initial_value=4, max_value=5) carried
    /// inside the wrapper at field 1. Empty payload returns (0, 0)
    /// (server signalling "shield cleared"); any decode error falls back
    /// to (0, 0) — matches Python's "force-clear on parse failure" rule
    /// so a stale shield_active doesn't survive a corrupt packet.
    /// </summary>
    public static (int Total, int MaxTotal) DecodeShieldList(ReadOnlySpan<byte> raw)
    {
        if (raw.IsEmpty) return (0, 0);
        try
        {
            var shields = DecodeFields(raw.ToArray());
            int totalValue = 0, totalMax = 0;
            if (shields.TryGetValue(1, out var entries))
            {
                foreach (var entry in entries)
                {
                    if (entry is byte[] bytes)
                    {
                        var sf = DecodeFields(bytes);
                        var value = ToLong(sf, 3);
                        var maxVal = ToLong(sf, 5);
                        totalValue += (int)Math.Max(0, Math.Min(int.MaxValue, value));
                        totalMax += (int)Math.Max(0, Math.Min(int.MaxValue, maxVal));
                    }
                    else
                    {
                        // Inline single shield — top-level field 3 = value,
                        // field 5 = max_val. Mirrors Python's `elif
                        // isinstance(shield_raw, int)` branch.
                        var value = ToLong(shields, 3);
                        var maxVal = ToLong(shields, 5);
                        totalValue = (int)Math.Max(0, Math.Min(int.MaxValue, value));
                        totalMax = (int)Math.Max(0, Math.Min(int.MaxValue, maxVal));
                        break;
                    }
                }
            }
            return (totalValue, totalMax);
        }
        catch
        {
            return (0, 0);
        }
    }

    private static long ToLong(Dictionary<int, List<object>> fields, int tag)
    {
        if (!fields.TryGetValue(tag, out var list) || list.Count == 0) return 0;
        return list[0] switch
        {
            ulong u => unchecked((long)u),
            long l => l,
            int i => i,
            _ => 0,
        };
    }

    public static uint ReadLittleEndianU32At(ReadOnlySpan<byte> data, int pos)
    {
        if (pos < 0 || pos + 4 > data.Length)
        {
            throw new ArgumentException("ReadLittleEndianU32At out of range");
        }
        return BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(pos, 4));
    }

    public static ulong ReadLittleEndianU64At(ReadOnlySpan<byte> data, int pos)
    {
        if (pos < 0 || pos + 8 > data.Length)
        {
            throw new ArgumentException("ReadLittleEndianU64At out of range");
        }
        return BinaryPrimitives.ReadUInt64LittleEndian(data.Slice(pos, 8));
    }

    public static float ReadLittleEndianF32At(ReadOnlySpan<byte> data, int pos)
    {
        if (pos < 0 || pos + 4 > data.Length)
        {
            throw new ArgumentException("ReadLittleEndianF32At out of range");
        }
        return BinaryPrimitives.ReadSingleLittleEndian(data.Slice(pos, 4));
    }

    /// <summary>Mirrors <c>decode_packed_varints</c>.</summary>
    public static List<long> DecodePackedVarints(ReadOnlySpan<byte> raw)
    {
        var result = new List<long>();
        var pos = 0;
        while (pos < raw.Length)
        {
            var slice = raw.Slice(pos);
            var val = Varint.ReadUInt64(slice, out var consumed);
            if (consumed <= 0) break;
            result.Add(unchecked((long)val));
            pos += consumed;
        }
        return result;
    }

    /// <summary>
    /// Mirrors <c>decode_resource_value_map</c>: pair the two parallel lists,
    /// reinterpret each as int32 with the over-wide low-32-bit mask, keep
    /// only positive ids and non-negative values.
    /// </summary>
    public static Dictionary<int, int> DecodeResourceValueMap(
        IReadOnlyList<long>? resourceIds,
        IReadOnlyList<long>? resources)
    {
        var result = new Dictionary<int, int>();
        if (resourceIds is null || resources is null) return result;
        var count = Math.Min(resourceIds.Count, resources.Count);
        for (var i = 0; i < count; i++)
        {
            var resId = Varint.ToInt32((ulong)resourceIds[i]);
            var value = Varint.ToInt32((ulong)resources[i]);
            if (resId > 0 && value >= 0)
            {
                result[resId] = value;
            }
        }
        return result;
    }

    /// <summary>Mirrors <c>append_decimal_key</c>.</summary>
    public static long AppendDecimalKey(long prefix, long suffix, int minWidth)
    {
        if (prefix < 0) prefix = 0;
        if (suffix < 0) suffix = 0;
        var width = DecimalDigits(suffix);
        if (width < minWidth) width = minWidth;
        long mul = 1;
        for (var i = 0; i < width; i++) mul *= 10;
        return prefix * mul + suffix;
    }

    /// <summary>Mirrors <c>compute_damage_key</c>.</summary>
    public static long ComputeDamageKey(long ownerId, long damageSource, long ownerLevel, long hitEventId)
    {
        if (ownerId <= 0) return 0;
        if (hitEventId < 0) hitEventId = 0;
        var damageType = damageSource == 2 ? 2 : (damageSource > 0 ? 3 : 1);
        return AppendDecimalKey(AppendDecimalKey(damageType, ownerId, 0), hitEventId, 2);
    }

    private static int DecimalDigits(long value)
    {
        var digits = 1;
        if (value < 0) value = 0;
        while (value >= 10)
        {
            value /= 10;
            digits++;
        }
        return digits;
    }

    /// <summary>Mirrors <c>parse_game_frame_headers</c>.</summary>
    public static List<(int MsgType, bool IsZstd, byte[] Payload)> ParseGameFrameHeaders(byte[] frame)
    {
        var result = new List<(int, bool, byte[])>();
        if (frame.Length < 6) return result;

        var span = frame.AsSpan();
        var offset = 0;
        while (offset < span.Length)
        {
            if (offset + 6 > span.Length) break;
            var pktSize = (int)BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, 4));
            if (pktSize < 6 || offset + pktSize > span.Length) break;
            var pktType = BinaryPrimitives.ReadUInt16BigEndian(span.Slice(offset + 4, 2));
            var msgType = pktType & 0x7FFF;
            var isZstd = (pktType & 0x8000) != 0;
            var payload = new byte[pktSize - 6];
            Buffer.BlockCopy(frame, offset + 6, payload, 0, payload.Length);
            result.Add((msgType, isZstd, payload));
            offset += pktSize;
        }
        return result;
    }

    /// <summary>
    /// Mirrors <c>parse_notify_header</c> — return <c>(methodId, payload)</c>
    /// when the leading 8 bytes match the expected service uuid, else null.
    /// </summary>
    public static (int MethodId, byte[] Payload)? ParseNotifyHeader(byte[] payload, ulong expectedServiceUuid)
    {
        if (payload.Length < 16) return null;
        var serviceUuid = BinaryPrimitives.ReadUInt64BigEndian(payload.AsSpan(0, 8));
        if (serviceUuid != expectedServiceUuid) return null;
        var methodId = (int)BinaryPrimitives.ReadUInt32BigEndian(payload.AsSpan(12, 4));
        var rest = new byte[payload.Length - 16];
        Buffer.BlockCopy(payload, 16, rest, 0, rest.Length);
        return (methodId, rest);
    }

    /// <summary>
    /// Mirrors <c>decode_fields</c> — group payload by field number for the
    /// supported wire types (varint, fixed64, length-delimited, fixed32).
    /// Group-start (3) and group-end (4) wire types break the loop, matching Python.
    /// </summary>
    public static Dictionary<int, List<object>> DecodeFields(byte[] data)
    {
        var fields = new Dictionary<int, List<object>>();
        var span = data.AsSpan();
        var pos = 0;
        while (pos < span.Length)
        {
            var slice = span.Slice(pos);
            var tag = Varint.ReadUInt64(slice, out var consumed);
            pos += consumed;
            var fieldNum = (int)(tag >> 3);
            var wireType = (int)(tag & 0x07);

            object value;
            switch (wireType)
            {
                case 0: // varint
                    {
                        var v = Varint.ReadUInt64(span.Slice(pos), out var n);
                        pos += n;
                        value = v;
                        break;
                    }
                case 1: // fixed64
                    {
                        if (pos + 8 > span.Length) return fields;
                        var u64 = BinaryPrimitives.ReadUInt64LittleEndian(span.Slice(pos, 8));
                        pos += 8;
                        value = unchecked((long)u64);
                        break;
                    }
                case 2: // length-delimited
                    {
                        var vlen = Varint.ReadUInt64(span.Slice(pos), out var n);
                        pos += n;
                        if ((ulong)(span.Length - pos) < vlen) return fields;
                        var bytes = new byte[(int)vlen];
                        Buffer.BlockCopy(data, pos, bytes, 0, bytes.Length);
                        pos += (int)vlen;
                        value = bytes;
                        break;
                    }
                case 5: // fixed32
                    {
                        if (pos + 4 > span.Length) return fields;
                        var u32 = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
                        pos += 4;
                        value = BitConverter.UInt32BitsToSingle(u32);
                        break;
                    }
                default:
                    return fields;
            }

            if (!fields.TryGetValue(fieldNum, out var list))
            {
                list = new List<object>();
                fields[fieldNum] = list;
            }
            list.Add(value);
        }
        return fields;
    }

    /// <summary>
    /// Mirrors <c>parse_dungeon_dirty_buffer</c>. Reads the padded i32-encoded
    /// nested container that drives `SyncDungeonDirtyData`. Returns the
    /// optional flow_state plus a list of target progress entries.
    /// </summary>
    public static (int? FlowState, List<DungeonTargetProgress> Targets) ParseDungeonDirtyBuffer(byte[] data)
    {
        var pos = 0;
        var span = data.AsSpan();
        int? flowState = null;
        var targets = new List<DungeonTargetProgress>();

        var begin = ReadI32Padded(span, ref pos);
        if (begin != -2) throw new InvalidDataException($"invalid dirty container begin tag: {begin}");
        var size = ReadI32Padded(span, ref pos);
        if (size == -3) return (flowState, targets);
        if (size < 0) throw new InvalidDataException($"invalid dirty container size: {size}");
        var rootEnd = pos + size;
        if (rootEnd > span.Length) throw new InvalidDataException("dirty container body exceeds buffer size");

        var field = ReadI32Padded(span, ref pos);
        while (field > 0 && pos <= span.Length)
        {
            if (field == 2)
            {
                ReadFlowSection(span, ref pos, ref flowState);
            }
            else if (field == 4)
            {
                ReadTargetSection(span, ref pos, targets);
            }
            else
            {
                pos = rootEnd;
            }
            if (pos + 8 > span.Length) break;
            field = ReadI32Padded(span, ref pos);
        }
        return (flowState, targets);
    }

    private static void ReadFlowSection(ReadOnlySpan<byte> span, ref int pos, ref int? flowState)
    {
        var begin = ReadI32Padded(span, ref pos);
        if (begin != -2) throw new InvalidDataException($"invalid dirty container begin tag: {begin}");
        var size = ReadI32Padded(span, ref pos);
        if (size == -3) return;
        if (size < 0) throw new InvalidDataException($"invalid dirty container size: {size}");
        var flowEnd = pos + size;
        if (flowEnd > span.Length) throw new InvalidDataException("dirty container body exceeds buffer size");
        var flowField = ReadI32Padded(span, ref pos);
        while (flowField > 0)
        {
            if (flowField == 1) flowState = ReadI32Padded(span, ref pos);
            else { pos = flowEnd; break; }
            if (pos + 8 > span.Length) break;
            flowField = ReadI32Padded(span, ref pos);
        }
        if (flowField != -3) pos = flowEnd;
    }

    private static void ReadTargetSection(ReadOnlySpan<byte> span, ref int pos, List<DungeonTargetProgress> targets)
    {
        var begin = ReadI32Padded(span, ref pos);
        if (begin != -2) throw new InvalidDataException($"invalid dirty container begin tag: {begin}");
        var size = ReadI32Padded(span, ref pos);
        if (size == -3) return;
        if (size < 0) throw new InvalidDataException($"invalid dirty container size: {size}");
        var targetEnd = pos + size;
        if (targetEnd > span.Length) throw new InvalidDataException("dirty container body exceeds buffer size");

        var targetField = ReadI32Padded(span, ref pos);
        while (targetField > 0)
        {
            if (targetField == 1)
            {
                var addCount = ReadI32Padded(span, ref pos);
                var removeCount = 0;
                var updateCount = 0;
                if (addCount != -4)
                {
                    if (addCount == -1)
                    {
                        addCount = ReadI32Padded(span, ref pos);
                    }
                    else
                    {
                        removeCount = ReadI32Padded(span, ref pos);
                        updateCount = ReadI32Padded(span, ref pos);
                    }
                    if (addCount < 0 || removeCount < 0 || updateCount < 0)
                    {
                        throw new InvalidDataException("negative dirty target map section size");
                    }
                    for (var i = 0; i < addCount; i++) targets.Add(ReadEntry(span, ref pos));
                    for (var i = 0; i < removeCount; i++) ReadI32Padded(span, ref pos);
                    for (var i = 0; i < updateCount; i++) targets.Add(ReadEntry(span, ref pos));
                }
            }
            else
            {
                pos = targetEnd;
                break;
            }
            if (pos + 8 > span.Length) break;
            targetField = ReadI32Padded(span, ref pos);
        }
        if (targetField != -3) pos = targetEnd;
    }

    private static DungeonTargetProgress ReadEntry(ReadOnlySpan<byte> span, ref int pos)
    {
        ReadI32Padded(span, ref pos); // skip entry header

        var targetId = 0;
        var nums = 0;
        var complete = 0;
        var begin = ReadI32Padded(span, ref pos);
        if (begin != -2) throw new InvalidDataException($"invalid dirty container begin tag: {begin}");
        var size = ReadI32Padded(span, ref pos);
        if (size == -3) return new DungeonTargetProgress(targetId, nums, complete);
        if (size < 0) throw new InvalidDataException($"invalid dirty container size: {size}");
        var entryEnd = pos + size;
        if (entryEnd > span.Length) throw new InvalidDataException("dirty container body exceeds buffer size");

        var mapField = ReadI32Padded(span, ref pos);
        while (mapField > 0)
        {
            if (mapField == 1) targetId = ReadI32Padded(span, ref pos);
            else if (mapField == 2) nums = ReadI32Padded(span, ref pos);
            else if (mapField == 3) complete = ReadI32Padded(span, ref pos);
            else { pos = entryEnd; break; }
            if (pos + 8 > span.Length) break;
            mapField = ReadI32Padded(span, ref pos);
        }
        if (mapField != -3) pos = entryEnd;
        return new DungeonTargetProgress(targetId, nums, complete);
    }

    private static int ReadI32Padded(ReadOnlySpan<byte> src, ref int pos)
    {
        if (pos + 8 > src.Length)
        {
            throw new InvalidDataException("unexpected eof while reading padded i32");
        }
        var u = BinaryPrimitives.ReadUInt32LittleEndian(src.Slice(pos, 4));
        pos += 8; // 4 value + 4 padding
        return unchecked((int)u);
    }

    /// <summary>
    /// Parse the V3.3.6 container-dirty binary stream that wraps a single
    /// (field_index, sub_field, value) tuple per packet. Mirrors
    /// <c>packet_parser._parse_dirty_stream</c> for the skeleton subset we
    /// support: field 2 (CharBase) sub_fields 5/Name + 35/FightPoint;
    /// field 16 (UserFightAttr) sub_fields 1/CurHp + 2/MaxHp + 3/OriginEnergy;
    /// field 22 (RoleLevel) sub_field 1/Level. Returns null when the
    /// header tag is missing, the stream is short, or the (field, sub_field)
    /// combination is outside the supported set — matches Python's silent
    /// drop on unrecognised fields. Wider field coverage (SeasonCenter,
    /// SeasonMedalInfo, MonsterHuntInfo, ProfessionList) is held for a
    /// follow-up session that wires the corresponding GameState slots.
    /// </summary>
    public static ContainerDirtyChange? ParseContainerDirtyStream(byte[] data)
    {
        if (data is null || data.Length < 12) return null;
        var span = data.AsSpan();
        var pos = 0;
        if (BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4)) != 0xFFFFFFFEu) return null;
        pos += 8; // skip ident + 4-byte validation
        if (pos + 4 > span.Length) return null;
        var fieldIndex = (int)BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
        pos += 4;

        if (pos + 8 > span.Length) return null;
        if (BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4)) != 0xFFFFFFFEu) return null;
        pos += 8;
        if (pos + 4 > span.Length) return null;
        var subField = (int)BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
        pos += 4;

        switch (fieldIndex)
        {
            case 2: // CharBase
                if (subField == 5)
                {
                    if (pos + 4 > span.Length) return null;
                    var slen = (int)BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
                    pos += 4;
                    if (slen < 0 || pos + slen > span.Length) return null;
                    var str = System.Text.Encoding.UTF8.GetString(span.Slice(pos, slen));
                    return new ContainerDirtyChange(2, 5, str, null, null);
                }
                if (subField == 35)
                {
                    if (pos + 4 > span.Length) return null;
                    var fp = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
                    return new ContainerDirtyChange(2, 35, null, fp, null);
                }
                return null;
            case 16: // UserFightAttr
                if (subField == 1 || subField == 2)
                {
                    if (pos + 4 > span.Length) return null;
                    var v = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
                    return new ContainerDirtyChange(16, subField, null, v, null);
                }
                if (subField == 3)
                {
                    if (pos + 4 > span.Length) return null;
                    var f = BinaryPrimitives.ReadSingleLittleEndian(span.Slice(pos, 4));
                    var i = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
                    return new ContainerDirtyChange(16, 3, null, i, f);
                }
                return null;
            case 22: // RoleLevel
                if (subField == 1)
                {
                    if (pos + 4 > span.Length) return null;
                    var lv = BinaryPrimitives.ReadUInt32LittleEndian(span.Slice(pos, 4));
                    return new ContainerDirtyChange(22, 1, null, lv, null);
                }
                return null;
            default:
                return null;
        }
    }
}

public sealed record DungeonTargetProgress(int TargetId, int Nums, int Complete);

/// <summary>
/// One (field, sub_field, value) tuple decoded from a SyncContainerDirtyData
/// stream. Exactly one of <see cref="StringValue"/> / <see cref="IntValue"/>
/// / <see cref="FloatValue"/> is non-null for the supported subset; both
/// <see cref="IntValue"/> and <see cref="FloatValue"/> are populated for
/// field=16, sub=3 (OriginEnergy) so the bridge can pick whichever
/// interpretation matches the player's current stamina_max — same as
/// Python's <c>_decode_dirty_energy_value</c>.
/// </summary>
public sealed record ContainerDirtyChange(
    int FieldIndex,
    int SubField,
    string? StringValue,
    long? IntValue,
    float? FloatValue);
