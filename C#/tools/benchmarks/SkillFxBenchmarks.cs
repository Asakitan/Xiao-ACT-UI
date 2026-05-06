using BenchmarkDotNet.Attributes;
using SaoAuto.Overlay;
using SaoAuto.Overlay.SkillFx;

namespace SaoAuto.Benchmarks;

/// <summary>
/// Mirror of the SkillFx + UI-helper hot loops from
/// <c>_sao_cy_skillfx.pyx</c> and <c>_sao_cy_uihelpers.pyx</c>.
/// </summary>
[MemoryDiagnoser]
public class SkillFxBenchmarks
{
    [Params(64, 256)]
    public int Side;

    [Benchmark]
    public double RingAlpha_Sweep()
    {
        var radius = Side * 0.4;
        var hw = 2.5;
        var center = Side * 0.5;
        double acc = 0;
        for (var y = 0; y < Side; y++)
        {
            for (var x = 0; x < Side; x++)
            {
                acc += CySkillFx.RingAlpha(x - center, y - center, radius, hw, 1.0);
            }
        }
        return acc;
    }

    [Benchmark]
    public double[] BreathOffsets_1024()
    {
        return CyUiHelpers.BreathOffsets(0.123, 1024, 2.5, 6.0);
    }
}
