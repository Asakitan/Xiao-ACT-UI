using System.Net;
using System.Text;
using System.Text.Json;
using SaoAuto.Core.Configuration;

namespace SaoAuto.Core.Automation;

/// <summary>
/// S176 — HTTP cloud client for the Auto-Key script share endpoint.
/// Port of <c>auto_key_engine.AutoKeyCloudClient</c> (auto_key_engine.py
/// lines 542–587). Four endpoints:
/// <list type="bullet">
///   <item><c>GET /api/scripts?...</c> — search</item>
///   <item><c>GET /api/scripts/{id}</c> — fetch one</item>
///   <item><c>POST /api/upload-token/issue</c> — request upload token</item>
///   <item><c>POST /api/scripts</c> (X-SAO-Upload-Token header) — upload</item>
/// </list>
///
/// Unlike <see cref="BossRaidCloudClient"/> (which surfaces failures as
/// a synthetic <c>{error:"..."}</c> JSON object), this client *throws*
/// on HTTP / transport failure — matches Python's <c>raise RuntimeError(...)</c>
/// path. Successful responses come back as <see cref="JsonElement"/>;
/// empty 2xx bodies become an empty object (Python <c>{}</c>).
/// </summary>
public sealed class AutoKeyCloudClient : IDisposable
{
    public const string DefaultServerUrl = "http://doi.sakisense.top:15538";

    private readonly HttpClient _http;
    private readonly string _baseUrl;
    private readonly bool _ownsHttp;

    /// <summary>S180 — the canonicalised base URL (trimmed of trailing
    /// slash, with the canonical default substituted when input was
    /// null/whitespace). Useful for callers that want to inspect the
    /// effective URL without firing a request.</summary>
    public string BaseUrl => _baseUrl;

    /// <summary>S180 — build a client whose base URL reflects the
    /// <c>auto_key.server_url</c> field on disk (falling back to
    /// <see cref="DefaultServerUrl"/> when missing/blank). Caller owns
    /// the returned client's lifetime.</summary>
    public static AutoKeyCloudClient FromSettings(
        SettingsManager settings, HttpClient? http = null, TimeSpan? timeout = null)
    {
        ArgumentNullException.ThrowIfNull(settings);
        var config = AutoKeyConfigLoader.Load(settings);
        var url = string.IsNullOrWhiteSpace(config.ServerUrl) ? DefaultServerUrl : config.ServerUrl;
        return new AutoKeyCloudClient(url, http, timeout);
    }

    public AutoKeyCloudClient(string? baseUrl = null, HttpClient? http = null, TimeSpan? timeout = null)
    {
        _baseUrl = (string.IsNullOrWhiteSpace(baseUrl) ? DefaultServerUrl : baseUrl).TrimEnd('/');
        if (http is null)
        {
            _http = new HttpClient { Timeout = timeout ?? TimeSpan.FromSeconds(5) };
            _ownsHttp = true;
        }
        else
        {
            _http = http;
            if (timeout.HasValue) _http.Timeout = timeout.Value;
            _ownsHttp = false;
        }
    }

    public Task<JsonElement> SearchScriptsAsync(IReadOnlyDictionary<string, string?> query, CancellationToken ct = default)
    {
        var qs = string.Join("&",
            query.Where(kv => !string.IsNullOrEmpty(kv.Value))
                 .Select(kv => $"{Uri.EscapeDataString(kv.Key)}={Uri.EscapeDataString(kv.Value!)}"));
        var path = qs.Length == 0 ? "/api/scripts" : $"/api/scripts?{qs}";
        return RequestAsync(HttpMethod.Get, path, body: null, headers: null, ct);
    }

    public Task<JsonElement> GetScriptAsync(string scriptId, CancellationToken ct = default)
    {
        var safe = Uri.EscapeDataString(scriptId ?? string.Empty);
        return RequestAsync(HttpMethod.Get, $"/api/scripts/{safe}", body: null, headers: null, ct);
    }

    public Task<JsonElement> IssueUploadTokenAsync(JsonElement payload, CancellationToken ct = default)
        => RequestAsync(HttpMethod.Post, "/api/upload-token/issue", body: payload, headers: null, ct);

    public Task<JsonElement> UploadScriptAsync(JsonElement payload, string uploadToken, CancellationToken ct = default)
    {
        var headers = new Dictionary<string, string>
        {
            ["X-SAO-Upload-Token"] = uploadToken ?? string.Empty,
        };
        return RequestAsync(HttpMethod.Post, "/api/scripts", body: payload, headers, ct);
    }

    private async Task<JsonElement> RequestAsync(
        HttpMethod method, string path, object? body, IDictionary<string, string>? headers, CancellationToken ct)
    {
        var url = $"{_baseUrl}{path}";
        HttpResponseMessage? resp = null;
        string bodyText;
        try
        {
            using var req = new HttpRequestMessage(method, url);
            req.Headers.Accept.Clear();
            req.Headers.Accept.ParseAdd("application/json");
            if (body is JsonElement el && el.ValueKind != JsonValueKind.Undefined)
            {
                var json = el.GetRawText();
                req.Content = new StringContent(json, Encoding.UTF8, "application/json");
            }
            else if (body is not null and not JsonElement)
            {
                var json = JsonSerializer.Serialize(body);
                req.Content = new StringContent(json, Encoding.UTF8, "application/json");
            }
            if (headers is not null)
            {
                foreach (var (k, v) in headers) req.Headers.TryAddWithoutValidation(k, v ?? string.Empty);
            }

            resp = await _http.SendAsync(req, ct).ConfigureAwait(false);
            bodyText = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested)
        {
            resp?.Dispose();
            throw;
        }
        catch (Exception ex)
        {
            resp?.Dispose();
            // Python: `urllib.error.URLError` and other transport errors → RuntimeError(str(exc.reason)).
            throw new InvalidOperationException(ex.Message, ex);
        }

        using (resp)
        {
            if (resp.IsSuccessStatusCode)
            {
                if (string.IsNullOrEmpty(bodyText))
                {
                    using var emptyDoc = JsonDocument.Parse("{}");
                    return emptyDoc.RootElement.Clone();
                }
                try
                {
                    using var doc = JsonDocument.Parse(bodyText);
                    return doc.RootElement.Clone();
                }
                catch (JsonException ex)
                {
                    throw new InvalidOperationException($"invalid JSON: {ex.Message}", ex);
                }
            }

            // Python error path: try to parse body as JSON; if it has a "detail" key, use it;
            // else fall back to the raw text or the HTTP code.
            string? detail = null;
            if (!string.IsNullOrEmpty(bodyText))
            {
                try
                {
                    using var doc = JsonDocument.Parse(bodyText);
                    if (doc.RootElement.ValueKind == JsonValueKind.Object
                        && doc.RootElement.TryGetProperty("detail", out var d)
                        && d.ValueKind == JsonValueKind.String)
                    {
                        detail = d.GetString();
                    }
                }
                catch (JsonException) { /* swallow — fall through to text/code */ }
            }
            var message = !string.IsNullOrEmpty(detail)
                ? detail!
                : !string.IsNullOrEmpty(bodyText)
                    ? bodyText
                    : $"HTTP {(int)resp.StatusCode}";
            throw new InvalidOperationException(message);
        }
    }

    public void Dispose()
    {
        if (_ownsHttp) _http.Dispose();
    }
}
