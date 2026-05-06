using System.Buffers.Binary;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Packets;

/// <summary>
/// One-way TCP reassembler + game-frame extractor, ported from
/// <c>packet_capture.TcpReassembler</c>. The state machine is the load-bearing
/// piece of Star Resonance live capture; the comments preserve the Python
/// version-history reasoning so a future contributor can cross-reference the
/// regressions each guard prevents.
/// </summary>
/// <remarks>
/// <para>
/// Public surface mirrors Python: feed raw Ethernet frames via
/// <see cref="FeedRawFrame"/>; subscribe to <see cref="GamePacket"/> for fully
/// re-framed inbound packets and <see cref="ServerChange"/> for scene/reconnect
/// events. <see cref="ServerIdentified"/> exposes whether the endpoint lock is held.
/// </para>
/// <para>
/// IP fragment reassembly is delegated to <see cref="IpFragmentCache"/>.
/// Outbound packets only contribute to endpoint detection — they are never fed
/// into the inbound TCP stream.
/// </para>
/// </remarks>
public sealed class TcpReassembler
{
    private const int RecentPktLimit = 24;
    private const double ReconnectCooldownSeconds = 3.0;
    private const double ForceReconnectCooldownSeconds = 8.0;
    private const double TcpTimeoutSeconds = 30.0;
    private const double GapSkipSeconds = 2.0;
    private const double SceneOldStillAliveSeconds = 3.0;
    private const int CacheSegmentLimit = 2000;
    private const long SeqAnomalyThreshold = 1_000_000;
    private const long SeqWindowReplay = 1_000_000;
    private const uint SeqWraparoundMask = 0xFFFFFFFFu;

    private readonly object _gate = new();
    private readonly IpFragmentCache _fragments;
    private readonly ILogger _log;
    private readonly Func<DateTimeOffset> _clock;

    private TcpEndpoint? _serverAddr;
    private uint? _nextSeq;
    private readonly Dictionary<uint, byte[]> _segmentCache = new();
    private byte[] _buf = Array.Empty<byte>();
    private DateTimeOffset _lastSegmentAt;
    private DateTimeOffset _gapSince;

    private readonly Queue<RecentPkt> _recentPkts = new();
    private DateTimeOffset _lastReconnectAt;
    private DateTimeOffset _lastForceReconnectAt;

    private long _rawFrames;
    private long _tcpSegments;
    private long _completeGameFrames;
    private long _seqResets;
    private long _cacheOverflows;
    private long _gapSkips;
    private long _serverChanges;
    private long _replayedAfterChange;
    private long _forceReconnects;

    public TcpReassembler(ILogger<TcpReassembler>? logger = null, Func<DateTimeOffset>? clock = null)
    {
        _log = (ILogger?)logger ?? NullLogger.Instance;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _fragments = new IpFragmentCache(_clock);
    }

    public event Action<ReadOnlyMemory<byte>>? GamePacket;
    public event Action? ServerChange;

    public bool ServerIdentified
    {
        get { lock (_gate) { return _serverAddr.HasValue; } }
    }

    public TcpEndpoint? ServerAddress
    {
        get { lock (_gate) { return _serverAddr; } }
    }

    public TcpReassemblerStats Snapshot() => new(
        RawFrames: Interlocked.Read(ref _rawFrames),
        TcpSegments: Interlocked.Read(ref _tcpSegments),
        CompleteGameFrames: Interlocked.Read(ref _completeGameFrames),
        SeqResets: Interlocked.Read(ref _seqResets),
        CacheOverflows: Interlocked.Read(ref _cacheOverflows),
        GapSkips: Interlocked.Read(ref _gapSkips),
        ServerChanges: Interlocked.Read(ref _serverChanges),
        ReplayedAfterChange: Interlocked.Read(ref _replayedAfterChange),
        ForceReconnects: Interlocked.Read(ref _forceReconnects));

    public void Reset()
    {
        lock (_gate)
        {
            _serverAddr = null;
            _nextSeq = null;
            _segmentCache.Clear();
            _buf = Array.Empty<byte>();
            _lastSegmentAt = default;
            _gapSince = default;
        }
    }

    public bool ForceReconnect(string reason = "watchdog")
    {
        var now = _clock();
        TcpEndpoint? oldAddr;
        lock (_gate)
        {
            if ((now - _lastForceReconnectAt).TotalSeconds < ForceReconnectCooldownSeconds) return false;
            _lastForceReconnectAt = now;
            oldAddr = _serverAddr;
            _serverAddr = null;
            _nextSeq = null;
            _segmentCache.Clear();
            _buf = Array.Empty<byte>();
            _lastSegmentAt = default;
            _gapSince = default;
        }
        Interlocked.Increment(ref _forceReconnects);
        _log.LogWarning("[Capture] force reconnect: reason={Reason} old={Old}", reason, oldAddr?.Display ?? "<none>");
        return true;
    }

