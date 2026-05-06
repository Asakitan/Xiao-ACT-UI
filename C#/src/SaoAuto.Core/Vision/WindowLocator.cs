using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Vision;

/// <summary>
/// Game-window finder ported from <c>window_locator.WindowLocator</c>. The
/// concrete Win32 enumeration is delegated to <see cref="IWindowEnumerator"/>
/// so unit tests can inject a fake without P/Invoke. Title keywords match
/// case-insensitively; process-name match falls back when the title list misses.
/// </summary>
public sealed class WindowLocator
{
    private readonly IWindowEnumerator _enumerator;
    private readonly IReadOnlyList<string> _titleKeywords;
    private readonly HashSet<string> _processNames;
    private readonly ILogger _log;

    private IntPtr _cachedHwnd;
    private bool _loggedDiscovery;

    public WindowLocator(
        IWindowEnumerator enumerator,
        IEnumerable<string>? titleKeywords = null,
        IEnumerable<string>? processNames = null,
        ILogger? logger = null)
    {
        _enumerator = enumerator ?? throw new ArgumentNullException(nameof(enumerator));
        _titleKeywords = (titleKeywords ?? GameWindowConfig.DefaultTitleKeywords).ToArray();
        _processNames = new HashSet<string>(
            (processNames ?? GameWindowConfig.DefaultProcessNames).Select(p => p.ToLowerInvariant()));
        _log = logger ?? NullLogger.Instance;
    }

    public IReadOnlyList<string> TitleKeywords => _titleKeywords;
    public IReadOnlyCollection<string> ProcessNames => _processNames;

    /// <summary>
    /// Returns the cached candidate when it is still alive and sized correctly,
    /// otherwise re-enumerates all top-level windows and picks the first match.
    /// </summary>
    public WindowCandidate? FindGameWindow()
    {
        if (_cachedHwnd != IntPtr.Zero && _enumerator.IsAlive(_cachedHwnd))
        {
            var probed = _enumerator.Probe(_cachedHwnd);
            if (probed is { } cached && cached.LooksLikeGameWindow)
            {
                return cached;
            }
        }

        foreach (var candidate in _enumerator.Enumerate())
        {
            if (!candidate.LooksLikeGameWindow) continue;
            if (!Matches(candidate)) continue;

            _cachedHwnd = candidate.Hwnd;
            if (!_loggedDiscovery)
            {
                _loggedDiscovery = true;
                _log.LogInformation("[Vision] located game window: {Candidate}", candidate);
            }
            return candidate;
        }

        _cachedHwnd = IntPtr.Zero;
        return null;
    }

    /// <summary>Pure matcher exposed for tests.</summary>
    public bool Matches(WindowCandidate candidate)
    {
        var title = candidate.Title;
        if (!string.IsNullOrEmpty(title))
        {
            foreach (var keyword in _titleKeywords)
            {
                if (title.Contains(keyword, StringComparison.OrdinalIgnoreCase)) return true;
            }
        }
        if (_processNames.Count > 0 && candidate.ProcessName is { } proc)
        {
            if (_processNames.Contains(proc.ToLowerInvariant())) return true;
        }
        return false;
    }
}
