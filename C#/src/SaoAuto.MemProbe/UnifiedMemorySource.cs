using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using SaoAuto.Core.State;
using SaoAuto.MemProbe.Watchers;

namespace SaoAuto.MemProbe;

/// <summary>
/// PacketBridge-compatible facade integrating all live mem watchers.
/// Mirrors Python <c>mem_probe.unified_source.UnifiedDataSource</c>.
///
/// <para>Scope note: SmartLocator (~1.6 kLoC of IL2CPP heuristics) is
/// deferred — anchors.json is loaded directly as a "warm-start" path. If
/// anchors are missing the source reports <c>status=error</c> and the
/// caller is expected to fall back to the TCP packet bridge.</para>
/// </summary>
public sealed class UnifiedMemorySource : IDisposable
{
    public enum Mode { Auto, Memory, Hybrid }

    public Action<SelfSnapshot>? OnSelfUpdate { get; set; }
    public Action<SceneEvent>? OnSceneChange { get; set; }
    public Action<EntityUpdate>? OnMonsterUpdate { get; set; }
    public Action<BossEvent>? OnBossEvent { get; set; }
    public Action<bool>? OnCombatChange { get; set; }
    public Action<string, string>? OnStatusChange { get; set; }

    public Mode SelectedMode { get; }
    public string Status { get; private set; } = "init";
    public string LastError { get; private set; } = "";
    public bool Started => _started;

    private readonly IMemorySource _pm;
    private readonly MemoryAnchorStore _anchors;
    private readonly GameStateManager? _state;
    private readonly ILoggerFactory _lf;
    private readonly ILogger _log;
    private readonly object _lock = new();

    private MemSelfWatcher? _self;
    private MemSceneWatcher? _scene;
    private MemEntityWatcher? _entity;
    private MemCombatWatcher? _combat;
    private bool _started;

    public UnifiedMemorySource(IMemorySource pm, MemoryAnchorStore anchors,
        GameStateManager? state = null,
        Mode mode = Mode.Auto,
        ILoggerFactory? loggerFactory = null)
    {
        _pm = pm;
        _anchors = anchors;
        _state = state;
        SelectedMode = mode;
        _lf = loggerFactory ?? NullLoggerFactory.Instance;
        _log = _lf.CreateLogger<UnifiedMemorySource>();
    }

    public bool Start()
    {
        if (_started) return true;
        try
        {
            SetStatus("starting");
            SpinUpWatchers();
            _started = true;
            SetStatus("running");
            return true;
        }
        catch (Exception ex)
        {
            SetStatus("error", $"watchers init failed: {ex.Message}");
            _log.LogWarning(ex, "[UnifiedMem] start");
            return false;
        }
    }

    public void Stop()
    {
        foreach (var w in new IDisposable?[] { _self, _scene, _entity, _combat })
        {
            try { w?.Dispose(); } catch { }
        }
        _self = null; _scene = null; _entity = null; _combat = null;
        _started = false;
    }

    private void SpinUpWatchers()
    {
        // SELF
        var selfCfg = MemSelfWatcher.FromAnchors(_anchors);
        if (selfCfg is not null && selfCfg.CharObj != 0)
        {
            _self = new MemSelfWatcher(_pm, selfCfg, _lf.CreateLogger<MemSelfWatcher>())
            {
                OnSelfUpdate = HandleSelfUpdate,
                OnStatusChange = SetStatus,
                OnRelocateNeeded = TriggerRelocate,
            };
            _self.Start();
        }

        // SCENE
        var sceneAnchor = _anchors.GetV2Anchor("scene_manager");
        if (sceneAnchor is not null)
        {
            var addr = MemoryAnchorStore.ParseHex(sceneAnchor["obj_addr"]);
            if (addr != 0)
            {
                var cfg = new SceneReadConfig
                {
                    ObjAddr = addr,
                    SceneIdOff = MemoryAnchorStore.GetInt(sceneAnchor["scene_id_off"]),
                    DungeonIdOff = MemoryAnchorStore.GetInt(sceneAnchor["dungeon_id_off"]),
                    LayerOff = MemoryAnchorStore.GetInt(sceneAnchor["layer_off"]),
                };
                _scene = new MemSceneWatcher(_pm, cfg, _lf.CreateLogger<MemSceneWatcher>())
                {
                    OnSceneChange = HandleSceneChange,
                    OnStatusChange = SetStatus,
                };
                _scene.Start();
            }
        }

        // ENTITY (monster)
        var entityAnchor = _anchors.GetV2Anchor("entity_collection");
        if (entityAnchor is not null)
        {
            var monsterKlass = MemoryAnchorStore.ParseHex(entityAnchor["monster_klass_ptr"]);
            if (monsterKlass != 0)
            {
                var uuidOff = MemoryAnchorStore.GetInt(entityAnchor["uuid_off"], 0x10);
                var attrSlotOff = MemoryAnchorStore.GetInt(entityAnchor["attr_slot_off"]);
                var hpOff = MemoryAnchorStore.GetInt(entityAnchor["hp_off"]);
                var maxHpOff = MemoryAnchorStore.GetInt(entityAnchor["max_hp_off"], 0x20);
                var hpWidth = MemoryAnchorStore.GetInt(entityAnchor["hp_width"], 4);
                var effectiveHpOff = hpOff >= 0 ? hpOff : maxHpOff;
                var nested = attrSlotOff >= 0;
                var objBody = Math.Max(uuidOff, attrSlotOff + 8) + 0x40;
                var attrBody = Math.Max(effectiveHpOff, maxHpOff) + 0x80;
                if (!nested) objBody = Math.Max(objBody, attrBody);

                var specs = new List<(string, int, int)>
                {
                    ("uuid", uuidOff, 8),
                    (nested ? "attr.hp" : "hp", effectiveHpOff, hpWidth),
                    (nested ? "attr.max_hp" : "max_hp", maxHpOff, hpWidth),
                };
                foreach (var optName in new[] { "is_dead_off", "profession_id_off", "extinction_off", "max_extinction_off" })
                {
                    var v = MemoryAnchorStore.GetInt(entityAnchor[optName]);
                    if (v >= 0) specs.Add((optName.Replace("_off", ""), v, 4));
                }
                var enc = MemoryAnchorStore.GetStr(entityAnchor["hp_encoding"]) ?? "i32";
                var encMap = new Dictionary<string, string>();
                if (enc is "f32" or "f64") { encMap["hp"] = enc; encMap["max_hp"] = enc; }

                var entityCfg = new EntityReadConfig
                {
                    KlassPtr = monsterKlass,
                    FieldSpecs = specs,
                    BodySize = objBody,
                    Name = "monster",
                    AttrSlotOff = attrSlotOff,
                    AttrBodySize = attrBody,
                    FieldEncodings = encMap,
                };
                _entity = new MemEntityWatcher(_pm, new[] { entityCfg }, _lf.CreateLogger<MemEntityWatcher>())
                {
                    OnMonsterUpdate = HandleMonsterUpdate,
                    OnStatusChange = SetStatus,
                };
                _entity.Start();
            }
        }

        // COMBAT (in_combat + buff diff). Needs the SELF user_fight_attr.
        ulong attrObj = 0;
        var selfAnchor = _anchors.GetV2Anchor("self");
        if (selfAnchor?["substructs"]?.AsObject()?["user_fight_attr"]?.AsObject() is { } ufa)
            attrObj = MemoryAnchorStore.ParseHex(ufa["obj_addr"]);
        if (attrObj == 0)
            attrObj = MemoryAnchorStore.ParseHex(_anchors.SmartLocator["last_user_fight_attr"]);

        if (attrObj != 0)
        {
            var combatCfg = new CombatReadConfig { SelfAttrObj = attrObj };
            _combat = new MemCombatWatcher(_pm, combatCfg,
                entityProvider: _entity is null ? null : () => _entity.Entities(),
                logger: _lf.CreateLogger<MemCombatWatcher>())
            {
                OnCombatChange = HandleCombatChange,
                OnBossEvent = HandleBossEvent,
                OnStatusChange = SetStatus,
            };
            _combat.Start();
        }
    }

