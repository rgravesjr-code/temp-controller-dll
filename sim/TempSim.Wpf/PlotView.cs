using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace TempSim.Wpf;

/// <summary>Strip chart of the last <see cref="WindowSeconds"/>: sensors, ControlTemp, plant, setpoint, bands, limits, relay lanes.</summary>
public sealed class PlotView : FrameworkElement
{
    public sealed record Sample(double T, double Plant, double Temp1, double Temp2, double Ctrl, double Setpoint,
                                double HiBand, double LoBand, double HiLimit, double LoLimit, bool Heat, bool Cool, int Status);

    readonly List<Sample> _samples = new();
    public double WindowSeconds { get; set; } = 120;
    public int MaxSamples { get; set; } = 6000;

    static readonly Typeface s_font = new("Segoe UI");
    static readonly Pen s_axis = new(Brushes.Gray, 1);
    static readonly Pen s_grid = new(new SolidColorBrush(Color.FromRgb(230, 230, 230)), 1);
    static readonly Pen s_plant = new(new SolidColorBrush(Color.FromRgb(160, 160, 160)), 1) { DashStyle = DashStyles.Dash };
    static readonly Pen s_t1 = new(new SolidColorBrush(Color.FromRgb(230, 120, 0)), 1.2);
    static readonly Pen s_t2 = new(new SolidColorBrush(Color.FromRgb(0, 110, 220)), 1.2);
    static readonly Pen s_ctrl = new(Brushes.Black, 2);
    static readonly Pen s_sp = new(new SolidColorBrush(Color.FromRgb(0, 150, 0)), 1.5);
    static readonly Pen s_band = new(new SolidColorBrush(Color.FromRgb(0, 150, 0)), 1) { DashStyle = DashStyles.Dot };
    static readonly Pen s_limit = new(Brushes.Red, 1) { DashStyle = DashStyles.Dash };
    static readonly Brush s_heat = new SolidColorBrush(Color.FromArgb(120, 230, 60, 30));
    static readonly Brush s_cool = new SolidColorBrush(Color.FromArgb(120, 40, 120, 240));
    static readonly Brush s_stopped = new SolidColorBrush(Color.FromArgb(40, 255, 0, 0));
    static readonly Brush s_degraded = new SolidColorBrush(Color.FromArgb(40, 255, 180, 0));

    static PlotView()
    {
        foreach (var p in new[] { s_axis, s_grid, s_plant, s_t1, s_t2, s_ctrl, s_sp, s_band, s_limit }) p.Freeze();
        foreach (var b in new[] { s_heat, s_cool, s_stopped, s_degraded }) b.Freeze();
    }

    public void Add(Sample s)
    {
        _samples.Add(s);
        if (_samples.Count > MaxSamples) _samples.RemoveRange(0, _samples.Count - MaxSamples);
        InvalidateVisual();
    }
    public void Clear() { _samples.Clear(); InvalidateVisual(); }

