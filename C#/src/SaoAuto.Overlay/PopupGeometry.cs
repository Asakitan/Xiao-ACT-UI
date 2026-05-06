namespace SaoAuto.Overlay;

/// <summary>
/// Pure-math popup-window geometry helpers ported from the
/// <c>popup_*</c> batch in <c>_sao_cy_uihelpers.pyx</c> — drives the GPU
/// entity popup's visible-count, hit rects, fade alpha, and HUD frame
/// math. Each method is deterministic and side-effect free; the two
/// in-place "advance animation" Cython helpers
/// (<c>popup_advance_menu_animation</c>, <c>popup_advance_child_animation</c>)
/// are intentionally skipped — they mutate caller arrays and are
/// covered by the popup integration path, not these unit-level helpers.
/// </summary>
public static class PopupGeometry
{
    public static int VisibleCount(int menuCount, int maxVisible)
    {
        if (menuCount < 0) menuCount = 0;
        if (maxVisible < 0) maxVisible = 0;
        return menuCount < maxVisible ? menuCount : maxVisible;
    }

    public static int ColumnHeight(int menuCount, int maxVisible, int slot)
    {
        var visible = VisibleCount(menuCount, maxVisible);
        if (visible < 1) visible = 1;
        return slot * visible;
    }

    public static (int Dx, int Dy) ContentShift(double fadeAlpha)
    {
        var alpha = fadeAlpha;
        if (alpha < 0.0) alpha = 0.0;
        else if (alpha > 1.0) alpha = 1.0;
        int shiftX = (int)(-((1.0 - alpha) * 14.0 + 0.5));
        int shiftY = (int)((1.0 - alpha) * 10.0 + 0.5);
        return (shiftX, shiftY);
    }

    public static ((int X, int Y) MenuOrigin, (int X, int Y) ChildOrigin) Origins(
        double fadeAlpha, int hudPad, int menuX, int childX)
    {
        var (dx, dy) = ContentShift(fadeAlpha);
        return ((menuX + dx, hudPad + dy), (childX + dx, hudPad + dy));
    }

    public static (int Width, int Height) ContentSize(
        int menuCount, int childCount, int maxVisible, int slot,
        int rowStride, int menuWidth, int gap, int childWidth)
    {
        int menuH = ColumnHeight(menuCount, maxVisible, slot);
        int childH = childCount > 0 ? childCount * rowStride : 0;
        int innerH = menuH > childH ? menuH : childH;
        if (innerH < 1) innerH = 1;
        int innerW = menuWidth + gap + childWidth;
        return (innerW, innerH);
    }

    public static (int Width, int Height) WindowSize(
        int menuCount, int childCount, int reservedRows, int maxVisible,
        int slot, int rowStride, int menuWidth, int gap, int childWidth, int hudPad)
    {
        var (iw, ih) = ContentSize(menuCount, childCount, maxVisible, slot,
                                   rowStride, menuWidth, gap, childWidth);
        int reservedH = reservedRows > 0 ? reservedRows * rowStride : 0;
        int menuH = ColumnHeight(menuCount, maxVisible, slot);
        if (reservedH > ih) ih = reservedH;
        if (menuH > ih) ih = menuH;
        return (iw + hudPad * 2, ih + hudPad * 2);
    }

    public static (double SizeF, int SizePx, int Ox, int Oy) MenuButtonFrame(
        double sizeValue, double slot, double maxSize, int index)
    {
        var sizeF = sizeValue;
        if (sizeF < 1.0) sizeF = 1.0;
        if (sizeF > maxSize) sizeF = maxSize;
        int sizePx = (int)sizeF;
        if (sizeF > sizePx) sizePx += 1;
        if (sizePx < 1) sizePx = 1;
        int ox = (int)(((slot - sizeF) / 2.0) + 0.5);
        int oy = (int)((index * slot + (slot - sizeF) / 2.0) + 0.5);
        return (sizeF, sizePx, ox, oy);
    }

    /// <summary>One menu-button hit rectangle: (x1, y1, x2, y2) plus its index.</summary>
    public sealed record HitRect(int X1, int Y1, int X2, int Y2, int Index);

    public static IReadOnlyList<HitRect> MenuHitRects(int menuCount, int maxVisible, int slot, int xOff, int yOff)
    {
        int n = VisibleCount(menuCount, maxVisible);
        var result = new List<HitRect>(n);
        for (int i = 0; i < n; i++)
        {
            int y1 = yOff + i * slot;
            result.Add(new HitRect(xOff, y1, xOff + slot, y1 + slot, i));
        }
        return result;
    }

    public static int ChildHeight(int childCount, int rowStride)
        => childCount <= 0 ? 0 : childCount * rowStride;

