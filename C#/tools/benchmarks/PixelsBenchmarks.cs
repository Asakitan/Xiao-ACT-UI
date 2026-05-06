using BenchmarkDotNet.Attributes;
using SaoAuto.Overlay.Rendering;

namespace SaoAuto.Benchmarks;

/// <summary>
/// BGRA32 inner-loop benchmarks mirroring <c>_sao_cy_pixels.pyx</c>:
/// alpha-over blit, premultiply, nearest scale.
/// </summary>
[MemoryDiagnoser]
public class PixelsBenchmarks
{
    private byte[] _dst = Array.Empty<byte>();
    private byte[] _src = Array.Empty<byte>();

    [Params(256, 1024)]
    public int Side;

    [GlobalSetup]
    public void Setup()
    {
        var pixels = Side * Side * 4;
        _dst = new byte[pixels];
        _src = new byte[pixels];
        var rng = new Random(0xBEEF);
        rng.NextBytes(_dst);
        rng.NextBytes(_src);
    }

    [Benchmark]
    public void AlphaBlit_FullFrame()
    {
        CyPixelsExtras.AlphaBlit(_dst, _src, Side, Side, Side * 4, Side * 4);
    }

    [Benchmark]
    public void PremultiplyInPlace_FullFrame()
    {
        // Working copy so successive iterations stay representative.
        Array.Copy(_src, _dst, _dst.Length);
        PremultiplyHelpers.PremultiplyInPlace(_dst);
    }

    [Benchmark]
    public byte[] ScaleNearest_HalfSize()
    {
        return CyPixelsExtras.ScaleNearest(_src, Side, Side, Side / 2, Side / 2);
    }
}
