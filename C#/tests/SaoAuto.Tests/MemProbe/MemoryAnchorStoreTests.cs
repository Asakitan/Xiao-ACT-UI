using System.Text.Json.Nodes;
using SaoAuto.MemProbe;

namespace SaoAuto.Tests.MemProbe;

public class MemoryAnchorStoreTests
{
    [Fact]
    public void ParseHexAcceptsStringAndInt()
    {
        Assert.Equal(0x1414571CB0UL, MemoryAnchorStore.ParseHex(JsonValue.Create("0x1414571CB0")));
        Assert.Equal(0x100UL, MemoryAnchorStore.ParseHex(JsonValue.Create((long)0x100)));
        Assert.Equal(0UL, MemoryAnchorStore.ParseHex(null));
        Assert.Equal(0UL, MemoryAnchorStore.ParseHex(JsonValue.Create("garbage")));
    }

    [Fact]
    public void GetIntFallsBack()
    {
        Assert.Equal(42, MemoryAnchorStore.GetInt(JsonValue.Create(42)));
        Assert.Equal(-1, MemoryAnchorStore.GetInt(null));
        Assert.Equal(7, MemoryAnchorStore.GetInt(null, fallback: 7));
        Assert.Equal(15, MemoryAnchorStore.GetInt(JsonValue.Create("15")));
    }

    [Fact]
    public void LoadSaveRoundTripPreservesUnknownKeys()
    {
        var path = Path.Combine(Path.GetTempPath(), $"anchors-{Guid.NewGuid():N}.json");
        try
        {
            File.WriteAllText(path, """{"smart_locator":{"last_self_obj":"0x1414571CB0","mystery":"keep_me"}}""");
            var store = MemoryAnchorStore.Load(path);
            Assert.Equal(0x1414571CB0UL, MemoryAnchorStore.ParseHex(store.SmartLocator["last_self_obj"]));
            store.SmartLocator["new_field"] = 99;
            store.Save();

            var reread = MemoryAnchorStore.Load(path);
            Assert.Equal("keep_me", MemoryAnchorStore.GetStr(reread.SmartLocator["mystery"]));
            Assert.Equal(99, MemoryAnchorStore.GetInt(reread.SmartLocator["new_field"]));
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void GetV2AnchorReturnsNestedBlock()
    {
        var path = Path.Combine(Path.GetTempPath(), $"anchors-{Guid.NewGuid():N}.json");
        try
        {
            File.WriteAllText(path,
                """{"smart_locator":{"anchors":{"scene_manager":{"obj_addr":"0x1234","scene_id_off":16}}}}""");
            var store = MemoryAnchorStore.Load(path);
            var a = store.GetV2Anchor("scene_manager");
            Assert.NotNull(a);
            Assert.Equal(0x1234UL, MemoryAnchorStore.ParseHex(a!["obj_addr"]));
            Assert.Equal(16, MemoryAnchorStore.GetInt(a["scene_id_off"]));
            Assert.Null(store.GetV2Anchor("missing"));
        }
        finally { File.Delete(path); }
    }
}