    public void FeedRawFrame(ReadOnlySpan<byte> raw)
    {
        Interlocked.Increment(ref _rawFrames);
        var parsed = EthernetIpTcpParser.TryParse(raw);
        if (parsed is null) return;

        var f = parsed.Value;
        var payload = f.Payload;

        if (f.MoreFragments || f.FragmentOffset > 0)
        {
            var reassembled = _fragments.Feed(
                f.IpId, f.Source.Ipv4, f.Destination.Ipv4, protocol: 6,
                f.FragmentOffset, f.MoreFragments, payload);
            if (reassembled is null) return;
            // Re-parse TCP header from reassembled IP payload.
            if (reassembled.Length < 20) return;
            // Source/dst ports are first 2x2 bytes of the TCP header inside the IP payload.
            var srcPort = BinaryPrimitives.ReadUInt16BigEndian(reassembled.AsSpan(0, 2));
            var dstPort = BinaryPrimitives.ReadUInt16BigEndian(reassembled.AsSpan(2, 2));
            var seq = BinaryPrimitives.ReadUInt32BigEndian(reassembled.AsSpan(4, 4));
            var dataOffsetWords = (reassembled[12] >> 4) & 0x0F;
            var tcpHeaderLength = dataOffsetWords * 4;
            if (tcpHeaderLength < 20 || tcpHeaderLength > reassembled.Length) return;
            payload = reassembled.AsSpan(tcpHeaderLength).ToArray();
            f = f with
            {
                Source = f.Source with { Port = srcPort },
                Destination = f.Destination with { Port = dstPort },
                TcpSequence = seq,
                Payload = payload,
            };
        }

        if (payload.Length == 0) return;

        var addr = f.Source;
        var seqNumber = f.TcpSequence;
        var payloadCopy = payload; // Already a defensive copy from EthernetIpTcpParser.

        // Always cache recent packets BEFORE any early-return paths so a successful
        // identify can replay the segments that arrived just before lock-on.
        EnqueueRecent(new RecentPkt(addr, seqNumber, payloadCopy));

        if (TryInitialIdentify(addr, payloadCopy, seqNumber)) return;
        if (!_serverAddr.HasValue) return; // never happens — TryInitialIdentify guards this

        if (!addr.Equals(_serverAddr.Value))
        {
            HandleSceneChange(addr, payloadCopy, seqNumber);
            return;
        }

        HandleSameServerReconnectIfNeeded(addr, payloadCopy, seqNumber);
        FeedTcp(seqNumber, payloadCopy);
    }

    private void EnqueueRecent(RecentPkt pkt)
    {
        lock (_gate)
        {
            _recentPkts.Enqueue(pkt);
            while (_recentPkts.Count > RecentPktLimit) _recentPkts.Dequeue();
        }
    }

    /// <summary>
    /// First-pass server lock: accept the first payload that matches strict OR
    /// loose c3SB. Replays buffered recent packets afterward so the initial
    /// SyncContainerData / EnterGame can still be parsed.
    /// </summary>
    private bool TryInitialIdentify(TcpEndpoint addr, byte[] payload, uint seq)
    {
        bool needFeed;
        lock (_gate)
        {
            if (_serverAddr.HasValue) return false;
            if (!FrameSignatureScanner.ScanC3SbNested(payload)
                && !ContainsShortC3Sb(payload)
                && !IsIdentifyStrict(payload))
            {
                return true; // suppress further processing (no lock yet)
            }
            _serverAddr = addr;
            _log.LogInformation("[Capture] identified game server: {Addr}", addr.Display);
            needFeed = true;
        }
        var replayed = ReplayRecentForAddr(addr, excludeSeq: seq, seqWindow: 0);
        if (replayed > 0)
        {
            _log.LogInformation("[Capture] replayed {N} recent packets (initial)", replayed);
        }
        if (needFeed)
        {
            FeedTcp(seq, payload);
        }
        return true; // handled — do not fall through to scene/feed
    }

