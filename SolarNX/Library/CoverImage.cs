using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

namespace SolarNX.Library;

/// <summary>
/// Normalizes a user-supplied cover (any size/format System.Drawing can read) to the exact tile the
/// Switch draws: 240×360 (2:3, the shape of Steam's library capsules) - the region the user picked in
/// the editor's crop dialog (GameEntry.CoverCrop), or a centre crop when none was picked - JPEG
/// q85 - typically 20-40KB, so even a few hundred games stay a small transfer and fit comfortably in
/// the client's memory. Doing it here keeps the Switch side to a plain "decode JPEG, draw texture".
/// </summary>
internal static class CoverImage
{
    public const int Width = 240;
    public const int Height = 360;

    // Matches the client's LIBRARY_MAX_COVER_BYTES - anything bigger would be rejected there.
    public const int MaxBytes = 512 * 1024;

    /// <summary>Encoded JPEG, or null if the file is missing/unreadable (the entry is then sent
    /// without a cover rather than failing the whole library).</summary>
    public static byte[]? ToSwitchJpeg(string? path, CoverCrop? crop = null)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
        try
        {
            using var source = Image.FromFile(path);
            using var target = new Bitmap(Width, Height);
            using (var g = Graphics.FromImage(target))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.CompositingQuality = CompositingQuality.HighQuality;

                if (crop is { IsValid: true })
                {
                    // The user's chosen region, stretched into the tile (it's already 2:3).
                    var src = new RectangleF(
                        (float)(crop.X * source.Width), (float)(crop.Y * source.Height),
                        (float)(crop.Width * source.Width), (float)(crop.Height * source.Height));
                    g.DrawImage(source, new Rectangle(0, 0, Width, Height), src, GraphicsUnit.Pixel);
                }
                else
                {
                    // Cover-fill: scale so the image covers the whole tile, crop the overflow evenly.
                    var scale = Math.Max((double)Width / source.Width, (double)Height / source.Height);
                    var drawWidth = (int)Math.Ceiling(source.Width * scale);
                    var drawHeight = (int)Math.Ceiling(source.Height * scale);
                    g.DrawImage(source, (Width - drawWidth) / 2, (Height - drawHeight) / 2, drawWidth, drawHeight);
                }
            }

            var jpegCodec = ImageCodecInfo.GetImageEncoders().First(c => c.FormatID == ImageFormat.Jpeg.Guid);
            using var parameters = new EncoderParameters(1);
            parameters.Param[0] = new EncoderParameter(Encoder.Quality, 85L);
            using var stream = new MemoryStream();
            target.Save(stream, jpegCodec, parameters);
            var bytes = stream.ToArray();
            return bytes.Length <= MaxBytes ? bytes : null;
        }
        catch (Exception ex)
        {
            Logger.Log("CoverImage", $"couldn't convert cover {path}: {ex.Message}");
            return null;
        }
    }
}
