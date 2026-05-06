namespace SaoAuto.Core.Packets;

/// <summary>
/// Well-known byte signatures used for game frame identification.
/// Values mirror <c>packet_capture.C3SB_SIGNATURE</c> / <c>C3SB_SHORT</c>.
/// </summary>
public static class PacketCodes
{
    /// <summary>
    /// Long c3SB signature with the <c>0x00</c> sentinel byte.
    /// Used for strict identification (matches <c>FrameDown</c> envelopes).
    /// </summary>
    public static ReadOnlySpan<byte> C3SbLong => new byte[] { 0x00, 0x63, 0x33, 0x53, 0x42, 0x00 };

    /// <summary>
    /// Short c3SB signature (4 bytes). Used for the loose first-pass identifier
    /// before a server endpoint is locked. Strict identification must use
    /// <see cref="C3SbLong"/> to avoid mis-classifying chat / voice / CDN flows.
    /// </summary>
    public static ReadOnlySpan<byte> C3SbShort => new byte[] { 0x63, 0x33, 0x53, 0x42 };

    /// <summary>
    /// Game frame envelope is <c>[4B big-endian length][payload]</c>; this is the
    /// minimum length of a sane frame header.
    /// </summary>
    public const int FrameHeaderLength = 4;

    /// <summary>
    /// Service UUID embedded in every Notify payload's leading 8 bytes
    /// (mirrors Python <c>SERVICE_UUID_C3SB = 0x0000000063335342</c>).
    /// Used by <see cref="CyPacketExtras.ParseNotifyHeader"/> to gate
    /// non-c3SB notifies before the parser dispatches them.
    /// </summary>
    public const ulong ServiceUuidC3Sb = 0x0000_0000_6333_5342UL;
}