    protected override void OnRender(DrawingContext dc)
    {
        double W = ActualWidth, H = ActualHeight;
        dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, W, H));
        if (W < 50 || H < 50) return;
        const double left = 44, right = 8, top = 8, laneH = 14, bottom = 22;
        double plotH = H - top - bottom - 2 * laneH - 4;
        double plotW = W - left - right;
        if (_samples.Count == 0) { dc.DrawText(Text("no data"), new Point(left + 4, top + 4)); return; }

        var last = _samples[^1];
        double tEnd = Math.Max(last.T, WindowSeconds), tStart = tEnd - WindowSeconds;
        int first = _samples.FindIndex(s => s.T >= tStart);
        if (first < 0) first = 0;
        var view = _samples.GetRange(first, _samples.Count - first);

        // y range: limits with margin, widened by data
        double yMin = double.PositiveInfinity, yMax = double.NegativeInfinity;
        void Acc(double v) { if (!double.IsNaN(v) && !double.IsInfinity(v)) { yMin = Math.Min(yMin, v); yMax = Math.Max(yMax, v); } }
        Acc(last.LoLimit); Acc(last.HiLimit);
        foreach (var s in view) { Acc(s.Plant); Acc(s.Temp1); Acc(s.Temp2); Acc(s.Ctrl); }
        if (double.IsInfinity(yMin)) { yMin = 0; yMax = 100; }
        double pad = Math.Max(2, (yMax - yMin) * 0.08); yMin -= pad; yMax += pad;

        double X(double t) => left + (t - tStart) / WindowSeconds * plotW;
        double Y(double v) => top + (yMax - v) / (yMax - yMin) * plotH;

        // status shading (stopped / degraded), relay lanes
        double lane1 = top + plotH + 4, lane2 = lane1 + laneH + 2;
        for (int i = 1; i < view.Count; i++)
        {
            var a = view[i - 1]; var b = view[i];
            double x0 = X(a.T), x1 = Math.Max(X(b.T), x0 + 0.5);
            if (b.Status == 6) dc.DrawRectangle(s_stopped, null, new Rect(x0, top, x1 - x0, plotH));
            else if (b.Status == 7) dc.DrawRectangle(s_degraded, null, new Rect(x0, top, x1 - x0, plotH));
            if (b.Heat) dc.DrawRectangle(s_heat, null, new Rect(x0, lane1, x1 - x0, laneH));
            if (b.Cool) dc.DrawRectangle(s_cool, null, new Rect(x0, lane2, x1 - x0, laneH));
        }
        dc.DrawText(Text("heat"), new Point(4, lane1));
        dc.DrawText(Text("cool"), new Point(4, lane2));

        // grid + axes
        double step = NiceStep((yMax - yMin) / 6);
        for (double v = Math.Ceiling(yMin / step) * step; v <= yMax; v += step)
        {
            double y = Y(v);
            dc.DrawLine(s_grid, new Point(left, y), new Point(left + plotW, y));
            var ft = Text(v.ToString("0", CultureInfo.InvariantCulture));
            dc.DrawText(ft, new Point(left - ft.Width - 4, y - ft.Height / 2));
        }
        double tStep = NiceStep(WindowSeconds / 8);
        for (double t = Math.Ceiling(tStart / tStep) * tStep; t <= tEnd; t += tStep)
        {
            double x = X(t);
            dc.DrawLine(s_grid, new Point(x, top), new Point(x, top + plotH));
            var ft = Text(t.ToString("0", CultureInfo.InvariantCulture) + " s");
            dc.DrawText(ft, new Point(x - ft.Width / 2, H - bottom + 4));
        }
        dc.DrawRectangle(null, s_axis, new Rect(left, top, plotW, plotH));

        // reference lines
        void HLine(double v, Pen pen, string label)
        {
            if (double.IsNaN(v) || v < yMin || v > yMax) return;
            double y = Y(v);
            dc.DrawLine(pen, new Point(left, y), new Point(left + plotW, y));
            var ft = Text(label, pen.Brush);
            dc.DrawText(ft, new Point(left + plotW - ft.Width - 2, y - ft.Height));
        }
        HLine(last.HiLimit, s_limit, "HiLimit"); HLine(last.LoLimit, s_limit, "LoLimit");
        HLine(last.HiBand, s_band, "HiBand"); HLine(last.LoBand, s_band, "LoBand");
        HLine(last.Setpoint, s_sp, "Setpoint");

        // series
        dc.PushClip(new RectangleGeometry(new Rect(left, top, plotW, plotH)));
        Series(dc, view, s => s.Plant, s_plant, X, Y);
        Series(dc, view, s => s.Temp1, s_t1, X, Y);
        Series(dc, view, s => s.Temp2, s_t2, X, Y);
        Series(dc, view, s => s.Ctrl, s_ctrl, X, Y);
        dc.Pop();

        // legend
        double lx = left + 6, ly = top + 4;
        foreach (var (name, pen) in new[] { ("plant", s_plant), ("Temp1", s_t1), ("Temp2", s_t2), ("ControlTemp", s_ctrl) })
        {
            dc.DrawLine(pen, new Point(lx, ly + 7), new Point(lx + 18, ly + 7));
            var ft = Text(name);
            dc.DrawText(ft, new Point(lx + 22, ly));
            lx += 30 + ft.Width;
        }
    }

    static void Series(DrawingContext dc, List<Sample> view, Func<Sample, double> f, Pen pen, Func<double, double> X, Func<double, double> Y)
    {
        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            bool open = false;
            foreach (var s in view)
            {
                double v = f(s);
                if (double.IsNaN(v) || double.IsInfinity(v)) { open = false; continue; }
                var p = new Point(X(s.T), Y(v));
                if (!open) { ctx.BeginFigure(p, false, false); open = true; }
                else ctx.LineTo(p, true, false);
            }
        }
        geo.Freeze();
        dc.DrawGeometry(null, pen, geo);
    }

    static double NiceStep(double raw)
    {
        double p = Math.Pow(10, Math.Floor(Math.Log10(Math.Max(raw, 1e-9))));
        double m = raw / p;
        return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
    }

    static FormattedText Text(string s, Brush? brush = null) =>
        new(s, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, s_font, 11, brush ?? Brushes.DimGray, 1.0);
}
