namespace SaoAuto.Core.Configuration;

public static class AppVersion
{
    public const string Version = "3.0.1";
    public const string Label = "v" + Version;

    public const string DefaultUpdateHost = "http://47.82.157.220:9330";
    public const string UpdateChannel = "stable";
    public const string UpdateTarget = "windows-x64";

    public static int Compare(string left, string right)
    {
        if (string.IsNullOrWhiteSpace(left) && string.IsNullOrWhiteSpace(right)) return 0;
        if (string.IsNullOrWhiteSpace(left)) return -1;
        if (string.IsNullOrWhiteSpace(right)) return 1;

        var (leftCore, leftSuffix) = SplitSuffix(left.Trim());
        var (rightCore, rightSuffix) = SplitSuffix(right.Trim());

        var coreCompare = CompareCore(leftCore, rightCore);
        if (coreCompare != 0) return coreCompare;

        return string.Compare(leftSuffix, rightSuffix, StringComparison.OrdinalIgnoreCase);
    }

    private static (string core, string suffix) SplitSuffix(string value)
    {
        if (value.StartsWith("v", StringComparison.OrdinalIgnoreCase) || value.StartsWith("V", StringComparison.OrdinalIgnoreCase))
        {
            value = value[1..];
        }
        var dash = value.IndexOf('-');
        return dash < 0 ? (value, string.Empty) : (value[..dash], value[(dash + 1)..]);
    }

    private static int CompareCore(string left, string right)
    {
        var leftParts = left.Split('.');
        var rightParts = right.Split('.');
        var max = Math.Max(leftParts.Length, rightParts.Length);
        for (var i = 0; i < max; i++)
        {
            var li = i < leftParts.Length && int.TryParse(leftParts[i], out var lv) ? lv : 0;
            var ri = i < rightParts.Length && int.TryParse(rightParts[i], out var rv) ? rv : 0;
            if (li != ri) return li.CompareTo(ri);
        }
        return 0;
    }
}
