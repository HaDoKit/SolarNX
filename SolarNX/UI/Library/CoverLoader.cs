using System.Windows;
using System.Windows.Media.Imaging;
using SolarNX.Library;

namespace SolarNX.UI.Library;

/// <summary>Loads a cover for display without keeping the file locked (OnLoad + no image cache), so
/// GameLibrary can replace/delete cover files while the UI shows them.</summary>
internal static class CoverLoader
{
    /// <param name="decodeWidth">Decode-time width cap (0 = full size - the crop dialog needs real
    /// pixel dimensions).</param>
    public static BitmapImage? Load(string? path, int decodeWidth = 240)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
        try
        {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.CreateOptions = BitmapCreateOptions.IgnoreImageCache;
            if (decodeWidth > 0) image.DecodePixelWidth = decodeWidth;
            image.UriSource = new Uri(path);
            image.EndInit();
            image.Freeze();
            return image;
        }
        catch
        {
            return null; // unreadable image - shown as "no cover"
        }
    }

    /// <summary>What the Switch tile will show: the chosen crop region, or the same centre 2:3 crop
    /// CoverImage applies when none was chosen - so the preview never differs from the console.</summary>
    public static BitmapSource? LoadAsTile(string? path, CoverCrop? crop, int decodeWidth = 480)
    {
        var image = Load(path, decodeWidth);
        if (image is null) return null;

        var rect = crop is { IsValid: true } ? crop : CenterCrop(image.PixelWidth, image.PixelHeight);
        var px = new Int32Rect(
            (int)Math.Round(rect.X * image.PixelWidth),
            (int)Math.Round(rect.Y * image.PixelHeight),
            Math.Max(1, (int)Math.Round(rect.Width * image.PixelWidth)),
            Math.Max(1, (int)Math.Round(rect.Height * image.PixelHeight)));
        px.Width = Math.Min(px.Width, image.PixelWidth - px.X);
        px.Height = Math.Min(px.Height, image.PixelHeight - px.Y);
        if (px.Width <= 0 || px.Height <= 0) return image;

        var cropped = new CroppedBitmap(image, px);
        cropped.Freeze();
        return cropped;
    }

    /// <summary>Largest 2:3 rectangle centred in a width×height image, normalized.</summary>
    public static CoverCrop CenterCrop(double width, double height)
    {
        double cropW = width, cropH = width * 3 / 2;
        if (cropH > height) { cropH = height; cropW = height * 2 / 3; }
        return new CoverCrop
        {
            X = (width - cropW) / 2 / width,
            Y = (height - cropH) / 2 / height,
            Width = cropW / width,
            Height = cropH / height,
        };
    }
}
