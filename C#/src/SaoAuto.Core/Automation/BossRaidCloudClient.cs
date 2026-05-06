using System.Net;
using System.Text;
using System.Text.Json;

namespace SaoAuto.Core.Automation;

/// <summary>
/// HTTP cloud client for the Boss Raid profile share endpoint —
/// port of <c>boss_raid_engine.BossRaidCloudClient</c>. Four
/// endpoints: search (GET list), get (GET by id), upload (POST
/// with X-SAO-Upload-Token header), issue_upload_token (POST).
///
/// All methods return <see cref="JsonElement"/> matching Python's
/// dict-out semantics. On failure (network exception or HTTP
/// 4xx/5xx without a JSON body) the result is a synthetic
/// <c>{"error": "..."}</c> object — same shape as Python so the
/// caller never has to branch on exception type.
/// </summary>
public sealed class BossRaidCloudClient
{
    private readonly HttpClient _http;
    private readonly string _baseUrl;
    private readonly bool _ownsHttp;

    public BossRaidCloudClient(string baseUrl, HttpClient? http = null, TimeSpan? timeout = null)
    {
        _baseUrl = (baseUrl ?? string.Empty).TrimEnd('/');
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

    public Task<JsonElement> SearchAsync(IReadOnlyDictionary<string, string?> query, CancellationToken ct = default)
    {
        var qs = string.Join("&",
            query.Where(kv => !string.IsNullOrEmpty(kv.Value))
                 .Select(kv => $"{Uri.EscapeDataString(kv.Key)}={Uri.EscapeDataString(kv.Value!)}"));
        var path = qs.Length == 0 ? "/api/boss-raids" : $"/api/boss-raids?{qs}";
        return RequestAsync(HttpMethod.Get, path, body: null, headers: null, ct);
    }

    public Task<JsonElement> GetAsync(string remoteId, CancellationToken ct = default)
    {
        var safe = Uri.EscapeDataString(remoteId ?? string.Empty);
        return RequestAsync(HttpMethod.Get, $"/api/boss-raids/{safe}", body: null, headers: null, ct);
    }

    public Task<JsonElement> UploadAsync(JsonElement payload, string uploadToken, CancellationToken ct = default)
    {
        var headers = new Dictionary<string, string>
        {
            ["X-SAO-Upload-Token"] = uploadToken ?? string.Empty,
        };
        return RequestAsync(HttpMethod.Post, "/api/boss-raids", body: payload, headers, ct);
    }

    public Task<JsonElement> IssueUploadTokenAsync(JsonElement payload, CancellationToken ct = default)
    {
        return RequestAsync(HttpMethod.Post, "/api/upload-token/issue", body: payload, headers: null, ct);
    }

    private async Task<JsonElement> RequestAsync(
        HttpMethod method, string path, object? body, IDictionary<string, string>? headers, CancellationToken ct)
    {
        var url = $"{_baseUrl}{path}";
        try
        {
            using var req = new HttpRequestMessage(method, url);
            req.Headers.Accept.Clear();
            req.Headers.Accept.ParseAdd("application/json");
            if (body is JsonElement el)
            {
                var json = el.GetRawText();
                req.Content = new StringContent(json, Encoding.UTF8, "application/json");
            }
            else if (body is not null)
            {
                var json = JsonSerializer.Serialize(body);
                req.Content = new StringContent(json, Encoding.UTF8, "application/json");
            }
            if (headers is not null)
            {
                foreach (var (k, v) in headers) req.Headers.TryAddWithoutValidation(k, v ?? string.Empty);
            }

            using var resp = await _http.SendAsync(req, ct).ConfigureAwait(false);
            var bodyText = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            if (resp.IsSuccessStatusCode)
            {
                return ParseOrError(bodyText, resp.StatusCode, resp.ReasonPhrase);
            }
            // Python first tries to parse the error body as JSON, falls back to "HTTP {code}: {reason}".
            try
            {
                using var doc = JsonDocument.Parse(bodyText);
                return doc.RootElement.Clone();
            }
            catch
            {
                return ErrorElement($"HTTP {(int)resp.StatusCode}: {resp.ReasonPhrase ?? string.Empty}");
            }
        }
        catch (Exception ex)
        {
            return ErrorElement(ex.Message);
        }
    }

    private static JsonElement ParseOrError(string text, HttpStatusCode code, string? reason)
    {
        if (string.IsNullOrWhiteSpace(text))
            return ErrorElement($"HTTP {(int)code}: empty body");
        try
        {
            using var doc = JsonDocument.Parse(text);
            return doc.RootElement.Clone();
        }
        catch (JsonException ex)
        {
            return ErrorElement($"invalid JSON: {ex.Message}");
        }
    }

    internal static JsonElement ErrorElement(string message)
    {
        var obj = JsonSerializer.Serialize(new { error = message });
        using var doc = JsonDocument.Parse(obj);
        return doc.RootElement.Clone();
    }

    public void Dispose()
    {
        if (_ownsHttp) _http.Dispose();
    }
}
