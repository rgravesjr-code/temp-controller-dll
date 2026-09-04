using System.Globalization;
using TempSim.Core;
using TempSim.Core.Native;

namespace TempSim.Cli;

/// <summary>
/// TempSim.Cli - cross-platform console simulator for TempCtl v2 + CanTp.
///
///   TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N]
///               [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE]
///   TempSim.Cli --list
///   TempSim.Cli --rx IFACE [--seconds N]     (Linux: listen on a CAN interface and decode TempCtl BAMs with CanTp_RxFeed)
///
/// Every scenario writes DIR/NAME.csv (state table, one row per tick) and DIR/NAME.ncl (NI-XNET logfile of the
/// J1939 BAM frames). The CSV is bit-identical on Windows and Linux for the same config: that is the
/// cross-platform proof of the tempctl + cantp libraries.
/// </summary>
static class Program
{
    static int Main(string[] args)
    {
        string? scenario = "all", outDir = "out", configFile = null, canIface = null, nativeDir = null, table = null, rxIface = null;
        int? seconds = null, periodMs = null;
        bool realtime = false, quiet = false, list = false, help = false;
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{a} needs a value");
            switch (a)
            {
                case "--scenario": scenario = Next(); break;
                case "--out": outDir = Next(); break;
                case "--config": configFile = Next(); break;
                case "--seconds": seconds = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--period-ms": periodMs = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--can": canIface = Next(); break;
                case "--rx": rxIface = Next(); break;
                case "--native-dir": nativeDir = Next(); break;
                case "--table": table = Next(); break;
                case "--realtime": realtime = true; break;
                case "--quiet": quiet = true; break;
                case "--list": list = true; break;
                case "-h": case "--help": case "/?": help = true; break;
                default: Console.Error.WriteLine($"unknown argument {a}"); return 2;
            }
        }
        if (help) { PrintHelp(); return 0; }
        if (nativeDir != null) NativeLoader.NativeDir = nativeDir;
        NativeLoader.Register();
        Console.WriteLine($"TempSim.Cli 2.0.0  ({NativeLoader.Describe()})");

        var baseConfig = configFile != null ? SimConfig.Load(configFile) : new SimConfig();
        if (seconds != null) baseConfig.Seconds = seconds.Value;
        if (periodMs != null) baseConfig.PeriodMs = periodMs.Value;
        if (canIface != null) baseConfig.Can.Interface = canIface;

        if (list)
        {
            foreach (var s in Scenario.BuiltIn(baseConfig)) Console.WriteLine($"  {s.Name,-20} {s.Description}");
            return 0;
        }
        var msgTable = MessageTable.Load(table ?? MessageTable.DefaultPath);
        if (rxIface != null) return Receive(rxIface, msgTable, baseConfig.Seconds);

