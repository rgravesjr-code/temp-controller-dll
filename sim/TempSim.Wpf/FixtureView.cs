using System.Globalization;
using System.Windows;
using System.Windows.Media;
using TempSim.Core;

namespace TempSim.Wpf;

/// <summary>Live schematic. All movement is driven by simulation state/time, so pause also freezes the illustration.</summary>
public sealed class FixtureView : FrameworkElement
{
    Simulation? _sim;
    public void Update(Simulation sim) { _sim = sim; InvalidateVisual(); }
    static Brush B(string color) => (Brush)new BrushConverter().ConvertFromString(color)!;
    static readonly Brush Ink = B("#DCF1FF"), Muted = B("#89A8C3"), Cyan = B("#40DFFF"), Orange = B("#FFAB66"), Panel = B("#102B43"), Line = B("#315675");
    static readonly Typeface Font = new("Segoe UI");

    protected override void OnRender(DrawingContext d)
    {
        if (_sim == null || ActualWidth < 1 || ActualHeight < 1) return;
        var s = _sim; var f = s.Fixture; var c = s.Config.Fixture;
        double scale = Math.Min(ActualWidth / 920, ActualHeight / 330);
        d.PushTransform(new TranslateTransform((ActualWidth - 920 * scale) / 2, (ActualHeight - 330 * scale) / 2));
        d.PushTransform(new ScaleTransform(scale, scale));
        void Text(string text, double x, double y, double size = 12, Brush? brush = null) =>
            d.DrawText(new FormattedText(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Font, size, brush ?? Ink, 1), new Point(x, y));
        void Card(double x, double y, double w, double h, Brush? outline = null) => d.DrawRoundedRectangle(Panel, new Pen(outline ?? Line, 1.5), new Rect(x, y, w, h), 10, 10);
        void Rotor(double x, double y, double radius, double angle, bool active)
        {
            d.DrawEllipse(null, new Pen(active ? Cyan : Line, 2), new Point(x, y), radius, radius);
            d.PushTransform(new RotateTransform(angle, x, y));
            for (int i = 0; i < 4; i++)
            {
                d.PushTransform(new RotateTransform(i * 90, x, y));
                d.DrawRoundedRectangle(active ? Cyan : Muted, null, new Rect(x - 4, y - radius + 7, 8, radius - 7), 4, 4);
                d.Pop();
            }
            d.Pop();
            d.DrawEllipse(Ink, null, new Point(x, y), 5, 5);
        }
        Text("OIL CIRCUIT  /  LIVE FIXTURE", 18, 9, 14, Cyan);
        Text(c.Enabled ? "Estimated physics · illustrative rotation" : "Legacy scenario plant · fixture physics inactive", 575, 12, 11, Muted);

        // Clockwise closed circuit: pump -> inline heater -> UUT -> return.
        var route = new[] { new Point(100, 242), new Point(100, 122), new Point(420, 122), new Point(420, 205), new Point(635, 205), new Point(635, 280), new Point(100, 280), new Point(100, 242) };
        for (int i = 1; i < route.Length; i++) d.DrawLine(new Pen(Line, 10), route[i - 1], route[i]);
        bool flowing = c.Enabled && f.FlowLpm > 0.05;
        if (flowing)
        {
            double length = 0;
            for (int i = 1; i < route.Length; i++) length += (route[i] - route[i - 1]).Length;
            for (int j = 0; j < 38; j++)
            {
                double travel = ((j / 38.0 + f.FlowPhase) % 1) * length;
                for (int i = 1; i < route.Length; i++)
                {
                    var delta = route[i] - route[i - 1]; double len = delta.Length;
                    if (travel <= len) { d.DrawEllipse(Cyan, null, route[i - 1] + delta * (travel / len), 3, 3); break; }
                    travel -= len;
                }
            }
        }
        Text("SUPPLY →", 104, 96, 10, Muted);
        Card(190, 87, 170, 70, s.HeaterOn ? Orange : null);
        Text("INLINE OIL HEATER", 204, 98, 12, s.HeaterOn ? Orange : Muted);
        Text($"{(s.HeaterOn ? "ON" : "OFF")}  ·  {f.HeaterHeatW / 1000:0.00} kW delivered", 204, 127, 11);
        Card(56, 204, 130, 59, flowing ? Cyan : null);
        Rotor(83, 233, 19, f.PumpAngle, flowing);
        Text("OIL PUMP", 110, 213, 11);
        Text($"{f.FlowLpm:0.0} L/min", 110, 237, 11, Cyan);
        Text("← RETURN", 285, 291, 11, Muted);

        Card(444, 142, 174, 99, flowing ? Cyan : null);
        Text("UNIT UNDER TEST", 460, 155, 14);
        string unit = s.Config.Controller.TempUnits == 0 ? "°F" : "°C";
        Text($"{(c.Enabled ? f.OutletTemperature : s.Plant.Temperature):0.0} {unit}", 466, 177, 29, Cyan);
        d.DrawLine(new Pen(Line, 5), new Point(454, 219), new Point(606, 219));
        if (flowing)
            for (int i = 0; i < 7; i++) d.DrawEllipse(Cyan, null, new Point(454 + ((i / 7.0 + f.FlowPhase) % 1) * 152, 219), 2.5, 2.5);
        Text(c.Enabled ? $"ΔT out − in: {f.DeltaTemperature:+0.00;-0.00;0.00} {unit}" : "Legacy plant temperature", 454, 244, 11, Muted);
        if (c.Enabled)
        {
            Card(224, 165, 173, 61, Brushes.Aquamarine);
            Text("UUT INLET", 236, 173, 11, Brushes.Aquamarine);
            Text($"{f.InletTemperature:0.00} {unit}", 236, 191, 19, Brushes.Aquamarine);
            d.DrawLine(new Pen(Brushes.Aquamarine, 1), new Point(397, 205), new Point(420, 205));
            Card(657, 53, 173, 61, Brushes.Orchid);
            Text("UUT OUTLET", 669, 61, 11, Brushes.Orchid);
            Text($"{f.OutletTemperature:0.00} {unit}", 669, 79, 19, Brushes.Orchid);
        }
        for (int i = 0; i < c.MotorCount; i++)
        {
            double x = 422 + i * 82;
            d.DrawLine(new Pen(Line, 3), new Point(x + 29, 100), new Point(x + 29, 140));
            Card(x, 41, 66, 62, c.Enabled && f.MotorRpm > 1 ? Cyan : null);
            Text($"MOTOR M{i + 1}", x + 5, 46, 10, Ink);
            // Finned motor housing and exposed rotating shaft; distinct from fan blades.
            d.DrawRoundedRectangle(Line, new Pen(Muted, 1), new Rect(x + 8, 64, 33, 24), 3, 3);
            for (int fin = 0; fin < 4; fin++) d.DrawLine(new Pen(Muted, 1), new Point(x + 12 + fin * 7, 66), new Point(x + 12 + fin * 7, 86));
            d.DrawLine(new Pen(Muted, 3), new Point(x + 41, 76), new Point(x + 60, 76));
            d.DrawEllipse(Panel, new Pen(Cyan, 1), new Point(x + 51, 76), 8, 8);
            d.PushTransform(new RotateTransform(f.MotorAngle, x + 51, 76));
            d.DrawLine(new Pen(Cyan, 2), new Point(x + 51, 76), new Point(x + 56, 76));
            d.Pop();
            Text($"{f.MotorRpm:0} rpm", x + 7, 106, 10, Muted);
        }
        Card(750, 133, 145, 129, f.FanRpm > 1 ? Cyan : null);
        Rotor(822, 185, 32, f.FanAngle, c.Enabled && f.FanRpm > 1);
        Text("EXTERNAL FAN", 770, 144, 11);
        Text($"{f.FanRpm:0} rpm", 787, 225, 14, Cyan);
        Text(c.FanAutomatic ? "Controller cooler relay" : "Manual fan command", 754, 270, 11, Muted);
        for (int j = 0; j < 3; j++)
        {
            double y = 174 + j * 23;
            double offset = c.Enabled && f.FanRpm > 1 ? f.FanAngle / 360 * 22 : 0;
            d.DrawLine(new Pen(f.FanRpm > 1 ? Cyan : Line, 2), new Point(737 - offset, y), new Point(677 - offset, y));
            d.DrawLine(new Pen(f.FanRpm > 1 ? Cyan : Line, 2), new Point(677 - offset, y), new Point(684 - offset, y - 4));
        }
        Text($"Outlet probes  S1 {s.Temp1Raw:0.00}   S2 {s.Temp2Raw:0.00}", 410, 300, 12, Orange);
        Text($"Actuation heat {f.MotorHeatW:0} W  ·  Net loss {f.CoolingW:0} W", 18, 313, 11, Muted);
        d.Pop(); d.Pop();
    }
}