    public static IReadOnlyList<HitRect> ChildHitRects(
        int childCount, IReadOnlyList<int> rowAnimW,
        int xOff, int yOff, int listX, int rowStride, int rowH, int targetRowW)
    {
        int x1 = xOff + listX;
        var result = new List<HitRect>(childCount);
        for (int i = 0; i < childCount; i++)
        {
            int rw = i < rowAnimW.Count ? rowAnimW[i] : targetRowW;
            if (rw < 1) rw = 1;
            int y1 = yOff + i * rowStride;
            result.Add(new HitRect(x1, y1, x1 + rw, y1 + rowH, i));
        }
        return result;
    }

    /// <summary>
    /// Pick a region from cached popup hit rectangles. Returns
    /// <c>(label, index)</c> when a region matches, <c>("background", -1)</c>
    /// when the cursor falls inside <paramref name="bounds"/> only,
    /// and <c>null</c> otherwise. Mirrors <c>popup_pick_hit</c>.
    /// </summary>
    public static (string Label, int Index)? PickHit(
        IReadOnlyList<(HitRect Rect, string Label, int Index)> regions,
        (int X1, int Y1, int X2, int Y2) bounds, int x, int y)
    {
        foreach (var (rect, label, idx) in regions)
        {
            if (rect.X1 <= x && x < rect.X2 && rect.Y1 <= y && y < rect.Y2)
                return (label, idx);
        }
        if (bounds.X1 <= x && x < bounds.X2 && bounds.Y1 <= y && y < bounds.Y2)
            return ("background", -1);
        return null;
    }

    public sealed record HudDynamic(
        int Cx1, int Cy1, int Cx2, int Cy2,
        int ScanY, int DotYL, int DotYR);

    public static HudDynamic HudDynamicFrame(
        int contentW, int contentH, double phase,
        int plateePad, int hudMargin, int bracketLen)
    {
        int cx1 = plateePad - hudMargin;
        int cy1 = plateePad - hudMargin;
        int cx2 = plateePad + contentW + hudMargin;
        int cy2 = plateePad + contentH + hudMargin;
        const double scanPeriod = 6.0;
        double scanPos = (phase % scanPeriod) / scanPeriod;
        if (scanPos < 0) scanPos += 1.0; // C# % preserves sign for negatives
        int scanY = (int)(cy1 + (cy2 - cy1) * scanPos);
        int dotTravel = cy2 - cy1 - bracketLen * 2;
        if (dotTravel < 1) dotTravel = 1;
        int dotYL = cy1 + bracketLen + (int)(dotTravel * ((Math.Sin(phase * 0.8) + 1.0) * 0.5));
        int dotYR = cy1 + bracketLen + (int)(dotTravel * ((Math.Sin(phase * 0.8 + Math.PI) + 1.0) * 0.5));
        return new HudDynamic(cx1, cy1, cx2, cy2, scanY, dotYL, dotYR);
    }

    public static (double Dt, double TickNow) TickDt(double tickNow, double lastTickT)
    {
        double dt = lastTickT > 0.0 ? tickNow - lastTickT : 1.0 / 60.0;
        if (dt < 0.0) dt = 0.0;
        else if (dt > 0.10) dt = 0.10;
        return (dt, tickNow);
    }

    public static (double Alpha, bool Done, double T) FadeAlpha(
        double tickNow, double fadeT0, double fadeDuration, double fadeTarget)
    {
        double dur = fadeDuration;
        if (dur <= 0.0) dur = fadeTarget > 0.0 ? 0.45 : 0.30;
        double t = (tickNow - fadeT0) / dur;
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        double alpha = fadeTarget > 0.0 ? t : 1.0 - t;
        return (alpha, t >= 1.0, t);
    }

    public static (string Phase, double FadeT, bool Completed) ChildPhaseStep(
        string phase, double fadeT, double dt)
    {
        var ph = phase ?? "idle";
        bool completed = false;
        if (ph == "fadeout")
        {
            fadeT += dt / 0.16;
            if (fadeT > 1.0) fadeT = 1.0;
            if (fadeT >= 0.999) completed = true;
        }
        else if (ph == "fadein")
        {
            fadeT -= dt / 0.22;
            if (fadeT < 0.0) fadeT = 0.0;
            if (fadeT <= 0.001) { fadeT = 0.0; ph = "idle"; completed = true; }
        }
        return (ph, fadeT, completed);
    }

    public static int MaxChildRows<T>(IEnumerable<IReadOnlyCollection<T>>? childMenus)
    {
        if (childMenus is null) return 0;
        int max = 0;
        foreach (var items in childMenus)
        {
            if (items is null) continue;
            if (items.Count > max) max = items.Count;
        }
        return max;
    }
}
