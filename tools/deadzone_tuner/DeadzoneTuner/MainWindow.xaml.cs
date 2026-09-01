using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;

namespace DeadzoneTuner;

public partial class MainWindow : Window
{
    private bool _dragging;
    private bool _ready;
    private short _rawX;
    private short _rawY;

    public MainWindow()
    {
        InitializeComponent();
        _ready = true;
        UpdateLabels();
        Redraw();
    }

    private DeadzoneShape CurrentShape => AxialRadio?.IsChecked == true ? DeadzoneShape.Axial : DeadzoneShape.Radial;

    private AxisConfig RadialConfig => new()
    {
        Deadzone = (int)RDeadSlider.Value,
        AntiDeadzone = (int)RAntiSlider.Value,
        MaxZone = (int)RMaxSlider.Value,
    };

    private AxisConfig XAxisConfig => new()
    {
        Deadzone = (int)XDeadSlider.Value,
        AntiDeadzone = (int)XAntiSlider.Value,
        MaxZone = (int)XMaxSlider.Value,
    };

    private AxisConfig YAxisConfig => new()
    {
        Deadzone = (int)YDeadSlider.Value,
        AntiDeadzone = (int)YAntiSlider.Value,
        MaxZone = (int)YMaxSlider.Value,
    };

    private void Shape_Changed(object sender, RoutedEventArgs e)
    {
        if (!_ready) return; // Checked fires while InitializeComponent is still wiring up fields
        bool axial = CurrentShape == DeadzoneShape.Axial;
        RadialGroup.IsEnabled = !axial;
        AxialGroupX.IsEnabled = axial;
        AxialGroupY.IsEnabled = axial;
        Redraw();
    }

    private void Param_Changed(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (!_ready) return; // ValueChanged fires while InitializeComponent is still wiring up fields
        UpdateLabels();
        Redraw();
    }

    private void UpdateLabels()
    {
        RDeadLabel.Text = $"Deadzone: {(int)RDeadSlider.Value}";
        RAntiLabel.Text = $"Anti-deadzone: {(int)RAntiSlider.Value}%";
        RMaxLabel.Text = $"Maxzone: {(int)RMaxSlider.Value}%";
        XDeadLabel.Text = $"Deadzone: {(int)XDeadSlider.Value}";
        XAntiLabel.Text = $"Anti-deadzone: {(int)XAntiSlider.Value}%";
        XMaxLabel.Text = $"Maxzone: {(int)XMaxSlider.Value}%";
        YDeadLabel.Text = $"Deadzone: {(int)YDeadSlider.Value}";
        YAntiLabel.Text = $"Anti-deadzone: {(int)YAntiSlider.Value}%";
        YMaxLabel.Text = $"Maxzone: {(int)YMaxSlider.Value}%";
    }

    // --- Stick canvas: drag to set raw input, boundaries + raw/processed dots ---

    private void StickCanvas_SizeChanged(object sender, SizeChangedEventArgs e) => Redraw();
    private void CurveCanvas_SizeChanged(object sender, SizeChangedEventArgs e) => Redraw();

    private void StickCanvas_MouseDown(object sender, MouseButtonEventArgs e)
    {
        _dragging = true;
        StickCanvas.CaptureMouse();
        UpdateRawFromMouse(e.GetPosition(StickCanvas));
    }

    private void StickCanvas_MouseMove(object sender, MouseEventArgs e)
    {
        if (_dragging) UpdateRawFromMouse(e.GetPosition(StickCanvas));
    }

    private void StickCanvas_MouseUp(object sender, MouseButtonEventArgs e)
    {
        _dragging = false;
        StickCanvas.ReleaseMouseCapture();
    }

