namespace SaoAuto.Core.Packets;

/// <summary>
/// IPv4 fragment reassembly cache, ported from <c>packet_capture._IpFragmentCache</c>.
/// Stale entries are expired after <see cref="TimeoutSeconds"/>. Returns the
/// fully-reassembled IP payload on the call that completes the datagram, or
/// <c>null</c> while more fragments are pending.
/// </summary>
public sealed class IpFragmentCache
{
    public const double TimeoutSeconds = 30.0;

    private readonly Dictionary<string, Entry> _cache = new();
    private readonly Func<DateTimeOffset> _clock;

    public IpFragmentCache(Func<DateTimeOffset>? clock = null)
    {
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }

    /// <param name="fragmentOffset">Raw IP fragment offset field (already in 8-byte units).</param>
    public byte[]? Feed(
        ushort ipId,
        uint srcIp,
        uint dstIp,
        byte protocol,
        ushort fragmentOffset,
        bool moreFragments,
        ReadOnlySpan<byte> payload)
    {
        var key = $"{ipId}-{srcIp:x8}-{dstIp:x8}-{protocol}";
        var now = _clock();

        ExpireStale(now);

        if (!_cache.TryGetValue(key, out var entry))
        {
            entry = new Entry();
            _cache[key] = entry;
        }

        entry.Fragments[fragmentOffset] = payload.ToArray();
        entry.LastUpdated = now;

        if (!moreFragments)
        {
            entry.HasLastFragment = true;
            entry.Total = fragmentOffset * 8 + payload.Length;
        }

        if (entry.HasLastFragment && entry.Total > 0)
        {
            var buf = new byte[entry.Total];
            var covered = 0;
            foreach (var (offset, data) in entry.Fragments.OrderBy(p => p.Key))
            {
                var start = offset * 8;
                if (start >= buf.Length) continue;
                var copyLen = Math.Min(data.Length, buf.Length - start);
                Buffer.BlockCopy(data, 0, buf, start, copyLen);
                covered += data.Length;
            }
            if (covered >= entry.Total)
            {
                _cache.Remove(key);
                return buf;
            }
        }

        return null;
    }

    private void ExpireStale(DateTimeOffset now)
    {
        if (_cache.Count == 0) return;
        List<string>? expired = null;
        foreach (var (key, entry) in _cache)
        {
            if ((now - entry.LastUpdated).TotalSeconds > TimeoutSeconds)
            {
                (expired ??= new List<string>()).Add(key);
            }
        }
        if (expired is null) return;
        foreach (var key in expired) _cache.Remove(key);
    }

    public int PendingCount => _cache.Count;

    private sealed class Entry
    {
        public Dictionary<ushort, byte[]> Fragments { get; } = new();
        public DateTimeOffset LastUpdated { get; set; }
        public bool HasLastFragment { get; set; }
        public int Total { get; set; }
    }
}
