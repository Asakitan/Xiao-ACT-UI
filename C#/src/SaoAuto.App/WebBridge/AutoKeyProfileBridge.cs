using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S156 — Registers AutoKey profile CRUD handlers on a
/// <see cref="BridgeRouter"/>, backed by a shared
/// <see cref="AutoKeyProfileService"/>. Handles:
///
/// <list type="bullet">
/// <item><c>autokey.set_enabled</c> — payload <c>{enabled:bool}</c>
///   → reply <c>{enabled:bool}</c>.</item>
/// <item><c>autokey.profile.list</c> — no payload → reply
///   <c>{active_id, enabled, profiles:[{id, name, description,
///   profession_id, profession_name, source, remote_id, updated_at,
///   action_count, enabled_action_count}]}</c>.</item>
/// <item><c>autokey.profile.set_active</c> — payload <c>{id:string}</c>
///   → reply <c>{active_id, changed:bool}</c>.</item>
/// <item><c>autokey.profile.delete</c> — payload <c>{id:string}</c>
///   → reply <c>{remaining:int, active_id}</c>.</item>
/// <item><c>autokey.profile.clone</c> — payload <c>{id:string}</c>
///   → reply <c>{cloned_id, profile_name}</c> or <c>{error}</c>
///   when source id is absent.</item>
/// </list>
///
/// Dispose unregisters every command, restoring the router to its
/// pre-attach state. Handlers re-throw nothing — the router's
/// per-handler try/catch maps exceptions to <c>handler_exception</c>
/// replies.
/// </summary>
public sealed class AutoKeyProfileBridge : IDisposable
{
    private readonly AutoKeyProfileService _service;
    private readonly BridgeRouter _router;
    private readonly string[] _commands;
    private bool _disposed;

