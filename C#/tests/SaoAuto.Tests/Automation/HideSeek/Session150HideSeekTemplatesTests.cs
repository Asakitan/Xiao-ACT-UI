using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.Tests.Automation.HideSeek;

/// <summary>
/// S150 — Pin <see cref="HideSeekTemplates"/>: Build converts BGR
/// to grayscale with BT.601 weights, LoadAll walks steps, dedups
/// shared filenames, reports missing entries, never throws on
/// missing files.
/// </summary>
public class Session150HideSeekTemplatesTests
{
    [Fact]
    public void BuildConvertsBgrToGrayscale()
    {
        // 3×1 BGR: red, green, blue → 76, 150, 29 (matcher weights)
        var px = new byte[] { 0, 0, 255, 0, 255, 0, 255, 0, 0 };
        var tpl = HideSeekTemplates.Build("x.png", new HideSeekTemplates.DecodedImage(px, 3, 1, 9, 3));
        Assert.Equal("x.png", tpl.Name);
        Assert.Equal(3, tpl.Width);
        Assert.Equal(1, tpl.Height);
        Assert.InRange((int)tpl.Grayscale[0], 75, 77);
        Assert.InRange((int)tpl.Grayscale[1], 149, 151);
        Assert.InRange((int)tpl.Grayscale[2], 28, 30);
    }

    [Fact]
    public void LoadAllAsksDecoderForEachUniqueFileAndDedups()
    {
        var steps = HideSeekSteps.Default; // 5 steps with 5 distinct files 1..5.png
        var seen = new List<string>();
        var map = HideSeekTemplates.LoadAll("C:/assets", steps, path =>
        {
            seen.Add(path);
            return new HideSeekTemplates.DecodedImage(new byte[3] { 10, 20, 30 }, 1, 1, 3, 3);
        }, out var missing);

        Assert.Equal(steps.Select(s => s.ImageFile).Distinct().Count(), map.Count);
        Assert.Equal(map.Count, seen.Count); // decoder called once per unique file
        Assert.Empty(missing);
        Assert.All(map.Values, t => Assert.Equal(1, t.Width));
    }

    [Fact]
    public void LoadAllReportsMissingFilesWithoutThrowing()
    {
        var steps = HideSeekSteps.Default;
        var map = HideSeekTemplates.LoadAll("C:/assets", steps,
            _ => (HideSeekTemplates.DecodedImage?)null, out var missing);

        Assert.Empty(map);
        Assert.Equal(steps.Select(s => s.ImageFile).Distinct().Count(), missing.Count);
        Assert.All(missing, m => Assert.StartsWith("C:/assets", m.FullPath.Replace('\\', '/')));
    }

    [Fact]
    public void LoadAllSkipsRedundantDecodeForRepeatedImageFile()
    {
        // Build a custom step list where two steps share the same file.
        var step = HideSeekSteps.Default[0];
        var dup = new[]
        {
            step with { Name = "A", ImageFile = "shared.png" },
            step with { Name = "B", ImageFile = "shared.png" },
        };
        int decodeCalls = 0;
        var map = HideSeekTemplates.LoadAll("/dir", dup, _ =>
        {
            decodeCalls++;
            return new HideSeekTemplates.DecodedImage(new byte[1] { 7 }, 1, 1, 1, 1);
        }, out var missing);
        Assert.Equal(1, decodeCalls);
        Assert.Single(map);
        Assert.Empty(missing);
    }

    [Fact]
    public void LoadAllValidatesArguments()
    {
        Assert.Throws<ArgumentNullException>(() =>
            HideSeekTemplates.LoadAll(null!, HideSeekSteps.Default, _ => null, out _));
        Assert.Throws<ArgumentNullException>(() =>
            HideSeekTemplates.LoadAll("d", null!, _ => null, out _));
        Assert.Throws<ArgumentNullException>(() =>
            HideSeekTemplates.LoadAll("d", HideSeekSteps.Default, null!, out _));
    }
}
