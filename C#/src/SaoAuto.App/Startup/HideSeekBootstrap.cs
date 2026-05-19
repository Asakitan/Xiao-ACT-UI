using Microsoft.Extensions.Logging;
using SaoAuto.Core.Automation.HideSeek;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Vision;

namespace SaoAuto.App.Startup;

/// <summary>
/// S153 — Compose the full HideSeek pipeline from the App layer:
/// load PNG templates from <c>assets/</c>, wrap the shared
/// recognition capture in a <see cref="HideSeekFrameProvider"/>,
/// build a <see cref="Win32HideSeekInput"/>, and assemble the
/// state machine + tick host.
///
/// Mirrors how <see cref="RecognitionPipelineBootstrap"/> assembles
/// the recognition stack — single place the runners call so the
/// dependency graph stays out of <c>UiRunner</c>/<c>HeadlessRunner</c>.
/// </summary>
public static class HideSeekBootstrap
{
    /// <summary>
    /// Build the HideSeek tick host. Throws when no templates could be
    /// loaded (assets missing or all decodes failed); callers wrap the
    /// call in <see cref="HideSeekLifecycle.Start"/> so the failure is
    /// logged + swallowed rather than blocking startup.
    /// </summary>
    public static HideSeekTickHost Build(
        WindowLocator locator,
        ResourcePathResolver paths,
        Func<string, HideSeekTemplates.DecodedImage?>? decode = null,
        ILogger? log = null)
    {
        if (locator is null) throw new ArgumentNullException(nameof(locator));
        if (paths is null) throw new ArgumentNullException(nameof(paths));
        var decoder = decode ?? WpfImageDecoder.Decode;

        var steps = HideSeekSteps.Default;
        var templates = HideSeekTemplates.LoadAll(paths.Assets, steps, decoder, out var missing);
        if (templates.Count == 0)
        {
            throw new InvalidOperationException(
                $"HideSeek: no templates loaded from '{paths.Assets}' " +
                $"(missing={missing.Count}); aborting wiring.");
        }
        if (missing.Count > 0)
        {
            log?.LogWarning(
                "[HideSeek] {N} template(s) missing: {Files}",
                missing.Count, string.Join(", ", missing.Select(m => m.ImageFile)));
        }

        // Dedicated capture instance so the recognition tick and HideSeek
        // tick do not race on shared GDI buffers. Both pipelines use the
        // same WindowLocator so the located HWND is consistent.
        Func<WindowCandidate?> windowProvider = () => locator.FindGameWindow();
        var capture = new GdiFrameCapture(windowProvider);

        var frames = new HideSeekFrameProvider(capture, locator);
        var input = new Win32HideSeekInput();
        var machine = new HideSeekStateMachine(steps, templates, frames, input,
            status: (msg, step) => log?.LogDebug("[HideSeek] step={Step}: {Msg}", step, msg));
        return new HideSeekTickHost(machine);
    }
}