    public AutoKeyProfileBridge(AutoKeyProfileService service, BridgeRouter router)
    {
        _service = service ?? throw new ArgumentNullException(nameof(service));
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _commands = new[]
        {
            BridgeCommands.SetAutoKeyEnabled,
            BridgeCommands.ListAutoKeyProfiles,
            BridgeCommands.SetAutoKeyActiveProfile,
            BridgeCommands.DeleteAutoKeyProfile,
            BridgeCommands.CloneAutoKeyProfile,
            BridgeCommands.UpsertAutoKeyProfile,
            BridgeCommands.ExportAutoKeyProfile,
            BridgeCommands.ImportAutoKeyProfile,
        };
        router.Register(BridgeCommands.SetAutoKeyEnabled, HandleSetEnabled);
        router.Register(BridgeCommands.ListAutoKeyProfiles, _ => HandleList());
        router.Register(BridgeCommands.SetAutoKeyActiveProfile, HandleSetActive);
        router.Register(BridgeCommands.DeleteAutoKeyProfile, HandleDelete);
        router.Register(BridgeCommands.CloneAutoKeyProfile, HandleClone);
        router.Register(BridgeCommands.UpsertAutoKeyProfile, HandleUpsert);
        router.Register(BridgeCommands.ExportAutoKeyProfile, HandleExport);
        router.Register(BridgeCommands.ImportAutoKeyProfile, HandleImport);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var name in _commands)
            _router.Unregister(name);
    }

    private JsonObject HandleSetEnabled(JsonObject? payload)
    {
        var enabled = payload?["enabled"]?.GetValue<bool>() ?? false;
        var cfg = _service.SetEnabled(enabled);
        return new JsonObject { ["enabled"] = cfg.Enabled };
    }

    private JsonObject HandleList()
    {
        var cfg = _service.Load();
        var profiles = new JsonArray();
        foreach (var p in cfg.Profiles)
        {
            var s = AutoKeyProfileStore.SummarizeProfile(p);
            profiles.Add(new JsonObject
            {
                ["id"] = s.Id,
                ["name"] = s.ProfileName,
                ["description"] = s.Description,
                ["profession_id"] = s.ProfessionId,
                ["profession_name"] = s.ProfessionName,
                ["source"] = s.Source,
                ["remote_id"] = s.RemoteId,
                ["updated_at"] = s.UpdatedAt,
                ["action_count"] = s.ActionCount,
                ["enabled_action_count"] = s.EnabledActionCount,
            });
        }
        return new JsonObject
        {
            ["active_id"] = cfg.ActiveProfileId,
            ["enabled"] = cfg.Enabled,
            ["profiles"] = profiles,
        };
    }

    private JsonObject HandleSetActive(JsonObject? payload)
    {
        var id = payload?["id"]?.GetValue<string>() ?? "";
        var before = _service.Load().ActiveProfileId;
        var cfg = _service.SetActive(id);
        return new JsonObject
        {
            ["active_id"] = cfg.ActiveProfileId,
            ["changed"] = cfg.ActiveProfileId != before,
        };
    }

    private JsonObject HandleDelete(JsonObject? payload)
    {
        var id = payload?["id"]?.GetValue<string>() ?? "";
        var cfg = _service.Delete(id);
        return new JsonObject
        {
            ["remaining"] = cfg.Profiles.Length,
            ["active_id"] = cfg.ActiveProfileId,
        };
    }

    private JsonObject HandleClone(JsonObject? payload)
    {
        var id = payload?["id"]?.GetValue<string>() ?? "";
        var cloned = _service.Clone(id);
        if (cloned is null)
            return new JsonObject { ["error"] = "source_not_found" };
        return new JsonObject
        {
            ["cloned_id"] = cloned.Id,
            ["profile_name"] = cloned.ProfileName,
        };
    }

    private JsonObject HandleUpsert(JsonObject? payload)
    {
        var profileNode = payload?["profile"];
        if (profileNode is not JsonObject)
            return new JsonObject { ["error"] = "missing_profile" };
        var activate = payload?["activate"]?.GetValue<bool>() ?? false;

        AutoKeyProfileSpecRecord normalized;
        using (var doc = JsonDocument.Parse(profileNode.ToJsonString()))
            normalized = AutoKeyProfileSpec.NormalizeProfile(doc.RootElement);

        var cfg = _service.Upsert(normalized, activate);
        return new JsonObject
        {
            ["id"] = normalized.Id,
            ["profile_name"] = normalized.ProfileName,
            ["activated"] = cfg.ActiveProfileId == normalized.Id,
            ["active_id"] = cfg.ActiveProfileId,
            ["total"] = cfg.Profiles.Length,
        };
    }

    private JsonObject HandleExport(JsonObject? payload)
    {
        var id = payload?["id"]?.GetValue<string>() ?? "";
        if (string.IsNullOrEmpty(id))
            return new JsonObject { ["error"] = "missing_id" };
        var profile = AutoKeyProfileStore.FindProfile(_service.Load(), id);
        if (profile is null)
            return new JsonObject { ["error"] = "not_found" };
        return new JsonObject
        {
            ["id"] = profile.Id,
            ["profile_name"] = profile.ProfileName,
            ["json"] = AutoKeyProfileStore.ExportProfileJson(profile),
        };
    }

    private JsonObject HandleImport(JsonObject? payload)
    {
        var raw = payload?["json"]?.GetValue<string>();
        if (string.IsNullOrWhiteSpace(raw))
            return new JsonObject { ["error"] = "missing_json" };
        var activate = payload?["activate"]?.GetValue<bool>() ?? false;

        AutoKeyProfileSpecRecord imported;
        try
        {
            using var doc = JsonDocument.Parse(raw);
            imported = AutoKeyProfileStore.ImportProfile(doc.RootElement);
        }
        catch (JsonException)
        {
            return new JsonObject { ["error"] = "invalid_json" };
        }
        var cfg = _service.Upsert(imported, activate);
        return new JsonObject
        {
            ["id"] = imported.Id,
            ["profile_name"] = imported.ProfileName,
            ["activated"] = cfg.ActiveProfileId == imported.Id,
            ["active_id"] = cfg.ActiveProfileId,
            ["total"] = cfg.Profiles.Length,
        };
    }
}
