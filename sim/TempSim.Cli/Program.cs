using System.Globalization;
using TempSim.Core;
using TempSim.Core.Native;

namespace TempSim.Cli;

/// <summary>
/// TempSim.Cli - cross-platform console simulator for TempCtl v4 + CanTp.
///
///   TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N] [--every S]
///               [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE.json|FILE.ecd]
///   TempSim.Cli --list
///   TempSim.Cli --rx IFACE [--seconds N]     (Linux: listen on a CAN interface and decode TempCtl BAMs with CanTp_RxFeed)
///
/// Every scenario writes DIR/NAME.csv (state table, one row per tick) and DIR/NAME.ncl (NI-XNET logfile of the
/// J1939 BAM frames), prints a state line every --every seconds (0 = every tick) and checks the scenario's built-in
/// expectations. Exit code 1 when a scenario had unpack mismatches or a failed expectation. The CSV is bit-identical
/// on Windows and Linux for the same config: that is the cross-platform proof of the tempctl + cantp libraries.
/// </summary>
static class Program
{
    /// <summary>TempSim's own version (sim\Directory.Build.props), independent of tempctl / cantp.</summary>
    static readonly string AppVersion = typeof(Program).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";

    static int Main(string[] args)
    {
        string? scenario = "all", outDir = "out", configFile = null, canIface = null, nativeDir = null, table = null, rxIface = null;
        int? seconds = null, periodMs = null;
        double every = 5;
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
                case "--every": every = double.Parse(Next(), CultureInfo.InvariantCulture); break;
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
        Console.WriteLine($"TempSim.Cli {AppVersion}  ({NativeLoader.Describe()})");

        var baseConfig = configFile != null ? SimConfig.Load(configFile) : new SimConfig();
        if (seconds != null) baseConfig.Seconds = seconds.Value;
        if (periodMs != null) baseConfig.PeriodMs = periodMs.Value;
        if (canIface != null) baseConfig.Can.Interface = canIface;

        if (list)
        {
            foreach (var s in Scenario.BuiltIn(baseConfig)) Console.WriteLine($"  {s.Name,-16} {s.Description}");
            return 0;
        }
        MessageTable msgTable;
        try { msgTable = MessageTable.LoadShipped(table); }
        catch (Exception ex) { Console.Error.WriteLine("ERROR: " + ex.Message); return 2; }
        Console.WriteLine($"  message table {msgTable.Source}: {msgTable.SignalCount} signals, {msgTable.Length}-byte {msgTable.Transport}" + (msgTable.Flat != null ? " (CanTp_DefineFlat)" : ""));
        Console.WriteLine("  shipped JSON/ECD pair checked against this build's generated layout");
        if (rxIface != null) return Receive(rxIface, msgTable, baseConfig.Seconds);
        // A real CAN bus must follow wall time; an accelerated run would overlap BAM transfers.
        if (!string.IsNullOrEmpty(baseConfig.Can.Interface)) realtime = true;

        if (string.Equals(scenario, "fixture", StringComparison.OrdinalIgnoreCase))
        {
            baseConfig.Fixture.Enabled = true;
            baseConfig.Profile.Clear(); baseConfig.Companion = null;
        }
        var scenarios = string.Equals(scenario, "fixture", StringComparison.OrdinalIgnoreCase)
            ? new[] { new Scenario("fixture", "Supply inlet and UUT outlet with actuation heating", baseConfig) }
            : string.Equals(scenario, "all", StringComparison.OrdinalIgnoreCase)
            ? Scenario.BuiltIn(baseConfig)
            : new[] { Scenario.Find(scenario!, baseConfig) ?? throw new ArgumentException($"no scenario '{scenario}' (try --list)") };
        if (seconds != null) foreach (var sc in scenarios) sc.Config.Seconds = seconds.Value;   // --seconds overrides the scenario's own length
        Directory.CreateDirectory(outDir);
        int worst = 0, checkedTotal = 0, failedTotal = 0, remainingTotal = 0;
        foreach (var sc in scenarios)
        {
            Console.WriteLine();
            Console.WriteLine($"=== {sc.Name}: {sc.Description}");
            using var csv = new CsvLogger(Path.Combine(outDir, sc.Name + ".csv"));
            using var ncl = new NclWriter(Path.Combine(outDir, sc.Name + ".ncl"));
            CsvLogger? csv1 = sc.Config.Companion != null ? new CsvLogger(Path.Combine(outDir, sc.Name + ".zone1.csv")) : null;
            double nextPrint = 0;
            var wallClock = System.Diagnostics.Stopwatch.StartNew();
            var res = sc.Run(msgTable, s =>
            {
                csv.Log(s); ncl.Log(s);
                if (csv1 != null && s.Companion != null) csv1.Log(s.Companion);
                if (!quiet && s.TimeSeconds + 1e-9 >= nextPrint)
                {
                    PrintRow(s);
                    nextPrint = every <= 0 ? 0 : nextPrint + every;
                }
            }, ev => { if (!quiet) Console.WriteLine($"      -> {ev}"); }, beforeStep: s =>
            {
                if (!realtime) return;
                var wait = TimeSpan.FromMilliseconds((s.Tick + 1) * (double)s.Config.PeriodMs) - wallClock.Elapsed;
                if (wait > TimeSpan.Zero) Thread.Sleep(wait);
            });
            var sim = res.Sim;
            csv1?.Dispose();
            Console.WriteLine($"    {csv.Rows} ticks, {ncl.Records} raw frames, unpack mismatches {sim.UnpackMismatches}, expectations {res.Checked - res.Failed.Count}/{res.Checked} ok, " +
                              $"final status {Controller.Describe(sim.Ctl.Status)}, warning {Controller.Describe(sim.Ctl.Warning)}");
            foreach (var f in res.Failed) Console.WriteLine($"    FAIL {f}");
            if (!res.Complete) Console.WriteLine($"    INCOMPLETE: {res.RemainingExpectations} expectations were not reached; increase --seconds.");
            Console.WriteLine($"    -> {csv.Path}, {ncl.Path}{(csv1 != null ? ", " + csv1.Path : "")}");
            checkedTotal += res.Checked; failedTotal += res.Failed.Count;
            remainingTotal += res.RemainingExpectations;
            if (!res.Complete) worst = 2;
            if (worst == 0 && (sim.UnpackMismatches > 0 || res.Failed.Count > 0)) worst = 1;
            sim.Dispose();
        }
        Console.WriteLine();
        Console.WriteLine($"{scenarios.Count} scenario(s): {checkedTotal - failedTotal}/{checkedTotal} expectations ok, {remainingTotal} not reached, {(worst == 0 ? "ALL OK" : worst == 2 ? "INCOMPLETE" : "FAILED")}");
        return worst;
    }

