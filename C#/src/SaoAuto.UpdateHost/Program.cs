using SaoAuto.Core.Updater;
using SaoAuto.UpdateHost;

var builder = WebApplication.CreateBuilder(args);
builder.Services.AddSingleton<ManifestStore>();

var app = builder.Build();

app.MapGet("/", () => Results.Ok(new
{
    service = "SaoAuto.UpdateHost",
    status = "running",
    version = SaoAuto.Core.Configuration.AppVersion.Label,
}));

app.MapGet("/latest", (ManifestStore store, string? channel = "stable", string? target = "windows-x64") =>
{
    var manifest = store.Latest(channel ?? "stable", target ?? "windows-x64");
    return manifest is null ? Results.NotFound() : Results.Ok(manifest);
});

app.MapPost("/publish", (ManifestStore store, UpdateManifest manifest) =>
{
    store.Publish(manifest);
    return Results.Created($"/latest?channel={manifest.Channel}&target={manifest.Target}", manifest);
});

app.MapGet("/summary", (ManifestStore store) => Results.Ok(store.Summary()));

app.Run();
