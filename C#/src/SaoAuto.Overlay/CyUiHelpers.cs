namespace SaoAuto.Overlay;

/// <summary>
/// UI animation helpers ported from <c>_sao_cy_uihelpers.pyx</c>:
/// breathing offsets, skill-fx layout math, level-text formatting,
/// dead-state predicates, session-int parsing.
/// </summary>
public static class CyUiHelpers
{
    /// <summary>
    /// Per-frame sin offsets for breathing animation. Mirrors
    /// <c>breath_offsets(now, count, period, amp)</c>: returns a list of
    /// <paramref name="count"/> floats sampled along a phase-shifted sin
    /// curve so panels look like they breathe.
    /// </summary>
    public static double[] BreathOffsets(double monotonicSeconds, int count, double periodSeconds, double amplitude)
    {
        if (count <= 0 || periodSeconds <= 0) return Array.Empty<double>();
        var twoPi = Math.PI * 2;
        var phase = (monotonicSeconds % periodSeconds) / periodSeconds * twoPi;
        var step = twoPi / count;
        var result = new double[count];
        for (var i = 0; i < count; i++)
        {
            result[i] = Math.Sin(phase + i * step) * amplitude;
        }
        return result;
    }

    /// <summary>Format a level into the canonical "60(+12)" or "60" string.</summary>
    public static string FormatLevelText(int levelBase, int levelExtra)
    {
        if (levelBase < 0) levelBase = 0;
        if (levelExtra > 0) return $"{levelBase}(+{levelExtra})";
        return levelBase.ToString();
    }

    /// <summary>
    /// Mirrors <c>_normalize_watched_skill_slots</c>: keep only positive ints,
    /// drop duplicates, return them sorted.
    /// </summary>
    public static int[] NormalizeWatchedSkillSlots(IEnumerable<int>? raw)
    {
        if (raw is null) return Array.Empty<int>();
        return raw.Where(s => s > 0).Distinct().OrderBy(s => s).ToArray();
    }

    /// <summary>
    /// True when the given hp_pct is "dead-ish" — Python tolerance allows up
    /// to 0.001 (the same threshold the parser uses to distinguish a death
    /// event from a stale-zero packet).
    /// </summary>
    public static bool IsDeadState(double hpPct) => hpPct <= 0.001;

    /// <summary>
    /// Mirrors <c>_session_int</c>: best-effort int parse with default 0
    /// — accepts ints, longs, parsable strings; returns default on null
    /// or anything else.
    /// </summary>
    public static long SessionInt(object? value, long defaultValue = 0)
    {
        if (value is null) return defaultValue;
        switch (value)
        {
            case long l: return l;
            case int i: return i;
            case double d: return (long)d;
            case string s when long.TryParse(s, out var parsed): return parsed;
            default: return defaultValue;
        }
    }

    /// <summary>Format session power for the player-info panel: 1234 → "1234", 1234567 → "123万".</summary>
    public static string FormatSessionPower(long power)
    {
        if (power < 0) power = 0;
        if (power < 10_000) return power.ToString();
        var wan = power / 10_000;
        return $"{wan}万";
    }

    /// <summary>
    /// Compute the layout for the SkillFX burst-ready overlay given the
    /// window dimensions and watched-slot count. Returns viewport position
    /// and per-slot rectangles. Mirrors <c>compute_skillfx_layout</c>.
    /// </summary>
    /// <summary>
    /// Banker's rounding (Python's built-in <c>round</c> + the
    /// <c>_round_even</c> Cython helper). Used by Python's
    /// <c>hp_fmt_int</c> / <c>bosshp_fmt_hp</c> small-number paths.
    /// </summary>
    public static long RoundEven(double v)
        => (long)Math.Round(v, MidpointRounding.ToEven);

    /// <summary>
    /// Half-up rounding for non-negative values (negatives clamp to 0).
    /// Mirrors Cython <c>_round_half_up_nonneg</c>: 0.5 → 1, 1.5 → 2,
    /// any negative → 0. Used everywhere DPS UI numbers are rendered.
    /// </summary>
    public static long RoundHalfUpNonneg(double v)
    {
        if (v <= 0.0) return 0;
        return (long)(v + 0.5);
    }

