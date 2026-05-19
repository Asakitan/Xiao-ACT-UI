using System.Buffers.Binary;
using SaoAuto.MemProbe;
using SaoAuto.MemProbe.Watchers;

namespace SaoAuto.Tests.MemProbe;

public class WatcherFixtureTests
{
    /// <summary>
    /// Build a SELF char obj at 0x10000 with attr ptr at +0x18 → 0x20000.
    /// Reads via MemSelfWatcher.ReadSnapshot through the public Health/Latest path.
    /// </summary>
    [Fact]
    public void SelfWatcher_ReadsHpFromNestedAttr()
    {
        using var pm = new FixtureMemorySource();
        var charBlob = new byte[0x300];
        BinaryPrimitives.WriteUInt64LittleEndian(charBlob.AsSpan(0x00, 8), 0xC0DECAFEUL); // uid
        BinaryPrimitives.WriteUInt64LittleEndian(charBlob.AsSpan(0x18, 8), 0x20000UL);    // attr ptr
        pm.Map(0x10000, charBlob);

        var attrBlob = new byte[0x100];
        BinaryPrimitives.WriteInt32LittleEndian(attrBlob.AsSpan(0x40, 4), 7777);   // cur_hp
        BinaryPrimitives.WriteInt32LittleEndian(attrBlob.AsSpan(0x48, 4), 9999);   // max_hp
        pm.Map(0x20000, attrBlob);

        var cfg = new SelfReadConfig
        {
            CharObj = 0x10000, UidOff = 0x00, AttrSlotOff = 0x18,
            CurHpOff = 0x40, MaxHpOff = 0x48, HpWidth = 4,
            CharId = 0xC0DECAFEUL,
        };
        using var w = new MemSelfWatcher(pm, cfg);
        SelfSnapshot? captured = null;
        w.OnSelfUpdate = s => captured = s;
        w.Start();
        WaitFor(() => captured is not null, 1000);
        w.Stop();

        Assert.NotNull(captured);
        Assert.Equal(0xC0DECAFEUL, captured!.Uid);
        Assert.Equal(7777, captured.Hp);
        Assert.Equal(9999, captured.MaxHp);
    }

    [Fact]
    public void SceneWatcher_ClassifiesAllTransitions()
    {
        using var pm = new FixtureMemorySource();
        var cfg = new SceneReadConfig { ObjAddr = 0x10000, SceneIdOff = 0, DungeonIdOff = 4, LayerOff = 8 };
        using var w = new MemSceneWatcher(pm, cfg);
        var events = new List<SceneEvent>();
        w.OnSceneChange = ev => events.Add(ev);

        w.DetectChange((Scene: 100, Dungeon: 0, Layer: 0));    // seed
        w.DetectChange((Scene: 200, Dungeon: 50, Layer: 1));   // dungeon_enter
        w.DetectChange((Scene: 201, Dungeon: 50, Layer: 2));   // layer_change
        w.DetectChange((Scene: 100, Dungeon: 0, Layer: 0));    // dungeon_leave
        w.DetectChange((Scene: 100, Dungeon: 0, Layer: 5));    // scene_restart

        Assert.Equal(4, events.Count);
        Assert.Equal("dungeon_enter", events[0].Reason);
        Assert.Equal("layer_change", events[1].Reason);
        Assert.True(events[1].PreserveCombat);
        Assert.Equal("dungeon_leave", events[2].Reason);
        Assert.Equal(50, events[2].DungeonId);  // uses prev_d
        Assert.Equal("scene_restart", events[3].Reason);
    }

    [Fact]
    public void EntityWatcher_ScansKlassAndReadsFields()
    {
        using var pm = new FixtureMemorySource();
        const ulong klass = 0xCAFEBABE_DEADBEEFUL;
        // monster obj at offset +16 inside region; klass slot at +0
        var region = new byte[256];
        BinaryPrimitives.WriteUInt64LittleEndian(region.AsSpan(16, 8), klass);
        // uuid at obj_off+0x10, hp at +0x20 (FLAT layout for simplicity)
        BinaryPrimitives.WriteUInt64LittleEndian(region.AsSpan(16 + 0x10, 8), 4242UL);
        BinaryPrimitives.WriteInt32LittleEndian(region.AsSpan(16 + 0x20, 4), 333);
        BinaryPrimitives.WriteInt32LittleEndian(region.AsSpan(16 + 0x24, 4), 999);
        pm.Map(0x100000, region);

        var cfg = new EntityReadConfig
        {
            KlassPtr = klass,
            FieldSpecs = new[] { ("uuid", 0x10, 8), ("hp", 0x20, 4), ("max_hp", 0x24, 4) },
            BodySize = 0x40,
            AttrSlotOff = -1,  // FLAT
        };
        using var w = new MemEntityWatcher(pm, new[] { cfg });
        var addrs = w.ScanKlass(klass).ToList();
        Assert.Single(addrs);
        Assert.Equal(0x100000UL + 16, addrs[0]);

        EntityUpdate? captured = null;
        w.OnMonsterUpdate = u => captured = u;
        // Inject the discovered obj into the watcher state via discovery+fast pass.
        w.Start();
        WaitFor(() => captured is not null, 1500);
        w.Stop();

        Assert.NotNull(captured);
        Assert.Equal(4242UL, captured!.Uuid);
        Assert.Equal(333, captured.Hp);
        Assert.Equal(999, captured.MaxHp);
    }

    [Fact]
    public void CombatWatcher_DetectsInCombatAndBuffEvent()
    {
        using var pm = new FixtureMemorySource();
        var attr = new byte[0x100];
        BinaryPrimitives.WriteInt32LittleEndian(attr.AsSpan(0x40, 4), 1);
        pm.Map(0x30000, attr);

        var cfg = new CombatReadConfig { SelfAttrObj = 0x30000, InCombatOff = 0x40 };
        using var w = new MemCombatWatcher(pm, cfg);
        bool? captured = null;
        w.OnCombatChange = b => captured = b;
        w.PollInCombat();
        Assert.True(captured);

        // toggle off
        BinaryPrimitives.WriteInt32LittleEndian(attr.AsSpan(0x40, 4), 0);
        w.PollInCombat();
        Assert.False(captured);
    }

    private static void WaitFor(Func<bool> cond, int maxMs)
    {
        var t = Environment.TickCount;
        while (Environment.TickCount - t < maxMs)
        {
            if (cond()) return;
            Thread.Sleep(20);
        }
    }
}
