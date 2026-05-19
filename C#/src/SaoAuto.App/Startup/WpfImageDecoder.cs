using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using SaoAuto.Core.Automation.HideSeek;

namespace SaoAuto.App.Startup;

/// <summary>
/// S153 — Thin WPF-backed PNG/BMP decoder for HideSeek templates.
/// Lives in the App project (not Core) because it depends on
/// PresentationCore. Returns null on missing file or decode failure so
/// <see cref="HideSeekTemplates.LoadAll"/> can record the entry as a
/// missing template without aborting load.
/// </summary>
public static class WpfImageDecoder
{
    public static HideSeekTemplates.DecodedImage? Decode(string path)
    {
        if (!File.Exists(path)) return null;
        try
        {
            using var fs = File.OpenRead(path);
            var src = BitmapFrame.Create(fs, BitmapCreateOptions.IgnoreColorProfile, BitmapCacheOption.OnLoad);
            BitmapSource fmt = src.Format == PixelFormats.Bgra32
                ? src
                : new FormatConvertedBitmap(src, PixelFormats.Bgra32, null, 0);
            var w = fmt.PixelWidth;
            var h = fmt.PixelHeight;
            var stride = w * 4;
            var buf = new byte[stride * h];
            fmt.CopyPixels(buf, stride, 0);
            return new HideSeekTemplates.DecodedImage(buf, w, h, stride, 4);
        }
        catch
        {
            return null;
        }
    }
}
