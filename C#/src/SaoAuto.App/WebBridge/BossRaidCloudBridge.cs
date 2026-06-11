using System.Collections.Immutable;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

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
    private readonly Func<DateTimeOffset> _clock;
    private readonly string[] _commands;
    private bool _disposed;

    public BossRaidCloudBridge(
        BridgeRouter router,
        BossRaidCloudClient client,
        SettingsManager? settings = null,
        Func<DateTimeOffset>? clock = null,
        Func<SettingsManager, BossRaidCloudClient>? clientFromSettings = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _client = client ?? throw new ArgumentNullException(nameof(client));
        _settings = settings;
        _clientFromSettings = clientFromSettings;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _commands = new[]
        {
            BridgeCommands.SearchBossRaids,
            BridgeCommands.GetBossRaid,
            BridgeCommands.IssueBossRaidUploadToken,
            BridgeCommands.UploadBossRaid,
            BridgeCommands.SetBossRaidServerUrl,
        };
        router.Register(BridgeCommands.SearchBossRaids, HandleSearch);
        router.Register(BridgeCommands.GetBossRaid, HandleGet);
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
            if (data is JsonObject obj && obj["results"] is JsonArray arr)
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
}
