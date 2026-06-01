namespace SaoAuto.Core.Automation.HideSeek;

/// <summary>
/// HS-08 — Optional debug image dump surface for HideSeek detection. When
/// attached, the detector / state machine calls <see cref="SaveStep"/> with
/// the per-step ROI, the colour-masked filtered image, and the bundled
/// template so an offline pipeline can diff against the Python reference
/// (hide_seek_engine.py:572-619 debug dump). Implementations are expected
/// to be best-effort: file-system writes / quota / formats are the sink's
/// concern, the detector doesn't see exceptions.
/// </summary>
public interface IHideSeekDebugSink
{
    /// <summary>
    /// Persist a single step's worth of grayscale frames.
    /// </summary>
    /// <param name="stepIndex">0-based step number within the current run.</param>
    /// <param name="stepLabel">Human-readable label (e.g. "step1_corner").</param>
    /// <param name="roiPixels">Grayscale ROI captured from the live screen.</param>
    /// <param name="roiWidth">ROI width in pixels.</param>
    /// <param name="roiHeight">ROI height in pixels.</param>
    /// <param name="filteredPixels">Colour-masked grayscale (post HSV filter).</param>
    /// <param name="filteredWidth">Filtered width (matches roi unless rescaled).</param>
    /// <param name="filteredHeight">Filtered height.</param>
    /// <param name="templatePixels">Bundled template the matcher used.</param>
    /// <param name="templateWidth">Template width.</param>
    /// <param name="templateHeight">Template height.</param>
    /// <param name="score">Final TM_CCOEFF_NORMED score (1 = perfect, -1 = anti-match).</param>
    /// <param name="found">Whether the step's threshold passed.</param>
    void SaveStep(
        int stepIndex,
        string stepLabel,
        ReadOnlySpan<byte> roiPixels, int roiWidth, int roiHeight,
        ReadOnlySpan<byte> filteredPixels, int filteredWidth, int filteredHeight,
        ReadOnlySpan<byte> templatePixels, int templateWidth, int templateHeight,
        double score, bool found);
}

/// <summary>
/// HS-08 — No-op sink used as the default so the detector never has to
/// null-check. Attach a real sink (PGM dumper, in-memory ring buffer) when
/// diagnostics are needed.
/// </summary>
public sealed class NullHideSeekDebugSink : IHideSeekDebugSink
{
    public static readonly NullHideSeekDebugSink Instance = new();

    public void SaveStep(
        int stepIndex,
        string stepLabel,
        ReadOnlySpan<byte> roiPixels, int roiWidth, int roiHeight,
        ReadOnlySpan<byte> filteredPixels, int filteredWidth, int filteredHeight,
        ReadOnlySpan<byte> templatePixels, int templateWidth, int templateHeight,
        double score, bool found)
    {
        // No-op by design.
    }
}
