using System.Text.Json.Nodes;
using SaoAuto.Proto;

namespace SaoAuto.ParityTests;

public class UnitTest1
{
    [Fact]
    public void ProtoPortKeepsCanonicalSourceName()
    {
        Assert.Equal("star_resonance.proto", ProtoPortStatus.CanonicalProtoFile);
    }

    [Fact]
    public void SampleParityFixtureLoadsAndMatchesItself()
    {
        var fixture = ParityFixture.Load("sample_parity.json");
        var clone = JsonNode.Parse(fixture.ToJsonString())!;
        Assert.Null(JsonParity.FirstDifference(fixture, clone));
    }

    [Fact]
    public void JsonParityDetectsValueDifferences()
    {
        var a = JsonNode.Parse("""{"hp": 100}""");
        var b = JsonNode.Parse("""{"hp": 200}""");
        var diff = JsonParity.FirstDifference(a, b);
        Assert.NotNull(diff);
        Assert.Contains("100", diff);
        Assert.Contains("200", diff);
    }

    [Fact]
    public void JsonParityRespectsNumericEpsilon()
    {
        var a = JsonNode.Parse("""{"pct": 0.892}""");
        var b = JsonNode.Parse("""{"pct": 0.892001}""");
        Assert.NotNull(JsonParity.FirstDifference(a, b));
        Assert.Null(JsonParity.FirstDifference(a, b, numericEpsilon: 1e-3));
    }

    [Fact]
    public void JsonParityDetectsMissingKey()
    {
        var a = JsonNode.Parse("""{"hp": 100, "lv": 60}""");
        var b = JsonNode.Parse("""{"hp": 100}""");
        var diff = JsonParity.FirstDifference(a, b);
        Assert.NotNull(diff);
        Assert.Contains("missing", diff);
    }

    [Fact]
    public void JsonParityDetectsExtraKey()
    {
        var a = JsonNode.Parse("""{"hp": 100}""");
        var b = JsonNode.Parse("""{"hp": 100, "lv": 60}""");
        var diff = JsonParity.FirstDifference(a, b);
        Assert.NotNull(diff);
        Assert.Contains("extra", diff);
    }
}