    private void HandleSceneChange(TcpEndpoint addr, byte[] payload, uint seq)
    {
        // Strict identification only — loose c3SB on a different addr is the
        // canonical false-positive that used to ping-pong _server_addr.
        if (!IsIdentifyStrict(payload)) return;

        var now = _clock();
        bool oldStillAlive;
        TcpEndpoint? oldAddr;
        lock (_gate)
        {
            oldStillAlive = (now - _lastSegmentAt).TotalSeconds < SceneOldStillAliveSeconds;
            oldAddr = _serverAddr;
            if (oldStillAlive) return;
            _serverAddr = addr;
            _nextSeq = null;
            _segmentCache.Clear();
            _buf = Array.Empty<byte>();
            _gapSince = default;
        }

        Interlocked.Increment(ref _serverChanges);
        _log.LogInformation("[Capture] scene server change: {Old} -> {New}",
            oldAddr?.Display ?? "<none>", addr.Display);

        try { ServerChange?.Invoke(); }
        catch (Exception ex) { _log.LogError(ex, "[Capture] ServerChange handler threw"); }

        var replayed = ReplayRecentForAddr(addr, excludeSeq: seq, seqWindow: 0);
        if (replayed > 0)
        {
            _log.LogInformation("[Capture] replayed {N} recent packets after scene change", replayed);
        }
        FeedTcp(seq, payload);
    }

    private void HandleSameServerReconnectIfNeeded(TcpEndpoint addr, byte[] payload, uint seq)
    {
        bool seqMatch;
        bool seqAnomalous = false;
        lock (_gate)
        {
            if (_nextSeq is null)
            {
                seqMatch = true;
            }
            else
            {
                var ns = _nextSeq.Value;
                seqMatch = ns == seq || _segmentCache.ContainsKey(seq);
                if (!seqMatch)
                {
                    var diffFwd = (long)((seq - ns) & SeqWraparoundMask);
                    var diffBwd = (long)((ns - seq) & SeqWraparoundMask);
                    seqAnomalous = diffFwd > SeqAnomalyThreshold && diffBwd > SeqAnomalyThreshold;
                }
            }
        }

        if (seqMatch || !seqAnomalous) return;

        if (!IsIdentifyStrict(payload)) return;

        var now = _clock();
        bool fired = false;
        lock (_gate)
        {
            if ((now - _lastReconnectAt).TotalSeconds < ReconnectCooldownSeconds) return;
            _lastReconnectAt = now;
            _log.LogInformation("[Capture] same-server reconnect: expected={Expected} got={Got}",
                _nextSeq?.ToString() ?? "<unset>", seq);
            _nextSeq = null;
            _segmentCache.Clear();
            _buf = Array.Empty<byte>();
            _gapSince = default;
            fired = true;
        }
        if (fired)
        {
            try { ServerChange?.Invoke(); }
            catch (Exception ex) { _log.LogError(ex, "[Capture] ServerChange handler threw"); }
            ReplayRecentForAddr(addr, excludeSeq: seq, seqWindow: SeqWindowReplay);
        }
    }

    private static bool IsIdentifyStrict(byte[] data)
    {
        if (data.Length > 6)
        {
            var rawType = BinaryPrimitives.ReadUInt16BigEndian(data.AsSpan(4, 2));
            var isZstd = (rawType & 0x8000) != 0;
            var msg = rawType & 0x7FFF;
            if (msg == 6) // FrameDown
            {
                if (isZstd) return true; // zstd FrameDown — cannot scan, but combo is unique to game traffic
                if (FrameSignatureScanner.ScanC3SbNested(data.AsSpan(6))) return true;
            }
        }
        // Login Return: 4B BE size = 0x62, type = 0x0003
        if (data.Length >= 0x62 &&
            data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x62 &&
            data[4] == 0x00 && data[5] == 0x03)
        {
            return true;
        }
        return false;
    }

    private static bool ContainsShortC3Sb(byte[] data)
    {
        // Substring search for the 4-byte literal "c3SB". Buffer is typically
        // a few KB; a naive scan is fast enough.
        for (var i = 0; i <= data.Length - 4; i++)
        {
            if (data[i] == 0x63 && data[i + 1] == 0x33 && data[i + 2] == 0x53 && data[i + 3] == 0x42)
            {
                return true;
            }
        }
        return false;
    }

