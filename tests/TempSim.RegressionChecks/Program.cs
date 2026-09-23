using System.Diagnostics;
using System.Text.Json;
using TempSim.Core;
using TempSim.Core.Native;

int count = 0;
void Check(bool ok, string label)
{
    if (!ok) throw new Exception(label);
    count++; Console.WriteLine("ok " + label);
}
void Reject(Action action, string label)
{
    bool rejected = false;
    try { action(); }
    catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or ArgumentException or IOException) { rejected = true; }
    Check(rejected, label);
}
var table = MessageTable.LoadShipped();
Console.WriteLine(NativeLoader.Describe());

// Statistical properties catch the historical [-1,3) scaling bug without duplicating the PRNG implementation.
var rng = new Rng(123);
double sum = 0, min = 1, max = -1;
for (int i = 0; i < 100_000; i++) { double v = rng.NextSigned(); sum += v; min = Math.Min(min, v); max = Math.Max(max, v); }
Check(min >= -1 && max < 1, "noise stays within the configured symmetric amplitude");
Check(min < -0.99 && max > 0.99 && Math.Abs(sum / 100_000) < 0.01, "noise covers both tails and has zero mean");

SimConfig Config() => new()
{
    StartOnInit = false, Sensor1 = new() { Offset = 0 }, Sensor2 = new() { Offset = 0 },
    Controller = new() { Temp2Enable = false, FeedbackEnable = false, FilterPoints = 1 },
    Plant = new() { Initial = 20, Ambient = 20, LagPerSec = 0, HeatRate = 10 }
};
var cfg = Config(); cfg.StartTickMs = uint.MaxValue - 499;
using (var sim = new Simulation(cfg, table))
{
    for (int i = 0; i < 9; i++) sim.Step();
    uint eventMs = 0;
    sim.Step(s => { eventMs = s.NowMs; s.Sensor1.Fault = SensorFault.Open; s.Start(); });
    Check(eventMs == 500 && sim.TimeSeconds == 1, "event uses its named sample time, including U32 wrap");
    Check(sim.Ctl.Temp1OorAccumMs == 0, "Start cannot charge the preceding stopped interval");
    sim.Step(); Check(sim.Ctl.Temp1OorAccumMs == 100, "first elapsed active interval charges 100 ms");
    sim.Step(s => s.Reset()); Check(sim.Ctl.Temp1OorAccumMs == 0, "Reset plus Start uses the current sample time");
}
cfg = Config(); cfg.StartOnInit = true;
using (var sim = new Simulation(cfg, table))
{
    for (int i = 0; i < 6; i++) sim.Step();
    Check(sim.HeaterOn, "plant timing probe starts while heating");
    double previous = sim.Plant.Temperature;
    sim.Step(s => s.Stop());
    Check(sim.Plant.Temperature == previous + 1 && !sim.HeaterOn, "Stop at sample time preserves heat delivered during the previous interval");
    sim.Step(); Check(sim.Plant.Temperature == previous + 1, "stopped relay supplies no heat in the following interval");
}
cfg = Config(); cfg.Companion = Config();
using (var sim = new Simulation(cfg, table))
{
    sim.Step(s =>
    {
        Check(s.NowMs == 100 && s.Companion!.NowMs == 100, "all zone clocks advance before lifecycle events");
        s.Companion!.Sensor1.Fault = SensorFault.Open; s.Companion.Start();
    });
    Check(sim.Companion!.Ctl.Temp1OorAccumMs == 0 && !sim.Ctl.Started, "companion Start is timestamped correctly and leaves the other zone idle");
}
var truncated = new Scenario("partial", "test", new SimConfig { Seconds = 0 })
    .Expect(1, "future", _ => true).Run(table);
Check(!truncated.Complete && truncated.Checked == 0 && truncated.RemainingExpectations == 1, "zero-duration scenario reports unreached expectations");
truncated.Sim.Dispose();

// Contract identity, not row count alone, is required even when both external files have changed together.
MessageTable CopyTable() => JsonSerializer.Deserialize<MessageTable>(JsonSerializer.Serialize(table))!;
var bad = CopyTable(); (bad.Signals[0], bad.Signals[1]) = (bad.Signals[1], bad.Signals[0]);
Reject(bad.ValidateContract, "reject reordered 28-signal table");
bad = CopyTable(); bad.SigDefs[0][4] *= 2;
Reject(bad.ValidateContract, "reject changed wire scaling with unchanged row count");
bad = CopyTable(); bad.MsgDef[2]++;
Reject(bad.ValidateContract, "reject changed message size");
bad = CopyTable(); bad.SigDefs[0] = new double[7];
Reject(bad.ValidateContract, "reject malformed signal row");
Check(MessageTable.LoadEcd(MessageTable.DefaultEcdPath!).DifferenceFrom(table) == null, "shipped JSON and ECD agree with the embedded contract");

