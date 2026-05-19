namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// S150 — Mirrors Python <c>HideSeekEngine._load_templates</c>. Walks
/// the configured step list, asks the host to decode each image file
/// (path → BGR pixels) and pre-converts to the grayscale buffer the
/// matcher consumes. Decoding is injected so Core stays free of any
/// platform-specific image library (System.Drawing, SkiaSharp, …).
/// </summary>
public static class HideSeekTemplates
{
    /// <summary>
    /// Result of a host-side decode: raw BGR/BGRA bytes plus stride
    /// + channels so packed and stride-padded buffers are both
    /// honoured. Stride is in bytes per row, channels is 3 or 4.
    /// </summary>
    public readonly record struct DecodedImage(byte[] Pixels, int Width, int Height, int Stride, int Channels);

    /// <summary>
    /// Build a single template from already-decoded BGR(A) pixels.
    /// Converts to grayscale using the same BT.601 weights the
    /// matcher uses.
    /// </summary>
    public static HideSeekTemplate Build(string name, DecodedImage image)
    {
        var gray = HideSeekTemplateMatcher.ToGrayscale(
            image.Pixels, image.Width, image.Height, image.Stride, image.Channels);
        return new HideSeekTemplate(name, gray, image.Width, image.Height);
    }

    /// <summary>
    /// Load every unique image file referenced by <paramref name="steps"/>.
    /// <paramref name="decode"/> receives the full path and returns the
    /// decoded BGR buffer or null when the file is missing / unreadable —
    /// missing entries are surfaced via <paramref name="missing"/> so the
    /// host can warn the user without throwing.
    /// </summary>
    /// <param name="assetsDir">Directory containing 1.png..N.png.</param>
    /// <param name="steps">Step table whose ImageFile fields drive loading.</param>
    /// <param name="decode">Host-side PNG/BMP decoder; null = file missing or unreadable.</param>
    /// <param name="missing">Receives the list of (file, fullPath) pairs that failed to decode.</param>
    public static IReadOnlyDictionary<string, HideSeekTemplate> LoadAll(
        string assetsDir,
        IReadOnlyList<HideSeekStep> steps,
        Func<string, DecodedImage?> decode,
        out IReadOnlyList<(string ImageFile, string FullPath)> missing)
    {
        ArgumentNullException.ThrowIfNull(assetsDir);
        ArgumentNullException.ThrowIfNull(steps);
        ArgumentNullException.ThrowIfNull(decode);

        var map = new Dictionary<string, HideSeekTemplate>(StringComparer.OrdinalIgnoreCase);
        var miss = new List<(string, string)>();
        foreach (var step in steps)
        {
            var img = step.ImageFile;
            if (map.ContainsKey(img)) continue; // dedup — Confirm-3/4 share files in some setups.
            var path = Path.Combine(assetsDir, img);
            var decoded = decode(path);
            if (decoded is { } d)
                map[img] = Build(img, d);
            else
                miss.Add((img, path));
        }
        missing = miss;
        return map;
    }
}
