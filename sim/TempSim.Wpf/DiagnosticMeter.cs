using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace TempSim.Wpf;

/// <summary>Explicit state and numbers remain readable even when no timer is active.</summary>
public sealed class DiagnosticMeter : StackPanel
{
    readonly TextBlock _title = new() { FontWeight = FontWeights.SemiBold, FontSize = 12 };
    readonly TextBlock _state = new() { FontSize = 11, Margin = new Thickness(0, 2, 0, 3) };
    readonly Border _fill = new() { HorizontalAlignment = HorizontalAlignment.Left, CornerRadius = new CornerRadius(2) };
    readonly Grid _track = new() { Height = 5, Background = new SolidColorBrush(Color.FromRgb(31, 61, 83)) };
    double _fraction;
    public string Caption { get => _title.Text; set => _title.Text = value; }
    public DiagnosticMeter()
    {
        Margin = new Thickness(3, 5, 3, 5);
        Children.Add(_title); Children.Add(_state); Children.Add(_track);
        _track.Children.Add(_fill);
        _track.SizeChanged += (_, _) => _fill.Width = _track.ActualWidth * _fraction;
    }
    public void Update(double max, double value, bool enabled, bool frozen, bool accumulator, Brush active)
    {
        max = double.IsFinite(max) ? Math.Max(0, max) : 0;
        value = double.IsFinite(value) ? Math.Max(0, value) : 0;
        _fraction = enabled && max > 0 ? Math.Clamp(value / max, 0, 1) : 0;
        var color = !enabled ? Brushes.LightSlateGray : frozen ? Brushes.Salmon : value > 0 ? active : Brushes.MediumAquamarine;
        _fill.Background = color;
        _fill.Width = _track.ActualWidth * _fraction;
        _state.Foreground = color;
        _state.Text = !enabled ? "Disabled"
            : frozen ? $"Fault snapshot · {value:0} / {max:0} ms"
            : accumulator ? $"{(value > 0 ? "Accumulating / recovering" : "Clear")} · {value:0} / {max:0} ms"
            : value > 0 ? $"Timing · {value:0} / {max:0} ms remaining"
            : $"Idle · not timing (delay {max:0} ms)";
        ToolTip = accumulator ? "Fills with invalid sensor readings; drains as readings recover. A full bar reaches the failure threshold."
            : "Shows time remaining. The colored bar drains while timing; idle means no countdown is currently active.";
    }
}