    private int ReplayRecentForAddr(TcpEndpoint addr, uint excludeSeq, long seqWindow)
    {
        List<(uint Seq, byte[] Payload)> candidates;
        lock (_gate)
        {
            candidates = new List<(uint, byte[])>();
            foreach (var pkt in _recentPkts)
            {
                if (!pkt.Addr.Equals(addr)) continue;
                if (pkt.Seq == excludeSeq) continue;
                candidates.Add((pkt.Seq, pkt.Payload));
            }
        }

        if (seqWindow > 0)
        {
            var filtered = new List<(uint, byte[])>(candidates.Count);
            foreach (var (s, p) in candidates)
            {
                var fwd = (long)((s - excludeSeq) & SeqWraparoundMask);
                var bwd = (long)((excludeSeq - s) & SeqWraparoundMask);
                if (Math.Min(fwd, bwd) <= seqWindow) filtered.Add((s, p));
            }
            candidates = filtered;
        }

        if (candidates.Count == 0)
        {
            // Drop stale recent-pkt entries for this addr so a future genuine
            // reconnect doesn't dredge them up.
            PurgeRecentForAddr(addr);
            return 0;
        }

        candidates.Sort((a, b) => a.Seq.CompareTo(b.Seq));
        var replayed = 0;
        foreach (var (s, p) in candidates)
        {
            try
            {
                FeedTcp(s, p);
                replayed++;
            }
            catch (Exception ex)
            {
                _log.LogDebug(ex, "[Capture] replay seq={Seq} failed", s);
            }
        }
        if (replayed > 0)
        {
            Interlocked.Add(ref _replayedAfterChange, replayed);
        }
        PurgeRecentForAddr(addr);
        return replayed;
    }

    private void PurgeRecentForAddr(TcpEndpoint addr)
    {
        lock (_gate)
        {
            if (_recentPkts.Count == 0) return;
            var kept = new Queue<RecentPkt>(_recentPkts.Count);
            foreach (var pkt in _recentPkts)
            {
                if (!pkt.Addr.Equals(addr)) kept.Enqueue(pkt);
            }
            _recentPkts.Clear();
            foreach (var p in kept) _recentPkts.Enqueue(p);
        }
    }

    private void FeedTcp(uint seq, byte[] data)
    {
        List<byte[]>? frames = null;
        bool needRealignScan = false;

        lock (_gate)
        {
            Interlocked.Increment(ref _tcpSegments);
            var now = _clock();

            // 30-second TCP timeout — wipe stream state.
            if (_lastSegmentAt != default && (now - _lastSegmentAt).TotalSeconds > TcpTimeoutSeconds)
            {
                _log.LogWarning("[Capture] TCP timeout, resetting stream");
                _nextSeq = null;
                _segmentCache.Clear();
                _buf = Array.Empty<byte>();
                _gapSince = default;
                Interlocked.Increment(ref _seqResets);
            }

            // Initialize next-seq from the first plausible game-frame header.
            if (_nextSeq is null)
            {
                if (data.Length < 4) return;
                var pktSize = BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(0, 4));
                if (pktSize < FrameSignatureScanner.MinValidFrameLength
                    || pktSize > FrameSignatureScanner.MaxValidFrameLength)
                {
                    return;
                }
                _nextSeq = seq;
            }

            // Drop already-consumed retransmits (wraparound-safe).
            var diff = (seq - _nextSeq.Value) & SeqWraparoundMask;
            if (diff > 0x80000000u) return;

            _segmentCache[seq] = data;

            // Sequential drain. Use a chunk list + single concatenation to avoid
            // O(n²) byte[] growth on busy maps (Python regression v2.3.9).
            var consumed = false;
            List<byte[]>? newChunks = null;
            while (_segmentCache.TryGetValue(_nextSeq.Value, out var chunk))
            {
                _segmentCache.Remove(_nextSeq.Value);
                (newChunks ??= new List<byte[]>()).Add(chunk);
                _nextSeq = (uint)((_nextSeq.Value + (uint)chunk.Length) & SeqWraparoundMask);
                _lastSegmentAt = now;
                consumed = true;
            }
            if (newChunks is { Count: > 0 })
            {
                var totalLen = _buf.Length;
                foreach (var c in newChunks) totalLen += c.Length;
                var combined = new byte[totalLen];
                var pos = 0;
                if (_buf.Length > 0)
                {
                    Buffer.BlockCopy(_buf, 0, combined, 0, _buf.Length);
                    pos = _buf.Length;
                }
                foreach (var c in newChunks)
                {
                    Buffer.BlockCopy(c, 0, combined, pos, c.Length);
                    pos += c.Length;
                }
                _buf = combined;
            }

            // Gap-skip: when a missing segment stalls the stream, after
            // GapSkipSeconds advance _nextSeq to the lowest cached seq.
            if (_segmentCache.Count > 0)
            {
                if (consumed)
                {
                    _gapSince = default;
                }
                else if (_gapSince == default)
                {
                    _gapSince = now;
                }
                else if ((now - _gapSince).TotalSeconds >= GapSkipSeconds)
                {
                    var minSeq = uint.MaxValue;
                    foreach (var k in _segmentCache.Keys)
                    {
                        if (k < minSeq) minSeq = k;
                    }
                    _log.LogWarning("[Capture] TCP gap skip: {N} segments cached, advancing seq",
                        _segmentCache.Count);
                    _buf = Array.Empty<byte>();
                    _nextSeq = minSeq;
                    _gapSince = default;
                    Interlocked.Increment(ref _gapSkips);

                    List<byte[]>? skipChunks = null;
                    while (_segmentCache.TryGetValue(_nextSeq.Value, out var chunk))
                    {
                        _segmentCache.Remove(_nextSeq.Value);
                        (skipChunks ??= new List<byte[]>()).Add(chunk);
                        _nextSeq = (uint)((_nextSeq.Value + (uint)chunk.Length) & SeqWraparoundMask);
                        _lastSegmentAt = now;
                    }
                    if (skipChunks is { Count: > 0 })
                    {
                        var totalLen = 0;
                        foreach (var c in skipChunks) totalLen += c.Length;
                        var combined = new byte[totalLen];
                        var pos = 0;
                        foreach (var c in skipChunks)
                        {
                            Buffer.BlockCopy(c, 0, combined, pos, c.Length);
                            pos += c.Length;
                        }
                        _buf = combined;
                    }
                }
            }
            else
            {
                _gapSince = default;
            }

            // Cache overflow guard.
            if (_segmentCache.Count > CacheSegmentLimit)
            {
                _log.LogWarning("[Capture] TCP cache overflow ({N}), reset", _segmentCache.Count);
                _nextSeq = null;
                _segmentCache.Clear();
                _buf = Array.Empty<byte>();
                _gapSince = default;
                Interlocked.Increment(ref _cacheOverflows);
                return;
            }

            // Frame extraction (still under lock; copies frame bytes out before
            // we publish to invokers outside the lock).
            frames = ExtractFramesLocked(out needRealignScan);
        }

