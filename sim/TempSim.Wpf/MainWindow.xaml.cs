using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using TempSim.Core;
using TempSim.Core.Native;

namespace TempSim.Wpf;

public partial class MainWindow : Window
{
    static readonly string SettingsDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TempSim");
    static readonly string SettingsFile = Path.Combine(SettingsDir, "settings.json");
    static readonly string WindowFile = Path.Combine(SettingsDir, "window.json");
    static readonly string LogDir = Path.Combine(SettingsDir, "logs");

    SimConfig _cfg = new();
    MessageTable _table = null!;
    Simulation _sim = null!;
    readonly DispatcherTimer _timer = new();
    int _speed = 1;
    bool _running = true;
    Queue<(double At, string Label, Action<Simulation> Apply)> _events = new();
    CsvLogger? _csv; NclWriter? _ncl;
    readonly List<Action> _refreshers = new();          // UI controls re-read from the model
    bool _suppress;                                     // while refreshing controls programmatically

    sealed record DecodedRow(int Index, string Name, string Value, string Unpacked, string Unit, string Bits);

    public MainWindow()
    {
        InitializeComponent();
        NativeLoader.Register();
        try { if (File.Exists(SettingsFile)) _cfg = SimConfig.Load(SettingsFile); } catch { _cfg = new SimConfig(); }
        _table = MessageTable.Load(MessageTable.DefaultPath);
        VersionText.Text = "TempSim 2.0.0  |  " + NativeLoader.Describe() + $"  |  {_table.Message} PGN {(_table.CanId >> 8) & 0x3FFFF} ({_table.Length} bytes)";
        foreach (var s in Scenario.BuiltIn()) ScenarioCombo.Items.Add(new ComboBoxItem { Content = s.Name, Tag = s.Name, ToolTip = s.Description });
        ScenarioCombo.SelectedIndex = 0;
        BuildPanels();
        Restart();
        LoadWindowBounds();
        _timer.Tick += OnTimer;
        _timer.Interval = TimeSpan.FromMilliseconds(_cfg.PeriodMs);
        Loaded += OnLoaded;
    }

    void OnLoaded(object? sender, RoutedEventArgs e)
    {
        if (App.ScreenshotPath != null) { RunScreenshot(); return; }
        _timer.Start();
    }

    // ------------------------------------------------------------------ simulation control
    void Restart()
    {
        _sim?.Dispose();
        _sim = new Simulation(_cfg.Clone(), _table);
        _events.Clear();
        EventText.Text = "";
        Plot.Clear();
        RefreshFromModels();
        UpdateUi();
    }

    void FireDueEvents(double tNext)
    {
        bool any = false;
        while (_events.Count > 0 && _events.Peek().At <= tNext)
        {
            var (at, label, apply) = _events.Dequeue();
            apply(_sim);
            EventText.Text = $"{at:0} s: {label}";
            any = true;
        }
        if (any) RefreshFromModels();
    }

    void StepOnce()
    {
        FireDueEvents((_sim.Tick + 1) * _sim.Config.PeriodMs / 1000.0);
        _sim.Step();
        _csv?.Log(_sim); _ncl?.Log(_sim);
    }

    void OnTimer(object? sender, EventArgs e)
    {
        if (!_running) return;
        int n = _speed;
        for (int i = 0; i < n; i++) StepOnce();
        UpdateUi();
    }

    void OnRunPause(object sender, RoutedEventArgs e) { _running = !_running; RunButton.Content = _running ? "Pause" : "Run"; }
    void OnReset(object sender, RoutedEventArgs e) { _sim.Reset(); UpdateUi(); }
    void OnRestart(object sender, RoutedEventArgs e) { Restart(); }
    void OnSpeedChanged(object sender, SelectionChangedEventArgs e) { _speed = SpeedCombo.SelectedIndex switch { 0 => 1, 1 => 5, 2 => 20, _ => 50 }; }

    void OnLoadScenario(object sender, RoutedEventArgs e)
    {
        var name = (ScenarioCombo.SelectedItem as ComboBoxItem)?.Tag as string;
        var sc = name != null ? Scenario.Find(name, _cfg) : null;
        if (sc == null) return;
        LoadScenario(sc);
    }

