using SaoAuto.Core.Vision;

namespace SaoAuto.Tests.Vision;

/// <summary>
/// S77 — HSV-aware bar detector. Verifies parity with
/// <c>recognition._detect_bar_pct</c> for the deterministic
/// shortcuts (full bar / empty bar / size guards) plus the
/// pure-math primitives (BGR→HSV, mean blur, percentile).
/// </summary>
public class Session77BarHsvDetectorTests
{
    // Gold band config (#FFAE35 → OpenCV HSV ≈ H=21 S=204 V=255).
    private static readonly BarColorConfig Gold = new(HMin: 14, HMax: 28, SMin: 120);

    private static byte[] MakeBgr(int width, int height, Func<int, int, (byte b, byte g, byte r)> px)
    {
        var buf = new byte[width * height * 3];
        for (var y = 0; y < height; y++)
        for (var x = 0; x < width; x++)
        {
            var (b, g, r) = px(x, y);
            var i = (y * width + x) * 3;
            buf[i] = b; buf[i + 1] = g; buf[i + 2] = r;
        }
        return buf;
    }

    // ── BGR→HSV ─────────────────────────────────────────────────────

    [Fact]
    public void BgrToHsv_RedGreenBlueWhiteBlack()
    {
        var pixels = new byte[]
        {
            0,   0, 255,    // red   → H=0   S=255 V=255
            0, 255, 0,      // green → H=120 (OpenCV/2 = 60) S=255 V=255
            255, 0, 0,      // blue  → H=240 (OpenCV/2 = 120) S=255 V=255
            255, 255, 255,  // white → H=0 S=0   V=255
            0, 0, 0,        // black → H=0 S=0   V=0
            53, 174, 255,   // gold  → H≈21 S≈204 V=255
        };
        BarHsvDetector.BgrToHsvInPlace(pixels, 6);
        Assert.Equal(0, pixels[0]); Assert.Equal(255, pixels[1]); Assert.Equal(255, pixels[2]);
        Assert.Equal(60, pixels[3]); Assert.Equal(255, pixels[4]); Assert.Equal(255, pixels[5]);
        Assert.Equal(120, pixels[6]); Assert.Equal(255, pixels[7]); Assert.Equal(255, pixels[8]);
        Assert.Equal(0, pixels[9]); Assert.Equal(0, pixels[10]); Assert.Equal(255, pixels[11]);
        Assert.Equal(0, pixels[12]); Assert.Equal(0, pixels[13]); Assert.Equal(0, pixels[14]);
        Assert.InRange(pixels[15], 16, 22);    // gold hue ≈ 18 (OpenCV/2)
        Assert.InRange(pixels[16], 195, 215);  // sat ≈ 204
        Assert.Equal(255, pixels[17]);
    }

    // ── Percentile ──────────────────────────────────────────────────

    [Fact]
    public void Percentile_LinearInterpolation()
    {
        var v = new double[] { 1.0, 2.0, 3.0, 4.0, 5.0 };
        Assert.Equal(1.0, BarHsvDetector.Percentile(v, 0));
        Assert.Equal(5.0, BarHsvDetector.Percentile(v, 100));
        Assert.Equal(3.0, BarHsvDetector.Percentile(v, 50));
        Assert.Equal(0.0, BarHsvDetector.Percentile(Array.Empty<double>(), 50));
        Assert.Equal(7.0, BarHsvDetector.Percentile(new[] { 7.0 }, 42));
    }

    // ── Mean blur basic ─────────────────────────────────────────────

    [Fact]
    public void MeanBlur_PreservesFlatRegion()
    {
        var w = 10; var h = 4;
        var src = new byte[w * h * 3];
        for (var i = 0; i < src.Length; i += 3) { src[i] = 100; src[i + 1] = 150; src[i + 2] = 200; }
        var blurred = BarHsvDetector.MeanBlur3x3Bgr(src, w, h);
        Assert.Equal(100, blurred[3 * 3]);   // interior pixel
        Assert.Equal(150, blurred[3 * 3 + 1]);
        Assert.Equal(200, blurred[3 * 3 + 2]);
    }

    // ── Detector size guards ────────────────────────────────────────

    [Fact]
    public void Detect_TinyImage_ReturnsEmpty()
    {
        var buf = new byte[2 * 1 * 3];
        Assert.Equal(BarHsvDetector.Result.Empty, BarHsvDetector.Detect(buf, 2, 1, Gold));
    }

    [Fact]
    public void Detect_ShortBuffer_ReturnsEmpty()
    {
        var buf = new byte[10];
        Assert.Equal(BarHsvDetector.Result.Empty, BarHsvDetector.Detect(buf, 100, 10, Gold));
    }

    // ── Full bar shortcut ───────────────────────────────────────────

    [Fact]
    public void Detect_AllGold_ReturnsFull()
    {
        var w = 120; var h = 12;
        var buf = MakeBgr(w, h, (x, y) => ((byte)53, (byte)174, (byte)255));
        var r = BarHsvDetector.Detect(buf, w, h, Gold);
        Assert.True(r.Pct >= 0.99, $"pct={r.Pct}");
        Assert.True(r.Confidence > 0.0);
    }

    // ── Empty bar (no hue match) ────────────────────────────────────

    [Fact]
    public void Detect_AllBlack_ReturnsEmpty()
    {
        var w = 100; var h = 10;
        var buf = MakeBgr(w, h, (x, y) => ((byte)0, (byte)0, (byte)0));
        var r = BarHsvDetector.Detect(buf, w, h, Gold);
        Assert.Equal(0.0, r.Pct);
        Assert.Equal(0.0, r.Confidence);
    }

    [Fact]
    public void Detect_AllBlue_ReturnsEmpty()
    {
        var w = 100; var h = 10;
        // Blue: H=120 — well outside gold band [14,28] → hue gate trips.
        var buf = MakeBgr(w, h, (x, y) => ((byte)200, (byte)40, (byte)40));
        var r = BarHsvDetector.Detect(buf, w, h, Gold);
        Assert.Equal(0.0, r.Pct);
    }

    // ── Partial fill (gold left, dark right) ────────────────────────

    [Fact]
    public void Detect_HalfFilledGold_ReturnsHalfish()
    {
        var w = 120; var h = 12;
        var buf = MakeBgr(w, h, (x, y) => x < 60
            ? ((byte)53, (byte)174, (byte)255)
            : ((byte)20, (byte)20, (byte)20));
        var r = BarHsvDetector.Detect(buf, w, h, Gold);
        // Allow generous tolerance — three estimates + smoothing introduce
        // a few percent of slop at the boundary.
        Assert.InRange(r.Pct, 0.40, 0.60);
        Assert.True(r.Confidence > 0.0);
    }

    [Fact]
    public void Detect_QuarterFilledGold_ReturnsQuarterish()
    {
        var w = 200; var h = 14;
        var buf = MakeBgr(w, h, (x, y) => x < 50
            ? ((byte)53, (byte)174, (byte)255)
            : ((byte)20, (byte)20, (byte)20));
        var r = BarHsvDetector.Detect(buf, w, h, Gold);
        Assert.InRange(r.Pct, 0.18, 0.32);
    }
}
