using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using SolarNX.Library;
using Point = System.Windows.Point;

namespace SolarNX.UI.Library;

/// <summary>
/// Lets the user pick which 2:3 part of a cover image becomes the Switch tile (CoverImage then
/// renders exactly that region). The frame is kept in source-pixel coordinates and always 2:3, so
/// screen scaling never changes the result; drag moves it, the slider/mouse wheel resize it around
/// its centre, and it's always clamped inside the image. <see cref="Result"/> is normalized (0..1).
/// </summary>
public partial class CoverCropWindow : Wpf.Ui.Controls.FluentWindow
{
    private readonly BitmapImage _image;
    private readonly double _srcW;
    private readonly double _srcH;
    private readonly double _maxFrameH; // tallest 2:3 frame that fits the image

    // Frame in source pixels; width is always _frameH * 2/3.
    private double _frameX, _frameY, _frameH;

    // Current letterbox mapping source → stage.
    private double _scale = 1, _offsetX, _offsetY;

    private Point? _dragStart;
    private double _dragFrameX, _dragFrameY;
    private bool _syncingSlider;

    public CoverCrop? Result { get; private set; }

    public CoverCropWindow(BitmapImage fullSizeImage, CoverCrop? initial)
    {
        InitializeComponent();
        _image = fullSizeImage;
        _srcW = fullSizeImage.PixelWidth;
        _srcH = fullSizeImage.PixelHeight;
        _maxFrameH = Math.Min(_srcH, _srcW * 3 / 2);
        SourceImage.Source = fullSizeImage;

        var start = initial is { IsValid: true } ? initial : CoverLoader.CenterCrop(_srcW, _srcH);
        _frameX = start.X * _srcW;
        _frameY = start.Y * _srcH;
        _frameH = Math.Clamp(start.Height * _srcH, 8, _maxFrameH);
        SetSliderFromFrame();

        Stage.SizeChanged += (_, _) => Redraw();
        Overlay.MouseLeftButtonDown += OnDragStart;
        Overlay.MouseMove += OnDragMove;
        Overlay.MouseLeftButtonUp += (_, _) => { _dragStart = null; Overlay.ReleaseMouseCapture(); };
        Overlay.MouseWheel += (_, e) => SizeSlider.Value = Math.Clamp(SizeSlider.Value + (e.Delta > 0 ? 0.05 : -0.05), SizeSlider.Minimum, SizeSlider.Maximum);
        SizeSlider.ValueChanged += (_, _) => { if (!_syncingSlider) ResizeAroundCentre(SizeSlider.Value * _maxFrameH); };
        CenterButton.Click += (_, _) =>
        {
            var c = CoverLoader.CenterCrop(_srcW, _srcH);
            _frameX = c.X * _srcW;
            _frameY = c.Y * _srcH;
            _frameH = c.Height * _srcH;
            SetSliderFromFrame();
            Redraw();
        };
        CancelButton.Click += (_, _) => { DialogResult = false; };
        OkButton.Click += (_, _) =>
        {
            Result = new CoverCrop
            {
                X = _frameX / _srcW,
                Y = _frameY / _srcH,
                Width = FrameW / _srcW,
                Height = _frameH / _srcH,
            };
            DialogResult = true;
        };

        Redraw();
    }

    private double FrameW => _frameH * 2 / 3;

    private void SetSliderFromFrame()
    {
        _syncingSlider = true;
        SizeSlider.Value = Math.Clamp(_frameH / _maxFrameH, SizeSlider.Minimum, SizeSlider.Maximum);
        _syncingSlider = false;
    }

    private void ResizeAroundCentre(double newHeight)
    {
        var cx = _frameX + FrameW / 2;
        var cy = _frameY + _frameH / 2;
        _frameH = Math.Clamp(newHeight, 8, _maxFrameH);
        _frameX = cx - FrameW / 2;
        _frameY = cy - _frameH / 2;
        Redraw();
    }

    private void OnDragStart(object sender, MouseButtonEventArgs e)
    {
        _dragStart = e.GetPosition(Overlay);
        _dragFrameX = _frameX;
        _dragFrameY = _frameY;
        Overlay.CaptureMouse();
    }

    private void OnDragMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (_dragStart is not { } start || e.LeftButton != MouseButtonState.Pressed) return;
        var p = e.GetPosition(Overlay);
        _frameX = _dragFrameX + (p.X - start.X) / _scale;
        _frameY = _dragFrameY + (p.Y - start.Y) / _scale;
        Redraw();
    }

    private void Redraw()
    {
        // Keep the frame inside the image.
        _frameX = Math.Clamp(_frameX, 0, Math.Max(0, _srcW - FrameW));
        _frameY = Math.Clamp(_frameY, 0, Math.Max(0, _srcH - _frameH));

        var stageW = Stage.ActualWidth;
        var stageH = Stage.ActualHeight;
        if (stageW > 0 && stageH > 0)
        {
            _scale = Math.Min(stageW / _srcW, stageH / _srcH);
            _offsetX = (stageW - _srcW * _scale) / 2;
            _offsetY = (stageH - _srcH * _scale) / 2;

            double left = _offsetX + _frameX * _scale, top = _offsetY + _frameY * _scale;
            double width = FrameW * _scale, height = _frameH * _scale;
            Place(Frame, left, top, width, height);
            Place(ShadeTop, 0, 0, stageW, top);
            Place(ShadeBottom, 0, top + height, stageW, Math.Max(0, stageH - top - height));
            Place(ShadeLeft, 0, top, left, height);
            Place(ShadeRight, left + width, top, Math.Max(0, stageW - left - width), height);
        }

        var px = new Int32Rect((int)_frameX, (int)_frameY,
            Math.Max(1, Math.Min((int)FrameW, (int)_srcW - (int)_frameX)),
            Math.Max(1, Math.Min((int)_frameH, (int)_srcH - (int)_frameY)));
        var preview = new CroppedBitmap(_image, px);
        preview.Freeze();
        PreviewImage.Source = preview;
    }

    private static void Place(FrameworkElement element, double left, double top, double width, double height)
    {
        Canvas.SetLeft(element, left);
        Canvas.SetTop(element, top);
        element.Width = Math.Max(0, width);
        element.Height = Math.Max(0, height);
    }
}
