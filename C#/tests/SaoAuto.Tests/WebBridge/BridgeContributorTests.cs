using System.Text.Json.Nodes;
using SaoAuto.App.WebBridge;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.State;

namespace SaoAuto.Tests.WebBridge;

public sealed class BridgeContributorTests : IDisposable
{
    private readonly string _workDir;
    private readonly string _path;

    public BridgeContributorTests()
    {
        _workDir = Path.Combine(Path.GetTempPath(), "saoauto-contributor-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        _path = Path.Combine(_workDir, "settings.json");
        File.WriteAllText(_path, "{}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_workDir, recursive: true); } catch { }
    }

    private SettingsManager NewSettings() => new(_path);

    [Fact]
    public void AttachContributorPassesHostContext()
    {
        var states = new GameStateManager();
        using var lifecycle = new WebBridgeLifecycle(states);
        var settings = NewSettings();
        var contributor = new RecordingContributor();

        using var attachment = lifecycle.AttachContributor(contributor, settings);

        Assert.Same(lifecycle.Router, contributor.Context!.Router);
        Assert.Same(lifecycle.Broadcaster, contributor.Context.Broadcaster);
        Assert.Same(settings, contributor.Context.Settings);
        Assert.Same(states, contributor.Context.States);
        Assert.Contains(RecordingContributor.Command, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachmentCanUnregisterContributorCommands()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var contributor = new RecordingContributor();

        var attachment = lifecycle.AttachContributor(contributor, NewSettings());
        attachment.Dispose();

        Assert.True(contributor.Disposed);
        Assert.DoesNotContain(RecordingContributor.Command, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void LifecycleDisposeDisposesContributorAttachments()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var contributor = new RecordingContributor();

        lifecycle.AttachContributor(contributor, NewSettings());
        lifecycle.Dispose();

        Assert.True(contributor.Disposed);
        Assert.DoesNotContain(RecordingContributor.Command, lifecycle.Router.RegisteredCommands);
    }

    [Fact]
    public void AttachContributorRejectsNullArguments()
    {
        using var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        var settings = NewSettings();

        Assert.Throws<ArgumentNullException>(() => lifecycle.AttachContributor(null!, settings));
        Assert.Throws<ArgumentNullException>(() => lifecycle.AttachContributor(new RecordingContributor(), null!));
    }

    [Fact]
    public void AttachContributorAfterDisposeThrows()
    {
        var lifecycle = new WebBridgeLifecycle(new GameStateManager());
        lifecycle.Dispose();

        Assert.Throws<ObjectDisposedException>(() => lifecycle.AttachContributor(new RecordingContributor(), NewSettings()));
    }

    private sealed class RecordingContributor : IBridgeContributor
    {
        public const string Command = "test.contributor";

        public BridgeContributorContext? Context { get; private set; }
        public bool Disposed { get; private set; }
        public string Name => "recording";

        public IDisposable Attach(BridgeContributorContext context)
        {
            Context = context ?? throw new ArgumentNullException(nameof(context));
            context.Router.Register(Command, _ => new JsonObject { ["ok"] = true });
            return new DelegateDisposable(() =>
            {
                Disposed = true;
                context.Router.Unregister(Command);
            });
        }
    }

    private sealed class DelegateDisposable(Action dispose) : IDisposable
    {
        private bool _disposed;

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            dispose();
        }
    }
}
