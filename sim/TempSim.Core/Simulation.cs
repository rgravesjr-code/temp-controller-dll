using System.Globalization;
using System.Text;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// One closed loop: plant (or scripted profile) -> sensors -> TcCheckTemp -> relays -> plant, with the zone's
/// diagnostics array (TcGetDiag) packed by CanTp into NI-XNET raw frame records (one J1939 BAM per tick), unpacked
/// again as the receive-side proof, and optionally written to a real Linux CAN interface. Deterministic for a given
/// config + scenario.
/// </summary>
public sealed class Simulation : IDisposable
{
    public const int RecordSize = 24;
    /// <summary>NI-XNET timestamps: 100 ns since 1601-01-01; the run starts at a fixed instant so logs are reproducible.</summary>
    public static readonly DateTime Epoch1601 = new(1601, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    public static readonly DateTime RunStart = new(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);

    public SimConfig Config { get; }
    public MessageTable Table { get; }
    public Controller Ctl { get; }
    public int Zone => Ctl.Zone;
    public Plant Plant { get; }
    public FixtureModel Fixture { get; private set; } = new();
    public SensorModel Sensor1 { get; }
    public SensorModel Sensor2 { get; }
    public RelayModel Heater { get; }
    public RelayModel Cooler { get; }
    public Rng Rng { get; private set; }
    /// <summary>The independent second zone of a two-zone scenario (null otherwise).</summary>
    public Simulation? Companion { get; }

    public int Tick { get; private set; }
    /// <summary>Shift applied to the host clock (scenario "time": backwards step, long gap).</summary>
    public long ClockOffsetMs { get; set; }
    public uint NowMs => unchecked((uint)((long)Config.StartTickMs + Tick * (long)Config.PeriodMs + ClockOffsetMs));
    public double TimeSeconds => Tick * Config.PeriodMs / 1000.0;
    public double Temp1Raw { get; private set; }
    public double Temp2Raw { get; private set; }
    public bool HeaterOn { get; private set; }
    public bool CoolerOn { get; private set; }

    /// <summary>The raw frame records of the last tick (CanTp_Pack output).</summary>
    public byte[] Frames { get; }
    public int FrameCount { get; }
    /// <summary>The values CanTp_Unpack recovered from <see cref="Frames"/> (receive side).</summary>
    public double[] Unpacked { get; }
    public int UnpackMismatches { get; private set; }
    public int Ticks { get; private set; }
    /// <summary>Every status code seen since Init (scenario checks).</summary>
    public HashSet<TcStatus> SeenStatuses { get; } = new();
    /// <summary>Per-tick (status, warning, doHeater, doCooler) since Init (determinism / independence checks).</summary>
    public List<(TcStatus St, TcWarning Wn, bool Dh, bool Dc)> Trace { get; } = new();

    readonly SocketCan? _bus;
    readonly List<Action<Simulation>> _observers = new();

    public Simulation(SimConfig config, MessageTable? table = null, int zone = 0)
    {
        NativeLoader.Register();
        Config = config;
        if (config.PeriodMs < 1 || config.PeriodMs > 10000) throw new ArgumentException("PeriodMs must be 1..10000.");
        if (config.Fixture.Enabled) config.Fixture.Validate();
        Table = table ?? MessageTable.Load(MessageTable.DefaultPath);
        if (Table.SignalCount != TcConst.DiagCount) throw new InvalidOperationException($"TempCtl.json does not match the controller's {TcConst.DiagCount} diagnostics");
        Table.Define(zone, Config.Can.SourceAddress);                       // CanTp slot = zone
        FrameCount = CanTpNative.CanTp_FrameCount(zone);
        Frames = new byte[CanTpNative.CanTp_OutputSize(zone)];
        Unpacked = new double[TcConst.DiagCount];

        Ctl = new Controller(zone);
        Plant = new Plant { Ambient = config.Plant.Ambient, LagPerSec = config.Plant.LagPerSec, HeatRate = config.Plant.HeatRate, CoolRate = config.Plant.CoolRate, Temperature = config.Plant.Initial };
        Sensor1 = new SensorModel { Offset = config.Sensor1.Offset, NoiseAmplitude = config.Sensor1.NoiseAmplitude, LagPerSec = config.Sensor1.LagPerSec };
        Sensor2 = new SensorModel { Offset = config.Sensor2.Offset, NoiseAmplitude = config.Sensor2.NoiseAmplitude, LagPerSec = config.Sensor2.LagPerSec };
        Heater = new RelayModel { DelayTicks = config.Heater.DelayTicks, StuckOpen = config.Heater.StuckOpen, StuckClosed = config.Heater.StuckClosed };
        Cooler = new RelayModel { DelayTicks = config.Cooler.DelayTicks, StuckOpen = config.Cooler.StuckOpen, StuckClosed = config.Cooler.StuckClosed };
        Rng = new Rng(config.Seed);
        Config.Profile.Sort((a, b) => a.AtSeconds.CompareTo(b.AtSeconds));
        if (!string.IsNullOrEmpty(config.Can.Interface) && zone == 0) _bus = SocketCan.Open(config.Can.Interface);
        Init();
        if (config.Companion != null && zone == 0) Companion = new Simulation(config.Companion, Table, 1);
    }

    /// <summary>Called after every tick with the fresh state (loggers, UI).</summary>
    public void AddObserver(Action<Simulation> observer) => _observers.Add(observer);

    /// <summary>Fresh run: plant and relays reset, the zone Reset (so no relay state survives from an earlier run) and TcInit.</summary>
    public void Init()
    {
        Tick = 0; Ticks = 0; UnpackMismatches = 0; ClockOffsetMs = 0;
        SeenStatuses.Clear(); Trace.Clear();
        Heater.Clear(); Cooler.Clear();
        Fixture = new FixtureModel();
        Fixture.SetTemperature(Plant.Temperature);
        HeaterOn = CoolerOn = false;
        ReadSensors(0.0);
        Ctl.Reset(NowMs);
        Ctl.LoadSetup(Config.Controller);
        Ctl.Init(NowMs);
        PackAndVerify();
    }

    /// <summary>A parameter change = TcInit with the full setup (a running zone keeps its relays, R9.3).</summary>
    public void ReInit(Action<SimConfig.ControllerConfig>? edit = null)
    {
        edit?.Invoke(Config.Controller);
        Ctl.LoadSetup(Config.Controller);
        Ctl.Init(NowMs);
        PackAndVerify();
    }

    /// <summary>The WPF app's settings panel: adopt the edited setup and re-Init.</summary>
    public void ApplyControllerConfig(SimConfig.ControllerConfig c) { Config.Controller = c; ReInit(); }

    /// <summary>Operator reset (TcReset): clears faults and history, keeps the setup and the plant.</summary>
    public void Reset()
    {
        Ctl.Reset(NowMs);
        PackAndVerify();
    }

    SimConfig.ProfilePoint? ProfileAt(double t)
    {
        SimConfig.ProfilePoint? last = null;
        foreach (var p in Config.Profile) { if (p.AtSeconds <= t + 1e-9) last = p; else break; }
        return last;
    }

    void ReadSensors(double dt)
    {
        double truth1 = Plant.Temperature, truth2 = Plant.Temperature;
        // TempCtl compares redundant probes; both remain at the UUT outlet.
        // Inlet and outlet are additional physical channels, not a redundant pair.
        if (Config.Fixture.Enabled) truth1 = truth2 = Fixture.OutletTemperature;
        var p = ProfileAt(TimeSeconds);
        if (p != null) { truth1 = p.Temp1; truth2 = p.Temp2; }
        Temp1Raw = Sensor1.Read(truth1, dt, Rng);
        Temp2Raw = Sensor2.Read(truth2, dt, Rng);
    }

    /// <summary>Advance one period.</summary>
    public void Step()
    {
        double dt = Config.PeriodMs / 1000.0;
        Tick++;
        // plant moves under the relay states decided last tick
        if (Config.Fixture.Enabled) Fixture.Step(dt, Plant, Config.Fixture, HeaterOn, CoolerOn, Config.Controller.TempUnits);
        else Plant.Step(dt, HeaterOn, CoolerOn);
        ReadSensors(dt);
        // the DO read-back reflects the physical state reached after the last command
        Ctl.CheckTemp(NowMs, Temp1Raw, Temp2Raw, Heater.Contact, Cooler.Contact);
        HeaterOn = Heater.Apply(Ctl.DoHeater);
        CoolerOn = Cooler.Apply(Ctl.DoCooler);
        PackAndVerify();
        Ticks++;
        SeenStatuses.Add(Ctl.Status);
        Trace.Add((Ctl.Status, Ctl.Warning, Ctl.DoHeater, Ctl.DoCooler));
        Companion?.Step();
        foreach (var o in _observers) o(this);
    }

    void PackAndVerify()
    {
        ulong ts = (ulong)(RunStart - Epoch1601).Ticks + (ulong)Tick * (ulong)Config.PeriodMs * 10_000UL;
        ulong spacing = (ulong)Config.Can.SpacingMs * 10_000UL;
        int rc = CanTpNative.Pack(Zone, Ctl.Diag, ts, spacing, Frames, out int written);
        if (rc != 0 || written != Frames.Length) throw new InvalidOperationException($"CanTp_Pack rc={rc} written={written}");
        rc = CanTpNative.Unpack(Zone, Frames, Unpacked, out _);
        if (rc != CanTpNative.Found) throw new InvalidOperationException($"CanTp_Unpack rc={rc}");
        for (int i = 0; i < TcConst.DiagCount; i++)
        {
            double v = Ctl.Diag[i];
            if (double.IsNaN(v)) continue;                                  // packed as "not available"
            double res = Table.Factor(i);
            double lo = Table.SigDefs[i][6], hi = Table.SigDefs[i][7];
            double expect = hi > lo ? Math.Clamp(v, lo, hi) : v;
            if (Math.Abs(Unpacked[i] - expect) > res * 0.5 + 1e-6) UnpackMismatches++;
        }
        _bus?.Send(Frames);
    }

    public void Dispose() { _bus?.Dispose(); Companion?.Dispose(); }

    // ---- text views -------------------------------------------------------------------------------------
    public static string CsvHeader =>
        "t_s,plant,temp1,temp2,ctrl,t1avg,t2avg,do_heat,do_cool,heater,cooler,status,warning,active," +
        "db_rem_ms,asp_rem_ms,cmp_rem_ms,hfb_rem_ms,cfb_rem_ms,t1_accum_ms,t2_accum_ms,t1_events,t2_events,hc_flag,hi_band,lo_band,rc,payload";

    /// <summary>One CSV row; numbers are rounded to 4 decimals so Windows and Linux runs compare byte for byte.</summary>
    public string CsvRow()
    {
        var c = CultureInfo.InvariantCulture;
        static string F(double v) => double.IsNaN(v) ? "NaN" : Math.Round(v, 4).ToString("0.####", CultureInfo.InvariantCulture);
        var k = Ctl;
        var sb = new StringBuilder();
        sb.Append(F(TimeSeconds)).Append(',').Append(F(Plant.Temperature)).Append(',')
          .Append(F(Temp1Raw)).Append(',').Append(F(Temp2Raw)).Append(',')
          .Append(F(k.ControlTemp)).Append(',').Append(F(k.Temp1Avg)).Append(',').Append(F(k.Temp2Avg)).Append(',')
          .Append(k.DoHeater ? 1 : 0).Append(',').Append(k.DoCooler ? 1 : 0).Append(',')
          .Append(HeaterOn ? 1 : 0).Append(',').Append(CoolerOn ? 1 : 0).Append(',')
          .Append((int)k.Status).Append(',').Append((int)k.Warning).Append(',').Append(k.ActiveSensor).Append(',')
          .Append(F(k.DeadbandRemainMs)).Append(',').Append(F(k.AtSetPtRemainMs)).Append(',').Append(F(k.CompareRemainMs)).Append(',')
          .Append(F(k.HeaterFbRemainMs)).Append(',').Append(F(k.CoolerFbRemainMs)).Append(',')
          .Append(F(k.Temp1OorAccumMs)).Append(',').Append(F(k.Temp2OorAccumMs)).Append(',')
          .Append(k.Temp1OorEventsPerHour).Append(',').Append(k.Temp2OorEventsPerHour).Append(',').Append(k.InitialHcFlag ? 1 : 0).Append(',')
          .Append(F(k.HiBand)).Append(',').Append(F(k.LoBand)).Append(',').Append(k.LastRc.ToString(c)).Append(',')
          .Append(PayloadHex());
        return sb.ToString();
    }

    /// <summary>The message payload reassembled from the TP.DT records, as hex.</summary>
    public string PayloadHex()
    {
        var sb = new StringBuilder();
        int len = Table.Length, got = 0;
        for (int o = RecordSize; o < Frames.Length && got < len; o += RecordSize)         // skip TP.CM
            for (int i = 1; i < 8 && got < len; i++, got++) sb.Append(Frames[o + 16 + i].ToString("x2"));
        return sb.ToString();
    }

    /// <summary>One line per raw record: timestamp offset, id, payload hex.</summary>
    public IEnumerable<string> FrameLines()
    {
        for (int o = 0; o + RecordSize <= Frames.Length; o += RecordSize)
        {
            ulong ts = BitConverter.ToUInt64(Frames, o);
            uint id = BitConverter.ToUInt32(Frames, o + 8);
            int len = Frames[o + 15];
            var hex = string.Join(' ', Enumerable.Range(0, len).Select(i => Frames[o + 16 + i].ToString("X2")));
            string kind = ((id >> 8) & 0xFF00) == 0xEC00 ? "TP.CM" : ((id >> 8) & 0xFF00) == 0xEB00 ? "TP.DT" : "PG";
            yield return $"{(ts - (ulong)(RunStart - Epoch1601).Ticks) / 10_000.0,9:0.0} ms  {id & 0x1FFFFFFF:X8}{((id & CanTpNative.XnetExtendedIdFlag) != 0 ? "x" : " ")} {kind}  {hex}";
        }
    }
}