    private void HandleSelfUpdate(SelfSnapshot snap)
    {
        try
        {
            _state?.Update(s => s with
            {
                HpCurrent = (int)snap.Hp,
                HpMax = (int)snap.MaxHp,
                LevelBase = snap.Level,
                ProfessionId = snap.ProfessionId,
                FightPoint = (int)snap.FightPoint,
                StaminaMax = snap.StaminaMax,
            });
        }
        catch (Exception ex) { _log.LogWarning(ex, "state.Update self"); }
        try { OnSelfUpdate?.Invoke(snap); } catch (Exception ex) { _log.LogWarning(ex, "OnSelfUpdate"); }
    }

    private void HandleSceneChange(SceneEvent ev)
    {
        try { OnSceneChange?.Invoke(ev); } catch (Exception ex) { _log.LogWarning(ex, "OnSceneChange"); }
    }

    private void HandleMonsterUpdate(EntityUpdate ev)
    {
        try { OnMonsterUpdate?.Invoke(ev); } catch (Exception ex) { _log.LogWarning(ex, "OnMonsterUpdate"); }
    }

    private void HandleBossEvent(BossEvent ev)
    {
        try { OnBossEvent?.Invoke(ev); } catch (Exception ex) { _log.LogWarning(ex, "OnBossEvent"); }
    }

    private void HandleCombatChange(bool inCombat)
    {
        try { _state?.Update(s => s with { InCombat = inCombat }); }
        catch (Exception ex) { _log.LogWarning(ex, "state.Update in_combat"); }
        try { OnCombatChange?.Invoke(inCombat); } catch (Exception ex) { _log.LogWarning(ex, "OnCombatChange"); }
    }

    private void SetStatus(string status, string error = "")
    {
        lock (_lock)
        {
            Status = status;
            if (!string.IsNullOrEmpty(error)) LastError = error;
        }
        try { OnStatusChange?.Invoke(status, error); } catch { }
    }

    /// <summary>
    /// Stub for SELF re-locate. Without SmartLocator we just re-read
    /// anchors.json from disk and rebuild the SELF watcher's config.
    /// </summary>
    private void TriggerRelocate()
    {
        if (_self is null || _anchors.Path is null) return;
        try
        {
            SetStatus("relocating");
            var fresh = MemoryAnchorStore.Load(_anchors.Path);
            var newCfg = MemSelfWatcher.FromAnchors(fresh);
            if (newCfg is null || newCfg.CharObj == 0)
            {
                SetStatus("error", "relocate: anchors.json missing self refs");
                return;
            }
            _self.Stop();
            _self = new MemSelfWatcher(_pm, newCfg, _lf.CreateLogger<MemSelfWatcher>())
            {
                OnSelfUpdate = HandleSelfUpdate,
                OnStatusChange = SetStatus,
                OnRelocateNeeded = TriggerRelocate,
            };
            _self.Start();
            SetStatus("running");
        }
        catch (Exception ex) { SetStatus("error", $"relocate failed: {ex.Message}"); }
    }

    public object Health() => new
    {
        started = _started,
        mode = SelectedMode.ToString(),
        status = Status,
        last_error = LastError,
        watchers = new
        {
            self = _self?.Health(),
            scene = _scene?.Health(),
            entity = _entity?.Health(),
            combat = _combat?.Health(),
        },
    };

    public void Dispose() => Stop();
}
