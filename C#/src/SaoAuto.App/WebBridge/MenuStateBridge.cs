using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// Provides the Python-compatible menu state payloads consumed by
/// <c>menu.html</c>'s AutoKey and BossRaid boot sync.
/// </summary>
public sealed class MenuStateBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly SettingsManager _settings;
    private readonly GameStateManager? _states;
    private readonly string[] _commands;
    private bool _disposed;

    public MenuStateBridge(BridgeRouter router, SettingsManager settings, GameStateManager? states = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _settings = settings ?? throw new ArgumentNullException(nameof(settings));
        _states = states;
        _commands = new[] { BridgeCommands.GetAutoKeyState, BridgeCommands.GetBossRaidState };
        _router.Register(BridgeCommands.GetAutoKeyState, _ => HandleAutoKeyState());
        _router.Register(BridgeCommands.GetBossRaidState, _ => HandleBossRaidState());
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var command in _commands)
            _router.Unregister(command);
    }

    private JsonObject HandleAutoKeyState()
        => new() { ["ok"] = true, ["state"] = BuildAutoKeyState(_settings, _states) };

    private JsonObject HandleBossRaidState()
        => new() { ["ok"] = true, ["state"] = BuildBossRaidState(_settings, _states) };

    internal static JsonObject BuildAutoKeyState(SettingsManager settings, GameStateManager? states)
    {
        var identityContext = CurrentIdentity(states);
        var config = AutoKeyConfigLoader.Load(settings, identityContext.Author);
        var active = AutoKeyProfileStore.ActiveProfile(config);
        var identity = IdentityObject(identityContext.Author, identityContext.Source);

        return new JsonObject
        {
            ["enabled"] = config.Enabled,
            ["active_profile_id"] = config.ActiveProfileId,
            ["active_profile_name"] = active?.ProfileName ?? string.Empty,
            ["profiles"] = AutoKeySummaries(config),
            ["profiles_full"] = AutoKeyProfilesFull(config),
            ["active_profile"] = active is null ? null : AutoKeyProfileObject(active),
            ["local_profile_count"] = config.Profiles.Length,
            ["server_url"] = string.IsNullOrWhiteSpace(config.ServerUrl)
                ? AutoKeyProfileSpec.DefaultServerUrl
                : config.ServerUrl,
            ["identity"] = identity.DeepClone(),
            ["upload_auth"] = AutoKeyUploadAuth(identity),
            ["last_remote_search"] = AutoKeySearchObject(config.LastRemoteSearch),
            ["runtime"] = new JsonObject(),
        };
    }

    internal static JsonObject BuildBossRaidState(SettingsManager settings, GameStateManager? states)
    {
        var identityContext = CurrentIdentity(states);
        using var authorDoc = JsonDocument.Parse(AuthorObject(identityContext.Author).ToJsonString());
        var config = BossRaidConfigStore.Load(settings, authorDoc.RootElement.Clone());
        var active = BossRaidProfile.ActiveProfile(config);
        var configNode = BossRaidConfigStore.ConfigToJsonObject(config);
        var profilesFull = CloneArray(configNode["profiles"] as JsonArray);
        var identity = IdentityObject(identityContext.Author, identityContext.Source);
        var serverUrl = string.IsNullOrWhiteSpace(config.ServerUrl)
            ? BossRaidProfile.DefaultServerUrl
            : config.ServerUrl;

        return new JsonObject
        {
            ["enabled"] = config.Enabled,
            ["active_profile_id"] = config.ActiveProfileId,
            ["active_profile_name"] = active?.ProfileName ?? string.Empty,
            ["profiles"] = BossRaidSummaries(config),
            ["profiles_full"] = profilesFull,
            ["active_profile"] = FindProfileNode(profilesFull, config.ActiveProfileId),
            ["local_profile_count"] = config.Profiles.Count,
            ["server_url"] = serverUrl,
            ["identity"] = identity.DeepClone(),
            ["upload_auth"] = BossRaidUploadAuth(identity, serverUrl),
            ["last_remote_search"] = CloneNode(configNode["last_remote_search"]) ?? new JsonObject(),
            ["runtime"] = new JsonObject(),
        };
    }

    private static IdentityContext CurrentIdentity(GameStateManager? states)
    {
        if (states is null)
            return new IdentityContext(AuthorSnapshot.Empty, "profile");
        var snapshot = states.Snapshot;
        return new IdentityContext(AutoKeyConfigLoader.AuthorFromState(snapshot), "packet");
    }

    private static JsonObject IdentityObject(AuthorSnapshot author, string source)
    {
        var missing = new JsonArray();
        if (string.IsNullOrWhiteSpace(author.PlayerUid)) missing.Add("player_uid");
        if (string.IsNullOrWhiteSpace(author.PlayerName)) missing.Add("player_name");
        if (author.ProfessionId <= 0) missing.Add("profession_id");

        return new JsonObject
        {
            ["player_uid"] = author.PlayerUid ?? string.Empty,
            ["player_name"] = author.PlayerName ?? string.Empty,
            ["profession_id"] = author.ProfessionId,
            ["profession_name"] = author.ProfessionName ?? string.Empty,
            ["source"] = string.IsNullOrWhiteSpace(source) ? "unknown" : source,
            ["ready"] = missing.Count == 0,
            ["missing"] = missing,
        };
    }

    private static JsonObject AuthorObject(AuthorSnapshot author) => new()
    {
        ["player_uid"] = author.PlayerUid ?? string.Empty,
        ["player_name"] = author.PlayerName ?? string.Empty,
        ["profession_id"] = author.ProfessionId,
        ["profession_name"] = author.ProfessionName ?? string.Empty,
    };

    private static JsonObject AutoKeyUploadAuth(JsonObject identity) => new()
    {
        ["ready"] = false,
        ["token_masked"] = string.Empty,
        ["expires_at"] = string.Empty,
        ["error"] = string.Empty,
        ["mode"] = string.Empty,
        ["identity"] = identity.DeepClone(),
    };

    private static JsonObject BossRaidUploadAuth(JsonObject identity, string serverUrl) => new()
    {
        ["token"] = string.Empty,
        ["ready"] = false,
        ["token_masked"] = string.Empty,
        ["expires_at"] = string.Empty,
        ["error"] = string.Empty,
        ["mode"] = string.Empty,
        ["identity"] = identity.DeepClone(),
        ["server_url"] = serverUrl,
    };

    private static JsonArray AutoKeySummaries(AutoKeyConfig config)
    {
        var arr = new JsonArray();
        foreach (var profile in config.Profiles)
        {
            var s = AutoKeyProfileStore.SummarizeProfile(profile);
            arr.Add(new JsonObject
            {
                ["id"] = s.Id,
                ["profile_name"] = s.ProfileName,
                ["description"] = s.Description,
                ["profession_id"] = s.ProfessionId,
                ["profession_name"] = s.ProfessionName,
                ["source"] = s.Source,
                ["remote_id"] = s.RemoteId is null ? null : JsonValue.Create(s.RemoteId),
                ["updated_at"] = s.UpdatedAt,
                ["action_count"] = s.ActionCount,
                ["enabled_action_count"] = s.EnabledActionCount,
            });
        }
        return arr;
    }

    private static JsonArray AutoKeyProfilesFull(AutoKeyConfig config)
    {
        var arr = new JsonArray();
        foreach (var profile in config.Profiles)
            arr.Add(AutoKeyProfileObject(profile));
        return arr;
    }

    private static JsonObject AutoKeyProfileObject(AutoKeyProfileSpecRecord profile)
    {
        using var doc = JsonDocument.Parse(AutoKeyProfileStore.ExportProfileJson(profile));
        var profileNode = doc.RootElement.GetProperty("profile").GetRawText();
        return JsonNode.Parse(profileNode)!.AsObject();
    }

    private static JsonObject AutoKeySearchObject(AutoKeyRemoteSearch search)
    {
        var results = new JsonArray();
        foreach (var result in search.Results)
            results.Add(JsonNode.Parse(result.GetRawText()));

        return new JsonObject
        {
            ["query"] = new JsonObject
            {
                ["q"] = search.Query.Q,
                ["profile_name"] = search.Query.ProfileName,
                ["player_uid"] = search.Query.PlayerUid,
                ["player_name"] = search.Query.PlayerName,
                ["profession_name"] = search.Query.ProfessionName,
                ["page"] = search.Query.Page,
                ["page_size"] = search.Query.PageSize,
            },
            ["results"] = results,
            ["error"] = search.Error,
            ["fetched_at"] = search.FetchedAt,
        };
    }

    private static JsonArray BossRaidSummaries(RaidConfig config)
    {
        var arr = new JsonArray();
        foreach (var profile in config.Profiles)
        {
            var s = BossRaidProfile.SummarizeProfile(profile);
            arr.Add(new JsonObject
            {
                ["id"] = s.Id,
                ["profile_name"] = s.ProfileName,
                ["description"] = s.Description,
                ["boss_total_hp"] = s.BossTotalHp,
                ["enrage_time_s"] = s.EnrageTimeS,
                ["simple_mode"] = s.SimpleMode,
                ["phase_count"] = s.PhaseCount,
                ["timeline_count"] = s.TimelineCount,
                ["source"] = s.Source,
                ["remote_id"] = s.RemoteId is null ? null : JsonValue.Create(s.RemoteId),
                ["updated_at"] = s.UpdatedAt,
            });
        }
        return arr;
    }

    private static JsonNode? FindProfileNode(JsonArray profiles, string id)
    {
        foreach (var node in profiles)
        {
            if (node is JsonObject obj
                && obj["id"]?.GetValue<string>() == id)
            {
                return CloneNode(obj);
            }
        }
        return null;
    }

    private static JsonArray CloneArray(JsonArray? array)
        => (CloneNode(array) as JsonArray) ?? new JsonArray();

    private static JsonNode? CloneNode(JsonNode? node)
        => node is null ? null : JsonNode.Parse(node.ToJsonString());

    private sealed record IdentityContext(AuthorSnapshot Author, string Source);
}