    private void UpdateRawFromMouse(Point p)
    {
        double w = StickCanvas.ActualWidth, h = StickCanvas.ActualHeight;
        double side = Math.Min(w, h);
        double cx = w / 2, cy = h / 2;
        double half = side / 2 - 8;

        double nx = (p.X - cx) / half;
        double ny = -(p.Y - cy) / half; // screen Y is inverted vs stick Y-up convention

        nx = Math.Clamp(nx, -1.0, 1.0);
        ny = Math.Clamp(ny, -1.0, 1.0);

        _rawX = (short)Math.Round(nx * (nx >= 0 ? 32767 : 32768));
        _rawY = (short)Math.Round(ny * (ny >= 0 ? 32767 : 32768));
        Redraw();
    }

    private static double ValueToOffset(double value, double half) => (value / 32768.0) * half;

    private void Redraw()
    {
        if (!_ready) return;
        DrawStick();
        DrawCurve();
    }

    private void DrawStick()
    {
        StickCanvas.Children.Clear();

        double w = StickCanvas.ActualWidth, h = StickCanvas.ActualHeight;
        if (w <= 0 || h <= 0) return;
        double side = Math.Min(w, h);
        double cx = w / 2, cy = h / 2;
        double half = side / 2 - 8;

        // Full-travel bounding square
        var bounds = new Rectangle
        {
            Width = half * 2, Height = half * 2,
            Stroke = Brushes.DimGray, StrokeThickness = 1,
        };
        Canvas.SetLeft(bounds, cx - half);
        Canvas.SetTop(bounds, cy - half);
        StickCanvas.Children.Add(bounds);

        // Axes
        AddLine(cx - half, cy, cx + half, cy, Brushes.DimGray);
        AddLine(cx, cy - half, cx, cy + half, Brushes.DimGray);

        var shape = CurrentShape;
        if (shape == DeadzoneShape.Radial)
        {
            var radial = RadialConfig;
            double deadR = ValueToOffset(radial.Deadzone, half);
            double maxR = (radial.MaxZone / 100.0) * half;
            AddEllipse(cx, cy, deadR, Brushes.OrangeRed, dashed: false);
            AddEllipse(cx, cy, maxR, Brushes.LimeGreen, dashed: true);
        }
        else
        {
            var xa = XAxisConfig;
            var ya = YAxisConfig;
            double deadX = ValueToOffset(xa.Deadzone, half);
            double deadY = ValueToOffset(ya.Deadzone, half);
            double maxX = (xa.MaxZone / 100.0) * half;
            double maxY = (ya.MaxZone / 100.0) * half;
            AddRect(cx, cy, deadX, deadY, Brushes.OrangeRed, dashed: false);
            AddRect(cx, cy, maxX, maxY, Brushes.LimeGreen, dashed: true);
        }

        // Raw input dot
        double rawPxX = cx + ValueToOffset(_rawX, half);
        double rawPxY = cy - ValueToOffset(_rawY, half);
        AddDot(rawPxX, rawPxY, Brushes.DeepSkyBlue, 8);

        // Processed output dot, via the native (C++) deadzone math
        var (outX, outY) = NativeDeadzone.Apply(_rawX, _rawY, shape, RadialConfig, XAxisConfig, YAxisConfig);
        double outPxX = cx + ValueToOffset(outX, half);
        double outPxY = cy - ValueToOffset(outY, half);
        AddLine(rawPxX, rawPxY, outPxX, outPxY, Brushes.Gray);
        AddDot(outPxX, outPxY, Brushes.Orange, 8);

        RawValueText.Text = $"Raw:  ({_rawX,6}, {_rawY,6})";
        OutValueText.Text = $"Out:  ({outX,6}, {outY,6})";
    }

