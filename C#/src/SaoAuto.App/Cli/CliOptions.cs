namespace SaoAuto.App.Cli;

/// <summary>
/// Mirrors <c>main.main()</c> argparse: <c>--test</c> single-shot recognition,
/// <c>--headless</c> no-HUD terminal mode, otherwise normal UI launch. Adds
/// <c>--settings &lt;path&gt;</c> as a Plan §4 step-1 test override.
/// </summary>
public sealed record CliOptions
{
    public bool TestMode { get; init; }
    public bool HeadlessMode { get; init; }
    public bool ShowHelp { get; init; }
    public string? SettingsPath { get; init; }
    public string? UnknownArg { get; init; }

    public RunMode RunMode => this switch
    {
        { ShowHelp: true } => RunMode.Help,
        { TestMode: true } => RunMode.Test,
        { HeadlessMode: true } => RunMode.Headless,
        _ => RunMode.Ui,
    };

    public static CliOptions Parse(IReadOnlyList<string> args)
    {
        var test = false;
        var headless = false;
        var help = false;
        string? settings = null;
        string? unknown = null;

        for (var i = 0; i < args.Count; i++)
        {
            var arg = args[i];
            switch (arg)
            {
                case "--test":
                    test = true;
                    break;
                case "--headless":
                    headless = true;
                    break;
                case "-h":
                case "--help":
                case "/?":
                    help = true;
                    break;
                case "--settings":
                    if (i + 1 < args.Count)
                    {
                        settings = args[++i];
                    }
                    else
                    {
                        unknown = arg;
                    }
                    break;
                default:
                    if (arg.StartsWith("--settings=", StringComparison.Ordinal))
                    {
                        settings = arg["--settings=".Length..];
                    }
                    else if (unknown is null)
                    {
                        unknown = arg;
                    }
                    break;
            }
        }

        return new CliOptions
        {
            TestMode = test,
            HeadlessMode = headless,
            ShowHelp = help,
            SettingsPath = settings,
            UnknownArg = unknown,
        };
    }

    public const string HelpText = """
        SaoAuto — game HUD and automation (C# port)

        Usage:
          SaoAuto.App                   Launch normal UI (entity / webview per settings.json)
          SaoAuto.App --test            Single-shot recognition test, prints result, exits
          SaoAuto.App --headless        No-HUD terminal mode; prints state until Ctrl+C
          SaoAuto.App --settings PATH   Override settings.json path (test/development only)
          SaoAuto.App --help            Show this message
        """;
}

public enum RunMode
{
    Ui,
    Test,
    Headless,
    Help,
}
