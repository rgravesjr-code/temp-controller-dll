using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;

namespace TempSim.Wpf;

/// <summary>A round indicator with a caption.</summary>
public sealed class Lamp : StackPanel
{
    readonly Ellipse _dot = new() { Width = 18, Height = 18, Stroke = Brushes.Gray, StrokeThickness = 1, Margin = new Thickness(0, 2, 0, 2) };
    readonly TextBlock _text = new() { FontSize = 11, TextAlignment = TextAlignment.Center, TextWrapping = TextWrapping.Wrap };
    Brush _on = Brushes.OrangeRed;
    static readonly Brush s_off = new SolidColorBrush(Color.FromRgb(220, 220, 220));

    public Lamp()
    {
        Orientation = Orientation.Vertical;
        HorizontalAlignment = HorizontalAlignment.Center;
        Children.Add(_dot);
        Children.Add(_text);
        IsOn = false;
    }
    public string Caption { get => _text.Text; set => _text.Text = value; }
    public string OnBrush { set => _on = (Brush)new BrushConverter().ConvertFromString(value)!; }
    public bool IsOn { set => _dot.Fill = value ? _on : s_off; }
}
