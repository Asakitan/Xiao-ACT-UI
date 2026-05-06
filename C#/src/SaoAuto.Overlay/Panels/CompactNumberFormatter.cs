namespace SaoAuto.Overlay.Panels;

/// <summary>
/// Compact number formatter ported from the DPS panel's
/// `_format_compact_number`. Produces strings like `1.2K`, `5M`, `1.5G`.
/// </summary>
public static class CompactNumberFormatter
{
    public static string Format(long value)
    {
        var negative = value < 0;
        var abs = negative ? -value : value;
        var formatted = abs switch
        {
            < 1_000L => abs.ToString(),
            < 10_000L => $"{abs / 1000.0:0.##}K",
            < 1_000_000L => $"{abs / 1000L}K",
            < 10_000_000L => $"{abs / 1_000_000.0:0.##}M",
            < 1_000_000_000L => $"{abs / 1_000_000L}M",
            < 10_000_000_000L => $"{abs / 1_000_000_000.0:0.##}G",
            _ => $"{abs / 1_000_000_000L}G",
        };
        return negative ? "-" + formatted : formatted;
    }

    public static string FormatPercent(double pct, int decimals = 0)
    {
        var clamped = Math.Clamp(pct, 0.0, 1.0) * 100.0;
        return decimals == 0
            ? $"{Math.Round(clamped):F0}%"
            : clamped.ToString("F" + decimals) + "%";
    }
}
