namespace SaoAuto.Overlay;

/// <summary>
/// One row of the GPU-popup session roster. Mirrors the dict shape Python
/// hands to <c>session_*</c> helpers in <c>_sao_cy_uihelpers.pyx</c>.
/// </summary>
public sealed record SessionRow(
    string Uid,
    string Name,
    long FightPowerValue,
    string FightPower,
    bool IsSelf);

/// <summary>One compact roster row for the GPU popup display.</summary>
public sealed record SessionVisibleRow(
    string ShortName,
    string Uid,
    string FightPower,
    bool IsSelf);

/// <summary>Result tuple for <see cref="SessionRoster.OpenAnimGeometry"/>.</summary>
public sealed record SessionOpenAnim(
    double T,
    double Ease,
    int Height,
    int Offset,
    bool HighlightOn);

/// <summary>
/// Roster + SAO-FX helpers ported from <c>_sao_cy_uihelpers.pyx</c>:
/// <c>session_rows_signature</c>, <c>session_self_uid</c>,
/// <c>clamp_session_first_index</c>, <c>session_scroll_delta</c>,
/// <c>session_scroll_first_index</c>, <c>session_visible_rows</c>,
/// <c>session_open_anim_geometry</c>, <c>sao_fx_coords</c>, <c>scan_x</c>.
/// </summary>
public static class SessionRoster
{
    public static IReadOnlyList<(string Uid, string Name, long Fp, bool IsSelf)> RowsSignature(
        IEnumerable<SessionRow>? rows)
    {
        if (rows is null) return Array.Empty<(string, string, long, bool)>();
        var sig = new List<(string, string, long, bool)>();
        foreach (var r in rows)
        {
            if (r is null) continue;
            sig.Add((r.Uid ?? string.Empty, r.Name ?? string.Empty, r.FightPowerValue, r.IsSelf));
        }
        return sig;
    }

    public static string SelfUid(IEnumerable<SessionRow>? rows)
    {
        if (rows is null) return string.Empty;
        foreach (var r in rows)
        {
            if (r is null) continue;
            if (r.IsSelf) return r.Uid ?? string.Empty;
        }
        return string.Empty;
    }

    public static int ClampFirstIndex(int first, int total, int visibleCount)
    {
        int maxFirst = total - visibleCount;
        if (maxFirst < 0) maxFirst = 0;
        if (first < 0) return 0;
        if (first > maxFirst) return maxFirst;
        return first;
    }

    /// <summary>
    /// Normalize Windows/X11 wheel events to roster scroll units. <paramref name="num"/>
    /// matches the X11 button number (4=up, 5=down); <paramref name="rawDelta"/> is
    /// the Windows WM_MOUSEWHEEL value (multiples of 120).
    /// </summary>
    public static int ScrollDelta(long num, long rawDelta)
    {
        if (num == 4) return -3;
        if (num == 5) return 3;
        int delta = (int)(-1.0 * (rawDelta / 120.0));
        if (delta == 0) return rawDelta > 0 ? -1 : 1;
        return delta;
    }

    public static int ScrollFirstIndex(int first, int total, int visibleCount, int delta)
        => ClampFirstIndex(first + delta, total, visibleCount);

    public static IReadOnlyList<SessionVisibleRow> VisibleRows(
        IReadOnlyList<SessionRow>? rows, int first, int visibleCount)
    {
        int total = rows?.Count ?? 0;
        int start = ClampFirstIndex(first, total, visibleCount);
        int end = start + visibleCount;
        if (end > total) end = total;
        var out_ = new List<SessionVisibleRow>(Math.Max(0, end - start));
        if (rows is null) return out_;
        for (int i = start; i < end; i++)
        {
            var r = rows[i];
            if (r is null) continue;
            out_.Add(new SessionVisibleRow(
                CyUiHelpers.ShortSessionName(r.Name),
                string.IsNullOrEmpty(r.Uid) ? "--" : r.Uid,
                string.IsNullOrEmpty(r.FightPower) ? "--" : r.FightPower,
                r.IsSelf));
        }
        return out_;
    }

    public static SessionOpenAnim OpenAnimGeometry(int panelH, double elapsed, double duration)
    {
        if (panelH < 1) panelH = 1;
        double t;
        if (duration <= 0.0) t = 1.0;
        else
        {
            t = elapsed / duration;
            if (t < 0.0) t = 0.0;
            else if (t > 1.0) t = 1.0;
        }
        double inv = 1.0 - t;
        double ease = 1.0 - inv * inv * inv;
        int height = (int)(panelH * ease + 0.5);
        if (height < 1) height = 1;
        int offset = (int)(64.0 * (1.0 - ease) + 0.5);
        return new SessionOpenAnim(t, ease, height, offset, t < 0.55);
    }

    /// <summary>
    /// Integer SAO-HUD decoration coords (left_far, left_near, right_far, right_near)
    /// for the shared panel tick. Mirrors <c>sao_fx_coords</c>.
    /// </summary>
    public static (int LeftFar, int LeftNear, int RightFar, int RightNear) SaoFxCoords(
        double tt, long panelId, int width)
    {
        double t = tt + (panelId % 17) * 0.13;
        int leftFar = (int)(10.0 + 5.0 * Math.Sin(t * 0.66));
        int leftNear = (int)(20.0 + 12.0 * Math.Sin(t * 1.35 + 0.8));
        int rightFar = (int)(width - 18.0 + 7.0 * Math.Sin(t * 0.72 + 1.1));
        int rightNear = (int)(width - 34.0 + 12.0 * Math.Sin(t * 1.45 + 2.1));
        return (leftFar, leftNear, rightFar, rightNear);
    }

    /// <summary>Tk scan-bar x coord for the player-info panel. Mirrors <c>scan_x</c>.</summary>
    public static int ScanX(int width, double now)
        => 10 + (int)((width - 20) * CyUiHelpers.ScanPhase(now));
}
