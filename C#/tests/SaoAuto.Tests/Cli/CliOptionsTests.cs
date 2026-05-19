using SaoAuto.App.Cli;

namespace SaoAuto.Tests.Cli;

public class CliOptionsTests
{
    [Fact]
    public void NoArgsRunsUiMode()
    {
        var options = CliOptions.Parse(Array.Empty<string>());
        Assert.Equal(RunMode.Ui, options.RunMode);
        Assert.False(options.TestMode);
        Assert.False(options.HeadlessMode);
        Assert.Null(options.SettingsPath);
    }

    [Theory]
    [InlineData("--test", RunMode.Test)]
    [InlineData("--headless", RunMode.Headless)]
    [InlineData("--help", RunMode.Help)]
    [InlineData("-h", RunMode.Help)]
    [InlineData("/?", RunMode.Help)]
    public void RecognizesPrimaryFlags(string arg, RunMode expected)
    {
        var options = CliOptions.Parse(new[] { arg });
        Assert.Equal(expected, options.RunMode);
    }

    [Fact]
    public void HelpBeatsTestAndHeadless()
    {
        var options = CliOptions.Parse(new[] { "--test", "--headless", "--help" });
        Assert.Equal(RunMode.Help, options.RunMode);
    }

    [Fact]
    public void TestBeatsHeadless()
    {
        var options = CliOptions.Parse(new[] { "--headless", "--test" });
        Assert.Equal(RunMode.Test, options.RunMode);
    }

    [Fact]
    public void SettingsAcceptsBothSpaceAndEqualsForms()
    {
        var spaced = CliOptions.Parse(new[] { "--settings", @"C:\fixtures\sao.json" });
        Assert.Equal(@"C:\fixtures\sao.json", spaced.SettingsPath);

        var inline = CliOptions.Parse(new[] { @"--settings=C:\fixtures\sao.json" });
        Assert.Equal(@"C:\fixtures\sao.json", inline.SettingsPath);
    }

    [Fact]
    public void UnknownArgIsCapturedNotSilentlyDropped()
    {
        var options = CliOptions.Parse(new[] { "--frobnicate" });
        Assert.Equal("--frobnicate", options.UnknownArg);
    }

    [Fact]
    public void DanglingSettingsFlagWithoutValueIsUnknown()
    {
        var options = CliOptions.Parse(new[] { "--settings" });
        Assert.Null(options.SettingsPath);
        Assert.Equal("--settings", options.UnknownArg);
    }
}