    void LoadScenario(Scenario sc)
    {
        _cfg = sc.Config.Clone();
        Restart();
        _events = new Queue<(double, string, Action<Simulation>)>(sc.Events.OrderBy(ev => ev.AtSeconds));
        EventText.Text = $"{sc.Name}: {sc.Description}";
    }

    void OnLogChanged(object sender, RoutedEventArgs e)
    {
        _csv?.Dispose(); _ncl?.Dispose(); _csv = null; _ncl = null;
        if (LogCheck.IsChecked == true)
        {
            Directory.CreateDirectory(LogDir);
            string stem = Path.Combine(LogDir, "tempsim-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            _csv = new CsvLogger(stem + ".csv"); _ncl = new NclWriter(stem + ".ncl");
            LogText.Text = "writing " + stem + ".csv / .ncl";
        }
        else LogText.Text = "";
    }

    // ------------------------------------------------------------------ UI update
    void UpdateUi()
    {
        var c = _sim.Ctl;
        Plot.Add(new PlotView.Sample(_sim.TimeSeconds, _sim.Plant.Temperature, _sim.Temp1Raw, _sim.Temp2Raw, c.ControlTemp,
            c.Get(TcSignal.Setpoint), c.HiBand, c.LoBand, c.Get(TcSignal.HiLimit), c.Get(TcSignal.LoLimit),
            c.HeatingCmd, c.CoolingCmd, (int)c.TempStatus));

        StatusText.Text = $"{c.TempStatus}   ({(int)c.TempStatus})";
        StatusText.Foreground = c.TempStatus switch
        {
            TcTempStatus.Stopped => Brushes.Red,
            TcTempStatus.ErrorPending or TcTempStatus.Degraded => new SolidColorBrush(Color.FromRgb(200, 110, 0)),
            TcTempStatus.Heating => new SolidColorBrush(Color.FromRgb(200, 50, 20)),
            TcTempStatus.Cooling => new SolidColorBrush(Color.FromRgb(30, 90, 200)),
            _ => Brushes.Black,
        };
        ControlTempText.Text = $"t = {_sim.TimeSeconds:0.0} s    ControlTemp {Fmt(c.ControlTemp)}   Temp1f {Fmt(c.Temp1Filtered)}   Temp2f {Fmt(c.Temp2Filtered)}   band [{Fmt(c.LoBand)}, {Fmt(c.HiBand)}]";
        ErrorText.Text = c.ErrorStatus == TcErrorBits.None ? "ErrorStatus: none" : $"ErrorStatus {(uint)c.ErrorStatus}: {Controller.Describe(c.ErrorStatus)}   (rc {c.LastRc})";
        float errTo = Math.Max(1, c.Get(TcSignal.ErrorTimeoutMs)), dbTo = Math.Max(1, c.Get(TcSignal.DeadbandTimeoutMs));
        ErrorBar.Maximum = errTo; ErrorBar.Value = Math.Min(errTo, c.ErrorRemainMs); ErrorRemainText.Text = c.ErrorRemainMs > 0 ? $"{c.ErrorRemainMs:0} ms" : "";
        DbBar.Maximum = dbTo; DbBar.Value = Math.Min(dbTo, c.DbRemainMs); DbRemainText.Text = c.DbRemainMs > 0 ? $"{c.DbRemainMs:0} ms" : "";
        HeatLamp.IsOn = c.HeatingCmd; CoolLamp.IsOn = c.CoolingCmd;
        HeatFbLamp.IsOn = _sim.Heater.Contact; CoolFbLamp.IsOn = _sim.Cooler.Contact;
        SensorText.Text = $"Active sensor: {c.ActiveSensor}    Temp1 {Fmt(_sim.Temp1Raw)}   Temp2 {Fmt(_sim.Temp2Raw)}   plant {_sim.Plant.Temperature:0.00}";

        FramesTitle.Text = $"NI-XNET raw frames (CanTp_PackSgl): J1939 BAM, {_sim.FrameCount} frames x 24 bytes, payload {_table.Length} bytes, unpack mismatches {_sim.UnpackMismatches}";
        FramesText.Text = string.Join(Environment.NewLine, _sim.FrameLines()) + Environment.NewLine + "payload " + _sim.PayloadHex();
        var rows = new List<DecodedRow>(TcConst.SignalCount);
        for (int i = 0; i < TcConst.SignalCount; i++)
        {
            var sd = _table.SigDefs[i];
            rows.Add(new DecodedRow(i, _table.Signals[i], Fmt(c.Out[i]), Fmt(_sim.Unpacked[i]), _table.Units[i], $"{(int)sd[0]}|{(int)sd[1]}"));
        }
        DecodedList.ItemsSource = rows;
    }

    static string Fmt(double v) => double.IsNaN(v) ? "NaN" : v.ToString("0.###", CultureInfo.InvariantCulture);

    // ------------------------------------------------------------------ settings panels
    void BuildPanels()
    {
        var cc = _cfg.Controller;
        Num(ControllerPanel, "Setpoint", () => cc.Setpoint, v => { _cfg.Controller.Setpoint = v; });
        Num(ControllerPanel, "DeadbandHi (offset)", () => cc.DeadbandHi, v => _cfg.Controller.DeadbandHi = v);
        Num(ControllerPanel, "DeadbandLo (offset)", () => cc.DeadbandLo, v => _cfg.Controller.DeadbandLo = v);
        Num(ControllerPanel, "HiLimit", () => cc.HiLimit, v => _cfg.Controller.HiLimit = v);
        Num(ControllerPanel, "LoLimit", () => cc.LoLimit, v => _cfg.Controller.LoLimit = v);
        Num(ControllerPanel, "ErrorTimeout ms", () => cc.ErrorTimeoutMs, v => _cfg.Controller.ErrorTimeoutMs = v);
        Num(ControllerPanel, "DeadbandTimeout ms", () => cc.DeadbandTimeoutMs, v => _cfg.Controller.DeadbandTimeoutMs = v);
        Num(ControllerPanel, "FilterPoints (1..64)", () => cc.FilterPoints, v => _cfg.Controller.FilterPoints = v);
        Bool(ControllerPanel, "Temp2Enable", () => cc.Temp2Enable, v => _cfg.Controller.Temp2Enable = v);
        Num(ControllerPanel, "Temp2Tolerance", () => cc.Temp2Tolerance, v => _cfg.Controller.Temp2Tolerance = v);
        Bool(ControllerPanel, "FeedbackEnable", () => cc.FeedbackEnable, v => _cfg.Controller.FeedbackEnable = v);

        Num(PlantPanel, "Ambient", () => _sim.Plant.Ambient, v => { _cfg.Plant.Ambient = v; _sim.Plant.Ambient = v; });
        Num(PlantPanel, "Heat rate deg/s", () => _sim.Plant.HeatRate, v => { _cfg.Plant.HeatRate = v; _sim.Plant.HeatRate = v; });
        Num(PlantPanel, "Cool rate deg/s", () => _sim.Plant.CoolRate, v => { _cfg.Plant.CoolRate = v; _sim.Plant.CoolRate = v; });
        Num(PlantPanel, "Lag to ambient 1/s", () => _sim.Plant.LagPerSec, v => { _cfg.Plant.LagPerSec = v; _sim.Plant.LagPerSec = v; });
        Num(PlantPanel, "Plant temperature now", () => _sim.Plant.Temperature, v => _sim.Plant.Temperature = v);

        SensorPanel(Sensor1Panel, () => _sim.Sensor1, () => _cfg.Sensor1);
        SensorPanel(Sensor2Panel, () => _sim.Sensor2, () => _cfg.Sensor2);

        Bool(RelayPanel, "Heater stuck open (never closes)", () => _sim.Heater.StuckOpen, v => { _cfg.Heater.StuckOpen = v; _sim.Heater.StuckOpen = v; });
        Bool(RelayPanel, "Heater stuck closed", () => _sim.Heater.StuckClosed, v => { _cfg.Heater.StuckClosed = v; _sim.Heater.StuckClosed = v; });
        Num(RelayPanel, "Heater answer delay (ticks)", () => _sim.Heater.DelayTicks, v => { _cfg.Heater.DelayTicks = (int)v; _sim.Heater.DelayTicks = (int)v; });
        Bool(RelayPanel, "Cooler stuck open", () => _sim.Cooler.StuckOpen, v => { _cfg.Cooler.StuckOpen = v; _sim.Cooler.StuckOpen = v; });
        Bool(RelayPanel, "Cooler stuck closed", () => _sim.Cooler.StuckClosed, v => { _cfg.Cooler.StuckClosed = v; _sim.Cooler.StuckClosed = v; });
        Num(RelayPanel, "Cooler answer delay (ticks)", () => _sim.Cooler.DelayTicks, v => { _cfg.Cooler.DelayTicks = (int)v; _sim.Cooler.DelayTicks = (int)v; });
    }

    void SensorPanel(Panel p, Func<SensorModel> model, Func<SimConfig.SensorConfig> cfg)
    {
        Num(p, "Offset", () => model().Offset, v => { cfg().Offset = v; model().Offset = v; });
        Num(p, "Noise amplitude", () => model().NoiseAmplitude, v => { cfg().NoiseAmplitude = v; model().NoiseAmplitude = v; });
        Num(p, "Lag 1/s (0 = none)", () => model().LagPerSec, v => { cfg().LagPerSec = v; model().LagPerSec = v; });
        Combo(p, "Fault", Enum.GetNames<SensorFault>(), () => (int)model().Fault, v => model().Fault = (SensorFault)v);
        Num(p, "Stuck value", () => model().StuckValue, v => model().StuckValue = v);
        var row = new DockPanel { Margin = new Thickness(0, 2, 0, 2) };
        var check = new CheckBox { Content = "Override", VerticalAlignment = VerticalAlignment.Center, Width = 80 };
        var value = new TextBlock { Width = 50, VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Right, Margin = new Thickness(4, 0, 0, 0) };
        var slider = new Slider { Minimum = -20, Maximum = 150, TickFrequency = 10, IsSnapToTickEnabled = false, VerticalAlignment = VerticalAlignment.Center };
        DockPanel.SetDock(check, Dock.Left); DockPanel.SetDock(value, Dock.Right);
        row.Children.Add(check); row.Children.Add(value); row.Children.Add(slider);
        check.Checked += (_, _) => { if (!_suppress) { model().Override = true; model().OverrideValue = slider.Value; } };
        check.Unchecked += (_, _) => { if (!_suppress) model().Override = false; };
        slider.ValueChanged += (_, e) => { value.Text = e.NewValue.ToString("0.0", CultureInfo.InvariantCulture); if (!_suppress) model().OverrideValue = e.NewValue; };
        _refreshers.Add(() => { check.IsChecked = model().Override; slider.Value = model().Override ? model().OverrideValue : slider.Value; value.Text = slider.Value.ToString("0.0", CultureInfo.InvariantCulture); });
        p.Children.Add(row);
    }

    void Num(Panel p, string label, Func<double> get, Action<float> set)
    {
        var row = new DockPanel();
        var box = new TextBox { Width = 80, TextAlignment = TextAlignment.Right };
        DockPanel.SetDock(box, Dock.Right);
        row.Children.Add(box);
        row.Children.Add(new Label { Content = label });
        void Commit()
        {
            if (_suppress) return;
            if (float.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v)) { set(v); ApplyConfig(); box.Background = Brushes.White; }
            else box.Background = Brushes.MistyRose;
        }
        box.LostFocus += (_, _) => Commit();
        box.KeyDown += (_, e) => { if (e.Key == Key.Enter) { Commit(); Keyboard.ClearFocus(); } };
        _refreshers.Add(() => { if (!box.IsKeyboardFocused) box.Text = get().ToString("0.###", CultureInfo.InvariantCulture); });
        p.Children.Add(row);
    }