    /// <summary>
    /// Format a non-negative double to a fixed number of fractional digits
    /// using half-up rounding. Mirrors <c>dps_to_fixed_half_up</c>.
    /// digits ≤ 0 returns the integer rounding only.
    /// </summary>
    public static string DpsToFixedHalfUp(double value, int digits)
    {
        if (digits <= 0) return RoundHalfUpNonneg(value).ToString();
        double scale = 1.0;
        for (int i = 0; i < digits; i++) scale *= 10.0;
        long rounded = RoundHalfUpNonneg(value * scale);
        long iscale = (long)scale;
        long whole = rounded / iscale;
        long frac = rounded - whole * iscale;
        return whole.ToString() + "." + frac.ToString().PadLeft(digits, '0');
    }

    /// <summary>
    /// Compact DPS-style number formatter — splits at 1K and 1M with
    /// 1-fractional-digit precision under the 100K / 10M thresholds.
    /// Mirrors <c>dps_fmt_num</c>.
    /// </summary>
    public static string DpsFmtNum(double value)
    {
        if (value >= 1_000_000.0)
            return DpsToFixedHalfUp(value / 1_000_000.0, value >= 10_000_000.0 ? 0 : 1) + "M";
        if (value >= 1_000.0)
            return DpsToFixedHalfUp(value / 1_000.0, value >= 100_000.0 ? 0 : 1) + "K";
        return RoundHalfUpNonneg(value).ToString("N0", System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// FightPower-style formatter — like <see cref="DpsFmtNum"/> but uses
    /// 2 decimals at the M boundary and returns empty for non-positive.
    /// Mirrors <c>dps_fmt_fp</c>.
    /// </summary>
    public static string DpsFmtFp(double value)
    {
        if (value <= 0.0) return string.Empty;
        if (value >= 1_000_000.0)
            return DpsToFixedHalfUp(value / 1_000_000.0, 2) + "M";
        if (value >= 1_000.0)
            return DpsToFixedHalfUp(value / 1_000.0, value >= 100_000.0 ? 0 : 1) + "K";
        return RoundHalfUpNonneg(value).ToString("N0", System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>
    /// "MM:SS" duration formatter; clamps negatives to zero. Mirrors
    /// <c>dps_fmt_time</c>.
    /// </summary>
    public static string DpsFmtTime(double seconds)
    {
        long s = (long)seconds;
        if (s < 0) s = 0;
        return $"{s / 60:D2}:{s % 60:D2}";
    }

    /// <summary>Boss-bar HP formatter — B/M/K/comma at the canonical thresholds.</summary>
    public static string BossHpFmtHp(double value)
    {
        if (value >= 1_000_000_000.0)
            return (value / 1_000_000_000.0).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + "B";
        if (value >= 1_000_000.0)
            return (value / 1_000_000.0).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + "M";
        if (value >= 10_000.0)
            return (value / 1_000.0).ToString("F1", System.Globalization.CultureInfo.InvariantCulture) + "K";
        return RoundEven(value).ToString("N0", System.Globalization.CultureInfo.InvariantCulture);
    }

    /// <summary>Standard ease-out cubic, t clamped to [0,1].</summary>
    public static double EaseOutCubic(double t)
    {
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        double inv = 1.0 - t;
        return 1.0 - inv * inv * inv;
    }

    /// <summary>Linear interpolation with t clamped to [0,1].</summary>
    public static double LerpClamped(double a, double b, double t)
    {
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        return a + (b - a) * t;
    }

    /// <summary>Cubic open-reveal eased progress for the session-list opening anim.</summary>
    public static double CubicOpenReveal(double elapsed, double duration)
    {
        if (duration <= 0.0) return 1.0;
        double t = elapsed / duration;
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        double inv = 1.0 - t;
        return 1.0 - inv * inv * inv;
    }

    /// <summary>HUD scan phase: <c>(sin(now*1.5)+1)/2</c>.</summary>
    public static double ScanPhase(double now) => (Math.Sin(now * 1.5) + 1.0) / 2.0;

    /// <summary>
    /// Idle panel-float (dx, dy) integer offsets, sin-driven with the same
    /// 0.82 / 0.61 phase split as the Python tick.
    /// </summary>
    public static (int Dx, int Dy) PanelFloatOffsets(double t, double phase, double amp)
    {
        int dx = (int)(amp * Math.Sin(t * 0.82 + phase));
        int dy = (int)(amp * Math.Sin(t * 0.61 + phase + 1.2));
        return (dx, dy);
    }

    /// <summary>Trim a session-roster name to the compact column width (14 chars + ellipsis).</summary>
    public static string ShortSessionName(string? name)
    {
        var text = (name ?? string.Empty).Trim();
        if (text.Length == 0) return "--";
        if (text.Length <= 14) return text;
        return text.Substring(0, 13) + "\u2026";
    }

    /// <summary>Half-up round to integer (non-negative; negatives clamp to 0). Mirrors <c>dps_round_half_up_int</c>.</summary>
    public static long DpsRoundHalfUpInt(double value) => RoundHalfUpNonneg(value);

    /// <summary>Format an HP/STA integer with thousands commas, banker's-rounded. Mirrors <c>hp_fmt_int</c>.</summary>
    public static string HpFmtInt(double value)
        => RoundEven(value).ToString("N0", System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>Result tuple for <see cref="HpLayoutMetrics"/>: (stage_w, panel_w, id_x, id_w, cover_x, box_x, sta_x).</summary>
    public sealed record HpLayout(int StageW, int PanelW, int IdX, int IdW, int CoverX, int BoxX, int StaX);

    /// <summary>HP overlay CSS-derived layout metrics. Mirrors <c>hp_layout_metrics</c>.</summary>
    public static HpLayout HpLayoutMetrics(
        int screenW, double hudVwPct, double stageWidthPct, double shadowGutter,
        int coverW, int boxW, int staW)
    {
        if (screenW < 1) screenW = 1;
        int viewportW = (int)RoundEven(screenW * hudVwPct);
        int stageW = (int)RoundEven(viewportW * stageWidthPct);
        int idX = (int)RoundEven(stageW * 0.032 - 25.0);
        int idW = (int)RoundEven(stageW * 0.396 + 55.0);
        int coverX = (int)RoundEven(stageW * 0.452 + 25.0);
        int boxX = (int)RoundEven(stageW * 0.47 + 25.0);
        int staX = boxX;
        int rightEdge = idX + idW;
        int tmp = coverX + coverW;
        if (tmp > rightEdge) rightEdge = tmp;
        tmp = boxX + boxW;
        if (tmp > rightEdge) rightEdge = tmp;
        tmp = staX + staW;
        if (tmp > rightEdge) rightEdge = tmp;
        int panelW = (int)(rightEdge + shadowGutter);
        if (panelW < rightEdge + shadowGutter) panelW += 1;
        return new HpLayout(stageW, panelW, idX, idW, coverX, boxX, staX);
    }

    /// <summary>Screen X for the HP stage anchor. Mirrors <c>hp_stage_screen_x</c>.</summary>
    public static int HpStageScreenX(int screenW, double windowLeftPct, double hudVwPct, double stageLeftPct)
        => (int)RoundEven(screenW * windowLeftPct + screenW * hudVwPct * stageLeftPct);

    /// <summary>Linear interpolation between two RGBA colors, t clamped to [0,1]. Mirrors <c>mix_rgba</c>.</summary>
    public static (int R, int G, int B, int A) MixRgba(
        (int R, int G, int B, int A) a, (int R, int G, int B, int A) b, double t)
    {
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        return (
            (int)(a.R + (b.R - a.R) * t),
            (int)(a.G + (b.G - a.G) * t),
            (int)(a.B + (b.B - a.B) * t),
            (int)(a.A + (b.A - a.A) * t));
    }

    /// <summary>Translate a polygon's points by (dx, dy). Mirrors <c>offset_poly</c>.</summary>
    public static IReadOnlyList<(int X, int Y)> OffsetPoly(IEnumerable<(int X, int Y)> points, int dx, int dy)
    {
        var result = new List<(int, int)>();
        foreach (var (x, y) in points) result.Add((x + dx, y + dy));
        return result;
    }

    /// <summary>
    /// Sort monsters by (hp_pct DESC, last_damage_ts DESC). Mirrors
    /// <c>sort_recent_monsters</c>. Caller supplies value-extractor lambdas
    /// so the helper stays decoupled from any concrete monster type.
    /// </summary>
    public static IReadOnlyList<T> SortRecentMonsters<T>(
        IReadOnlyList<T>? monsters,
        Func<T, long> hpSelector,
        Func<T, long> maxHpSelector,
        Func<T, double> lastDamageTsSelector)
    {
        if (monsters is null || monsters.Count == 0) return Array.Empty<T>();
        var decorated = new List<(double NegHpPct, double NegTs, int OrigIdx, T Item)>(monsters.Count);
        for (int i = 0; i < monsters.Count; i++)
        {
            var m = monsters[i];
            long hp = hpSelector(m);
            long maxHp = maxHpSelector(m);
            if (maxHp <= 0) maxHp = hp > 0 ? hp : 1;
            double pct = maxHp > 0 ? (double)hp / maxHp : 0.0;
            double ts = lastDamageTsSelector(m);
            decorated.Add((-pct, -ts, i, m));
        }
        decorated.Sort((x, y) =>
        {
            int c = x.NegHpPct.CompareTo(y.NegHpPct);
            if (c != 0) return c;
            c = x.NegTs.CompareTo(y.NegTs);
            if (c != 0) return c;
            return x.OrigIdx.CompareTo(y.OrigIdx);
        });
        var out_ = new T[decorated.Count];
        for (int i = 0; i < decorated.Count; i++) out_[i] = decorated[i].Item;
        return out_;
    }

    /// <summary>Half-up round toward zero magnitude — Python
    /// <c>_round_pos</c>: <c>v &gt;= 0 ? (int)(v + 0.5) : (int)(v - 0.5)</c>.</summary>
    private static int RoundPos(double v) =>
        v >= 0.0 ? (int)(v + 0.5) : (int)(v - 0.5);

    /// <summary>Full-parity port of <c>compute_skillfx_layout</c>:
    /// derives window / viewport / callout / payload-slot geometry from
    /// the game client rect plus a list of screen-space slot rects.
    /// Returns <c>null</c> when the client rect is empty or no usable
    /// slot rects survive the filter (positive index + positive size).</summary>
    public static SkillFxLayoutResult? ComputeSkillFxLayout(
        SkillFxClientRect? clientRect,
        IEnumerable<SkillFxInputSlot>? slotRects,
        IEnumerable<SkillFxInputSlot>? fallbackSlotRects = null)
    {
        if (clientRect is null) return null;
        var cr = clientRect.Value;
        int clientLeft = cr.Left;
        int clientTop = cr.Top;
        int clientRight = cr.Right;
        int clientBottom = cr.Bottom;
        int clientW = Math.Max(1, clientRight - clientLeft);
        int clientH = Math.Max(1, clientBottom - clientTop);

        var slots = new List<SkillFxInputSlot>();
        if (slotRects is not null)
        {
            foreach (var item in slotRects)
            {
                if (item.Index <= 0 || item.W <= 0 || item.H <= 0) continue;
                slots.Add(item);
            }
        }
        if (slots.Count == 0 && fallbackSlotRects is not null)
        {
            foreach (var item in fallbackSlotRects)
            {
                if (item.Index <= 0 || item.W <= 0 || item.H <= 0) continue;
                slots.Add(item);
            }
        }
        if (slots.Count == 0) return null;

        int minX = slots[0].X;
        int maxY = slots[0].Y + slots[0].H;
        foreach (var s in slots)
        {
            if (s.X < minX) minX = s.X;
            if (s.Y + s.H > maxY) maxY = s.Y + s.H;
        }

        int padX = Math.Max(18, RoundPos(clientW * 0.012));
        int padY = Math.Max(18, RoundPos(clientH * 0.016));
        int padLeft = Math.Max(96, RoundPos(clientW * 0.055));
        int padRight = Math.Max(84, RoundPos(clientW * 0.044));
        int winX = minX - padLeft;
        if (winX < 0) winX = 0;
        int winY = clientTop;
        if (winY < 0) winY = 0;
        int width = (clientRight - winX) + padRight;
        if (width < 420) width = 420;
        int height = (maxY - winY) + padY;
        if (height < 220) height = 220;
        int calloutW = Math.Max(440, RoundPos(clientW * 0.29));
        int calloutH = Math.Max(128, RoundPos(clientH * 0.115));
        int calloutMarginX = Math.Max(28, RoundPos(clientW * 0.022));
        int calloutMarginY = Math.Max(24, RoundPos(clientH * 0.040));
        int calloutX = width - calloutW - calloutMarginX;
        if (calloutX < calloutMarginX) calloutX = calloutMarginX;
        int calloutY = calloutMarginY;

        var payload = new List<SkillFxSlot>(slots.Count);
        foreach (var s in slots)
        {
            payload.Add(new SkillFxSlot(
                Index: s.Index,
                X: s.X - winX,
                Y: s.Y - winY,
                W: s.W,
                H: s.H));
        }
        payload.Sort((a, b) => a.Index.CompareTo(b.Index));

        int paddingXFinal = padX;
        if (padLeft > paddingXFinal) paddingXFinal = padLeft;
        if (padRight > paddingXFinal) paddingXFinal = padRight;

        return new SkillFxLayoutResult(
            Window: new SkillFxWindow(winX, winY, width, height),
            Viewport: new SkillFxViewport(
                Width: width,
                Height: height,
                PaddingX: paddingXFinal,
                PaddingY: padY,
                Callout: new SkillFxCallout(calloutX, calloutY, calloutW, calloutH)),
            Slots: payload);
    }

    /// <summary>Mirrors <c>boss_monster_usable</c> — decide if a monster
    /// can drive the boss bar and whether the caller should flip
    /// <c>is_dead</c> back to false (server reused the UUID for a respawn).</summary>
    public static (bool Usable, bool Revive) BossMonsterUsable(long hp, long maxHp, bool isDead)
    {
        bool revive = false;
        bool dead = isDead;
        if (dead && hp > 0)
        {
            revive = true;
            dead = false;
        }
        if (dead) return (false, revive);
        return ((maxHp > 0 || hp > 0), revive);
    }

    /// <summary>Mirrors <c>player_panel_anim_size</c> — scale a panel rect
    /// by animation progress, clamped to at least 1px each side.</summary>
    public static (int W, int H) PlayerPanelAnimSize(int width, int height, double t)
    {
        int outW = (int)(width * t);
        int outH = (int)(height * t);
        if (outW < 1) outW = 1;
        if (outH < 1) outH = 1;
        return (outW, outH);
    }

    /// <summary>Mirrors <c>popup_max_child_rows</c> — find the largest
    /// child-menu row count across a (key → list) dictionary.</summary>
    public static int PopupMaxChildRows<TKey, TItem>(IReadOnlyDictionary<TKey, IReadOnlyList<TItem>>? childMenus)
        where TKey : notnull
    {
        if (childMenus is null) return 0;
        int max = 0;
        foreach (var items in childMenus.Values)
        {
            if (items is null) continue;
            if (items.Count > max) max = items.Count;
        }
        return max;
    }

    /// <summary>Mirrors <c>menu_bar_slot_index</c> — map GPU menubar
    /// cursor coords to a button slot index, or null when out of range.</summary>
    public static int? MenuBarSlotIndex(double x, double y, int maxSize, int slot, int buttonCount)
    {
        if (buttonCount <= 0 || slot <= 0) return null;
        if (x < 0.0 || y < 0.0 || x >= maxSize) return null;
        int idx = (int)(y / slot);
        if (idx < 0 || idx >= buttonCount) return null;
        return idx;
    }

    /// <summary>One quantized button row in <see cref="MenuBarSnapshotSig"/>.</summary>
    public sealed record MenuBarButton(double SizeQ, double HoverQ, bool Active, string Icon);

    /// <summary>Snapshot input shape for <see cref="MenuBarSnapshotSig"/>.</summary>
    public sealed record MenuBarSnapshot(double Size, double HoverT, bool Active, string Icon);

    /// <summary>Dedup signature for the GPU menu-bar painter (Python
    /// <c>menu_bar_snapshot_sig</c>). Quantizes <c>size</c> to ¼ steps and
    /// <c>hover_t</c> to 1/20 steps so micro-jitter doesn't repaint.</summary>
    public sealed record MenuBarSig(int StripW, int StripH, int Count, IReadOnlyList<MenuBarButton> Buttons);

    public static MenuBarSig MenuBarSnapshotSig(int stripW, int stripH, IEnumerable<MenuBarSnapshot>? snapshots)
    {
        if (snapshots is null) return new MenuBarSig(stripW, stripH, 0, Array.Empty<MenuBarButton>());
        var buttons = new List<MenuBarButton>();
        foreach (var s in snapshots)
        {
            // Python: <int>(size * 4.0 + 0.5) / 4.0  — truncation after +0.5 == half-up rounding
            double sizeQ = (int)(s.Size * 4.0 + 0.5) / 4.0;
            double hoverQ = (int)(s.HoverT * 20.0 + 0.5) / 20.0;
            buttons.Add(new MenuBarButton(sizeQ, hoverQ, s.Active, s.Icon ?? string.Empty));
        }
        return new MenuBarSig(stripW, stripH, buttons.Count, buttons);
    }

    /// <summary>Roster batch row used by <see cref="BuildBatchSig"/>.</summary>
    public sealed record RosterBatchRow(string Uid, string Name, string Profession, long FightPower, int Level)
        : IComparable<RosterBatchRow>
    {
        public int CompareTo(RosterBatchRow? other)
        {
            if (other is null) return 1;
            // Python tuple sort: lex over uid, name, prof, fp, lv.
            int c = string.CompareOrdinal(Uid, other.Uid);
            if (c != 0) return c;
            c = string.CompareOrdinal(Name, other.Name);
            if (c != 0) return c;
            c = string.CompareOrdinal(Profession, other.Profession);
            if (c != 0) return c;
            c = FightPower.CompareTo(other.FightPower);
            if (c != 0) return c;
            return Level.CompareTo(other.Level);
        }
    }

    /// <summary>Mirrors <c>build_batch_sig</c> — sorted-tuple signature
    /// of a player roster batch for overlay-push deduplication.</summary>
    public static IReadOnlyList<RosterBatchRow> BuildBatchSig(IEnumerable<RosterBatchRow>? batch)
    {
        if (batch is null) return Array.Empty<RosterBatchRow>();
        var copy = batch.ToList();
        if (copy.Count == 0) return Array.Empty<RosterBatchRow>();
        copy.Sort();
        return copy;
    }

    /// <summary>Per-additional-target row inside <see cref="BossBarSig"/>.</summary>
    public sealed record BossBarAdditional(
        string Name, double HpPct, double ExtinctionPct, bool HasBreakData,
        int BreakingStage, bool ShieldActive, double ShieldPct);

    /// <summary>Boss-bar overlay dedup signature (Python
    /// <c>build_boss_bar_sig</c>). All round-to-3 values use Python-style
    /// half-to-even via <see cref="Math.Round(double, int)"/>.</summary>
    public sealed record BossBarSig(
        bool Active, double HpPct, string HpSource, long CurrentHp, long TotalHp,
        bool ShieldActive, double ShieldPct, long BreakingStage, bool HasBreakData,
        double ExtinctionPct, long Extinction, long MaxExtinction,
        bool StopBreakingTicking, bool InOverdrive, bool Invincible, string BossName,
        IReadOnlyList<BossBarAdditional> Additional);

    /// <summary>Input row shape for <see cref="BuildBossBarSig"/> additionals.</summary>
    public sealed record BossBarAdditionalInput(
        string? Name, double HpPct, double ExtinctionPct, bool HasBreakData,
        int BreakingStage, bool ShieldActive, double ShieldPct);

    /// <summary>Input data shape for <see cref="BuildBossBarSig"/>.</summary>
    public sealed record BossBarSigInput(
        bool Active, double HpPct, string? HpSource, long CurrentHp, long TotalHp,
        bool ShieldActive, double ShieldPct, long BreakingStage, bool HasBreakData,
        double ExtinctionPct, long Extinction, long MaxExtinction,
        bool StopBreakingTicking, bool InOverdrive, bool Invincible, string? BossName);

    public static BossBarSig BuildBossBarSig(BossBarSigInput data, IEnumerable<BossBarAdditionalInput>? additional)
    {
        var addItems = new List<BossBarAdditional>();
        if (additional is not null)
        {
            foreach (var u in additional)
            {
                addItems.Add(new BossBarAdditional(
                    u.Name ?? string.Empty,
                    Math.Round(u.HpPct, 3),
                    Math.Round(u.ExtinctionPct, 3),
                    u.HasBreakData,
                    u.BreakingStage,
                    u.ShieldActive,
                    Math.Round(u.ShieldPct, 3)));
            }
        }
        return new BossBarSig(
            data.Active, data.HpPct, data.HpSource ?? string.Empty,
            data.CurrentHp, data.TotalHp,
            data.ShieldActive, data.ShieldPct, data.BreakingStage, data.HasBreakData,
            data.ExtinctionPct, data.Extinction, data.MaxExtinction,
            data.StopBreakingTicking, data.InOverdrive, data.Invincible,
            data.BossName ?? string.Empty,
            addItems);
    }
}

public readonly record struct SkillFxClientRect(int Left, int Top, int Right, int Bottom);

public readonly record struct SkillFxInputSlot(int Index, int X, int Y, int W, int H);

public sealed record SkillFxWindow(int X, int Y, int W, int H);

public sealed record SkillFxCallout(int X, int Y, int W, int H);

public sealed record SkillFxViewport(int Width, int Height, int PaddingX, int PaddingY, SkillFxCallout Callout);

public sealed record SkillFxLayoutResult(SkillFxWindow Window, SkillFxViewport Viewport, IReadOnlyList<SkillFxSlot> Slots);

public sealed record SkillFxSlot(int Index, int X, int Y, int W, int H);
