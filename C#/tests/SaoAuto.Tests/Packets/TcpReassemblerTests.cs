using System.Buffers.Binary;
using SaoAuto.Core.Packets;

namespace SaoAuto.Tests.Packets;

/// <summary>
/// Drive <see cref="TcpReassembler"/> with synthetic Ethernet frames built
/// in-process. Frames are minimal but real: Ethernet → IPv4 → TCP → game payload.
/// Every assertion here pins behavior the Python <c>TcpReassembler</c> regression-tested
/// across a dozen versions; comments name the rule each test guards.
/// </summary>
public class TcpReassemblerTests
{
    [Fact]
    public void IdentifiesServerOnFirstFrameContainingC3Sb()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var reassembler = new TcpReassembler(clock: () => clock.Now);
        var captured = new List<byte[]>();
        reassembler.GamePacket += mem => captured.Add(mem.ToArray());

        // Build one game frame whose payload contains the loose c3SB literal.
        var gamePayload = BuildGameFrameWithLooseC3Sb(seed: 0xAA);
        var raw = BuildEthIpTcpFrame(srcIp: (10, 0, 0, 1), srcPort: 443,
            dstIp: (10, 0, 0, 2), dstPort: 51000, seq: 1000, payload: gamePayload);

        reassembler.FeedRawFrame(raw);

        Assert.True(reassembler.ServerIdentified);
        Assert.NotNull(reassembler.ServerAddress);
        Assert.Equal((ushort)443, reassembler.ServerAddress!.Value.Port);

        // Payload _is_ a complete game frame, so it should also have been emitted.
        Assert.Single(captured);
        Assert.Equal(gamePayload, captured[0]);