    void Bool(Panel p, string label, Func<bool> get, Action<bool> set)
    {
        var check = new CheckBox { Content = label };
        check.Checked += (_, _) => { if (!_suppress) { set(true); ApplyConfig(); } };
        check.Unchecked += (_, _) => { if (!_suppress) { set(false); ApplyConfig(); } };
        _refreshers.Add(() => check.IsChecked = get());
        p.Children.Add(check);
    }

    void Combo(Panel p, string label, string[] options, Func<int> get, Action<int> set)
    {
        var row = new DockPanel();
        var combo = new ComboBox { Width = 110, Margin = new Thickness(2) };
        foreach (var o in options) combo.Items.Add(o);
        DockPanel.SetDock(combo, Dock.Right);
        row.Children.Add(combo);
        row.Children.Add(new Label { Content = label });
        combo.SelectionChanged += (_, _) => { if (!_suppress && combo.SelectedIndex >= 0) set(combo.SelectedIndex); };
        _refreshers.Add(() => combo.SelectedIndex = get());
        p.Children.Add(row);
    }

    /// <summary>Push the (possibly edited) controller configuration into the running simulation.</summary>
    void ApplyConfig()
    {
        _sim.ApplyControllerConfig(_cfg.Controller);
        RefreshFromModels();
    }

