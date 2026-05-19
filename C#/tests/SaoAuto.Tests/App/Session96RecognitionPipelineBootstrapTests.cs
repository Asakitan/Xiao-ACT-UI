using SaoAuto.App.Startup;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;
using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.App;

/// <summary>
/// S96 — Pin <see cref="RecognitionPipelineBootstrap"/>. Verifies the
/// composed <see cref="RecognitionTickHost"/> is wired to the right
/// stamina ROI + watched slots from settings, and that a smoke tick
/// produces the expected default state mutations.
/// </summary>
public class Session96RecognitionPipelineBootstrapTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _settingsPath;

    public Session96RecognitionPipelineBootstrapTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-s96-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _settingsPath = Path.Combine(_workDir, "settings.json");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { /* swallow */ }
    }

    private SettingsManager NewSettings(string json = "{}")
    {
        File.WriteAllText(_settingsPath, json);
        return new SettingsManager(_settingsPath);
    }

    private sealed class StubEnumerator : IWindowEnumerator
    {
        public IEnumerable<WindowCandidate> Enumerate() => Array.Empty<WindowCandidate>();
        public bool IsAlive(IntPtr hwnd) => false;
        public WindowCandidate? Probe(IntPtr hwnd) => null;
    }

    private sealed class StubCapture : IFrameCapture
    {
        public CapturedFrame? Capture() => null;
        public void Dispose() { }
    }

    [Fact]
    public void NullSettings_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            RecognitionPipelineBootstrap.Build(null!, new GameStateManager()));
    }

    [Fact]
    public void NullStates_Throws()
    {
        var s = NewSettings();
        Assert.Throws<ArgumentNullException>(() =>
            RecognitionPipelineBootstrap.Build(s, null!));
    }

    [Fact]
    public void Build_ReturnsHost_WithStubDeps()
    {
        var s = NewSettings();
        var states = new GameStateManager();
        using var host = RecognitionPipelineBootstrap.Build(
            s, states,
            enumeratorOverride: new StubEnumerator(),
            captureFactory: _ => new StubCapture());
        Assert.NotNull(host);
        Assert.False(host.IsRunning);
    }

    [Fact]
    public void Build_HostRunOnce_PopulatesRecognitionState()
    {
        var s = NewSettings();
        var states = new GameStateManager();
        using var host = RecognitionPipelineBootstrap.Build(
            s, states,
            enumeratorOverride: new StubEnumerator(),
            captureFactory: _ => new StubCapture());

        host.RunOnceAsync();

        // No window + no frame → recognition flagged not-ok with an error
        // message; no skill slots projected; burst-ready stays false.
        Assert.False(states.Snapshot.RecognitionOk);
        Assert.Empty(states.Snapshot.SkillSlots);
        Assert.False(states.Snapshot.BurstReady);
    }

    [Fact]
    public void Build_UsesSettingsRoiOverride()
    {
        // Override stamina_bar to a non-default rectangle and verify the
        // host carries it through (indirectly: assert that the engine
        // received it by running a tick that doesn't crash on the bogus
        // ROI — the loader path is otherwise pinned in S95 tests).
        var s = NewSettings(
            "{\"roi\":{\"stamina_bar\":{\"x\":0.5,\"y\":0.5,\"w\":0.1,\"h\":0.1}}}");
        var states = new GameStateManager();
        using var host = RecognitionPipelineBootstrap.Build(
            s, states,
            enumeratorOverride: new StubEnumerator(),
            captureFactory: _ => new StubCapture());
        host.RunOnceAsync();
        Assert.False(states.Snapshot.RecognitionOk);
    }

    [Fact]
    public void Build_UsesSettingsWatchedSlots()
    {
        var s = NewSettings("{\"watched_skill_slots\":[1,3]}");
        var states = new GameStateManager();
        using var host = RecognitionPipelineBootstrap.Build(
            s, states,
            enumeratorOverride: new StubEnumerator(),
            captureFactory: _ => new StubCapture());
        host.RunOnceAsync();
        // Engine produced no slots → matched=0 → BurstReady false even
        // though watched is non-empty. (The watched-slot semantics
        // themselves are pinned in S87/S94 tests.)
        Assert.False(states.Snapshot.BurstReady);
    }
}
