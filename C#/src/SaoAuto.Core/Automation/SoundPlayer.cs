using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Automation;

/// <summary>
/// Sound playback contract. Mirrors <c>sao_sound.play_sound</c>:
/// async enqueue, single-thread player, no blocking on the caller.
/// </summary>
public interface ISoundPlayer : IDisposable
{
    bool Enabled { get; set; }
    /// <summary>Master volume 0..100 (kept for parity; the WAV impl is hardcoded full-volume).</summary>
    int VolumePct { get; set; }
    /// <summary>Enqueue a clip by filesystem path. Silently drops if disabled or queue full.</summary>
    void Play(string path);
    void Stop();
}

/// <summary>
/// Minimal WAV-only player using <see cref="System.Media.SoundPlayer"/>. No
/// external dependency. NAudio could be swapped in later for MP3/OGG support
/// without touching <see cref="AutomationCore"/>.
/// </summary>
public sealed class WavSoundPlayer : ISoundPlayer
{
    private readonly SoundConcurrencyGuard _guard;
    private readonly ILogger _log;
    private readonly BlockingCollection<string> _queue = new(new ConcurrentQueue<string>(), boundedCapacity: 32);
    private readonly Thread _worker;
    private readonly CancellationTokenSource _cts = new();
    private int _disposed;

    public bool Enabled { get; set; } = true;
    public int VolumePct { get; set; } = 70;

    public WavSoundPlayer(SoundConcurrencyGuard? guard = null, ILogger? logger = null)
    {
        _guard = guard ?? new SoundConcurrencyGuard();
        _log = logger ?? NullLogger.Instance;
        _worker = new Thread(WorkerLoop) { IsBackground = true, Name = "SaoAuto-Sound" };
        _worker.Start();
    }

    public void Play(string path)
    {
        if (!Enabled || string.IsNullOrWhiteSpace(path)) return;
        if (_cts.IsCancellationRequested) return;
        try { _queue.TryAdd(path); }
        catch (InvalidOperationException) { /* completed */ }
    }

    public void Stop()
    {
        try { _queue.CompleteAdding(); } catch { }
    }

    private void WorkerLoop()
    {
        try
        {
            foreach (var path in _queue.GetConsumingEnumerable(_cts.Token))
            {
                if (!Enabled) continue;
                if (!_guard.TryBeginPlayback(path)) continue;
                try
                {
                    if (!OperatingSystem.IsWindows())
                    {
                        _log.LogDebug("[Sound] skipped {Path} (non-windows host)", path);
                        continue;
                    }
                    if (!File.Exists(path))
                    {
                        _log.LogWarning("[Sound] missing clip {Path}", path);
                        continue;
                    }
                    // SND_FILENAME (0x20000) | SND_SYNC (0x0).  WAV only — matches the
                    // pygame.mixer baseline; MP3/OGG support would need NAudio later.
                    PlaySound(path, IntPtr.Zero, 0x00020000);
                }
                catch (Exception ex)
                {
                    _log.LogWarning(ex, "[Sound] failed to play {Path}", path);
                }
                finally
                {
                    _guard.EndPlayback(path);
                }
            }
        }
        catch (OperationCanceledException) { /* shutting down */ }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        try { _cts.Cancel(); } catch { }
        try { _queue.CompleteAdding(); } catch { }
        try { _worker.Join(500); } catch { }
        _queue.Dispose();
        _cts.Dispose();
    }

    [DllImport("winmm.dll", CharSet = CharSet.Unicode, SetLastError = false)]
    private static extern bool PlaySound(string? pszSound, IntPtr hmod, uint fdwSound);
}

/// <summary>No-op player for tests / headless mode.</summary>
public sealed class NullSoundPlayer : ISoundPlayer
{
    public bool Enabled { get; set; } = true;
    public int VolumePct { get; set; } = 70;
    public List<string> Played { get; } = new();
    public void Play(string path) { if (Enabled) Played.Add(path); }
    public void Stop() { }
    public void Dispose() { }
}
