using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>A scripted run: a config plus timed events. The built-in ones cover the v2 features.</summary>
public sealed class Scenario
{
    public string Name { get; }
    public string Description { get; }
    public SimConfig Config { get; }
    public List<(double AtSeconds, string Label, Action<Simulation> Apply)> Events { get; } = new();

    public Scenario(string name, string description, SimConfig config) { Name = name; Description = description; Config = config; }

    public Scenario At(double seconds, string label, Action<Simulation> apply) { Events.Add((seconds, label, apply)); return this; }

    /// <summary>Run to completion; events fire before the tick whose time reaches them.</summary>
    public Simulation Run(MessageTable? table, Action<Simulation>? observer = null, Action<string>? onEvent = null)
    {
        var sim = new Simulation(Config, table);
        if (observer != null) sim.AddObserver(observer);
        var pending = new Queue<(double, string, Action<Simulation>)>(Events.OrderBy(e => e.AtSeconds));
        int ticks = Config.Seconds * 1000 / Config.PeriodMs;
        for (int i = 1; i <= ticks; i++)
        {
            double t = i * Config.PeriodMs / 1000.0;
            while (pending.Count > 0 && pending.Peek().Item1 <= t)
            {
                var (_, label, apply) = pending.Dequeue();
                apply(sim);
                onEvent?.Invoke($"{t,7:0.0} s  {label}");
            }
            sim.Step();
        }
        return sim;
    }

    // ---- built-in scenarios ---------------------------------------------------------------------------
    public static IReadOnlyList<Scenario> BuiltIn(SimConfig? baseConfig = null)
    {
        SimConfig Base() => (baseConfig ?? new SimConfig()).Clone();
        var list = new List<Scenario>();

        var warm = Base();
        list.Add(new Scenario("warmup", "Cold start at ambient: filter warm-up, heat pending, heating to setpoint, then in-band cycling.", warm));

        var step = Base();
        list.Add(new Scenario("setpoint-step", "Setpoint 50 -> 30 at 60 s (cooler engages), -> 70 at 90 s.", step)
            .At(60, "Setpoint 30", s => s.Ctl.Set(TcSignal.Setpoint, 30))
            .At(90, "Setpoint 70", s => s.Ctl.Set(TcSignal.Setpoint, 70)));

        var fail = Base();
        list.Add(new Scenario("sensor-failover", "Sensor 1 opens (NaN) at 40 s: relays drop, T1 fails after ErrorTimeout, control fails over to sensor 2 (degraded). Sensor 1 returns at 60 s but stays failed until the operator reset at 80 s.", fail)
            .At(40, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .At(60, "Sensor 1 back", s => s.Sensor1.Fault = SensorFault.None)
            .At(80, "Operator reset", s => s.Reset()));

        var both = Base();
        list.Add(new Scenario("both-sensors-fail", "Sensor 2 sticks at 200 at 30 s (T2 hi, degraded), then sensor 1 opens at 50 s: stopped. Reset at 70 s with both healthy.", both)
            .At(30, "Sensor 2 stuck at 200", s => { s.Sensor2.Fault = SensorFault.StuckValue; s.Sensor2.StuckValue = 200; })
            .At(50, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .At(70, "Sensors healthy, reset", s => { s.Sensor1.Fault = SensorFault.None; s.Sensor2.Fault = SensorFault.None; s.Reset(); }));

        var dis = Base();
        list.Add(new Scenario("disagree", "Sensor 2 drifts +8 deg at 40 s: disagreement bit after ErrorTimeout, control continues on sensor 1.", dis)
            .At(40, "Sensor 2 offset +8", s => s.Sensor2.Offset = 8.0));

        var fb = Base();
        list.Add(new Scenario("feedback-fault", "Heater relay sticks open at 30 s (feedback never follows the command): heater feedback bit, operation continues. Cooler sticks closed at 80 s: cooler feedback bit, and the cooler drives the plant below LoLimit until both sensors fail lo and the controller stops.", fb)
            .At(30, "Heater stuck open", s => s.Heater.StuckOpen = true)
            .At(80, "Cooler stuck closed", s => s.Cooler.StuckClosed = true));

        var single = Base();
        single.Controller.Temp2Enable = false;
        single.Controller.FeedbackEnable = false;
        list.Add(new Scenario("single-sensor", "Minimum system (one sensor, no feedback): sensor opens at 50 s -> stopped after ErrorTimeout; reset at 70 s.", single)
            .At(50, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .At(56, "Sensor 1 back", s => s.Sensor1.Fault = SensorFault.None)
            .At(70, "Operator reset", s => s.Reset()));

        return list;
    }

    public static Scenario? Find(string name, SimConfig? baseConfig = null) =>
        BuiltIn(baseConfig).FirstOrDefault(s => string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
}