string scratch = Path.Combine(Path.GetTempPath(), "tempsim-regression-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(scratch);
try
{
    File.Copy(MessageTable.DefaultPath, Path.Combine(scratch, "TempCtl.json"));
    Reject(() => MessageTable.LoadShipped(MessageTable.DefaultEcdPath, scratch), "explicit ECD override cannot bypass a missing shipped ECD");
    File.WriteAllBytes(Path.Combine(scratch, "tempctl.ecd"), new byte[8]);
    Reject(() => MessageTable.LoadShipped(directory: scratch), "reject corrupt shipped ECD");
    var nclPath = Path.Combine(scratch, "timing.ncl");
    using (var sim = new Simulation(Config(), table))
    using (var ncl = new NclWriter(nclPath))
    {
        for (int i = 0; i < 30; i++) { sim.Step(); ncl.Log(sim); }
        Check(ncl.Records == 3 * sim.FrameCount && sim.Ticks == 30, "CAN message cycle is independent of 100 ms controller checks");
    }
    byte[] log = File.ReadAllBytes(nclPath);
    ulong prev = 0; bool ordered = true;
    for (int o = 12; o < log.Length; o += Simulation.RecordSize)
    { ulong ts = BitConverter.ToUInt64(log, o); ordered &= ts >= prev; prev = ts; }
    Check(ordered, "NCL frame timestamps never go backwards between BAMs");
    var sentAt = new List<double>(); var watch = Stopwatch.StartNew();
    using (var sender = new PacedFrameSender(_ => sentAt.Add(watch.Elapsed.TotalMilliseconds)))
    {
        byte[] frames = log.AsSpan(12, 8 * Simulation.RecordSize).ToArray();
        sender.Send(frames);
        Reject(() => sender.Send(frames), "live sender rejects overlapping transfers instead of accumulating a queue");
    }
    Check(sentAt.Count == 8 && sentAt[^1] - sentAt[0] >= 340, "live sender honors the complete transfer's 350 ms span");
    var failedSender = new PacedFrameSender(_ => throw new IOException("bus disconnected"));
    failedSender.Send(log.AsSpan(12, Simulation.RecordSize).ToArray());
    Reject(failedSender.Dispose, "live bus write failures propagate");
}
finally { Directory.Delete(scratch, recursive: true); }
cfg = Config(); cfg.Can.MessagePeriodMs = 100;
Reject(() => { using var s = new Simulation(cfg, table); }, "reject a CAN cycle shorter than one BAM");

// R10 / 13.7.4: dedicated threads enter the actual native library on distinct zones.
// Compare every diagnostic at every step against a serial baseline (bitwise rolling hash).
ulong RunZone(int zone)
{
    var c = new Controller(zone);
    var settings = new SimConfig.ControllerConfig { Setpoint = 35 + zone, FeedbackEnable = false };
    c.LoadSetup(settings); c.Init(0); c.Start(0, true);
    ulong hash = 14695981039346656037;
    for (int i = 1; i <= 4000; i++)
    {
        uint now = (uint)(i * 100);
        if (i % 197 == 0) c.Reset(now);
        if (i % 151 == 0) c.Stop(now);
        if (i % 41 == 0) c.Start(now, true);
        if (i % 313 == 0) c.Init(now);
        double t = i % 61 < 2 ? double.NaN : 20 + (i + zone) % 55;
        c.CheckTemp(now, t, 30 + zone, false, false, i % 137 != 0);
        foreach (double d in c.Diag) hash = unchecked((hash ^ (ulong)BitConverter.DoubleToInt64Bits(d)) * 1099511628211);
    }
    return hash;
}
var expected = Enumerable.Range(0, TcConst.MaxZones).Select(RunZone).ToArray();
var actual = new ulong[TcConst.MaxZones];
var errors = new Exception?[TcConst.MaxZones];
using var ready = new CountdownEvent(TcConst.MaxZones);
using var start = new ManualResetEventSlim();
var threads = Enumerable.Range(0, TcConst.MaxZones).Select(zone => new Thread(() =>
{
    ready.Signal(); start.Wait();
    try { actual[zone] = RunZone(zone); } catch (Exception ex) { errors[zone] = ex; }
})).ToArray();
foreach (var thread in threads) thread.Start();
ready.Wait(); start.Set();
foreach (var thread in threads) thread.Join();
Check(errors.All(e => e == null) && expected.SequenceEqual(actual), "16 concurrent zones match every serial diagnostic across 64,000 samples");
Console.WriteLine($"TempSim regression checks: {count} passed, 0 failed");
