namespace SaoAuto.Core.Configuration;

public static class UiMode
{
    public const string Entity = "entity";
    public const string WebView = "webview";
    public const string LegacySao = "sao";

    public const string Default = WebView;

    public static string Normalize(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return Default;
        }

        var trimmed = value.Trim().ToLowerInvariant();
        return trimmed == LegacySao ? Entity : trimmed;
    }
}
