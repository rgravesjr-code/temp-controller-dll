using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using TempSim.Core;

namespace TempSim.Wpf;

public sealed class FixtureConfigWindow : Window
{
    public FixtureConfig Result { get; }
    readonly List<Action> _read = new();
    public FixtureConfigWindow(FixtureConfig original)
    {
        Result = new SimConfig { Fixture = original }.Clone().Fixture;
        Title = "Fixture configuration · estimated physics"; Width = 640; Height = 790;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = (System.Windows.Media.Brush)new System.Windows.Media.BrushConverter().ConvertFromString("#0E2438")!;
        Foreground = System.Windows.Media.Brushes.AliceBlue;
        var root = new DockPanel { Margin = new Thickness(20) }; Content = root;
        var bottom = new StackPanel(); DockPanel.SetDock(bottom, Dock.Bottom); root.Children.Add(bottom);
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Foreground = System.Windows.Media.Brushes.Salmon, Margin = new Thickness(0, 8, 0, 8) };
        bottom.Children.Add(error);
        var apply = new Button { Content = "Apply physics", HorizontalAlignment = HorizontalAlignment.Right, Padding = new Thickness(24, 8, 24, 8) };
        bottom.Children.Add(apply);
        apply.Click += (_, _) => { try { foreach (var read in _read) read(); Result.Validate(); DialogResult = true; } catch (Exception ex) { error.Text = ex.Message; } };
        var panel = new StackPanel(); root.Children.Add(new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        panel.Children.Add(new TextBlock { Text = "STARTING ESTIMATES", FontSize = 22 });
        panel.Children.Add(new TextBlock { Text = "Separate supply oil (inlet) and UUT oil + fixture (outlet). Actuation heats the UUT; circulation carries that heat back into the loop. Both controller probes measure the outlet. Changes apply together; temperatures are preserved.", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 8, 0, 16) });
        void Heading(string text) => panel.Children.Add(new TextBlock { Text = text, FontSize = 16, Margin = new Thickness(0, 14, 0, 6) });
        void Bool(string label, string name)
        {
            var p = typeof(FixtureConfig).GetProperty(name)!;
            var box = new CheckBox { Content = label, IsChecked = (bool)p.GetValue(Result)! };
            panel.Children.Add(box); _read.Add(() => p.SetValue(Result, box.IsChecked == true));
        }
        void Number(string label, string name)
        {
            var p = typeof(FixtureConfig).GetProperty(name)!;
            var row = new DockPanel { Margin = new Thickness(0, 2, 8, 2) };
            var box = new TextBox { Text = Convert.ToString(p.GetValue(Result), CultureInfo.InvariantCulture), Width = 100, TextAlignment = TextAlignment.Right };
            DockPanel.SetDock(box, Dock.Right); row.Children.Add(box); row.Children.Add(new Label { Content = label }); panel.Children.Add(row);
            _read.Add(() =>
            {
                if (!double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out double v) || !double.IsFinite(v)) throw new ArgumentException($"{label}: enter a finite number.");
                if (p.PropertyType == typeof(int)) { if (v != Math.Truncate(v) || v < 1 || v > 3) throw new ArgumentException("Motor count must be 1, 2 or 3."); p.SetValue(Result, (int)v); }
                else p.SetValue(Result, v);
            });
        }
        Bool("Use fixture physics (off = legacy scenario plant)", nameof(FixtureConfig.Enabled));
        Heading("Motors / unit under test");
        Bool("Motors running", nameof(FixtureConfig.MotorsRunning));
        Number("Motor count (1–3)", nameof(FixtureConfig.MotorCount));
        Number("Command speed · rpm", nameof(FixtureConfig.MotorRpm));
        Number("Rated speed · rpm", nameof(FixtureConfig.MotorRatedRpm));
        Number("Rated power per motor · W", nameof(FixtureConfig.MotorPowerW));
        Number("Load fraction · 0–1", nameof(FixtureConfig.MotorLoad));
        Number("Fraction of shaft power heating fixture · 0–1", nameof(FixtureConfig.MotorHeatFraction));
        Number("Motor response time · s", nameof(FixtureConfig.MotorResponseSeconds));
        Heading("Oil loop / inline heater");
        Bool("Oil pump running", nameof(FixtureConfig.PumpRunning));
        Number("Pump target flow · L/min", nameof(FixtureConfig.PumpFlowLpm));
        Number("Pump response time · s", nameof(FixtureConfig.PumpResponseSeconds));
        Number("Heater rated power · W", nameof(FixtureConfig.HeaterPowerW));
        Number("Flow for 50% heater transfer · L/min", nameof(FixtureConfig.TransferFlowLpm));
        Number("Total loop oil volume · L", nameof(FixtureConfig.OilVolumeL));
        Number("Oil held inside UUT · L (part of total)", nameof(FixtureConfig.UutOilVolumeL));
        Number("Oil density · kg/L", nameof(FixtureConfig.OilDensityKgL));
        Number("Oil specific heat · J/(kg·K)", nameof(FixtureConfig.OilSpecificHeat));
        Number("Fixture mass · kg", nameof(FixtureConfig.FixtureMassKg));
        Number("Fixture specific heat · J/(kg·K)", nameof(FixtureConfig.FixtureSpecificHeat));
        Number("Passive heat loss · W/K", nameof(FixtureConfig.PassiveConductance));
        Heading("External fan");
        Bool("Automatic fan (follows controller cooler relay)", nameof(FixtureConfig.FanAutomatic));
        Bool("Manual fan running (when automatic is off)", nameof(FixtureConfig.FanRunning));
        Number("Fan command speed · rpm", nameof(FixtureConfig.FanRpm));
        Number("Fan rated speed · rpm", nameof(FixtureConfig.FanRatedRpm));
        Number("Fan cooling at rated speed · W/K", nameof(FixtureConfig.FanConductance));
        Number("Fan response time · s", nameof(FixtureConfig.FanResponseSeconds));
    }
}
