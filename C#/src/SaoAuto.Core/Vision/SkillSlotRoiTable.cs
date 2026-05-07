using System.Collections.Concurrent;

namespace SaoAuto.Core.Vision;

/// <summary>
/// S82 — Skill-slot ROI table. Bit-faithful port of
/// <c>config.get_skill_slot_rects</c> +
/// <c>get_skill_slot_client_rects</c> + <c>VISUAL_RECT_SPECS</c>
/// (config.py 1115–1336).
///
/// The skill bar at the bottom of the HUD has 9 slots whose
/// (right, bottom, width, height) anchors are calibrated against a
/// 1920 × 1080 client area. For arbitrary client sizes we scale each
/// anchor proportionally — same convention as the Python helpers.
///
/// `GetSkillSlotRects(clientLeft, clientTop, clientRight, clientBottom)`
/// returns *screen-space* bboxes (x1,y1,x2,y2) — used by the live
/// capture path for direct ROI cropping.
///
/// `GetSkillSlotClientRects(clientW, clientH)` returns *client-relative*
/// (x,y,w,h) — used by debug overlays / WPF panels that draw inside
/// the game window's client area.
///
/// Both helpers cache by client geometry (changes only on move/resize).
/// Thread-safe via <see cref="ConcurrentDictionary{TKey,TValue}"/>.
/// </summary>
public static class SkillSlotRoiTable
{
    public const double BaseClientWidth = 1920.0;
    public const double BaseClientHeight = 1080.0;

    /// <summary>
    /// Anchor spec keyed by visual rect name. Right/bottom are the
    /// lower-right corner of the rect at the base 1920×1080 size;
    /// width/height are the box extents.
    /// </summary>
    private static readonly Dictionary<string, VisualRectSpec> Specs = new()
    {
        ["stamina_bar_visual"] = new(1214, 1050, 250, 10),
        ["skill_slot_1"] = new(720, 1003, 52, 85),
        ["skill_slot_2"] = new(767, 1002, 47, 83),
        ["skill_slot_3"] = new(816, 1003, 49, 85),
        ["skill_slot_4"] = new(864, 1003, 49, 90),
        ["skill_slot_5"] = new(911, 1002, 45, 87),
        ["skill_slot_6"] = new(960, 1003, 49, 89),
        ["skill_slot_7"] = new(1032, 1009, 72, 119),
        ["skill_slot_8"] = new(1104, 1012, 73, 124),
        ["skill_slot_9"] = new(1177, 1007, 74, 119),
    };

    private static readonly ConcurrentDictionary<(int L, int T, int R, int B), IReadOnlyList<SkillSlotScreenRect>>
        _bboxCache = new();
    private static readonly ConcurrentDictionary<(int W, int H), IReadOnlyList<SkillSlotClientRect>>
        _clientCache = new();

    public static VisualRectSpec? GetSpec(string name) =>
        Specs.TryGetValue(name, out var s) ? s : null;

    /// <summary>
    /// Slot index → on-screen visual slot. Identity in the current
    /// build (Python's <c>SKILL_SLOT_VISUAL_INDEX</c> is 1→1..9→9).
    /// Kept as a method so a future remap is a single-line change.
    /// </summary>
    public static int GetSkillSlotVisualIndex(int slotIndex) =>
        slotIndex >= 1 && slotIndex <= 9 ? slotIndex : slotIndex;

    /// <summary>
    /// Map a visual-rect spec to its screen-space bbox inside the
    /// supplied client rectangle. Returns null on null spec / zero
    /// client area. Mirrors <c>anchored_rect_spec_to_pixels</c>.
    /// </summary>
    public static (int X1, int Y1, int X2, int Y2)? GetVisualRectBbox(
        string name, int clientLeft, int clientTop, int clientRight, int clientBottom)
    {
        if (!Specs.TryGetValue(name, out var spec)) return null;
        var clientW = Math.Max(1, clientRight - clientLeft);
        var clientH = Math.Max(1, clientBottom - clientTop);
        var x2 = clientLeft + (int)Math.Round(clientW * (spec.Right / BaseClientWidth));
        var y2 = clientTop + (int)Math.Round(clientH * (spec.Bottom / BaseClientHeight));
        var width = Math.Max(1, (int)Math.Round(clientW * (spec.Width / BaseClientWidth)));
        var height = Math.Max(1, (int)Math.Round(clientH * (spec.Height / BaseClientHeight)));
        return (x2 - width, y2 - height, x2, y2);
    }

