using System.Buffers.Binary;
using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

public class CyPacketExtrasTests
{
    [Theory]
    [InlineData("deep_sleep", 200)]
    [InlineData("season_attr", 100)]
    [InlineData("season_attr_lv", 100)]
    [InlineData("season_medal", 50)]
    [InlineData("monster_hunt", 10)]
    [InlineData("battlepass", 5)]
    [InlineData("battlepass_data", 3)]
    [InlineData("unknown", 0)]
    [InlineData("", 0)]
    [InlineData(null, 0)]
    public void LevelExtraSourcePriorityMatchesPythonTable(string? source, int expected)
    {
        Assert.Equal(expected, CyPacketExtras.LevelExtraSourcePriority(source));
    }

    [Fact]
    public void AttrsMatchMonsterHintReturnsTrueOnIntersection()
    {
        var hint = new HashSet<int> { 11320, 11321, 50001 };
        Assert.True(CyPacketExtras.AttrsMatchMonsterHint(new[] { 1, 2, 11321 }, hint));
        Assert.False(CyPacketExtras.AttrsMatchMonsterHint(new[] { 1, 2, 3 }, hint));
        Assert.False(CyPacketExtras.AttrsMatchMonsterHint(null, hint));
        Assert.False(CyPacketExtras.AttrsMatchMonsterHint(new[] { 1 }, null));
    }

    [Fact]
    public void RawVarintToInt32MasksToLow32Bits()
    {
        // 10-byte all-ones encoding → -1
        var bytes = new byte[] { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
        Assert.Equal(-1, CyPacketExtras.RawVarintToInt32(bytes));
    }

    [Fact]
    public void DecodeFloat32FromRawReadsLittleEndian()
    {
        var bytes = BitConverter.GetBytes(0.5f);
        Assert.Equal(0.5f, CyPacketExtras.DecodeFloat32FromRaw(bytes));
        Assert.Null(CyPacketExtras.DecodeFloat32FromRaw(new byte[] { 0x01 }));
    }

    [Fact]
    public void PackedVarintsRoundTrip()
    {
        var bytes = new byte[] { 0x01, 0xAC, 0x02, 0x7F };
        var values = CyPacketExtras.DecodePackedVarints(bytes);
        Assert.Equal(new long[] { 1, 300, 127 }, values);
    }

    [Fact]
    public void DecodeResourceValueMapPairsAndFiltersNegatives()
    {
        var ids = new long[] { 1, 2, -3, 4 };
        var vals = new long[] { 10, 20, 30, -1 };
        var map = CyPacketExtras.DecodeResourceValueMap(ids, vals);
        // id=-3 dropped (not > 0); id=4 dropped because value < 0.
        Assert.Equal(2, map.Count);
        Assert.Equal(10, map[1]);
        Assert.Equal(20, map[2]);
    }

    [Fact]
    public void AppendDecimalKeyConcatenatesWithPadding()
    {
        Assert.Equal(105L, CyPacketExtras.AppendDecimalKey(1, 5, 2));     // "1" + "05"
        Assert.Equal(123L, CyPacketExtras.AppendDecimalKey(1, 23, 0));    // "1" + "23"
        Assert.Equal(1003L, CyPacketExtras.AppendDecimalKey(10, 3, 2));   // "10" + "03"
    }

    [Fact]
    public void ComputeDamageKeyMixesType()
    {
        // damage_source == 2 → type 2
        // damage_source > 0 (not 2) → type 3
        // damage_source == 0 → type 1
        var k1 = CyPacketExtras.ComputeDamageKey(ownerId: 100, damageSource: 0, ownerLevel: 60, hitEventId: 5);
        var k2 = CyPacketExtras.ComputeDamageKey(ownerId: 100, damageSource: 2, ownerLevel: 60, hitEventId: 5);
        var k3 = CyPacketExtras.ComputeDamageKey(ownerId: 100, damageSource: 1, ownerLevel: 60, hitEventId: 5);
        // Type stamps embed in the leading digit; expect distinct keys.
        Assert.NotEqual(k1, k2);
        Assert.NotEqual(k2, k3);
        Assert.NotEqual(k1, k3);
        Assert.Equal(0L, CyPacketExtras.ComputeDamageKey(0, 0, 0, 0));
    }

    [Fact]
    public void ParseGameFrameHeadersSplitsConcatenatedFrames()
    {
        var frame1 = BuildEnvelope(MessageType.Notify, false, new byte[] { 0x14 });
        var frame2 = BuildEnvelope(MessageType.FrameDown, true, new byte[] { 0x00, 0x00 });
        var combined = frame1.Concat(frame2).ToArray();

        var headers = CyPacketExtras.ParseGameFrameHeaders(combined);
        Assert.Equal(2, headers.Count);
        Assert.Equal((int)MessageType.Notify, headers[0].MsgType);
        Assert.False(headers[0].IsZstd);
        Assert.Equal(new byte[] { 0x14 }, headers[0].Payload);
        Assert.Equal((int)MessageType.FrameDown, headers[1].MsgType);
        Assert.True(headers[1].IsZstd);
    }

    [Fact]
    public void ParseNotifyHeaderRequiresMatchingServiceUuid()
    {
        var payload = new byte[16 + 4];
        BinaryPrimitives.WriteUInt64BigEndian(payload.AsSpan(0, 8), 0xCAFEBABEu);
        BinaryPrimitives.WriteUInt32BigEndian(payload.AsSpan(12, 4), 0x14u);
        payload[16] = 0xDE;
        payload[17] = 0xAD;

        Assert.Null(CyPacketExtras.ParseNotifyHeader(payload, expectedServiceUuid: 0xDEADBEEF));

        var ok = CyPacketExtras.ParseNotifyHeader(payload, expectedServiceUuid: 0xCAFEBABEu);
        Assert.NotNull(ok);
        Assert.Equal(0x14, ok!.Value.MethodId);
        Assert.Equal(payload.Length - 16, ok.Value.Payload.Length);
    }

    [Fact]
    public void DecodeFieldsGroupsByFieldNumber()
    {
        // Manually build proto wire bytes: field 1 varint=42, field 2 length-delim "ok"
        var bytes = new List<byte>();
        bytes.Add((byte)((1 << 3) | 0)); bytes.Add(42);
        bytes.Add((byte)((2 << 3) | 2)); bytes.Add(2); bytes.Add((byte)'o'); bytes.Add((byte)'k');
        var fields = CyPacketExtras.DecodeFields(bytes.ToArray());
        Assert.Equal(2, fields.Count);
        Assert.Equal(42UL, fields[1][0]);
        var lenDelim = (byte[])fields[2][0];
        Assert.Equal("ok", System.Text.Encoding.UTF8.GetString(lenDelim));
    }

    private static byte[] BuildEnvelope(MessageType type, bool isZstd, byte[] payload)
    {
        var size = 4 + 2 + payload.Length;
        var frame = new byte[size];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)size);
        var rawType = (ushort)((isZstd ? 0x8000 : 0) | (ushort)type);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), rawType);
        Buffer.BlockCopy(payload, 0, frame, 6, payload.Length);
        return frame;
    }
}