        if (frames is { Count: > 0 })
        {
            foreach (var frame in frames)
            {
                try
                {
                    GamePacket?.Invoke(frame);
                    Interlocked.Increment(ref _completeGameFrames);
                }
                catch (Exception ex)
                {
                    _log.LogError(ex, "[Capture] frame handler threw");
                }
            }
        }
    }

    private List<byte[]>? ExtractFramesLocked(out bool needRealignScan)
    {
        // _gate must be held by caller. Mirrors Python's `_extract_frames` exactly:
        // accumulate offset across all extracted frames, then slice the buffer once.
        needRealignScan = false;
        if (_buf.Length < FrameSignatureScanner.MinValidFrameLength) return null;

        List<byte[]>? frames = null;
        var offset = 0;
        var bufLen = _buf.Length;

        while (bufLen - offset >= FrameSignatureScanner.MinValidFrameLength)
        {
            var size = (int)BinaryPrimitives.ReadUInt32BigEndian(_buf.AsSpan(offset, 4));
            if (size < FrameSignatureScanner.MinValidFrameLength
                || size > FrameSignatureScanner.MaxValidFrameLength)
            {
                needRealignScan = true;
                break;
            }
            if (bufLen - offset < size)
            {
                // Header valid, body not yet available — wait for more data.
                break;
            }

            var frame = new byte[size];
            Buffer.BlockCopy(_buf, offset, frame, 0, size);
            (frames ??= new List<byte[]>()).Add(frame);
            offset += size;
        }

        if (offset > 0)
        {
            // Slice buffer once after the batch — Python "v2.3.9" optimization.
            var sliceLen = bufLen - offset;
            if (sliceLen <= 0)
            {
                _buf = Array.Empty<byte>();
            }
            else
            {
                var next = new byte[sliceLen];
                Buffer.BlockCopy(_buf, offset, next, 0, sliceLen);
                _buf = next;
            }
        }

        if (needRealignScan)
        {
            var realignOffset = FrameSignatureScanner.FindFrameRealign(_buf);
            if (realignOffset > 0)
            {
                _log.LogWarning("[Capture] frame realign: skipping {N} bytes", realignOffset);
                var sliceLen = _buf.Length - realignOffset;
                var next = new byte[sliceLen];
                Buffer.BlockCopy(_buf, realignOffset, next, 0, sliceLen);
                _buf = next;
            }
            else
            {
                _log.LogError("[Capture] no realign anchor; clearing stream");
                _buf = Array.Empty<byte>();
                _nextSeq = null;
                _segmentCache.Clear();
            }
        }

        return frames;
    }

    private readonly record struct RecentPkt(TcpEndpoint Addr, uint Seq, byte[] Payload);
}
