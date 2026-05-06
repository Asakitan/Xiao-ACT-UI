namespace SaoAuto.Core.Packets;

/// <summary>
/// Protobuf varint helpers ported from <c>_sao_cy_packet.pyx</c>.
/// All readers operate on <see cref="ReadOnlySpan{T}"/> so they avoid allocations
/// on the parser hot path. Unlike <c>Google.Protobuf.CodedInputStream</c>, these
/// helpers tolerate over-wide encodings (10-byte payloads on 32-bit fields) by
/// masking to the low 32 bits before signed conversion — a quirk of the live
/// Star Resonance combat AOI attribute stream.
/// </summary>
public static class Varint
{
    /// <summary>
    /// Read a varint, returning its raw unsigned 64-bit value.
    /// On success, <paramref name="bytesRead"/> is set to the number of bytes consumed.
    /// On underrun (no terminator within <paramref name="data"/>) the partial value
    /// computed so far is returned and <paramref name="bytesRead"/> is set to
    /// <c>data.Length</c>; callers that need strict framing must check
    /// <c>bytesRead &gt; 0 &amp;&amp; (data[bytesRead - 1] &amp; 0x80) == 0</c>.
    /// </summary>
    public static ulong ReadUInt64(ReadOnlySpan<byte> data, out int bytesRead)
    {
        ulong result = 0;
        var shift = 0;
        var i = 0;
        while (i < data.Length)
        {
            var b = data[i++];
            if (shift < 64)
            {
                result |= (ulong)(b & 0x7F) << shift;
            }
            if ((b & 0x80) == 0)
            {
                bytesRead = i;
                return result;
            }
            shift += 7;
        }
        bytesRead = i;
        return result;
    }

    /// <summary>
    /// Read an unsigned varint and project it onto a 32-bit signed integer using
    /// two's-complement semantics. Mirrors <c>varint_to_int32</c> in
    /// <c>_sao_cy_packet.pyx</c> — over-wide payloads are masked to the low
    /// 32 bits before sign extension.
    /// </summary>
    public static int ReadInt32(ReadOnlySpan<byte> data, out int bytesRead)
    {
        var raw = ReadUInt64(data, out bytesRead);
        return ToInt32(raw);
    }

    /// <summary>Read an unsigned varint as a 64-bit signed integer.</summary>
    public static long ReadInt64(ReadOnlySpan<byte> data, out int bytesRead)
    {
        var raw = ReadUInt64(data, out bytesRead);
        return ToInt64(raw);
    }

    /// <summary>
    /// Two's-complement reinterpretation of a 64-bit varint payload.
    /// Mirrors <c>varint_to_int64</c>.
    /// </summary>
    public static long ToInt64(ulong value) => unchecked((long)value);

    /// <summary>
    /// Two's-complement reinterpretation of a 32-bit varint payload, masking
    /// to the low 32 bits first. Mirrors <c>varint_to_int32</c>:
    /// <code>
    /// u = val &amp; 0xFFFFFFFF
    /// return u &gt; 0x7FFFFFFF ? (u - 0x80000000) - 0x80000000 : u
    /// </code>
    /// </summary>
    public static int ToInt32(ulong value)
    {
        var low = unchecked((uint)(value & 0xFFFFFFFFul));
        return unchecked((int)low);
    }

    /// <summary>
    /// Decode a length-prefixed UTF-8 string in <c>protobufjs reader.string()</c>
    /// shape: <c>[varint length][utf-8 bytes]</c>. Falls back to decoding the
    /// entire buffer as UTF-8 when the leading varint length is invalid or
    /// missing — matches the legacy Python helper.
    /// </summary>
    public static string DecodeString(ReadOnlySpan<byte> raw)
    {
        if (raw.IsEmpty) return string.Empty;
        try
        {
            var len = (int)ReadUInt64(raw, out var pos);
            var end = pos + len;
            if (len > 0 && end <= raw.Length)
            {
                return System.Text.Encoding.UTF8.GetString(raw.Slice(pos, len));
            }
        }
        catch
        {
            // fall through to whole-buffer decode
        }
        try
        {
            return System.Text.Encoding.UTF8.GetString(raw);
        }
        catch
        {
            return string.Empty;
        }
    }
}
