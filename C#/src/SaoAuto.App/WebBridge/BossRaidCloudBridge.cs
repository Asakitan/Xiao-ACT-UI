using System.Collections.Immutable;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S179 — Registers the four Boss-Raid cloud commands on a
/// <see cref="BridgeRouter"/>, backed by a
/// <see cref="BossRaidCloudClient"/>.
///
/// <list type="bullet">
/// <item><c>bossraid.cloud.search</c> — payload <c>{query:{q,page,page_size,...}}</c>
///   → reply <c>{ok, data?, error?}</c></item>
/// <item><c>bossraid.cloud.get</c> — payload <c>{id:string}</c>
///   → reply <c>{ok, data?, error?}</c></item>
/// <item><c>bossraid.cloud.issue_token</c> — payload arbitrary object
///   → reply <c>{ok, data?, error?}</c></item>
/// <item><c>bossraid.cloud.upload</c> — payload <c>{token:string, profile:object}</c>
///   → reply <c>{ok, data?, error?}</c></item>
/// </list>
///
/// Unlike <see cref="AutoKeyCloudClient"/> (which throws on failure),
/// <see cref="BossRaidCloudClient"/> returns a synthetic
/// <c>{error:"..."}</c> object. This bridge normalises both posture
/// types into the same <c>{ok, data?, error?}</c> reply envelope so
/// JS doesn't have to branch on which underlying client served the
/// request.
/// </summary>
public sealed class BossRaidCloudBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly BossRaidCloudClient _client;
    private readonly SettingsManager? _settings;
    private readonly Func<SettingsManager, BossRaidCloudClient>? _clientFromSettings;
    private readonly GameStateManager? _states;
    private readonly Func<DateTimeOffset> _clock;
    private readonly string[] _commands;
    private string _uploadToken = string.Empty;
    private string _uploadExpiresAt = string.Empty;
    private string _uploadMode = string.Empty;
    private string _uploadIdentitySignature = string.Empty;
    private string _uploadServerUrl = string.Empty;
    private bool _disposed;

    public BossRaidCloudBridge(
        BridgeRouter router,
        BossRaidCloudClient client,
        SettingsManager? settings = null,
        Func<DateTimeOffset>? clock = null,
        Func<SettingsManager, BossRaidCloudClient>? clientFromSettings = null,
        GameStateManager? states = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _client = client ?? throw new ArgumentNullException(nameof(client));
        _settings = settings;
        _clientFromSettings = clientFromSettings;
        _states = states;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _commands = new[]
        {
            BridgeCommands.SearchBossRaids,
            BridgeCommands.GetBossRaid,
            BridgeCommands.DownloadBossRaidRemote,
            BridgeCommands.RefreshBossRaidUploadAuth,
            BridgeCommands.IssueBossRaidUploadToken,
            BridgeCommands.UploadBossRaid,
            BridgeCommands.SetBossRaidServerUrl,
        };
        router.Register(BridgeCommands.SearchBossRaids, HandleSearch);
        router.Register(BridgeCommands.GetBossRaid, HandleGet);
        router.Register(BridgeCommands.DownloadBossRaidRemote, HandleDownload);
        router.Register(BridgeCommands.RefreshBossRaidUploadAuth, HandleRefreshUploadAuth);
        router.Register(BridgeCommands.IssueBossRaidUploadToken, HandleIssueToken);
        router.Register(BridgeCommands.UploadBossRaid, HandleUpload);
        router.Register(BridgeCommands.SetBossRaidServerUrl, HandleSetServerUrl);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var name in _commands)
            _router.Unregister(name);
    }

    private JsonObject HandleSearch(JsonObject? payload)
    {
        var query = new Dictionary<string, string?>();
        var qNode = payload?["query"] as JsonObject;
        if (qNode is not null)
        {
            foreach (var kv in qNode)
            {
                var v = kv.Value;
                query[kv.Key] = v switch
                {
                    null => null,
                    JsonValue jv => jv.TryGetValue<string>(out var s) ? s : v.ToJsonString().Trim('"'),
                    _ => v.ToJsonString(),
                };
            }
        }
        var reply = InvokeWithClient((client, ct) => client.SearchAsync(query, ct));
        if (_settings is not null && reply["ok"]?.GetValue<bool>() == true)
        {
            PersistSearch(qNode, reply["data"]);
            var results = ExtractResultsArray(reply["data"]);
            reply["results"] = CloneArray(results);
            reply["state"] = MenuStateBridge.BuildBossRaidState(_settings, _states);
        }
        return reply;
    }

    private void PersistSearch(JsonObject? queryNode, JsonNode? data)
    {
        if (_settings is null) return;
        try
        {
            var config = BossRaidConfigStore.Load(_settings);
            var results = new List<JsonElement>();
            var arr = ExtractResultsArray(data);
            if (arr.Count > 0)
            {
                foreach (var item in arr)
                {
                    using var doc = JsonDocument.Parse((item ?? new JsonObject()).ToJsonString());
                    results.Add(doc.RootElement.Clone());
                }
            }
            var newSearch = config.LastRemoteSearch with
            {
                Query = BuildQueryRecord(queryNode, config.LastRemoteSearch.Query),
                Results = results,
                Error = string.Empty,
                FetchedAt = _clock().ToString("yyyy-MM-ddTHH:mm:ssZ", System.Globalization.CultureInfo.InvariantCulture),
            };
            BossRaidConfigStore.Save(_settings, config with { LastRemoteSearch = newSearch });
        }
        catch
        {
            // Best-effort; swallow.
        }
    }

    private static JsonArray ExtractResultsArray(JsonNode? data)
    {
        if (data is JsonObject obj)
        {
            if (obj["results"] is JsonArray results)
                return CloneArray(results);
            if (obj["items"] is JsonArray items)
                return CloneArray(items);
        }
        return new JsonArray();
    }

    private static RaidRemoteQuery BuildQueryRecord(JsonObject? qNode, RaidRemoteQuery fallback)
    {
        if (qNode is null) return fallback;
        string Str(string key) =>
            qNode[key] is JsonValue jv && jv.TryGetValue<string>(out var s) ? s : string.Empty;
        int Int(string key, int @default, int min, int max)
        {
            if (qNode[key] is JsonValue v)
            {
                if (v.TryGetValue<int>(out var i)) return Math.Max(min, Math.Min(max, i));
                if (v.TryGetValue<string>(out var ss) && int.TryParse(ss, out var parsed))
                    return Math.Max(min, Math.Min(max, parsed));
            }
            return @default;
        }
        return new RaidRemoteQuery(
            Q: Str("q"),
            Page: Int("page", 1, 1, int.MaxValue),
            PageSize: Int("page_size", 20, 1, 100));
    }

    private JsonObject HandleGet(JsonObject? payload)
    {
        var id = payload?["id"]?.GetValue<string>() ?? string.Empty;
        if (string.IsNullOrEmpty(id))
            return new JsonObject { ["ok"] = false, ["error"] = "missing_id" };
        return InvokeWithClient((client, ct) => client.GetAsync(id, ct));
    }

    private JsonObject HandleDownload(JsonObject? payload)
    {
        if (_settings is null)
            return new JsonObject { ["ok"] = false, ["error"] = "settings_unavailable" };

        var id = ReadString(payload?["id"]).Trim();
        if (string.IsNullOrWhiteSpace(id))
            return new JsonObject { ["ok"] = false, ["error"] = "missing_id" };

        var reply = InvokeWithClient((client, ct) => client.GetAsync(id, ct));
        if (reply["ok"]?.GetValue<bool>() != true)
            return reply;
        if (reply["data"] is not JsonObject data)
            return new JsonObject { ["ok"] = false, ["error"] = "bad_response" };

        var profileNode = data["profile"] ?? new JsonObject();
        JsonElement profileElement;
        try
        {
            using var profileDoc = JsonDocument.Parse(profileNode.ToJsonString());
            profileElement = profileDoc.RootElement.Clone();
        }
        catch (JsonException)
        {
            return new JsonObject { ["ok"] = false, ["error"] = "bad_profile" };
        }

        var author = CurrentAuthorElement();
        var config = BossRaidConfigStore.Load(_settings, author);
        var profile = BossRaidProfile.NormalizeProfile(profileElement, author, source: "downloaded");
        if (string.IsNullOrWhiteSpace(profile.Id) || config.Profiles.Any(item => item.Id == profile.Id))
            profile = profile with { Id = BossRaidProfile.NewIdFactory("boss") };

        var remoteId = ReadString(data["id"]).Trim();
        profile = profile with
        {
            RemoteId = string.IsNullOrWhiteSpace(remoteId) ? null : remoteId,
            Source = "downloaded",
            UpdatedAt = BossRaidProfile.UtcNowIsoFactory(),
        };
        config = BossRaidProfile.UpsertProfile(config, profile, activate: false);
        BossRaidConfigStore.Save(_settings, config);
        return new JsonObject
        {
            ["ok"] = true,
            ["profile_id"] = profile.Id,
            ["remote_id"] = profile.RemoteId is null ? null : JsonValue.Create(profile.RemoteId),
            ["state"] = MenuStateBridge.BuildBossRaidState(_settings, _states),
        };
    }

    private JsonObject HandleRefreshUploadAuth(JsonObject? payload)
    {
        var force = ReadBool(payload?["force"]);
        var identity = CurrentIdentityObject();
        var serverUrl = CurrentServerUrl();
        if (identity["ready"]?.GetValue<bool>() != true)
        {
            ClearUploadAuth();
            var missing = string.Join(", ", identity["missing"]!.AsArray().Select(item => item?.GetValue<string>() ?? string.Empty));
            var auth = BuildUploadAuth(string.Empty, string.Empty, $"Identity is incomplete: {missing}", string.Empty, identity, serverUrl);
            return AuthReply(auth);
        }

        if (!force && UploadAuthValid(identity, serverUrl))
        {
            return AuthReply(BuildUploadAuth(_uploadToken, _uploadExpiresAt, string.Empty, _uploadMode, identity, serverUrl));
        }

        using var issueDoc = JsonDocument.Parse(new JsonObject
        {
            ["player_uid"] = identity["player_uid"]?.GetValue<string>() ?? string.Empty,
            ["player_name"] = identity["player_name"]?.GetValue<string>() ?? string.Empty,
            ["profession_id"] = identity["profession_id"]?.GetValue<int>() ?? 0,
            ["profession_name"] = identity["profession_name"]?.GetValue<string>() ?? string.Empty,
        }.ToJsonString());
        var reply = InvokeWithClient((client, ct) => client.IssueUploadTokenAsync(issueDoc.RootElement.Clone(), ct));
        if (reply["ok"]?.GetValue<bool>() != true)
        {
            ClearUploadAuth();
            var error = ReadString(reply["error"]);
            return AuthReply(BuildUploadAuth(string.Empty, string.Empty, error, string.Empty, identity, serverUrl));
        }

        var data = reply["data"] as JsonObject;
        var token = ReadString(data?["token"]).Trim();
        var expiresAt = ReadString(data?["expires_at"]).Trim();
        var mode = ReadString(data?["mode"]).Trim();
        if (string.IsNullOrWhiteSpace(token))
        {
            ClearUploadAuth();
            return AuthReply(BuildUploadAuth(string.Empty, expiresAt, "Upload token is empty", mode, identity, serverUrl));
        }

        _uploadToken = token;
        _uploadExpiresAt = expiresAt;
        _uploadMode = mode;
        _uploadIdentitySignature = IdentitySignature(identity);
        _uploadServerUrl = serverUrl;
        return AuthReply(BuildUploadAuth(token, expiresAt, string.Empty, mode, identity, serverUrl));
    }

    private JsonObject HandleIssueToken(JsonObject? payload)
    {
        var body = ExtractPayloadElement(payload, "payload");
        if (body is null)
            return new JsonObject { ["ok"] = false, ["error"] = "missing_payload" };
        return InvokeWithClient((client, ct) => client.IssueUploadTokenAsync(body.Value, ct));
    }

    private JsonObject HandleUpload(JsonObject? payload)
    {
        var token = payload?["token"]?.GetValue<string>() ?? string.Empty;
        var body = ExtractPayloadElement(payload, "profile");
        if (body is null)
            return new JsonObject { ["ok"] = false, ["error"] = "missing_profile" };
        return InvokeWithClient((client, ct) => client.UploadAsync(body.Value, token, ct));
    }

    private JsonObject HandleSetServerUrl(JsonObject? payload)
    {
        if (_settings is null)
            return new JsonObject { ["ok"] = false, ["error"] = "settings_unavailable" };
        var url = ReadString(payload?["url"]).Trim();
        var config = BossRaidConfigStore.Load(_settings);
        BossRaidConfigStore.Save(_settings, config with { ServerUrl = url });
        ClearUploadAuth();
        return new JsonObject
        {
            ["ok"] = true,
            ["server_url"] = url,
        };
    }

    private static JsonElement? ExtractPayloadElement(JsonObject? payload, string key)
    {
        var node = payload?[key];
        if (node is null) return null;
        using var doc = JsonDocument.Parse(node.ToJsonString());
        return doc.RootElement.Clone();
    }

    private JsonObject InvokeWithClient(Func<BossRaidCloudClient, CancellationToken, Task<JsonElement>> action)
    {
        if (_settings is not null && _clientFromSettings is not null)
        {
            using var client = _clientFromSettings(_settings);
            return Invoke(ct => action(client, ct));
        }
        return Invoke(ct => action(_client, ct));
    }

    private static JsonObject Invoke(Func<CancellationToken, Task<JsonElement>> action)
    {
        try
        {
            var data = action(CancellationToken.None).GetAwaiter().GetResult();
            // BossRaidCloudClient returns {error:"..."} on failure; normalise to ok/error.
            if (data.ValueKind == JsonValueKind.Object
                && data.TryGetProperty("error", out var errProp)
                && errProp.ValueKind == JsonValueKind.String)
            {
                return new JsonObject { ["ok"] = false, ["error"] = errProp.GetString() };
            }
            return new JsonObject
            {
                ["ok"] = true,
                ["data"] = JsonNode.Parse(data.GetRawText()),
            };
        }
        catch (InvalidOperationException ex)
        {
            return new JsonObject { ["ok"] = false, ["error"] = ex.Message };
        }
    }

    private static string ReadString(JsonNode? node)
    {
        if (node is null) return string.Empty;
        if (node is JsonValue jsonValue && jsonValue.TryGetValue<string>(out var text))
            return text;
        return node.ToString();
    }

    private static bool ReadBool(JsonNode? node)
    {
        if (node is null) return false;
        if (node is JsonValue jsonValue)
        {
            if (jsonValue.TryGetValue<bool>(out var b)) return b;
            if (jsonValue.TryGetValue<string>(out var s)
                && bool.TryParse(s, out var parsed)) return parsed;
        }
        return false;
    }

    private JsonObject AuthReply(JsonObject auth)
    {
        var ok = auth["ready"]?.GetValue<bool>() == true;
        var reply = new JsonObject
        {
            ["ok"] = ok,
            ["message"] = auth["error"]?.GetValue<string>() ?? string.Empty,
            ["upload_auth"] = CloneObject(auth),
        };
        if (_settings is not null)
        {
            var state = MenuStateBridge.BuildBossRaidState(_settings, _states);
            state["upload_auth"] = CloneObject(auth);
            reply["state"] = state;
        }
        return reply;
    }

    private JsonObject BuildUploadAuth(
        string token,
        string expiresAt,
        string error,
        string mode,
        JsonObject identity,
        string serverUrl)
    {
        var ready = !string.IsNullOrWhiteSpace(token) && string.IsNullOrWhiteSpace(error);
        return new JsonObject
        {
            ["token"] = string.Empty,
            ["ready"] = ready,
            ["token_masked"] = BossRaidProfile.MaskToken(token),
            ["expires_at"] = expiresAt ?? string.Empty,
            ["error"] = error ?? string.Empty,
            ["mode"] = mode ?? string.Empty,
            ["identity"] = CloneObject(identity),
            ["server_url"] = serverUrl,
        };
    }

    private bool UploadAuthValid(JsonObject identity, string serverUrl)
    {
        if (string.IsNullOrWhiteSpace(_uploadToken)) return false;
        if (!string.Equals(_uploadServerUrl, serverUrl, StringComparison.Ordinal)) return false;
        if (!string.Equals(_uploadIdentitySignature, IdentitySignature(identity), StringComparison.Ordinal)) return false;
        if (DateTimeOffset.TryParse(_uploadExpiresAt, out var expires)
            && expires <= _clock().AddSeconds(30))
        {
            return false;
        }
        return true;
    }

    private void ClearUploadAuth()
    {
        _uploadToken = string.Empty;
        _uploadExpiresAt = string.Empty;
        _uploadMode = string.Empty;
        _uploadIdentitySignature = string.Empty;
        _uploadServerUrl = string.Empty;
    }

    private string CurrentServerUrl()
    {
        if (_settings is null)
            return string.IsNullOrWhiteSpace(_client.BaseUrl) ? BossRaidProfile.DefaultServerUrl : _client.BaseUrl;
        var config = BossRaidConfigStore.Load(_settings, CurrentAuthorElement());
        return string.IsNullOrWhiteSpace(config.ServerUrl)
            ? BossRaidProfile.DefaultServerUrl
            : config.ServerUrl;
    }

    private JsonObject CurrentIdentityObject()
    {
        var author = _states is null
            ? AuthorSnapshot.Empty
            : AutoKeyConfigLoader.AuthorFromState(_states.Snapshot);
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
            ["source"] = _states is null ? "profile" : "packet",
            ["ready"] = missing.Count == 0,
            ["missing"] = missing,
        };
    }

    private static string IdentitySignature(JsonObject identity)
        => string.Join("|", new[]
        {
            identity["player_uid"]?.GetValue<string>() ?? string.Empty,
            identity["player_name"]?.GetValue<string>() ?? string.Empty,
            (identity["profession_id"]?.GetValue<int>() ?? 0).ToString(System.Globalization.CultureInfo.InvariantCulture),
            identity["profession_name"]?.GetValue<string>() ?? string.Empty,
        });

    private static JsonObject CloneObject(JsonObject obj)
        => JsonNode.Parse(obj.ToJsonString())!.AsObject();

    private static JsonArray CloneArray(JsonArray array)
        => JsonNode.Parse(array.ToJsonString())!.AsArray();

    private JsonElement CurrentAuthorElement()
    {
        var author = _states is null
            ? AuthorSnapshot.Empty
            : AutoKeyConfigLoader.AuthorFromState(_states.Snapshot);
        var node = new JsonObject
        {
            ["player_uid"] = author.PlayerUid ?? string.Empty,
            ["player_name"] = author.PlayerName ?? string.Empty,
            ["profession_id"] = author.ProfessionId,
            ["profession_name"] = author.ProfessionName ?? string.Empty,
        };
        using var doc = JsonDocument.Parse(node.ToJsonString());
        return doc.RootElement.Clone();
    }
}