    void RefreshFromModels()
    {
        _suppress = true;
        try { foreach (var r in _refreshers) r(); } finally { _suppress = false; }
    }

    // ------------------------------------------------------------------ persistence
    sealed class WindowBounds { public double Left { get; set; } public double Top { get; set; } public double Width { get; set; } public double Height { get; set; } public bool Maximized { get; set; } }

    void LoadWindowBounds()
    {
        try
        {
            if (!File.Exists(WindowFile)) return;
            var b = JsonSerializer.Deserialize<WindowBounds>(File.ReadAllText(WindowFile));
            if (b == null || b.Width < 400 || b.Height < 300) return;
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = b.Left; Top = b.Top; Width = b.Width; Height = b.Height;
            if (b.Maximized) WindowState = WindowState.Maximized;
        }
        catch { }
    }

    void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _timer.Stop();
        _csv?.Dispose(); _ncl?.Dispose();
        if (App.ScreenshotPath != null) return;             // screenshot runs never touch the user's settings
        try
        {
            Directory.CreateDirectory(SettingsDir);
            _cfg.Save(SettingsFile);
            var r = WindowState == WindowState.Normal ? new Rect(Left, Top, Width, Height) : RestoreBounds;
            File.WriteAllText(WindowFile, JsonSerializer.Serialize(new WindowBounds { Left = r.Left, Top = r.Top, Width = r.Width, Height = r.Height, Maximized = WindowState == WindowState.Maximized }));
        }
        catch { }
        _sim.Dispose();
    }

    // ------------------------------------------------------------------ headless verification
    void RunScreenshot()
    {
        var sc = Scenario.Find(App.ScreenshotScenario) ?? Scenario.BuiltIn()[0];
        for (int i = 0; i < ScenarioCombo.Items.Count; i++)
            if ((string)((ComboBoxItem)ScenarioCombo.Items[i]).Tag == sc.Name) ScenarioCombo.SelectedIndex = i;
        LoadScenario(sc);
        int ticks = App.ScreenshotSeconds * 1000 / _sim.Config.PeriodMs;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        for (int i = 0; i < ticks; i++)
        {
            StepOnce();
            if (i % 5 == 0) Plot.Add(new PlotView.Sample(_sim.TimeSeconds, _sim.Plant.Temperature, _sim.Temp1Raw, _sim.Temp2Raw, _sim.Ctl.ControlTemp,
                _sim.Ctl.Get(TcSignal.Setpoint), _sim.Ctl.HiBand, _sim.Ctl.LoBand, _sim.Ctl.Get(TcSignal.HiLimit), _sim.Ctl.Get(TcSignal.LoLimit),
                _sim.Ctl.HeatingCmd, _sim.Ctl.CoolingCmd, (int)_sim.Ctl.TempStatus));
        }
        double simMs = sw.Elapsed.TotalMilliseconds;
        UpdateUi();
        Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, () =>
        {
            UpdateLayout();
            var rtb = new RenderTargetBitmap((int)ActualWidth, (int)ActualHeight, 96, 96, PixelFormats.Pbgra32);
            rtb.Render(this);
            var enc = new PngBitmapEncoder();
            enc.Frames.Add(BitmapFrame.Create(rtb));
            using (var f = File.Create(App.ScreenshotPath!)) enc.Save(f);
            File.WriteAllText(Path.ChangeExtension(App.ScreenshotPath!, ".perf.txt"),
                $"scenario {sc.Name}, {ticks} ticks simulated in {simMs:0.0} ms ({simMs / ticks * 1000:0.0} us/tick incl. pack+unpack), " +
                $"final status {_sim.Ctl.TempStatus}, errors {Controller.Describe(_sim.Ctl.ErrorStatus)}, unpack mismatches {_sim.UnpackMismatches}\n");
            Application.Current.Shutdown(_sim.UnpackMismatches == 0 ? 0 : 1);
        });
    }
}