    /// <summary>
    /// Map a visual-rect spec to a client-relative (x,y,w,h). Mirrors
    /// <c>anchored_rect_spec_to_client_rect</c>.
    /// </summary>
    public static (int X, int Y, int W, int H)? GetVisualRectClientRect(
        string name, int clientW, int clientH)
    {
        if (!Specs.TryGetValue(name, out var spec)) return null;
        if (clientW <= 0 || clientH <= 0) return null;
        var x2 = (int)Math.Round(clientW * (spec.Right / BaseClientWidth));
        var y2 = (int)Math.Round(clientH * (spec.Bottom / BaseClientHeight));
        var width = Math.Max(1, (int)Math.Round(clientW * (spec.Width / BaseClientWidth)));
        var height = Math.Max(1, (int)Math.Round(clientH * (spec.Height / BaseClientHeight)));
        return (x2 - width, y2 - height, width, height);
    }

    /// <summary>
    /// Resolve all 9 skill-slot ROIs in screen-space against the
    /// supplied client rect. Cached per client-rect tuple.
    /// </summary>
    public static IReadOnlyList<SkillSlotScreenRect> GetSkillSlotRects(
        int clientLeft, int clientTop, int clientRight, int clientBottom) =>
        _bboxCache.GetOrAdd(
            (clientLeft, clientTop, clientRight, clientBottom),
            key => BuildScreenRects(key.L, key.T, key.R, key.B));

    /// <summary>
    /// Resolve all 9 skill-slot ROIs in client-relative coords. Cached
    /// per (width, height) tuple.
    /// </summary>
    public static IReadOnlyList<SkillSlotClientRect> GetSkillSlotClientRects(
        int clientW, int clientH) =>
        _clientCache.GetOrAdd(
            (clientW, clientH),
            key => BuildClientRects(key.W, key.H));

    /// <summary>Union of all 9 slot anchors as a normalized ROI.</summary>
    public static (double X, double Y, double W, double H) SkillBarRoi { get; } = ComputeSkillBarRoi();

    public static void ClearCache()
    {
        _bboxCache.Clear();
        _clientCache.Clear();
    }

    private static IReadOnlyList<SkillSlotScreenRect> BuildScreenRects(int l, int t, int r, int b)
    {
        var list = new List<SkillSlotScreenRect>(9);
        for (var idx = 1; idx <= 9; idx++)
        {
            var visualIdx = GetSkillSlotVisualIndex(idx);
            var name = $"skill_slot_{visualIdx}";
            var bbox = GetVisualRectBbox(name, l, t, r, b);
            if (bbox is null) continue;
            list.Add(new SkillSlotScreenRect(idx, visualIdx, bbox.Value.X1, bbox.Value.Y1,
                bbox.Value.X2, bbox.Value.Y2, Specs[name]));
        }
        return list;
    }

    private static IReadOnlyList<SkillSlotClientRect> BuildClientRects(int w, int h)
    {
        var list = new List<SkillSlotClientRect>(9);
        for (var idx = 1; idx <= 9; idx++)
        {
            var visualIdx = GetSkillSlotVisualIndex(idx);
            var name = $"skill_slot_{visualIdx}";
            var rect = GetVisualRectClientRect(name, w, h);
            if (rect is null) continue;
            list.Add(new SkillSlotClientRect(idx, visualIdx, rect.Value.X, rect.Value.Y,
                rect.Value.W, rect.Value.H, Specs[name]));
        }
        return list;
    }

    private static (double X, double Y, double W, double H) ComputeSkillBarRoi()
    {
        double left = double.PositiveInfinity, top = double.PositiveInfinity;
        double right = double.NegativeInfinity, bottom = double.NegativeInfinity;
        for (var idx = 1; idx <= 9; idx++)
        {
            if (!Specs.TryGetValue($"skill_slot_{idx}", out var s)) continue;
            var l = s.Right - s.Width;
            var t = s.Bottom - s.Height;
            if (l < left) left = l;
            if (t < top) top = t;
            if (s.Right > right) right = s.Right;
            if (s.Bottom > bottom) bottom = s.Bottom;
        }
        if (double.IsPositiveInfinity(left)) return (0.0, 0.0, 0.0, 0.0);
        return (left / BaseClientWidth, top / BaseClientHeight,
                (right - left) / BaseClientWidth, (bottom - top) / BaseClientHeight);
    }
}

public readonly record struct VisualRectSpec(int Right, int Bottom, int Width, int Height);

public readonly record struct SkillSlotScreenRect(
    int Index,
    int VisualIndex,
    int X1,
    int Y1,
    int X2,
    int Y2,
    VisualRectSpec Spec)
{
    public int Width => X2 - X1;
    public int Height => Y2 - Y1;
}

public readonly record struct SkillSlotClientRect(
    int Index,
    int VisualIndex,
    int X,
    int Y,
    int W,
    int H,
    VisualRectSpec Spec);
