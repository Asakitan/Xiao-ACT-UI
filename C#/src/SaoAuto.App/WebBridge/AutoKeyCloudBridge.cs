using System.Collections.Immutable;
using System.Text.Json;
using System.Text.Json.Nodes;
using SaoAuto.Core.Automation;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.WebBridge;

/// <summary>
/// S177 — Registers the four AutoKey cloud script-share commands on a
/// <see cref="BridgeRouter"/>, backed by an <see cref="AutoKeyCloudClient"/>.
///
/// <list type="bullet">
/// <item><c>autokey.cloud.search</c> — payload <c>{query:{q,page,page_size,...}}</c>
///   → reply <c>{ok:bool, data?:any, error?:string}</c></item>
/// <item><c>autokey.cloud.get</c> — payload <c>{id:string}</c>
///   → reply <c>{ok:bool, data?:any, error?:string}</c></item>
/// <item><c>autokey.cloud.issue_token</c> — payload arbitrary object
///   → reply <c>{ok:bool, data?:any, error?:string}</c></item>
/// <item><c>autokey.cloud.upload</c> — payload <c>{token:string, profile:object}</c>
///   → reply <c>{ok:bool, data?:any, error?:string}</c></item>
/// </list>
///
/// The cloud client's async methods are awaited synchronously (mirrors
/// Python's blocking call site in <c>auto_key_engine</c>). Transport /
/// HTTP failures are caught and surfaced as <c>{ok:false, error}</c> so
/// the bridge reply stays a clean <see cref="JsonObject"/> regardless of
/// upstream outcome. Other (unexpected) exceptions propagate so
/// <see cref="BridgeRouter"/>'s catch turns them into
/// <c>handler_exception</c> replies.
/// </summary>
public sealed class AutoKeyCloudBridge : IDisposable
{
    private readonly BridgeRouter _router;
    private readonly AutoKeyCloudClient _client;
    private readonly SettingsManager? _settings;
    private readonly Func<DateTimeOffset> _clock;
    private readonly string[] _commands;
    private bool _disposed;

    public AutoKeyCloudBridge(
        BridgeRouter router,
        AutoKeyCloudClient client,
        SettingsManager? settings = null,
        Func<DateTimeOffset>? clock = null)
    {
        _router = router ?? throw new ArgumentNullException(nameof(router));
        _client = client ?? throw new ArgumentNullException(nameof(client));
        _settings = settings;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _commands = new[]
        {
            BridgeCommands.SearchAutoKeyScripts,
            BridgeCommands.GetAutoKeyScript,
            BridgeCommands.IssueAutoKeyUploadToken,
            BridgeCommands.UploadAutoKeyScript,
        };
        router.Register(BridgeCommands.SearchAutoKeyScripts, HandleSearch);
        router.Register(BridgeCommands.GetAutoKeyScript, HandleGet);
        router.Register(BridgeCommands.IssueAutoKeyUploadToken, HandleIssueToken);
        router.Register(BridgeCommands.UploadAutoKeyScript, HandleUpload);
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
        var reply = Invoke(ct => _client.SearchScriptsAsync(query, ct));
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
            var config = AutoKeyConfigLoader.Load(_settings);
            var results = ImmutableArray<JsonElement>.Empty;
            if (data is JsonObject obj && obj["results"] is JsonArray arr)
            {
                var builder = ImmutableArray.CreateBuilder<JsonElement>(arr.Count);
                foreach (var item in arr)
                {
                    using var doc = JsonDocument.Parse((item ?? new JsonObject()).ToJsonString());
                    builder.Add(doc.RootElement.Clone());
                }
                results = builder.ToImmutable();
            }
            var newSearch = config.LastRemoteSearch with
            {
                Query = BuildQueryRecord(queryNode, config.LastRemoteSearch.Query),
                Results = results,
                Error = string.Empty,
                FetchedAt = _clock().ToString("yyyy-MM-ddTHH:mm:ssZ", System.Globalization.CultureInfo.InvariantCulture),
            };
            AutoKeyConfigLoader.Save(_settings, config with { LastRemoteSearch = newSearch });
            _settings.Save();
        }
        catch
        {
            // Swallow — persistence is best-effort; a write failure must
            // not corrupt the bridge reply that already went out.
        }
    }

    private static AutoKeyRemoteQuery BuildQueryRecord(JsonObject? qNode, AutoKeyRemoteQuery fallback)
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
        return new AutoKeyRemoteQuery(
            Q: Str("q"),
            ProfileName: Str("profile_name"),
            PlayerUid: Str("player_uid"),
            PlayerName: Str("player_name"),
            ProfessionName: Str("profession_name"),
            Page: Int("page", 1, 1, int.MaxValue),
            PageSize: Int("page_size", 20, 1, 100));
    }

    private JsonObject HandleGet(JsonObject? payload)
    {
        var id = payload?["id"]?.GetValue<string>() ?? string.Empty;
        if (string.IsNullOrEmpty(id))
            return new JsonObject { ["ok"] = false, ["error"] = "missing_id" };
        return Invoke(ct => _client.GetScriptAsync(id, ct));
    }

    private JsonObject HandleIssueToken(JsonObject? payload)
    {
        var body = ExtractPayloadElement(payload, "payload");
        if (body is null)
            return new JsonObject { ["ok"] = false, ["error"] = "missing_payload" };
        return Invoke(ct => _client.IssueUploadTokenAsync(body.Value, ct));
    }

    private JsonObject HandleUpload(JsonObject? payload)
    {
        var token = payload?["token"]?.GetValue<string>() ?? string.Empty;
        var body = ExtractPayloadElement(payload, "profile");
        if (body is null)
            return new JsonObject { ["ok"] = false, ["error"] = "missing_profile" };
        return Invoke(ct => _client.UploadScriptAsync(body.Value, token, ct));
    }

    private static JsonElement? ExtractPayloadElement(JsonObject? payload, string key)
    {
        var node = payload?[key];
        if (node is null) return null;
        using var doc = JsonDocument.Parse(node.ToJsonString());
        return doc.RootElement.Clone();
    }

    private static JsonObject Invoke(Func<CancellationToken, Task<JsonElement>> action)
    {
        try
        {
            var data = action(CancellationToken.None).GetAwaiter().GetResult();
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
}
