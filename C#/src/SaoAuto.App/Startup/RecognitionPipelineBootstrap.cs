using Microsoft.Extensions.Logging;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.App.Startup;

/// <summary>
/// S96 — Compose the recognition pipeline (window locator → frame capture →
/// stamina/skill recognition engine → tick host) from the user's settings
/// + game-state manager. The single entry point the App-layer runners
/// (HeadlessRunner / UiRunner) need to spin up the recognition loop.
///
/// Mirrors Python <c>RecognitionEngine.__init__</c> + the
/// <c>main.run_headless</c> wiring that constructs the equivalent
/// dependency graph (recognition.py 130–180; main.py 60–95).
///
/// The resulting <see cref="RecognitionTickHost"/> is fully wired:
/// <list type="bullet">
/// <item><description>stamina ROI from <see cref="RoiLoader"/> ("stamina_bar")</description></item>
/// <item><description>watched skill-slot indices from <see cref="WatchedSkillSlotsLoader"/></description></item>
/// <item><description>window finder from <see cref="WindowLocator"/></description></item>
/// </list>
///
/// <see cref="IFrameCapture"/> + <see cref="IWindowEnumerator"/> can be
/// injected for tests; defaults are <see cref="GdiFrameCapture"/> +
/// <see cref="Win32WindowEnumerator"/>.
/// </summary>
public static class RecognitionPipelineBootstrap
{
    public static RecognitionTickHost Build(
        SettingsManager settings,
        GameStateManager states,
        ILogger? logger = null,
        IWindowEnumerator? enumeratorOverride = null,
        Func<Func<WindowCandidate?>, IFrameCapture>? captureFactory = null)
    {
        if (settings is null) throw new ArgumentNullException(nameof(settings));
        if (states is null) throw new ArgumentNullException(nameof(states));

        var log = logger ?? SaoLog.For("recognition");

        var enumerator = enumeratorOverride ?? new Win32WindowEnumerator();
        var titleKeywords = WindowMatchersLoader.LoadKeywords(settings);
        var processNames = WindowMatchersLoader.LoadProcessNames(settings);
        var locator = new WindowLocator(
            enumerator,
            titleKeywords: titleKeywords,
            processNames: processNames,
            logger: log);
        Func<WindowCandidate?> windowProvider = () => locator.FindGameWindow();

        var capture = captureFactory is null
            ? new GdiFrameCapture(windowProvider)
            : captureFactory(windowProvider);

        var staminaRoi = RoiLoader.Load(settings, "stamina_bar");
        var watched = WatchedSkillSlotsLoader.Load(settings);

        var engine = new RecognitionTickEngine(capture, windowProvider, staminaRoi);
        var host = new RecognitionTickHost(engine, states, watchedSlots: watched);

        log.LogInformation(
            "recognition pipeline built (roi={Roi}, watched=[{Watched}])",
            staminaRoi, string.Join(',', watched));
        return host;
    }
}
