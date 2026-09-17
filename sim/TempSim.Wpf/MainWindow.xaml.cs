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
    Queue<(double At, string Label, Func<Simulation, bool> Check)> _checks = new();
    int _checksOk, _checksFailed;
    CsvLogger? _csv; NclWriter? _ncl;
    readonly List<Action> _refreshers = new();          // UI controls re-read from the model
    bool _suppress;                                     // while refreshing controls programmatically

    sealed record DecodedRow(int Index, string Name, string Value, string Unpacked, string Unit, string Bits);

    static readonly Brush s_orange = new SolidColorBrush(Color.FromRgb(200, 110, 0));
    static readonly Brush s_heat = new SolidColorBrush(Color.FromRgb(200, 50, 20));
    static readonly Brush s_cool = new SolidColorBrush(Color.FromRgb(30, 90, 200));

    public MainWindow()
    {
        InitializeComponent();
        NativeLoader.Register();
        try { if (File.Exists(SettingsFile)) _cfg = SimConfig.Load(SettingsFile); } catch { _cfg = new SimConfig(); }
        _cfg.Profile.Clear(); _cfg.Companion = null;                       // the interactive app drives the plant, one zone
        _table = MessageTable.Load(MessageTable.DefaultPath);
        var av = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = $"TempSim {(av != null ? $"{av.Major}.{av.Minor}.{av.Build}" : "?")}  |  " + NativeLoader.Describe() + $"  |  {_table.Message} PGN {(_table.CanId >> 8) & 0x3FFFF} ({_table.Length} bytes)";
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
        _events.Clear(); _checks.Clear(); _checksOk = _checksFailed = 0;
        EventText.Text = "";
        Plot.Clear();
        RefreshFromModels();
        UpdateUi();
    }

    void FireDueEvents(double tNext)
    {
        bool any = false;
        while (_events.Count > 0 && _events.Peek().At <= tNext + 1e-9)
        {
            var (at, label, apply) = _events.Dequeue();
            apply(_sim);
            EventText.Text = $"{at:0.0} s: {label}";
            any = true;
        }
        if (any) RefreshFromModels();
    }

    void CheckDue(double t)
    {
        while (_checks.Count > 0 && _checks.Peek().At <= t + 1e-9)
        {
            var (at, label, check) = _checks.Dequeue();
            bool ok;
            try { ok = check(_sim); } catch { ok = false; }
            if (ok) _checksOk++; else _checksFailed++;
            EventText.Text = $"{at:0.0} s: {(ok ? "ok" : "FAIL")} {label}";
        }
    }

    void StepOnce()
    {
        double t = (_sim.Tick + 1) * _sim.Config.PeriodMs / 1000.0;
        FireDueEvents(t);
        _sim.Step();
        CheckDue(t);
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
        _checks = new Queue<(double, string, Func<Simulation, bool>)>(sc.Expectations.OrderBy(ev => ev.AtSeconds));
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
    PlotView.Sample CurrentSample()
    {
        var c = _sim.Ctl; var k = _sim.Config.Controller;
        return new PlotView.Sample(_sim.TimeSeconds, _sim.Plant.Temperature, _sim.Temp1Raw, _sim.Temp2Raw, c.ControlTemp,
            k.Setpoint, c.HiBand, c.LoBand, k.HiLimit, k.LoLimit, c.DoHeater, c.DoCooler, (int)c.Status, (int)c.Warning);
    }

    void UpdateUi()
    {
        var c = _sim.Ctl; var k = _sim.Config.Controller;
        Plot.Add(CurrentSample());

        StatusText.Text = Controller.Describe(c.Status);
        StatusText.Foreground = c.IsFault ? Brushes.Red : c.Status switch
        {
            TcStatus.HeaterON => s_heat,
            TcStatus.CoolerON => s_cool,
            TcStatus.HeatPending or TcStatus.CoolPending => s_orange,
            TcStatus.TempCtrlDisabled => Brushes.Gray,
            _ => Brushes.Black,
        };
        WarningText.Text = "Warning: " + Controller.Describe(c.Warning) + (c.Warning == TcWarning.RunningOnTemp2 ? "  (control on sensor 2 until Reset/Init)" : "");
        WarningText.Foreground = c.Warning == TcWarning.NoWarning ? Brushes.DimGray : s_orange;
        ControlTempText.Text = $"t = {_sim.TimeSeconds:0.0} s    ControlTemp {Fmt(c.ControlTemp)}   Temp1Avg {Fmt(c.Temp1Avg)}   Temp2Avg {Fmt(c.Temp2Avg)}   band [{Fmt(c.LoBand)}, {Fmt(c.HiBand)}]   rc {c.LastRc}";

        Bar(DbBar, DbText, k.DeadbandTimeoutMs, c.DeadbandRemainMs);
        Bar(AspBar, AspText, k.AtSetPtTimeoutMs, c.AtSetPtRemainMs);
        Bar(CmpBar, CmpText, k.TempCompareTimeoutMs, c.CompareRemainMs);
        Bar(HfbBar, HfbText, k.RelayFeedbackTimeoutMs, c.HeaterFbRemainMs);
        Bar(CfbBar, CfbText, k.RelayFeedbackTimeoutMs, c.CoolerFbRemainMs);
        Bar(Acc1Bar, Acc1Text, k.ErrorTimeoutMs, c.Temp1OorAccumMs);
        Bar(Acc2Bar, Acc2Text, k.ErrorTimeoutMs, c.Temp2OorAccumMs);
        HeatLamp.IsOn = c.DoHeater; CoolLamp.IsOn = c.DoCooler;
        HeatFbLamp.IsOn = _sim.Heater.Contact; CoolFbLamp.IsOn = _sim.Cooler.Contact;
        SensorText.Text = $"Active sensor {c.ActiveSensor}    Temp1 {Fmt(_sim.Temp1Raw)}   Temp2 {Fmt(_sim.Temp2Raw)} (corrected {Fmt(c.Temp2Corrected)})   plant {_sim.Plant.Temperature:0.00}\n" +
                          $"Initial_HC_Flag {(c.InitialHcFlag ? 1 : 0)}    out-of-range events/h  S1 {c.Temp1OorEventsPerHour}  S2 {c.Temp2OorEventsPerHour}    FilterPoints in use {c.AppliedFilterPoints}" +
                          (_checksOk + _checksFailed > 0 ? $"\nScenario expectations: {_checksOk} ok, {_checksFailed} failed" : "");

        FramesTitle.Text = $"NI-XNET raw frames (CanTp_Pack of the TcGetDiag array): J1939 BAM, {_sim.FrameCount} frames x 24 bytes, payload {_table.Length} bytes, unpack mismatches {_sim.UnpackMismatches}";
        FramesText.Text = string.Join(Environment.NewLine, _sim.FrameLines()) + Environment.NewLine + "payload " + _sim.PayloadHex();
        var rows = new List<DecodedRow>(TcConst.DiagCount);
        for (int i = 0; i < TcConst.DiagCount; i++)
        {
            var sd = _table.SigDefs[i];
            rows.Add(new DecodedRow(i, _table.Signals[i], Fmt(c.Diag[i]), Fmt(_sim.Unpacked[i]), _table.Units[i], $"{(int)sd[0]}|{(int)sd[1]}"));
        }
        DecodedList.ItemsSource = rows;
    }

    static void Bar(ProgressBar bar, TextBlock text, double max, double value)
    {
        max = Math.Max(1, max);
        bar.Maximum = max; bar.Value = Math.Min(max, value);
        text.Text = value > 0 ? $"{value:0} ms" : "";
    }

    static string Fmt(double v) => double.IsNaN(v) ? "NaN" : v.ToString("0.###", CultureInfo.InvariantCulture);

    // ------------------------------------------------------------------ settings panels
    void BuildPanels()
    {
        var cc = () => _cfg.Controller;
        Bool(ControllerPanel, "TempCtrlEnable (master switch)", () => cc().TempCtrlEnable, v => cc().TempCtrlEnable = v);
        Num(ControllerPanel, "TempUnits (0 degF, 1 degC, label only)", () => cc().TempUnits, v => cc().TempUnits = (int)v);
        Num(ControllerPanel, "Setpoint", () => cc().Setpoint, v => cc().Setpoint = v);
        Num(ControllerPanel, "DeadbandHi (offset)", () => cc().DeadbandHi, v => cc().DeadbandHi = v);
        Num(ControllerPanel, "DeadbandLo (offset)", () => cc().DeadbandLo, v => cc().DeadbandLo = v);
        Num(ControllerPanel, "HiLimit", () => cc().HiLimit, v => cc().HiLimit = v);
        Num(ControllerPanel, "LoLimit", () => cc().LoLimit, v => cc().LoLimit = v);
        Num(ControllerPanel, "ErrorTimeout ms", () => cc().ErrorTimeoutMs, v => cc().ErrorTimeoutMs = v);
        Num(ControllerPanel, "DeadbandTimeout ms", () => cc().DeadbandTimeoutMs, v => cc().DeadbandTimeoutMs = v);
        Num(ControllerPanel, "AtSetPtTimeout ms", () => cc().AtSetPtTimeoutMs, v => cc().AtSetPtTimeoutMs = v);
        Bool(ControllerPanel, "Temp2Enable", () => cc().Temp2Enable, v => cc().Temp2Enable = v);
        Num(ControllerPanel, "Temp2Offset", () => cc().Temp2Offset, v => cc().Temp2Offset = v);
        Num(ControllerPanel, "Temp2Tolerance", () => cc().Temp2Tolerance, v => cc().Temp2Tolerance = v);
        Num(ControllerPanel, "TempCompareTimeout ms", () => cc().TempCompareTimeoutMs, v => cc().TempCompareTimeoutMs = v);
        Num(ControllerPanel, "FilterPoints (1..64, else 4)", () => cc().FilterPoints, v => cc().FilterPoints = v);
        Bool(ControllerPanel, "FeedbackEnable", () => cc().FeedbackEnable, v => cc().FeedbackEnable = v);
        Num(ControllerPanel, "RelayFeedbackTimeout ms", () => cc().RelayFeedbackTimeoutMs, v => cc().RelayFeedbackTimeoutMs = v);

        Num(PlantPanel, "Ambient", () => _sim.Plant.Ambient, v => { _cfg.Plant.Ambient = v; _sim.Plant.Ambient = v; }, false);
        Num(PlantPanel, "Heat rate deg/s", () => _sim.Plant.HeatRate, v => { _cfg.Plant.HeatRate = v; _sim.Plant.HeatRate = v; }, false);
        Num(PlantPanel, "Cool rate deg/s", () => _sim.Plant.CoolRate, v => { _cfg.Plant.CoolRate = v; _sim.Plant.CoolRate = v; }, false);
        Num(PlantPanel, "Lag to ambient 1/s", () => _sim.Plant.LagPerSec, v => { _cfg.Plant.LagPerSec = v; _sim.Plant.LagPerSec = v; }, false);
        Num(PlantPanel, "Plant temperature now", () => _sim.Plant.Temperature, v => _sim.Plant.Temperature = v, false);

        SensorPanel(Sensor1Panel, () => _sim.Sensor1, () => _cfg.Sensor1);
        SensorPanel(Sensor2Panel, () => _sim.Sensor2, () => _cfg.Sensor2);

        Bool(RelayPanel, "Heater stuck open (never closes)", () => _sim.Heater.StuckOpen, v => { _cfg.Heater.StuckOpen = v; _sim.Heater.StuckOpen = v; }, false);
        Bool(RelayPanel, "Heater stuck closed", () => _sim.Heater.StuckClosed, v => { _cfg.Heater.StuckClosed = v; _sim.Heater.StuckClosed = v; }, false);
        Num(RelayPanel, "Heater answer delay (ticks)", () => _sim.Heater.DelayTicks, v => { _cfg.Heater.DelayTicks = (int)v; _sim.Heater.DelayTicks = (int)v; }, false);
        Bool(RelayPanel, "Cooler stuck open", () => _sim.Cooler.StuckOpen, v => { _cfg.Cooler.StuckOpen = v; _sim.Cooler.StuckOpen = v; }, false);
        Bool(RelayPanel, "Cooler stuck closed", () => _sim.Cooler.StuckClosed, v => { _cfg.Cooler.StuckClosed = v; _sim.Cooler.StuckClosed = v; }, false);
        Num(RelayPanel, "Cooler answer delay (ticks)", () => _sim.Cooler.DelayTicks, v => { _cfg.Cooler.DelayTicks = (int)v; _sim.Cooler.DelayTicks = (int)v; }, false);
    }

    void SensorPanel(Panel p, Func<SensorModel> model, Func<SimConfig.SensorConfig> cfg)
    {
        Num(p, "Offset", () => model().Offset, v => { cfg().Offset = v; model().Offset = v; }, false);
        Num(p, "Noise amplitude", () => model().NoiseAmplitude, v => { cfg().NoiseAmplitude = v; model().NoiseAmplitude = v; }, false);
        Num(p, "Lag 1/s (0 = none)", () => model().LagPerSec, v => { cfg().LagPerSec = v; model().LagPerSec = v; }, false);
        Combo(p, "Fault", Enum.GetNames<SensorFault>(), () => (int)model().Fault, v => model().Fault = (SensorFault)v);
        Num(p, "Stuck value", () => model().StuckValue, v => model().StuckValue = v, false);
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

    /// <param name="reinit">true: the value is part of the controller setup and a change means TcInit.</param>
    void Num(Panel p, string label, Func<double> get, Action<double> set, bool reinit = true)
    {
        var row = new DockPanel();
        var box = new TextBox { Width = 80, TextAlignment = TextAlignment.Right };
        DockPanel.SetDock(box, Dock.Right);
        row.Children.Add(box);
        row.Children.Add(new Label { Content = label });
        void Commit()
        {
            if (_suppress) return;
            if (double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v)) { set(v); if (reinit) ApplyConfig(); box.Background = Brushes.White; }
            else box.Background = Brushes.MistyRose;
        }
        box.LostFocus += (_, _) => Commit();
        box.KeyDown += (_, e) => { if (e.Key == Key.Enter) { Commit(); Keyboard.ClearFocus(); } };
        _refreshers.Add(() => { if (!box.IsKeyboardFocused) box.Text = get().ToString("0.###", CultureInfo.InvariantCulture); });
        p.Children.Add(row);
    }

    void Bool(Panel p, string label, Func<bool> get, Action<bool> set, bool reinit = true)
    {
        var check = new CheckBox { Content = label };
        check.Checked += (_, _) => { if (!_suppress) { set(true); if (reinit) ApplyConfig(); } };
        check.Unchecked += (_, _) => { if (!_suppress) { set(false); if (reinit) ApplyConfig(); } };
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

    /// <summary>Push the edited controller setup into the running simulation: TcInit with the full array (R9.3 keeps the relays).</summary>
    void ApplyConfig()
    {
        _sim.ApplyControllerConfig(_cfg.Controller);
        RefreshFromModels();
        UpdateUi();
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
            if (i % 5 == 0) Plot.Add(CurrentSample());
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
            bool ok = _sim.UnpackMismatches == 0 && _checksFailed == 0;
            File.WriteAllText(Path.ChangeExtension(App.ScreenshotPath!, ".perf.txt"),
                $"scenario {sc.Name}, {ticks} ticks simulated in {simMs:0.0} ms ({simMs / ticks * 1000:0.0} us/tick incl. TcGetDiag + pack + unpack), " +
                $"final status {Controller.Describe(_sim.Ctl.Status)}, warning {Controller.Describe(_sim.Ctl.Warning)}, unpack mismatches {_sim.UnpackMismatches}, " +
                $"expectations {_checksOk} ok / {_checksFailed} failed\n");
            Application.Current.Shutdown(ok ? 0 : 1);
        });
    }
}
