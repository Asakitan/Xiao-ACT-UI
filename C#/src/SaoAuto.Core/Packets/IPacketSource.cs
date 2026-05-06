namespace SaoAuto.Core.Packets;

/// <summary>
/// Source of raw Ethernet frames feeding the TCP reassembler.
/// Implementations: <see cref="FixturePacketSource"/> (file-backed, for tests
/// and parity replay) and the live SharpPcap source (Session 5b).
/// </summary>
public interface IPacketSource : IDisposable
{
    /// <summary>Stream raw Ethernet frames until the source is exhausted or cancelled.</summary>
    IAsyncEnumerable<RawFrame> ReadAsync(CancellationToken cancellationToken = default);
}

/// <summary>Raw Ethernet frame plus the wall-clock timestamp from the capture.</summary>
public readonly record struct RawFrame(DateTimeOffset Timestamp, ReadOnlyMemory<byte> Bytes);