    private void DrawCurve()
    {
        CurveCanvas.Children.Clear();
        double w = CurveCanvas.ActualWidth, h = CurveCanvas.ActualHeight;
        if (w <= 0 || h <= 0) return;

        double pad = 10;
        var axisX = new Line { X1 = pad, Y1 = h - pad, X2 = w - pad, Y2 = h - pad, Stroke = Brushes.DimGray, StrokeThickness = 1 };
        var axisY = new Line { X1 = pad, Y1 = pad, X2 = pad, Y2 = h - pad, Stroke = Brushes.DimGray, StrokeThickness = 1 };
        CurveCanvas.Children.Add(axisX);
        CurveCanvas.Children.Add(axisY);

        var shape = CurrentShape;
        var radial = RadialConfig;
        var xa = XAxisConfig;
        var ya = YAxisConfig;

        var poly = new Polyline { Stroke = Brushes.Orange, StrokeThickness = 2 };
        const int steps = 200;
        for (int i = 0; i <= steps; i++)
        {
            short input = (short)Math.Round(i / (double)steps * 32767);
            var (outX, _) = NativeDeadzone.Apply(input, 0, shape, radial, xa, ya);
            double px = pad + (w - 2 * pad) * (input / 32767.0);
            double py = (h - pad) - (h - 2 * pad) * (outX / 32767.0);
            poly.Points.Add(new Point(px, py));
        }
        CurveCanvas.Children.Add(poly);

        var label = new System.Windows.Controls.TextBlock
        {
            Text = "Response curve: output vs input magnitude (along the shown axis)",
            Foreground = Brushes.Gray,
        };
        Canvas.SetLeft(label, pad);
        Canvas.SetTop(label, 2);
        CurveCanvas.Children.Add(label);
    }

    private void AddDot(double x, double y, Brush brush, double diameter)
    {
        var e = new Ellipse { Width = diameter, Height = diameter, Fill = brush };
        Canvas.SetLeft(e, x - diameter / 2);
        Canvas.SetTop(e, y - diameter / 2);
        StickCanvas.Children.Add(e);
    }

    private void AddLine(double x1, double y1, double x2, double y2, Brush brush)
    {
        StickCanvas.Children.Add(new Line { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = brush, StrokeThickness = 1 });
    }

    private void AddEllipse(double cx, double cy, double r, Brush brush, bool dashed)
    {
        if (r <= 0) return;
        var e = new Ellipse { Width = r * 2, Height = r * 2, Stroke = brush, StrokeThickness = 1.5 };
        if (dashed) e.StrokeDashArray = new DoubleCollection { 4, 3 };
        Canvas.SetLeft(e, cx - r);
        Canvas.SetTop(e, cy - r);
        StickCanvas.Children.Add(e);
    }

    private void AddRect(double cx, double cy, double halfW, double halfH, Brush brush, bool dashed)
    {
        if (halfW <= 0 || halfH <= 0) return;
        var r = new Rectangle { Width = halfW * 2, Height = halfH * 2, Stroke = brush, StrokeThickness = 1.5 };
        if (dashed) r.StrokeDashArray = new DoubleCollection { 4, 3 };
        Canvas.SetLeft(r, cx - halfW);
        Canvas.SetTop(r, cy - halfH);
        StickCanvas.Children.Add(r);
    }

    private void CopyButton_Click(object sender, RoutedEventArgs e)
    {
        string snippet;
        if (CurrentShape == DeadzoneShape.Radial)
        {
            var r = RadialConfig;
            snippet =
                "static const StickDeadzoneConfig stick_deadzone = {\n" +
                "    .type   = DEADZONE_RADIAL,\n" +
                $"    .radial = {{ .deadzone = {r.Deadzone}, .antideadzone = {r.AntiDeadzone}, .maxzone = {r.MaxZone} }},\n" +
                "};\n";
        }
        else
        {
            var xa = XAxisConfig;
            var ya = YAxisConfig;
            snippet =
                "static const StickDeadzoneConfig stick_deadzone = {\n" +
                "    .type   = DEADZONE_AXIAL,\n" +
                $"    .x_axis = {{ .deadzone = {xa.Deadzone}, .antideadzone = {xa.AntiDeadzone}, .maxzone = {xa.MaxZone} }},\n" +
                $"    .y_axis = {{ .deadzone = {ya.Deadzone}, .antideadzone = {ya.AntiDeadzone}, .maxzone = {ya.MaxZone} }},\n" +
                "};\n";
        }
        Clipboard.SetText(snippet);
    }
}
