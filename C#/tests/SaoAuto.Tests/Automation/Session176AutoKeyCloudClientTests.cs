using System.Net;
using System.Text;
using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

/// <summary>
/// S176 — Pin <see cref="AutoKeyCloudClient"/>. Mirrors the existing
/// <c>BossRaidCloudClient</c> tests but covers the throw-on-failure
/// path (Python <c>raise RuntimeError(...)</c>) and the
/// <c>detail</c>-key fallback in error bodies.
/// </summary>
public class Session176AutoKeyCloudClientTests
{
    private sealed class FakeHandler : HttpMessageHandler
    {
        public List<HttpRequestMessage> Requests { get; } = new();
        public List<string> RequestBodies { get; } = new();
        public Func<HttpRequestMessage, HttpResponseMessage> Responder { get; set; } = _ =>
            new HttpResponseMessage(HttpStatusCode.OK)
            { Content = new StringContent("{}", Encoding.UTF8, "application/json") };

        protected override async Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Requests.Add(request);
            RequestBodies.Add(request.Content is null
                ? string.Empty
                : await request.Content.ReadAsStringAsync(cancellationToken));
            return Responder(request);
        }
    }

    private static (AutoKeyCloudClient client, FakeHandler handler) MakeClient(string baseUrl = "http://example.com")
    {
        var handler = new FakeHandler();
        var http = new HttpClient(handler);
        return (new AutoKeyCloudClient(baseUrl, http), handler);
    }

    [Fact]
    public void DefaultsToCanonicalServerUrl()
    {
        // No way to introspect _baseUrl directly, but constructor must not throw on null/empty.
        using var c1 = new AutoKeyCloudClient(null);
        using var c2 = new AutoKeyCloudClient("");
        using var c3 = new AutoKeyCloudClient("  ");
        Assert.Equal("http://doi.sakisense.top:15538", AutoKeyCloudClient.DefaultServerUrl);
    }

    [Fact]
    public async Task SearchScriptsBuildsQueryStringSkippingEmpty()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"results\":[]}", Encoding.UTF8, "application/json"),
        };

        var result = await client.SearchScriptsAsync(new Dictionary<string, string?>
        {
            ["q"] = "fire/dps",
            ["page"] = "2",
            ["empty"] = null,
            ["blank"] = "",
        });

        var url = handler.Requests[0].RequestUri!.AbsoluteUri;
        Assert.Contains("/api/scripts?", url);
        Assert.Contains("q=fire%2Fdps", url);
        Assert.Contains("page=2", url);
        Assert.DoesNotContain("empty=", url);
        Assert.DoesNotContain("blank=", url);
        Assert.Equal(JsonValueKind.Array, result.GetProperty("results").ValueKind);
    }

    [Fact]
    public async Task SearchScriptsNoQueryOmitsQuestionMark()
    {
        var (client, handler) = MakeClient();
        await client.SearchScriptsAsync(new Dictionary<string, string?>());
        var url = handler.Requests[0].RequestUri!.AbsoluteUri;
        Assert.EndsWith("/api/scripts", url);
    }

    [Fact]
    public async Task GetScriptEscapesId()
    {
        var (client, handler) = MakeClient();
        await client.GetScriptAsync("abc/123 def");
        var url = handler.Requests[0].RequestUri!.AbsoluteUri;
        Assert.EndsWith("/api/scripts/abc%2F123%20def", url);
    }

    [Fact]
    public async Task UploadSendsTokenHeaderAndJsonBody()
    {
        var (client, handler) = MakeClient();
        var payload = JsonDocument.Parse("{\"profile_name\":\"AK\"}").RootElement;
        await client.UploadScriptAsync(payload, "tok-1");

        var req = handler.Requests[0];
        Assert.Equal(HttpMethod.Post, req.Method);
        Assert.EndsWith("/api/scripts", req.RequestUri!.AbsoluteUri);
        Assert.True(req.Headers.TryGetValues("X-SAO-Upload-Token", out var v));
        Assert.Equal("tok-1", v!.Single());
        Assert.Contains("\"profile_name\":\"AK\"", handler.RequestBodies[0]);
    }

    [Fact]
    public async Task UploadEmptyTokenStillSendsHeader()
    {
        var (client, handler) = MakeClient();
        var payload = JsonDocument.Parse("{}").RootElement;
        await client.UploadScriptAsync(payload, uploadToken: null!);
        Assert.True(handler.Requests[0].Headers.Contains("X-SAO-Upload-Token"));
    }

    [Fact]
    public async Task IssueUploadTokenPostsJson()
    {
        var (client, handler) = MakeClient();
        var payload = JsonDocument.Parse("{\"player_uid\":\"u\"}").RootElement;
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"token\":\"abc\"}", Encoding.UTF8, "application/json"),
        };
        var result = await client.IssueUploadTokenAsync(payload);
        Assert.Equal(HttpMethod.Post, handler.Requests[0].Method);
        Assert.EndsWith("/api/upload-token/issue", handler.Requests[0].RequestUri!.AbsoluteUri);
        Assert.Equal("abc", result.GetProperty("token").GetString());
    }

    [Fact]
    public async Task EmptySuccessBodyReturnsEmptyObject()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        { Content = new StringContent(string.Empty) };
        var result = await client.GetScriptAsync("anything");
        Assert.Equal(JsonValueKind.Object, result.ValueKind);
        Assert.Empty(result.EnumerateObject());
    }

    [Fact]
    public async Task HttpErrorWithDetailRaisesWithDetail()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.BadRequest)
        {
            Content = new StringContent("{\"detail\":\"profile already exists\"}", Encoding.UTF8, "application/json"),
        };
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.UploadScriptAsync(JsonDocument.Parse("{}").RootElement, "tok"));
        Assert.Equal("profile already exists", ex.Message);
    }

    [Fact]
    public async Task HttpErrorWithoutJsonRaisesRawText()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.InternalServerError)
        { Content = new StringContent("server boom", Encoding.UTF8, "text/plain") };
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.GetScriptAsync("x"));
        Assert.Equal("server boom", ex.Message);
    }

    [Fact]
    public async Task HttpErrorWithEmptyBodyRaisesHttpCode()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.Unauthorized)
        { Content = new StringContent(string.Empty) };
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.GetScriptAsync("x"));
        Assert.Equal("HTTP 401", ex.Message);
    }

    [Fact]
    public async Task TransportFailureRaisesInvalidOperationException()
    {
        var handler = new FakeHandler
        {
            Responder = _ => throw new HttpRequestException("connection refused"),
        };
        using var http = new HttpClient(handler);
        using var client = new AutoKeyCloudClient("http://example.com", http);
        var ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.GetScriptAsync("x"));
        Assert.Equal("connection refused", ex.Message);
    }

    [Fact]
    public async Task SuccessNonJsonRaises()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        { Content = new StringContent("not json at all", Encoding.UTF8, "text/plain") };
        await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.GetScriptAsync("x"));
    }
}
