using System.Net;
using System.Text;
using System.Text.Json;
using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class BossRaidCloudClientTests
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

    private static (BossRaidCloudClient client, FakeHandler handler) MakeClient(
        string baseUrl = "http://example.com")
    {
        var handler = new FakeHandler();
        var http = new HttpClient(handler) { BaseAddress = null };
        return (new BossRaidCloudClient(baseUrl, http), handler);
    }

    [Fact]
    public async Task SearchBuildsQueryStringFromNonEmptyEntries()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent("{\"results\":[]}", Encoding.UTF8, "application/json"),
        };

        var result = await client.SearchAsync(new Dictionary<string, string?>
        {
            ["q"] = "boss fire",
            ["page"] = "2",
            ["empty"] = null,
            ["blank"] = "",
        });

        Assert.Single(handler.Requests);
        var url = handler.Requests[0].RequestUri!.AbsoluteUri;
        Assert.Contains("/api/boss-raids?", url);
        Assert.Contains("q=boss%20fire", url);
        Assert.Contains("page=2", url);
        Assert.DoesNotContain("empty=", url);
        Assert.DoesNotContain("blank=", url);
        Assert.Equal(JsonValueKind.Array, result.GetProperty("results").ValueKind);
    }

    [Fact]
    public async Task GetEscapesRemoteId()
    {
        var (client, handler) = MakeClient();
        await client.GetAsync("abc/123 def");
        var url = handler.Requests[0].RequestUri!.AbsoluteUri;
        Assert.EndsWith("/api/boss-raids/abc%2F123%20def", url);
    }

    [Fact]
    public async Task UploadSendsTokenHeaderAndJsonBody()
    {
        var (client, handler) = MakeClient();
        var payload = JsonDocument.Parse("{\"profile_name\":\"Boss\"}").RootElement;
        await client.UploadAsync(payload, "secret-token");

        var req = handler.Requests[0];
        Assert.Equal(HttpMethod.Post, req.Method);
        Assert.True(req.Headers.TryGetValues("X-SAO-Upload-Token", out var values));
        Assert.Equal("secret-token", values!.Single());
        Assert.Contains("\"profile_name\":\"Boss\"", handler.RequestBodies[0]);
        Assert.Equal("application/json", req.Content!.Headers.ContentType!.MediaType);
    }

    [Fact]
    public async Task UploadEmptyTokenStillSendsHeader()
    {
        var (client, handler) = MakeClient();
        var payload = JsonDocument.Parse("{}").RootElement;
        await client.UploadAsync(payload, uploadToken: null!);
        Assert.True(handler.Requests[0].Headers.Contains("X-SAO-Upload-Token"));
    }

    [Fact]
    public async Task IssueUploadTokenPostsJson()
    {
        var (client, handler) = MakeClient();
        var payload = JsonDocument.Parse("{\"player_uid\":\"u1\"}").RootElement;
        await client.IssueUploadTokenAsync(payload);
        var req = handler.Requests[0];
        Assert.Equal(HttpMethod.Post, req.Method);
        Assert.EndsWith("/api/upload-token/issue", req.RequestUri!.AbsoluteUri);
        Assert.Contains("u1", handler.RequestBodies[0]);
    }

    [Fact]
    public async Task NetworkExceptionReturnsErrorObject()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => throw new HttpRequestException("connection refused");
        var result = await client.GetAsync("anything");
        Assert.Equal(JsonValueKind.Object, result.ValueKind);
        Assert.Contains("connection refused", result.GetProperty("error").GetString());
    }

    [Fact]
    public async Task HttpErrorWithJsonBodyIsReturnedAsIs()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.BadRequest)
        {
            Content = new StringContent("{\"error\":\"missing field\",\"code\":42}",
                                         Encoding.UTF8, "application/json"),
        };
        var result = await client.GetAsync("x");
        Assert.Equal("missing field", result.GetProperty("error").GetString());
        Assert.Equal(42, result.GetProperty("code").GetInt32());
    }

    [Fact]
    public async Task HttpErrorWithoutJsonBodyFallsBackToHttpMessage()
    {
        var (client, handler) = MakeClient();
        handler.Responder = _ => new HttpResponseMessage(HttpStatusCode.InternalServerError)
        {
            Content = new StringContent("oops", Encoding.UTF8, "text/plain"),
            ReasonPhrase = "Internal Server Error",
        };
        var result = await client.GetAsync("x");
        var msg = result.GetProperty("error").GetString();
        Assert.NotNull(msg);
        Assert.Contains("500", msg);
    }

    [Fact]
    public async Task TrailingSlashOnBaseUrlIsTrimmed()
    {
        var (client, handler) = MakeClient(baseUrl: "http://example.com///");
        await client.GetAsync("xyz");
        Assert.Equal("http://example.com/api/boss-raids/xyz", handler.Requests[0].RequestUri!.AbsoluteUri);
    }
}
