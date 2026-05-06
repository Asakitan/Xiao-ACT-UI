namespace SaoAuto.Overlay.Rendering;

/// <summary>
/// Alpha premultiplication helpers ported from <c>_sao_cy_pixels.pyx</c>.
/// `UpdateLayeredWindow` requires premultiplied BGRA: each colour channel
/// is scaled by alpha/255 in place.
/// </summary>
public static class PremultiplyHelpers
{
    /// <summary>Premultiply BGRA pixels in place, treating <paramref name="pixels"/> as 4-byte BGRA tuples.</summary>
    public static void PremultiplyInPlace(Span<byte> pixels)
    {
        if ((pixels.Length & 3) != 0)
        {
            throw new ArgumentException("BGRA buffer length must be a multiple of 4", nameof(pixels));
        }
        for (var i = 0; i < pixels.Length; i += 4)
        {
            var a = pixels[i + 3];
            if (a == 255) continue;
            if (a == 0)
            {
                pixels[i] = 0;
                pixels[i + 1] = 0;
                pixels[i + 2] = 0;
                continue;
            }
            pixels[i] = (byte)((pixels[i] * a + 127) / 255);
            pixels[i + 1] = (byte)((pixels[i + 1] * a + 127) / 255);
            pixels[i + 2] = (byte)((pixels[i + 2] * a + 127) / 255);
        }
    }

    /// <summary>
    /// True when the buffer is already premultiplied (every BGR channel ≤ A).
    /// Cheap diagnostic for tests/asserts; not a substitute for tracking
    /// content version on mutation.
    /// </summary>
    public static bool IsPremultiplied(ReadOnlySpan<byte> pixels)
    {
        if ((pixels.Length & 3) != 0) return false;
        for (var i = 0; i < pixels.Length; i += 4)
        {
            var a = pixels[i + 3];
            if (pixels[i] > a || pixels[i + 1] > a || pixels[i + 2] > a) return false;
        }
        return true;
    }
}
