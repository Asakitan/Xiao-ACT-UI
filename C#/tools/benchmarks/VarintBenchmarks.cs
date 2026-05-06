using BenchmarkDotNet.Attributes;
using SaoAuto.Core.Packets;

namespace SaoAuto.Benchmarks;

/// <summary>
/// Mirrors the Cython <c>_sao_cy_packet.read_uvarint</c> hot path.
/// Stripe a deterministic mix of 1-byte, 5-byte, and 10-byte varints so
/// the measurement is not dominated by branch-predictable inputs.
/// </summary>
[MemoryDiagnoser]
public class VarintBenchmarks
{
    private byte[] _stream = Array.Empty<byte>();

    [Params(1024, 16384)]
    public int Bytes;

    [GlobalSetup]
    public void Setup()
    {
        // Hand-roll a varint encoder: don't pull a dep just for setup.
        var buf = new List<byte>(capacity: Bytes + 16);
        ulong value = 1;
        var rng = new Random(0xC0DE);
        while (buf.Count < Bytes)
        {
            var pick = rng.Next(3);
            value = pick switch
            {
                0 => (ulong)rng.Next(1, 0x7F),                  // 1-byte
                1 => (ulong)rng.Next(0x80, 0x10_00_00),         // 3-byte
                _ => unchecked((ulong)rng.NextInt64(1L << 56, long.MaxValue)), // 10-byte
            };
            EncodeVarint(value, buf);
        }
        _stream = buf.ToArray();
    }

    [Benchmark]
    public ulong ReadUInt64_All()
    {
        ReadOnlySpan<byte> span = _stream;
        ulong sum = 0;
        var pos = 0;
        while (pos < span.Length)
        {
            sum ^= Varint.ReadUInt64(span.Slice(pos), out var read);
            pos += read;
            if (read == 0) break;
        }
        return sum;
    }

    private static void EncodeVarint(ulong value, List<byte> dst)
    {
        while (value >= 0x80)
        {
            dst.Add((byte)((value & 0x7F) | 0x80));
            value >>= 7;
        }
        dst.Add((byte)value);
    }
}
