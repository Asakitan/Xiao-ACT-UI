using System.Buffers.Binary;
using Google.Protobuf;
using SaoAuto.Core.Packets;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S67 — SyncContainerDirtyData skeleton decoder + custom binary stream
/// parser. Verifies CharBase/UserFightAttr/RoleLevel sub-fields round-trip,
/// header guards drop bad input, unsupported fields silently skip.
/// </summary>
public class Session67DecoderTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    /// <summary>
    /// Build a single-tuple dirty stream: header(0xFFFFFFFE + pad) + fieldIndex
    /// + sub-header(0xFFFFFFFE + pad) + subField + payload bytes.
    /// </summary>
    private static byte[] BuildStream(int fieldIndex, int subField, byte[] payload)
    {
        var buf = new byte[8 + 4 + 8 + 4 + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(0, 4), 0xFFFFFFFEu);
        // bytes 4..7 padding (zero)
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(8, 4), (uint)fieldIndex);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(12, 4), 0xFFFFFFFEu);
        // bytes 16..19 padding
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(20, 4), (uint)subField);
        Buffer.BlockCopy(payload, 0, buf, 24, payload.Length);
        return buf;
    }

    private static byte[] PayloadU32(uint v)
    {
        var b = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(b, v);
        return b;
    }

    private static byte[] PayloadString(string s)
    {
        var raw = System.Text.Encoding.UTF8.GetBytes(s);
        var b = new byte[4 + raw.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(0, 4), (uint)raw.Length);
        Buffer.BlockCopy(raw, 0, b, 4, raw.Length);
        return b;
    }

    private static byte[] WrapAsProto(byte[] stream)
    {
        var msg = new SyncContainerDirtyData
        {
            VData = new BufferStream { Buffer = ByteString.CopyFrom(stream) },
        };
        return msg.ToByteArray();
    }

    [Fact]
    public void ContainerDirty_charBaseName_extractsString()
    {
        var stream = BuildStream(2, 5, PayloadString("Yui"));
        ContainerDirtyEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(stream), 1.0,
            ev => { if (ev is ContainerDirtyEvent c) captured = c; });
        Assert.NotNull(captured);
        Assert.Equal(2, captured!.Change.FieldIndex);
        Assert.Equal(5, captured.Change.SubField);
        Assert.Equal("Yui", captured.Change.StringValue);
    }

    [Fact]
    public void ContainerDirty_charBaseFightPoint_extractsU32()
    {
        var stream = BuildStream(2, 35, PayloadU32(54321));
        ContainerDirtyEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(stream), 0.0,
            ev => { if (ev is ContainerDirtyEvent c) captured = c; });
        Assert.NotNull(captured);
        Assert.Equal(54321L, captured!.Change.IntValue);
    }

    [Fact]
    public void ContainerDirty_userFightAttr_curHpAndMaxHp()
    {
        var s1 = BuildStream(16, 1, PayloadU32(1234));
        ContainerDirtyEvent? cur = null;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(s1), 0.0,
            ev => { if (ev is ContainerDirtyEvent c) cur = c; });
        Assert.NotNull(cur);
        Assert.Equal((16, 1), (cur!.Change.FieldIndex, cur.Change.SubField));
        Assert.Equal(1234L, cur.Change.IntValue);

        var s2 = BuildStream(16, 2, PayloadU32(9999));
        ContainerDirtyEvent? max = null;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(s2), 0.0,
            ev => { if (ev is ContainerDirtyEvent c) max = c; });
        Assert.NotNull(max);
        Assert.Equal(9999L, max!.Change.IntValue);
    }

    [Fact]
    public void ContainerDirty_originEnergy_carriesBothFloatAndIntInterp()
    {
        // 4 raw bytes interpret as both u32 and f32 — match Python.
        var raw = new byte[4];
        BinaryPrimitives.WriteSingleLittleEndian(raw, 75.5f);
        var stream = BuildStream(16, 3, raw);
        ContainerDirtyEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(stream), 0.0,
            ev => { if (ev is ContainerDirtyEvent c) captured = c; });
        Assert.NotNull(captured);
        Assert.Equal(75.5f, captured!.Change.FloatValue);
        Assert.NotNull(captured.Change.IntValue);
    }

    [Fact]
    public void ContainerDirty_roleLevel_extractsLevel()
    {
        var stream = BuildStream(22, 1, PayloadU32(75));
        ContainerDirtyEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(stream), 0.0,
            ev => { if (ev is ContainerDirtyEvent c) captured = c; });
        Assert.NotNull(captured);
        Assert.Equal(75L, captured!.Change.IntValue);
    }

    [Fact]
    public void ContainerDirty_unsupportedFieldIndex_dropsSilently()
    {
        // SeasonCenter (50) — supported in Python but skeleton skips it.
        var stream = BuildStream(50, 2, PayloadU32(10));
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(stream), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void ContainerDirty_badHeaderTag_dropsSilently()
    {
        var stream = new byte[] { 0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0, 2, 0, 0, 0 };
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(stream), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void ContainerDirty_shortBuffer_dropsSilently()
    {
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, WrapAsProto(new byte[3]), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void ContainerDirty_missingVData_dropsSilently()
    {
        var msg = new SyncContainerDirtyData();
        var fired = false;
        Registry.Dispatch(NotifyMethod.SyncContainerDirtyData, msg.ToByteArray(), 0.0,
            _ => fired = true);
        Assert.False(fired);
    }

    [Fact]
    public void ContainerDirty_isRegistered()
    {
        Assert.True(Registry.TryGet(NotifyMethod.SyncContainerDirtyData, out var d));
        Assert.Equal(NotifyMethod.SyncContainerDirtyData, d.MethodId);
    }
}