        var stats = reassembler.Snapshot();
        Assert.Equal(1, stats.RawFrames);
        Assert.Equal(1, stats.CompleteGameFrames);
    }

    [Fact]
    public void NonGameTrafficBeforeIdentificationIsIgnored()
    {
        var reassembler = new TcpReassembler();
        var captured = new List<byte[]>();
        reassembler.GamePacket += mem => captured.Add(mem.ToArray());

        var noise = new byte[] { 0x00, 0x00, 0x01, 0x00, 0xCA, 0xFE, 0xBA, 0xBE }; // valid frame size, no c3SB
        var raw = BuildEthIpTcpFrame(srcIp: (10, 0, 0, 1), srcPort: 443,
            dstIp: (10, 0, 0, 2), dstPort: 51000, seq: 5000, payload: noise);
        reassembler.FeedRawFrame(raw);

        Assert.False(reassembler.ServerIdentified);
        Assert.Empty(captured);
    }

    [Fact]
    public void AfterIdentificationFurtherFramesFromSameAddrAreExtracted()
    {
        var reassembler = new TcpReassembler();
        var captured = new List<byte[]>();
        reassembler.GamePacket += mem => captured.Add(mem.ToArray());

        // Lock onto server with first frame.
        var first = BuildGameFrameWithLooseC3Sb(seed: 0xAA);
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000, payload: first));

        // Send a second, in-order, "ordinary" game frame (no c3SB needed once locked).
        var second = BuildOrdinaryGameFrame(seed: 0xBB, msgType: 4);
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000 + (uint)first.Length, payload: second));

        Assert.Equal(2, captured.Count);
        Assert.Equal(first, captured[0]);
        Assert.Equal(second, captured[1]);
    }

    [Fact]
    public void OutOfOrderSegmentsReassembleSequentially()
    {
        var reassembler = new TcpReassembler();
        var captured = new List<byte[]>();
        reassembler.GamePacket += mem => captured.Add(mem.ToArray());

        var frame1 = BuildGameFrameWithLooseC3Sb(seed: 0xAA, payloadSize: 24);
        var frame2 = BuildOrdinaryGameFrame(seed: 0xBB, msgType: 4, payloadSize: 24);

        // Lock the server.
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000, payload: frame1));
        Assert.Single(captured);

        // Now split frame2 into two TCP segments, send the SECOND half first.
        var halfSize = frame2.Length / 2;
        var firstHalf = frame2.AsSpan(0, halfSize).ToArray();
        var secondHalf = frame2.AsSpan(halfSize).ToArray();
        var expectedSeq2 = 1000u + (uint)frame1.Length;

        // Out of order: send second half (later seq) before the first half (earlier seq).
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: expectedSeq2 + (uint)firstHalf.Length, payload: secondHalf));
        // Frame still pending — only the locked frame in captured.
        Assert.Single(captured);

        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: expectedSeq2, payload: firstHalf));

        Assert.Equal(2, captured.Count);
        Assert.Equal(frame2, captured[1]);
    }

    [Fact]
    public void GapSkipAdvancesSeqAfterTimeout()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var reassembler = new TcpReassembler(clock: () => clock.Now);
        var captured = new List<byte[]>();
        reassembler.GamePacket += mem => captured.Add(mem.ToArray());

        var first = BuildGameFrameWithLooseC3Sb(seed: 0xAA);
        var second = BuildOrdinaryGameFrame(seed: 0xBB, msgType: 4);

        // Lock onto server with first frame.
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000, payload: first));

        // Skip a segment by jumping the seq forward past `first`. The "second"
        // arrives at first.Length + GAP bytes ahead, so _nextSeq is stuck.
        var gap = (uint)16; // pretend 16 bytes were lost
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000 + (uint)first.Length + gap, payload: second));
        Assert.Single(captured); // still only the first frame

        // Advance the clock past GAP_SKIP_SEC and feed any segment to trigger
        // the gap-skip branch. We send a duplicate of `second` to keep the
        // segment cache populated.
        clock.Advance(TimeSpan.FromSeconds(2.5));
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000 + (uint)first.Length + gap, payload: second));

        Assert.Equal(2, captured.Count);
        Assert.Equal(second, captured[1]);
        Assert.Equal(1, reassembler.Snapshot().GapSkips);
    }

    [Fact]
    public void ForceReconnectClearsServerLockAndCooldownsRepeats()
    {
        var clock = new TestClock(DateTimeOffset.FromUnixTimeSeconds(1_700_000_000));
        var reassembler = new TcpReassembler(clock: () => clock.Now);

        var first = BuildGameFrameWithLooseC3Sb(seed: 0xAA);
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000, payload: first));
        Assert.True(reassembler.ServerIdentified);

        Assert.True(reassembler.ForceReconnect("test"));
        Assert.False(reassembler.ServerIdentified);

        // Within cooldown — second call should be rejected.
        Assert.False(reassembler.ForceReconnect("test"));

        clock.Advance(TimeSpan.FromSeconds(9));
        Assert.True(reassembler.ForceReconnect("test"));

        Assert.Equal(2, reassembler.Snapshot().ForceReconnects);
    }

    [Fact]
    public void ResetClearsLockAndStreamState()
    {
        var reassembler = new TcpReassembler();
        var first = BuildGameFrameWithLooseC3Sb(seed: 0xAA);
        reassembler.FeedRawFrame(BuildEthIpTcpFrame(
            srcIp: (10, 0, 0, 1), srcPort: 443, dstIp: (10, 0, 0, 2), dstPort: 51000,
            seq: 1000, payload: first));
        Assert.True(reassembler.ServerIdentified);

        reassembler.Reset();
        Assert.False(reassembler.ServerIdentified);
        Assert.Null(reassembler.ServerAddress);
    }

    // ─── helpers ─────────────────────────────────────────────────────────

    /// <summary>
    /// Build a complete game frame that contains the loose 4-byte c3SB literal
    /// somewhere inside its payload (so the reassembler can identify the server).
    /// Frame format: [4B BE size][2B msg type][... bytes including c3SB]
    /// </summary>
    private static byte[] BuildGameFrameWithLooseC3Sb(byte seed, int payloadSize = 32)
    {
        var totalSize = 4 + 2 + payloadSize;
        var frame = new byte[totalSize];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)totalSize);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), 4); // msg type 4 (arbitrary, non-FrameDown)

        // Fill payload with seed-based data, then plant the c3SB literal at offset 8.
        for (var i = 6; i < frame.Length; i++) frame[i] = (byte)(seed + i);
        var c3sbAt = 6 + 4;
        frame[c3sbAt + 0] = 0x63;
        frame[c3sbAt + 1] = 0x33;
        frame[c3sbAt + 2] = 0x53;
        frame[c3sbAt + 3] = 0x42;
        return frame;
    }

    /// <summary>Build a non-c3SB game frame (used after server is locked).</summary>
    private static byte[] BuildOrdinaryGameFrame(byte seed, ushort msgType, int payloadSize = 32)
    {
        var totalSize = 4 + 2 + payloadSize;
        var frame = new byte[totalSize];
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(0, 4), (uint)totalSize);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(4, 2), msgType);
        for (var i = 6; i < frame.Length; i++) frame[i] = (byte)(seed ^ i);
        return frame;
    }

    /// <summary>Build a minimal Eth+IPv4+TCP frame wrapping <paramref name="payload"/>.</summary>
    private static byte[] BuildEthIpTcpFrame(
        (byte a, byte b, byte c, byte d) srcIp,
        ushort srcPort,
        (byte a, byte b, byte c, byte d) dstIp,
        ushort dstPort,
        uint seq,
        byte[] payload)
    {
        const int eth = 14;
        var ipLen = 20;
        var tcpLen = 20;
        var total = eth + ipLen + tcpLen + payload.Length;
        var frame = new byte[total];

        // Ethernet: dst+src MAC zero, EtherType = IPv4
        frame[12] = 0x08; frame[13] = 0x00;

        // IPv4
        var ipStart = eth;
        frame[ipStart] = 0x45; // version=4, ihl=5
        var ipTotal = ipLen + tcpLen + payload.Length;
        frame[ipStart + 2] = (byte)(ipTotal >> 8);
        frame[ipStart + 3] = (byte)(ipTotal & 0xFF);
        frame[ipStart + 8] = 64; // ttl
        frame[ipStart + 9] = 6;  // tcp
        frame[ipStart + 12] = srcIp.a; frame[ipStart + 13] = srcIp.b;
        frame[ipStart + 14] = srcIp.c; frame[ipStart + 15] = srcIp.d;
        frame[ipStart + 16] = dstIp.a; frame[ipStart + 17] = dstIp.b;
        frame[ipStart + 18] = dstIp.c; frame[ipStart + 19] = dstIp.d;

        // TCP
        var tcpStart = ipStart + ipLen;
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(tcpStart, 2), srcPort);
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(tcpStart + 2, 2), dstPort);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(tcpStart + 4, 4), seq);
        frame[tcpStart + 12] = 0x50; // data offset = 5

        Buffer.BlockCopy(payload, 0, frame, tcpStart + tcpLen, payload.Length);
        return frame;
    }

    private sealed class TestClock
    {
        public DateTimeOffset Now { get; private set; }
        public TestClock(DateTimeOffset start) => Now = start;
        public void Advance(TimeSpan delta) => Now = Now.Add(delta);
    }
}
