namespace SaoAuto.Core.Configuration;

public sealed class ResourcePathResolver
{
    public string BaseDir { get; }
    public string BundleDir { get; }

    public ResourcePathResolver(string baseDir, string bundleDir)
    {
        BaseDir = baseDir ?? throw new ArgumentNullException(nameof(baseDir));
        BundleDir = bundleDir ?? throw new ArgumentNullException(nameof(bundleDir));
    }

    public static ResourcePathResolver ForCurrentProcess()
    {
        var exeDir = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var bundleDir = Path.Combine(exeDir, "runtime");
        if (!Directory.Exists(bundleDir))
        {
            bundleDir = exeDir;
        }
        return new ResourcePathResolver(exeDir, bundleDir);
    }

    public string Settings => Path.Combine(BaseDir, "settings.json");
    public string Assets => Resolve("assets");
    public string Sounds => Resolve("assets", "sounds");
    public string Fonts => Resolve("assets", "fonts");
    public string Web => Resolve("web");
    public string Proto => Resolve("proto");

    public string Staging => Path.Combine(BaseDir, "staging");
    public string Backup => Path.Combine(BaseDir, "backup");
    public string Temp => Path.Combine(BaseDir, "temp");
    public string UpdateState => Path.Combine(BaseDir, "update_state.json");

    public string Resolve(params string[] parts)
    {
        if (parts.Length == 0)
        {
            return BaseDir;
        }
        var top = Path.Combine(new[] { BaseDir }.Concat(parts).ToArray());
        if (Directory.Exists(top) || File.Exists(top))
        {
            return top;
        }
        return Path.Combine(new[] { BundleDir }.Concat(parts).ToArray());
    }
}
