namespace SaoAuto.Core.Updater;

/// <summary>
/// Update manifest as served by the host's <c>/latest</c> endpoint.
/// Mirrors Python's <c>sao_updater.fetch_latest</c> JSON shape.
/// </summary>
public sealed record UpdateManifest(
    string Version,
    string Channel,
    string Target,
    string PackageUrl,
    string PackageSha256,
    long PackageSize,
    UpdatePackageKind Kind,
    string? Notes,
    DateTimeOffset PublishedAt);

public enum UpdatePackageKind
{
    Full,
    Delta,
    RuntimeDelta,
}
