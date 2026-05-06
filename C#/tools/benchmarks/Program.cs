using BenchmarkDotNet.Running;

namespace SaoAuto.Benchmarks;

/// <summary>
/// Entry point for the Cython-parity microbenchmark suite. Run from the
/// repo root with:
///
///   dotnet run -c Release --project sao_auto/C#/tools/benchmarks/SaoAuto.Benchmarks.csproj
///
/// Filter by class with `--filter '*Varint*'`. Goal of this skeleton is
/// to make perf regressions visible — not to lock down absolute numbers.
/// </summary>
public static class Program
{
    public static int Main(string[] args)
    {
        BenchmarkSwitcher.FromAssembly(typeof(Program).Assembly).Run(args);
        return 0;
    }
}