        var scenarios = string.Equals(scenario, "all", StringComparison.OrdinalIgnoreCase)
            ? Scenario.BuiltIn(baseConfig)
            : new[] { Scenario.Find(scenario!, baseConfig) ?? throw new ArgumentException($"no scenario '{scenario}' (try --list)") };
        Directory.CreateDirectory(outDir);
        int worst = 0;
        foreach (var sc in scenarios)
        {
            Console.WriteLine();
            Console.WriteLine($"=== {sc.Name}: {sc.Description}");
            using var csv = new CsvLogger(Path.Combine(outDir, sc.Name + ".csv"));
            using var ncl = new NclWriter(Path.Combine(outDir, sc.Name + ".ncl"));
            int lastPrinted = -1;
            var t0 = DateTime.UtcNow;
            var sim = sc.Run(msgTable, s =>
            {
                csv.Log(s); ncl.Log(s);
                if (!quiet && (int)s.TimeSeconds != lastPrinted && s.Tick % (1000 / s.Config.PeriodMs) == 0)
                {
                    lastPrinted = (int)s.TimeSeconds;
                    if (lastPrinted % 5 == 0) PrintRow(s);
                }
                if (realtime)
                {
                    var due = t0 + TimeSpan.FromMilliseconds(s.Tick * (double)s.Config.PeriodMs);
                    var wait = due - DateTime.UtcNow;
                    if (wait > TimeSpan.Zero) Thread.Sleep(wait);
                }
            }, ev => { if (!quiet) Console.WriteLine($"      -> {ev}"); });
            Console.WriteLine($"    {csv.Rows} ticks, {ncl.Records} raw frames, unpack mismatches {sim.UnpackMismatches}, final status {sim.Ctl.TempStatus}, errors {Controller.Describe(sim.Ctl.ErrorStatus)}");
            Console.WriteLine($"    -> {csv.Path}, {ncl.Path}");
            if (sim.UnpackMismatches > 0) worst = 1;
            sim.Dispose();
        }
        return worst;
    }

    static void PrintRow(Simulation s)
    {
        static string F(double v) => double.IsNaN(v) ? "  NaN" : v.ToString("0.0", CultureInfo.InvariantCulture).PadLeft(5);
        if (s.TimeSeconds == 0 || Math.Abs(s.TimeSeconds % 30) < 1e-9)
            Console.WriteLine("       t   plant  temp1  temp2   ctrl  heat cool  hFb cFb  err   status        act  errRem  dbRem");
        Console.WriteLine($"  {s.TimeSeconds,6:0.0} {F(s.Plant.Temperature)}  {F(s.Temp1Raw)}  {F(s.Temp2Raw)}  {F(s.Ctl.ControlTemp)}   {(s.Ctl.HeatingCmd ? 1 : 0)}    {(s.Ctl.CoolingCmd ? 1 : 0)}    {(s.HeaterOn ? 1 : 0)}   {(s.CoolerOn ? 1 : 0)}  {(uint)s.Ctl.ErrorStatus,4}  {s.Ctl.TempStatus,-13} {s.Ctl.ActiveSensor}   {s.Ctl.ErrorRemainMs,5:0}  {s.Ctl.DbRemainMs,5:0}");
    }

    /// <summary>Linux receive side: reassemble TempCtl BAMs from a live bus with CanTp_RxFeed and print the decoded values.</summary>
    static int Receive(string iface, MessageTable table, int seconds)
    {
        table.Define(Simulation.Slot);                      // SA placeholder 0xFE: accept any source
        using var bus = SocketCan.Open(iface);
        var rec = new byte[Simulation.RecordSize];
        var values = new double[table.SignalCount];
        var deadline = DateTime.UtcNow.AddSeconds(seconds);
        int frames = 0, messages = 0;
        Console.WriteLine($"listening on {iface} for {seconds} s ...");
        while (DateTime.UtcNow < deadline)
        {
            if (!bus.TryReceive(rec, 200)) continue;
            frames++;
            if (CanTpNative.RxFeed(Simulation.Slot, rec, values) != CanTpNative.Found) continue;
            messages++;
            Console.WriteLine($"#{messages}: " + string.Join("  ", Enumerable.Range(0, table.SignalCount).Select(i =>
                $"{table.Signals[i]}={values[i].ToString("0.###", CultureInfo.InvariantCulture)}")));
        }
        Console.WriteLine($"{frames} frames, {messages} TempCtl messages");
        return messages > 0 ? 0 : 1;
    }

    static void PrintHelp()
    {
        Console.WriteLine("""
            TempSim.Cli - TempCtl v2 + CanTp closed-loop simulator (console)

              TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N]
                          [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE]
              TempSim.Cli --list                        built-in scenarios
              TempSim.Cli --rx IFACE [--seconds N]      Linux: decode TempCtl BAMs from a CAN interface (CanTp_RxFeed)

            Outputs per scenario: DIR/NAME.csv (state per tick) and DIR/NAME.ncl (NI-XNET logfile of the BAM frames).
            --config FILE   JSON with plant/sensor/relay/controller/CAN settings (see docs)
            --can IFACE     Linux only: also transmit every frame on a SocketCAN interface (e.g. can1)
            --realtime      pace the run at the simulation period instead of running flat out
            --native-dir    folder holding tempctl/cantp libraries (default: next to the executable)
            """);
    }
}
