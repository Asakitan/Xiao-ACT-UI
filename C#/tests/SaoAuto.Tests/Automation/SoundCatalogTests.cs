using SaoAuto.Core.Automation;

namespace SaoAuto.Tests.Automation;

public class SoundCatalogTests
{
    [Fact]
    public void ResolveReturnsAbsolutePathFromShortName()
    {
        var cat = new SoundCatalog(@"C:\sfx");
        var p = cat.Resolve("click");
        Assert.NotNull(p);
        Assert.EndsWith("click.wav", p);
        Assert.True(Path.IsPathRooted(p));
    }

    [Fact]
    public void ResolveReturnsNullForUnknownName()
    {
        var cat = new SoundCatalog(@"C:\sfx");
        Assert.Null(cat.Resolve("does_not_exist"));
    }

    [Fact]
    public void OverridesReplaceDefaults()
    {
        var cat = new SoundCatalog(@"C:\sfx", new Dictionary<string, string>
        {
            ["click"] = "custom_click.wav",
            ["new_sound"] = "new.wav",
        });
        Assert.EndsWith("custom_click.wav", cat.Resolve("click"));
        Assert.EndsWith("new.wav", cat.Resolve("new_sound"));
        Assert.Contains("new_sound", cat.Names);
    }

    [Fact]
    public void SetRegistersNewMapping()
    {
        var cat = new SoundCatalog(@"C:\sfx");
        cat.Set("ding", "ding.wav");
        Assert.EndsWith("ding.wav", cat.Resolve("ding"));
    }

    [Fact]
    public void LevelUpEffectPlaysPrimaryAndChainsBurstReady()
    {
        var player = new NullSoundPlayer();
        var cat = new SoundCatalog(@"C:\sfx");
        var fx = new LevelUpEffect(player, cat);

        var played = fx.Play(oldLevel: 5, newLevel: 6);

        Assert.Equal(new[] { "levelup", "burst_ready" }, played);
        Assert.Equal(2, player.Played.Count);
        Assert.EndsWith("levelup.wav", player.Played[0]);
        Assert.EndsWith("burst_ready.wav", player.Played[1]);
    }

    [Fact]
    public void LevelUpEffectReturnsEmptyWhenLevelDidNotIncrease()
    {
        var player = new NullSoundPlayer();
        var fx = new LevelUpEffect(player, new SoundCatalog(@"C:\sfx"));
        Assert.Empty(fx.Play(7, 7));
        Assert.Empty(fx.Play(7, 6));
        Assert.Empty(player.Played);
    }

    [Fact]
    public void LevelUpEffectReturnsEmptyWhenPlayerDisabled()
    {
        var player = new NullSoundPlayer { Enabled = false };
        var fx = new LevelUpEffect(player, new SoundCatalog(@"C:\sfx"));
        Assert.Empty(fx.Play(1, 2));
    }
}
