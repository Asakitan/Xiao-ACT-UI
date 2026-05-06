using System.Buffers.Binary;
using System.Runtime.CompilerServices;

namespace SaoAuto.Core.Packets;

/// <summary>
/// File-backed <see cref="IPacketSource"/>. The fixture format is intentionally
/// trivial so it can be produced by a Python helper (Session 5b) and read back
/// here without taking a libpcap dependency in tests:
/// <code>
///   magic   : 0x53415049 ("SAPI") big-endian
///   version : u32 BE = 1
///   per record:
///     timestamp_ms : i64 BE (UnixMilliseconds)
///     length       : u32 BE
///     payload      : raw Ethernet frame
/// </code>
/// Truncated trailing records are skipped silently; integrity is the producer's
/// responsibility (live capture is not exercised here).
/// </summary>
public sealed class FixturePacketSource : IPacketSource
{
    public const uint Magic = 0x53_41_50_49; // "SAPI"
    public const uint CurrentVersion = 1;

    private readonly Stream _stream;
    private readonly bool _ownsStream;

    public FixturePacketSource(Stream stream, bool ownsStream = true)
    {
        _stream = stream ?? throw new ArgumentNullException(nameof(stream));
        _ownsStream = ownsStream;
    }

    public static FixturePacketSource OpenFile(string path) =>
        new(File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read));

    public async IAsyncEnumerable<RawFrame> ReadAsync(
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        if (!await TryReadHeaderAsync(cancellationToken).ConfigureAwait(false))
        {
            yield break;
        }
        var header = new byte[12];
        while (!cancellationToken.IsCancellationRequested)
        {
            var read = await ReadExactlyAsync(header, cancellationToken).ConfigureAwait(false);
            if (read < header.Length) yield break;

            var timestampMs = BinaryPrimitives.ReadInt64BigEndian(header.AsSpan(0, 8));
            var length = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(8, 4));
            if (length == 0) continue;
            if (length > 1 << 24)
            {
                // Sanity cap — fixtures should not contain >16 MiB Ethernet frames.
                yield break;
            }

            var buffer = new byte[(int)length];
            var payloadRead = await ReadExactlyAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (payloadRead < buffer.Length) yield break;

            yield return new RawFrame(DateTimeOffset.FromUnixTimeMilliseconds(timestampMs), buffer);
        }
    }

    private async Task<bool> TryReadHeaderAsync(CancellationToken ct)
    {
        var buffer = new byte[8];
        var read = await ReadExactlyAsync(buffer, ct).ConfigureAwait(false);
        if (read < buffer.Length) return false;

        var magic = BinaryPrimitives.ReadUInt32BigEndian(buffer.AsSpan(0, 4));
        var version = BinaryPrimitives.ReadUInt32BigEndian(buffer.AsSpan(4, 4));
        if (magic != Magic)
        {
            throw new InvalidDataException($"FixturePacketSource: bad magic 0x{magic:X8} (expected 0x{Magic:X8})");
        }
        if (version != CurrentVersion)
        {
            throw new InvalidDataException($"FixturePacketSource: unsupported version {version} (expected {CurrentVersion})");
        }
        return true;
    }

    private async Task<int> ReadExactlyAsync(byte[] buffer, CancellationToken ct)
    {
        var total = 0;
        while (total < buffer.Length)
        {
            var n = await _stream.ReadAsync(buffer.AsMemory(total), ct).ConfigureAwait(false);
            if (n == 0) break;
            total += n;
        }
        return total;
    }

    public void Dispose()
    {
        if (_ownsStream)
        {
            _stream.Dispose();
        }
    }

    /// <summary>
    /// Helper for tests / Python-side fixture writers — emits the fixture
    /// header followed by a single record. Useful to assemble fixtures
    /// in-process during unit tests.
    /// </summary>
    public static void WriteFixture(Stream output, IEnumerable<RawFrame> frames)
    {
        Span<byte> header = stackalloc byte[8];
        BinaryPrimitives.WriteUInt32BigEndian(header[..4], Magic);
        BinaryPrimitives.WriteUInt32BigEndian(header[4..8], CurrentVersion);
        output.Write(header);

        Span<byte> recordHeader = stackalloc byte[12];
        foreach (var frame in frames)
        {
            BinaryPrimitives.WriteInt64BigEndian(recordHeader[..8], frame.Timestamp.ToUnixTimeMilliseconds());
            BinaryPrimitives.WriteUInt32BigEndian(recordHeader[8..12], (uint)frame.Bytes.Length);
            output.Write(recordHeader);
            output.Write(frame.Bytes.Span);
        }
    }
}
