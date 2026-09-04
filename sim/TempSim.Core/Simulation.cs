using System.Globalization;
using System.Text;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// One closed loop: plant -> sensors -> TempCtl -> relays -> plant, with the controller's output array packed by
/// CanTp into NI-XNET raw frame records (one J1939 BAM per tick), unpacked again as the receive-side proof, and
/// optionally written to a real Linux CAN interface. Deterministic for a given config + scenario.
/// </summary>
public sealed class Simulation : IDisposable
{
    public const int Slot = 0;
    public const int RecordSize = 24;
    /// <summary>NI-XNET timestamps: 100 ns since 1601-01-01; the run starts at a fixed instant so logs are reproducible.</summary>
    public static readonly DateTime Epoch1601 = new(1601, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    public static readonly DateTime RunStart = new(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);

    public SimConfig Config { get; }
    public MessageTable Table { get; }
    public Controller Ctl { get; }
    public Plant Plant { get; }
    public SensorModel Sensor1 { get; }
    public SensorModel Sensor2 { get; }
    public RelayModel Heater { get; }
    public RelayModel Cooler { get; }
    public Rng Rng { get; private set; }

    public int Tick { get; private set; }
    public uint NowMs => (uint)(Tick * (long)Config.PeriodMs);
    public double TimeSeconds => Tick * Config.PeriodMs / 1000.0;
    public double Temp1Raw { get; private set; }
    public double Temp2Raw { get; private set; }
    public bool HeaterOn { get; private set; }
    public bool CoolerOn { get; private set; }

    /// <summary>The raw frame records of the last tick (CanTp_PackSgl output).</summary>
    public byte[] Frames { get; }
    public int FrameCount { get; }
    /// <summary>The values CanTp_Unpack recovered from <see cref="Frames"/> (receive side).</summary>
    public double[] Unpacked { get; }
    public int UnpackMismatches { get; private set; }
    public int Ticks { get; private set; }

    readonly SocketCan? _bus;
    readonly List<Action<Simulation>> _observers = new();

    public Simulation(SimConfig config, MessageTable? table = null)
    {
        NativeLoader.Register();
        Config = config;
        Table = table ?? MessageTable.Load(MessageTable.DefaultPath);
        if (Table.SignalCount != TcConst.SignalCount) throw new InvalidOperationException("TempCtl.json does not match the controller's 27 signals");
        Table.Define(Slot, Config.Can.SourceAddress);
        FrameCount = CanTpNative.CanTp_FrameCount(Slot);
        Frames = new byte[CanTpNative.CanTp_OutputSize(Slot)];
        Unpacked = new double[TcConst.SignalCount];

        Ctl = new Controller(0);
        Plant = new Plant { Ambient = config.Plant.Ambient, LagPerSec = config.Plant.LagPerSec, HeatRate = config.Plant.HeatRate, CoolRate = config.Plant.CoolRate, Temperature = config.Plant.Initial };
        Sensor1 = new SensorModel { Offset = config.Sensor1.Offset, NoiseAmplitude = config.Sensor1.NoiseAmplitude, LagPerSec = config.Sensor1.LagPerSec };
        Sensor2 = new SensorModel { Offset = config.Sensor2.Offset, NoiseAmplitude = config.Sensor2.NoiseAmplitude, LagPerSec = config.Sensor2.LagPerSec };
        Heater = new RelayModel { DelayTicks = config.Heater.DelayTicks, StuckOpen = config.Heater.StuckOpen, StuckClosed = config.Heater.StuckClosed };
        Cooler = new RelayModel { DelayTicks = config.Cooler.DelayTicks, StuckOpen = config.Cooler.StuckOpen, StuckClosed = config.Cooler.StuckClosed };
        Rng = new Rng(config.Seed);
        ApplyControllerConfig(config.Controller);
        if (!string.IsNullOrEmpty(config.Can.Interface)) _bus = SocketCan.Open(config.Can.Interface);
        Init();
    }

    public void ApplyControllerConfig(SimConfig.ControllerConfig c)
    {
        Ctl.Set(TcSignal.Setpoint, c.Setpoint);
        Ctl.Set(TcSignal.DeadbandHi, c.DeadbandHi);
        Ctl.Set(TcSignal.DeadbandLo, c.DeadbandLo);
        Ctl.Set(TcSignal.HiLimit, c.HiLimit);
        Ctl.Set(TcSignal.LoLimit, c.LoLimit);
        Ctl.Set(TcSignal.ErrorTimeoutMs, c.ErrorTimeoutMs);
        Ctl.Set(TcSignal.DeadbandTimeoutMs, c.DeadbandTimeoutMs);
        Ctl.Set(TcSignal.FilterPoints, c.FilterPoints);
        Ctl.Set(TcSignal.Temp2Enable, c.Temp2Enable ? 1 : 0);
        Ctl.Set(TcSignal.Temp2Tolerance, c.Temp2Tolerance);
        Ctl.Set(TcSignal.FeedbackEnable, c.FeedbackEnable ? 1 : 0);
    }

    /// <summary>Called after every tick with the fresh state (loggers, UI).</summary>
    public void AddObserver(Action<Simulation> observer) => _observers.Add(observer);

    /// <summary>Init the controller with the current sensor readings and relays off (also used by "restart").</summary>
    public void Init()
    {
        Tick = 0; Ticks = 0; UnpackMismatches = 0;
        Heater.Clear(); Cooler.Clear();
        HeaterOn = CoolerOn = false;
        ReadSensors(0.0);
        Ctl.Set(TcSignal.HeaterFeedback, 0);
        Ctl.Set(TcSignal.CoolerFeedback, 0);
        Ctl.Set(TcSignal.HeatingCmd, 0);
        Ctl.Set(TcSignal.CoolingCmd, 0);
        Ctl.Init(NowMs);
        PackAndVerify();
    }

    /// <summary>Operator reset: clears latched faults, keeps the plant and the filters.</summary>
    public void Reset()
    {
        Ctl.Reset(NowMs);
        PackAndVerify();
    }

    void ReadSensors(double dt)
    {
        Temp1Raw = Sensor1.Read(Plant.Temperature, dt, Rng);
        Temp2Raw = Sensor2.Read(Plant.Temperature, dt, Rng);
        Ctl.Set(TcSignal.Temp1, (float)Temp1Raw);
        Ctl.Set(TcSignal.Temp2, (float)Temp2Raw);
    }

    /// <summary>Advance one period.</summary>
    public void Step()
    {
        double dt = Config.PeriodMs / 1000.0;
        Tick++;
        // plant moves under the relay states decided last tick
        Plant.Step(dt, HeaterOn, CoolerOn);
        ReadSensors(dt);
        // relay contacts (feedback) reflect the physical state reached after the last command
        Ctl.Set(TcSignal.HeaterFeedback, Heater.Contact ? 1 : 0);
        Ctl.Set(TcSignal.CoolerFeedback, Cooler.Contact ? 1 : 0);
        Ctl.Step(NowMs);
        HeaterOn = Heater.Apply(Ctl.HeatingCmd);
        CoolerOn = Cooler.Apply(Ctl.CoolingCmd);
        PackAndVerify();
        Ticks++;
        foreach (var o in _observers) o(this);
    }

    void PackAndVerify()
    {
        ulong ts = (ulong)(RunStart - Epoch1601).Ticks + (ulong)Tick * (ulong)Config.PeriodMs * 10_000UL;
        ulong spacing = (ulong)Config.Can.SpacingMs * 10_000UL;
        int rc = CanTpNative.PackSgl(Slot, Ctl.Out, ts, spacing, Frames, out int written);
        if (rc != 0 || written != Frames.Length) throw new InvalidOperationException($"CanTp_PackSgl rc={rc} written={written}");
        rc = CanTpNative.Unpack(Slot, Frames, Unpacked, out _);
        if (rc != CanTpNative.Found) throw new InvalidOperationException($"CanTp_Unpack rc={rc}");
        for (int i = 0; i < TcConst.SignalCount; i++)
        {
            float v = Ctl.Out[i];
            if (float.IsNaN(v)) continue;                                  // packed as "not available"
            double res = Table.Factor(i);
            double lo = Table.SigDefs[i][6], hi = Table.SigDefs[i][7];
            double expect = hi > lo ? Math.Clamp(v, lo, hi) : v;
            if (Math.Abs(Unpacked[i] - expect) > res * 0.5 + 1e-6) UnpackMismatches++;
        }
        _bus?.Send(Frames);
    }

    public void Dispose() => _bus?.Dispose();

    // ---- text views -------------------------------------------------------------------------------------
    public static string CsvHeader =>
        "t_s,plant,temp1,temp2,ctrl,t1f,t2f,heat_cmd,cool_cmd,heater,cooler,err,status,active,err_remain_ms,db_remain_ms,hi_band,lo_band,rc,payload";

    /// <summary>One CSV row; numbers are rounded to 4 decimals so Windows and Linux runs compare byte for byte.</summary>
    public string CsvRow()
    {
        var c = CultureInfo.InvariantCulture;
        static string F(double v) => double.IsNaN(v) ? "NaN" : Math.Round(v, 4).ToString("0.####", CultureInfo.InvariantCulture);
        var sb = new StringBuilder();
        sb.Append(F(TimeSeconds)).Append(',').Append(F(Plant.Temperature)).Append(',')
          .Append(F(Temp1Raw)).Append(',').Append(F(Temp2Raw)).Append(',')
          .Append(F(Ctl.ControlTemp)).Append(',').Append(F(Ctl.Temp1Filtered)).Append(',').Append(F(Ctl.Temp2Filtered)).Append(',')
          .Append(Ctl.HeatingCmd ? 1 : 0).Append(',').Append(Ctl.CoolingCmd ? 1 : 0).Append(',')
          .Append(HeaterOn ? 1 : 0).Append(',').Append(CoolerOn ? 1 : 0).Append(',')
          .Append((uint)Ctl.ErrorStatus).Append(',').Append((int)Ctl.TempStatus).Append(',').Append(Ctl.ActiveSensor).Append(',')
          .Append(F(Ctl.ErrorRemainMs)).Append(',').Append(F(Ctl.DbRemainMs)).Append(',')
          .Append(F(Ctl.HiBand)).Append(',').Append(F(Ctl.LoBand)).Append(',').Append(Ctl.LastRc.ToString(c)).Append(',')
          .Append(PayloadHex());
        return sb.ToString();
    }

    /// <summary>The 50-byte message payload reassembled from the TP.DT records, as hex.</summary>
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
