using Google.Protobuf;
using SaoAuto.Core.Bridge;
using SaoAuto.Core.Packets;
using SaoAuto.Core.State;
using Star;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// S114 — extend <c>ExtractMonsterCoreAttrs</c> with NAME (id=1, utf-8
/// length-delimited string) and ID (id=10, varint template_id).
/// Decoder writes the new fields onto <see cref="EntityAppearance"/>
/// and <see cref="NearDeltaAttrUpdate"/>; bridge propagates to
/// <see cref="MonsterData"/>. Name uses first-non-empty-wins so a
/// delta with an empty name doesn't blank a previously seen name.
/// </summary>
public class Session114NameTemplateIdTests
{
    private static readonly MethodDecoderRegistry Registry =
        MethodDecoderRegistry.BuildDefault();

    private static ByteString Varint(int value)
    {
        var buf = new List<byte>(5);
        var v = (uint)value;
        while (v >= 0x80) { buf.Add((byte)(v | 0x80)); v >>= 7; }
        buf.Add((byte)v);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static ByteString StringRaw(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s);
        var buf = new List<byte>(5 + bytes.Length);
        // varint length prefix
        var len = (uint)bytes.Length;
        while (len >= 0x80) { buf.Add((byte)(len | 0x80)); len >>= 7; }
        buf.Add((byte)len);
        buf.AddRange(bytes);
        return ByteString.CopyFrom(buf.ToArray());
    }

    private static Attr A(int id, ByteString raw) => new() { Id = id, RawData = raw };

    private static AoiSyncDelta Delta(long uuid, params Attr[] attrs)
    {
        var d = new AoiSyncDelta { Uuid = uuid, Attrs = new AttrCollection() };
        foreach (var a in attrs) d.Attrs.Attrs.Add(a);
        return d;
    }

    [Fact]
    public void Decoder_UnpacksNameAndTemplateId_OnDelta()
    {
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7,
            A(1, StringRaw("BossKirito")),
            A(10, Varint(98765))));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        var u = Assert.Single(captured!.AttrUpdates);
        Assert.Equal("BossKirito", u.Name);
        Assert.True(u.HasName);
        Assert.Equal(98765, u.TemplateId);
        Assert.True(u.HasTemplateId);
        Assert.False(u.HasCurHp);
    }

    [Fact]
    public void Decoder_UnpacksNameOnAppearance()
    {
        var msg = new SyncNearEntities();
        msg.Appear.Add(new Entity
        {
            Uuid = 7,
            EntType = EEntityType.Entmonster,
            Attrs = new AttrCollection
            {
                Attrs =
                {
                    A(1, StringRaw("Heathcliff")),
                    A(10, Varint(42)),
                    A(11310, Varint(50_000)),
                },
            },
        });

        NearEntitiesEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearEntities, msg.ToByteArray(), 1.0,
            e => { if (e is NearEntitiesEvent n) captured = n; });

        Assert.NotNull(captured);
        var app = Assert.Single(captured!.Appear);
        Assert.Equal("Heathcliff", app.Name);
        Assert.Equal(42, app.TemplateId);
        Assert.Equal(50_000, app.CurHp);
    }

    [Fact]
    public void Decoder_EmptyNamePayload_IsEmptyString()
    {
        // Length-prefix 0 → empty string. Falls through the explicit
        // length branch and the whole-payload-utf8 fallback both yield
        // empty.
        var msg = new SyncNearDeltaInfo();
        msg.DeltaInfos.Add(Delta(7, A(1, ByteString.CopyFrom(new byte[] { 0x00 }))));

        NearDeltaEvent? captured = null;
        Registry.Dispatch(NotifyMethod.SyncNearDeltaInfo, msg.ToByteArray(), 1.0,
            e => { if (e is NearDeltaEvent n) captured = n; });

        Assert.NotNull(captured);
        // Empty-name delta still emits an update because HasName=true
        // (the attr was on the wire); bridge will keep the existing
        // name via first-non-empty-wins.
        var u = Assert.Single(captured!.AttrUpdates);
        Assert.Equal(string.Empty, u.Name);
        Assert.True(u.HasName);
    }

    [Fact]
    public void Bridge_AppearanceWritesNameTemplateIdToMonsterData()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[]
            {
                new EntityAppearance(7, (int)EEntityType.Entmonster)
                {
                    Name = "Heathcliff",
                    TemplateId = 42,
                    CurHp = 5_000,
                },
            },
            Array.Empty<EntityDisappearance>(), 1.0));

        var mon = state.Snapshot.MonsterDataMap[7];
        Assert.Equal("Heathcliff", mon.Name);
        Assert.Equal(42, mon.TemplateId);
    }

    [Fact]
    public void Bridge_DeltaUpdatesTemplateId()
    {
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, 0, 0, 0)
                {
                    TemplateId = 99,
                    HasTemplateId = true,
                },
            },
        });

        Assert.Equal(99, state.Snapshot.MonsterDataMap[7].TemplateId);
    }

    [Fact]
    public void Bridge_FirstNonEmptyNameWins()
    {
        // Lazy-create with empty name → delta with real name fills it
        // → second delta with empty name does NOT blank it.
        var state = new GameStateManager();
        var bridge = new PacketBridge(state, new PacketParser());
        bridge.Apply(new NearEntitiesEvent(
            new[] { new EntityAppearance(7, (int)EEntityType.Entmonster) { CurHp = 1 } },
            Array.Empty<EntityDisappearance>(), 1.0));
        Assert.Equal(string.Empty, state.Snapshot.MonsterDataMap[7].Name);

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 2.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, 0, 0, 0) { Name = "Asuna", HasName = true },
            },
        });
        Assert.Equal("Asuna", state.Snapshot.MonsterDataMap[7].Name);

        bridge.Apply(new NearDeltaEvent(new[] { 7L }, 3.0)
        {
            AttrUpdates = new[]
            {
                new NearDeltaAttrUpdate(7, 0, 0, 0) { Name = string.Empty, HasName = true },
            },
        });
        Assert.Equal("Asuna", state.Snapshot.MonsterDataMap[7].Name);
    }
}