    static int s_rows;
    static void PrintRow(Simulation s)
    {
        static string F(double v) => double.IsNaN(v) ? "  NaN" : v.ToString("0.0", CultureInfo.InvariantCulture).PadLeft(5);
        static string R(double v) => v.ToString("0", CultureInfo.InvariantCulture).PadLeft(5);
        var k = s.Ctl;
        if (s_rows++ % 25 == 0)
            Console.WriteLine("       t   plant  temp1  temp2   ctrl  dH dC  hf cf  status                             warning                      act  dbRem aspRem cmpRem hfbRem cfbRem  acc1  acc2 ev1 ev2 hc  perm ocRem st");
        Console.WriteLine($"  {s.TimeSeconds,6:0.0} {F(s.Plant.Temperature)}  {F(s.Temp1Raw)}  {F(s.Temp2Raw)}  {F(k.ControlTemp)}   {(k.DoHeater ? 1 : 0)}  {(k.DoCooler ? 1 : 0)}   {(s.HeaterOn ? 1 : 0)}  {(s.CoolerOn ? 1 : 0)}  " +
                          $"{Controller.Describe(k.Status),-34} {Controller.Describe(k.Warning),-28} {k.ActiveSensor}  {R(k.DeadbandRemainMs)} {R(k.AtSetPtRemainMs)} {R(k.CompareRemainMs)} {R(k.HeaterFbRemainMs)} {R(k.CoolerFbRemainMs)} {R(k.Temp1OorAccumMs)} {R(k.Temp2OorAccumMs)} {k.Temp1OorEventsPerHour,3} {k.Temp2OorEventsPerHour,3}  {(k.InitialHcFlag ? 1 : 0)}  {(double.IsNaN(k.RunPermissive) ? " NaN" : (s.RunPermissive ? 1 : 0).ToString().PadLeft(4))} {R(k.OperatingConditionRemainMs)} {(k.Started ? 1 : 0),2}");
    }

    /// <summary>Linux receive side: reassemble TempCtl BAMs from a live bus with CanTp_RxFeed and print the decoded values.</summary>
    static int Receive(string iface, MessageTable table, int seconds)
    {
        table.Define(0);                                    // SA placeholder 0xFE: accept any source
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
            if (CanTpNative.RxFeed(0, rec, values) != CanTpNative.Found) continue;
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
            TempSim.Cli - TempCtl v4 + CanTp closed-loop simulator (console)

              TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N] [--every S]
                          [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE.json|FILE.ecd]
              TempSim.Cli --list                        built-in scenarios (the v3.0.0 handoff's 15 under the v4 lifecycle + the 14 of the v4.0.0 handoff)
              TempSim.Cli --rx IFACE [--seconds N]      Linux: decode TempCtl BAMs from a CAN interface (CanTp_RxFeed)

            Outputs per scenario: DIR/NAME.csv (state per tick) and DIR/NAME.ncl (NI-XNET logfile of the BAM frames);
            two-zone scenarios also write DIR/NAME.zone1.csv. Each scenario's expectations are checked and reported;
            exit code 1 on any failed expectation or unpack mismatch, 2 when expectations were not reached.
            --every S       print a state line every S seconds (default 5; 0 = every tick)
            --scenario fixture  run fixture physics; also writes NAME.fixture.csv (inlet/outlet/delta/heat/flow)
            --config FILE   JSON with plant/sensor/relay/controller/CAN settings and an optional temperature profile (see docs)
            --can IFACE     Linux only: transmit scheduled messages on SocketCAN (e.g. can1); implies --realtime
            --realtime      pace the run at the simulation period instead of running flat out
            --native-dir    folder holding tempctl/cantp libraries (default: next to the executable)
            --table FILE    the CanTp definition of the diagnostics message: TempCtl.json (dbc2tables) or tempctl.ecd
                            (CanTp_DefineFlat); the shipped pair next to the executable is cross-checked at start-up
            Lifecycle: a run issues TcInit then TcStart (StartOnInit, default true; a scenario may leave the zone
            IdleStopped); RunPermissive is the live input of every TcCheckTemp; scenario events call Start / Stop /
            Reset and flip the permissive. CSV columns run_perm, oc_rem_ms, started carry the new diagnostics.
            """);
    }
}
