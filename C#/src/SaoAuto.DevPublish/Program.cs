using System.IO.Compression;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json;
using SaoAuto.Core.Updater;

// SaoAuto.DevPublish — packages an install directory into a zip and POSTs
// a manifest to a local UpdateHost. Mirrors the non-GUI half of
// `dev_publish.py`. The WPF GUI sibling (dev_publish_gui.py) is its own
// session.
//
// Usage:
//   SaoAuto.DevPublish pack <source-dir> <out.zip>
//   SaoAuto.DevPublish publish <out.zip> --version 2.4.1 --channel stable \
//        --target windows-x64 --url http://localhost:5000/publish \
//        --package-url https://example.com/sao-2.4.1.zip [--notes "..."] [--kind full|delta|runtime-delta]

if (args.Length == 0) { PrintHelp(); return 0; }
try
{
    var rest = args.Skip(1).ToArray();
    return args[0].ToLowerInvariant() switch
    {
        "pack"    => RunPack(rest),
        "publish" => await RunPublishAsync(rest),
        "hash"    => RunHash(rest),
        "help" or "--help" or "-h" => PrintHelp(),
        _ => Fail($"unknown command: {args[0]}"),
    };
}
catch (Exception ex)
{
    Console.Error.WriteLine($"error: {ex.Message}");
    return 1;
}

static int PrintHelp()
{
    Console.WriteLine("SaoAuto.DevPublish — package + publish update manifests");
    Console.WriteLine();
    Console.WriteLine("  pack    <source-dir> <out.zip>            zip up an install directory");
    Console.WriteLine("  hash    <file>                             print SHA-256 of a file");
    Console.WriteLine("  publish <zip> --version <v> [--channel stable] [--target windows-x64]");
    Console.WriteLine("                --url <host>/publish --package-url <url> [--notes \"...\"] [--kind full]");
    return 0;
}

static int Fail(string msg) { Console.Error.WriteLine($"error: {msg}"); return 64; }

static int RunPack(string[] a)
{
    if (a.Length < 2) return Fail("pack <source-dir> <out.zip>");
    var src = a[0]; var dst = a[1];
    if (!Directory.Exists(src)) return Fail($"source not a directory: {src}");
    if (File.Exists(dst)) File.Delete(dst);
    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(dst)) ?? ".");
    ZipFile.CreateFromDirectory(src, dst, CompressionLevel.Optimal, includeBaseDirectory: false);
    var sha = ApplyEngine.Sha256Hex(dst);
    var size = new FileInfo(dst).Length;
    Console.WriteLine($"packed {src} -> {dst}");
    Console.WriteLine($"size:   {size:N0} bytes");
    Console.WriteLine($"sha256: {sha}");
    return 0;
}

static int RunHash(string[] a)
{
    if (a.Length < 1) return Fail("hash <file>");
    Console.WriteLine(ApplyEngine.Sha256Hex(a[0]));
    return 0;
}

static async Task<int> RunPublishAsync(string[] a)
{
    if (a.Length < 1) return Fail("publish <zip> --version <v> --url <host>/publish --package-url <url>");
    var zip = a[0];
    if (!File.Exists(zip)) return Fail($"zip not found: {zip}");

    string? version = null, channel = "stable", target = "windows-x64",
        host = null, packageUrl = null, notes = null, kind = "full";
    for (int i = 1; i < a.Length - 1; i += 2)
    {
        switch (a[i])
        {
            case "--version":     version = a[i + 1]; break;
            case "--channel":     channel = a[i + 1]; break;
            case "--target":      target = a[i + 1]; break;
            case "--url":         host = a[i + 1]; break;
            case "--package-url": packageUrl = a[i + 1]; break;
            case "--notes":       notes = a[i + 1]; break;
            case "--kind":        kind = a[i + 1]; break;
            default: return Fail($"unknown flag: {a[i]}");
        }
    }
    if (version is null || host is null || packageUrl is null)
        return Fail("--version / --url / --package-url required");

    var manifest = new UpdateManifest(
        Version: version,
        Channel: channel ?? "stable",
        Target: target ?? "windows-x64",
        PackageUrl: packageUrl,
        PackageSha256: ApplyEngine.Sha256Hex(zip),
        PackageSize: new FileInfo(zip).Length,
        Kind: ParseKind(kind),
        Notes: notes,
        PublishedAt: DateTimeOffset.UtcNow);

    using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
    var resp = await http.PostAsJsonAsync(host, manifest, new JsonSerializerOptions
    {
        Converters = { new System.Text.Json.Serialization.JsonStringEnumConverter() },
    });
    Console.WriteLine($"POST {host} → {(int)resp.StatusCode} {resp.StatusCode}");
    var body = await resp.Content.ReadAsStringAsync();
    if (!string.IsNullOrEmpty(body)) Console.WriteLine(body);
    return resp.IsSuccessStatusCode ? 0 : 2;
}

static UpdatePackageKind ParseKind(string? s) => (s ?? "full").ToLowerInvariant() switch
{
    "delta" => UpdatePackageKind.Delta,
    "runtime-delta" or "runtime_delta" => UpdatePackageKind.RuntimeDelta,
    _ => UpdatePackageKind.Full,
};
