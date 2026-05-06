using System.Buffers.Binary;

namespace SaoAuto.Core.Packets;

/// <summary>
/// Game-frame signature helpers ported from <c>_sao_cy_packet</c>:
/// <list type="bullet">
///   <item><c>scan_c3sb_nested</c> — strict identification of FrameDown(type=6) envelopes carrying a nested c3SB.</item>
///   <item><c>find_frame_realign</c> — bounded scan for the next plausible game-frame header after corruption.</item>
/// </list>
/// </summary>
public static class FrameSignatureScanner
{
    public const int MaxValidFrameLength = 0x000F_FFFF; // 1 048 575
    public const int MinValidFrameLength = 6;
    public const int MaxRealignScanBytes = 65536;

    /// <summary>
    /// Scan for a nested game frame whose payload begins with the long c3SB
    /// signature at offset +5 inside the inner payload (matches Python's
    /// <c>scan_c3sb_nested</c>: <c>00 63 33 53 42 00</c> at <c>payload_start + 5..10</c>).
    /// </summary>
    public static bool ScanC3SbNested(ReadOnlySpan<byte> data)
    {
        var offset = 0;
        while (offset + 4 < data.Length)
        {
            var plen = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(offset, 4));
            if (plen < MinValidFrameLength || plen > MaxValidFrameLength) break;

            var end = offset + (int)plen;
            if (end > data.Length) break;

            var payloadStart = offset + 4;
            if (end - payloadStart > 11)
            {
                var p = data.Slice(payloadStart);
                if (p[5] == 0x00 &&
                    p[6] == 0x63 &&
                    p[7] == 0x33 &&
                    p[8] == 0x53 &&
                    p[9] == 0x42 &&
                    p[10] == 0x00)
                {
                    return true;
                }
            }
            offset = end;
        }
        return false;
    }

    /// <summary>
    /// Locate the next plausible frame header offset (BE32 length within
    /// <c>[6, 0xFFFFF]</c> followed by a known msg type 2/3/4/5/6) within
    /// <paramref name="maxScan"/> bytes. Returns <c>-1</c> when no candidate
    /// is found. Used to recover from a corrupt header without dropping the
    /// whole TCP buffer.
    /// </summary>
    public static int FindFrameRealign(ReadOnlySpan<byte> data, int maxScan = MaxRealignScanBytes)
    {
        var scanEnd = data.Length - 5;
        if (maxScan > 0 && scanEnd > maxScan) scanEnd = maxScan;
        if (scanEnd <= 1) return -1;

        for (var i = 1; i < scanEnd; i++)
        {
            var sz = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(i, 4));
            if (sz < MinValidFrameLength || sz > MaxValidFrameLength) continue;

            var rawType = BinaryPrimitives.ReadUInt16BigEndian(data.Slice(i + 4, 2));
            var msg = rawType & 0x7FFF;
            if (msg is 2 or 3 or 4 or 5 or 6)
            {
                return i;
            }
        }
        return -1;
    }
}
