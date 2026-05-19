using System.Text.Json.Nodes;

namespace SaoAuto.ParityTests;

/// <summary>
/// Sanity checks for fixtures emitted by <c>tools/parity-fixtures/*.py</c>.
/// Per-subsystem replay parity (running the C# port against the same input
/// and diffing against the expected JSON) lands in dedicated tests as the
/// matching live subsystems get wired up. These tests just guarantee that
/// the canonical envelope and committed sample data are well-formed.
/// </summary>
public class FixtureEnvelopeTests
{
    [Theory]
    [InlineData("overlay_layout.json")]
    public void FixtureHasCanonicalEnvelope(string relativePath)
    {
        var fx = ParityFixture.Load(relativePath);
        Assert.True(fx.ContainsKey("kind"), "fixture missing 'kind'");
        Assert.True(fx.ContainsKey("source"), "fixture missing 'source'");
        Assert.True(fx.ContainsKey("data"), "fixture missing 'data'");
        Assert.IsType<JsonObject>(fx["data"]);
    }

    [Fact]
    public void OverlayLayoutFixtureMatchesPlannerMath()
    {
        var fx = ParityFixture.Load("overlay_layout.json");
        Assert.Equal("overlay_layout", (string?)fx["kind"]);

        var data = fx["data"]!.AsObject();
        var lanes = data["lanes"]!.AsArray();
        Assert.Equal(3, lanes.Count);

        var screen = data["screen"]!.AsObject();
        var margins = data["margins"]!.AsObject();
        var screenW = (int)screen["w"]!;
        var rightMargin = (int)margins["right"]!;
        var topMargin = (int)margins["top"]!;
        var gap = (int)margins["gap"]!;

        var cy = topMargin;
        foreach (var laneNode in lanes)
        {
            var lane = laneNode!.AsObject();
            var w = (int)lane["w"]!;
            var h = (int)lane["h"]!;
            var x = (int)lane["x"]!;
            var y = (int)lane["y"]!;
            Assert.Equal(screenW - rightMargin - w, x);
            Assert.Equal(cy, y);
            cy += h + gap;
        }
    }
}
