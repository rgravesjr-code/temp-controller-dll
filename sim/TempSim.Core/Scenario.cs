using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// A scripted run: a config, timed events (applied before the tick whose time reaches them) and timed
/// expectations (checked after that tick). The built-in ones are the 15 required scenarios of the TempCtl v3.0.0
/// handoff (section 10); their expectations are the release gate of TempSim.Cli.
/// </summary>
public sealed class Scenario
{
    public string Name { get; }
    public string Description { get; }
    public SimConfig Config { get; }
    public List<(double AtSeconds, string Label, Action<Simulation> Apply)> Events { get; } = new();
    public List<(double AtSeconds, string Label, Func<Simulation, bool> Check)> Expectations { get; } = new();

    public Scenario(string name, string description, SimConfig config) { Name = name; Description = description; Config = config; }

    public Scenario At(double seconds, string label, Action<Simulation> apply) { Events.Add((seconds, label, apply)); return this; }
    public Scenario Expect(double seconds, string label, Func<Simulation, bool> check) { Expectations.Add((seconds, label, check)); return this; }

    public sealed class Result
    {
        public Simulation Sim { get; init; } = null!;
        public int Checked { get; set; }
        public List<string> Failed { get; } = new();
    }

    /// <summary>Run to completion; events fire before the tick whose time reaches them, expectations after it.</summary>
    public Result Run(MessageTable? table, Action<Simulation>? observer = null, Action<string>? onEvent = null)
    {
        var sim = new Simulation(Config, table);
        var result = new Result { Sim = sim };
        if (observer != null) sim.AddObserver(observer);
        var pending = new Queue<(double, string, Action<Simulation>)>(Events.OrderBy(e => e.AtSeconds));
        var checks = new Queue<(double, string, Func<Simulation, bool>)>(Expectations.OrderBy(e => e.AtSeconds));
        int ticks = Config.Seconds * 1000 / Config.PeriodMs;
        for (int i = 1; i <= ticks; i++)
        {
            double t = i * Config.PeriodMs / 1000.0;
            while (pending.Count > 0 && pending.Peek().Item1 <= t + 1e-9)
            {
                var (_, label, apply) = pending.Dequeue();
                apply(sim);
                onEvent?.Invoke($"{t,7:0.0} s  {label}");
            }
            sim.Step();
            while (checks.Count > 0 && checks.Peek().Item1 <= t + 1e-9)
            {
                var (_, label, check) = checks.Dequeue();
                bool ok;
                try { ok = check(sim); } catch (Exception ex) { ok = false; label += $" [{ex.GetType().Name}: {ex.Message}]"; }
                result.Checked++;
                if (!ok) result.Failed.Add($"{t:0.0} s: {label}");
                onEvent?.Invoke($"{t,7:0.0} s  {(ok ? "ok  " : "FAIL")} {label}   [status {Controller.Describe(sim.Ctl.Status)}, warning {Controller.Describe(sim.Ctl.Warning)}, relays {(sim.Ctl.DoHeater ? 1 : 0)}/{(sim.Ctl.DoCooler ? 1 : 0)}]");
            }
        }
        return result;
    }

    // ---- built-in scenarios (TEMPCTL-v3.0.0-HANDOFF section 10) ------------------------------------------
    public static IReadOnlyList<Scenario> BuiltIn(SimConfig? baseConfig = null)
    {
        SimConfig Base() { var config = (baseConfig ?? new SimConfig()).Clone(); config.Fixture.Enabled = false; return config; }
        static SimConfig Single(SimConfig c) { c.Controller.Temp2Enable = false; c.Controller.FeedbackEnable = false; return c; }
        /// exact readings: no sensor offsets / noise, no Temp2Offset, values from a profile
        static SimConfig Flat(SimConfig c) { c.Sensor1.Offset = 0; c.Sensor2.Offset = 0; c.Controller.Temp2Offset = 0; c.Profile.Clear(); return c; }
        static void P(SimConfig c, double at, double t1, double? t2 = null) => c.Profile.Add(new SimConfig.ProfilePoint(at, t1, t2 ?? t1));
        var list = new List<Scenario>();
        SimConfig c;

        // 1 single-sensor heat-up
        c = Single(Base()); c.Seconds = 60;
        list.Add(new Scenario("heat-up", "S1 Single sensor, cold start out of band: HeatPending -> HeaterON -> at-setpoint countdown -> TempAtSetPt -> in-band idle.", c)
            .Expect(0.1, "HeatPending on the first tick", s => s.Ctl.Status == TcStatus.HeatPending && !s.Ctl.DoHeater && s.Ctl.DeadbandRemainMs == 500)
            .Expect(0.5, "still pending 100 ms before the deadband timeout", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.DeadbandRemainMs == 100)
            .Expect(0.6, "HeaterON after DeadbandTimeout", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && !s.Ctl.InitialHcFlag)
            .Expect(20, "reached the setpoint, released after AtSetPtTimeout, idle in band", s => s.SeenStatuses.Contains(TcStatus.TempAtSetPt) && s.Ctl.InitialHcFlag && !s.Ctl.IsFault)
            .Expect(60, "no fault, no warning through the cycling", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning));

        // 2 setpoint change by re-Init while heating
        c = Base(); c.Seconds = 60;
        list.Add(new Scenario("setpoint-reinit", "S2 Setpoint 50 -> 70 by TcInit at 3 s while heating: relays kept, control continues to the new setpoint; 70 -> 40 at 40 s while idle: cooler engages.", c)
            .At(3.0, "TcInit setpoint 70", s => s.ReInit(k => k.Setpoint = 70))
            .Expect(3.0, "heater kept across the re-Init, bands moved", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && s.Ctl.HiBand == 75 && s.Ctl.AtSetPtRemainMs == 0)
            .Expect(20, "reached 70 and released", s => s.SeenStatuses.Contains(TcStatus.TempAtSetPt) && s.Ctl.HiBand == 75)
            .At(40.0, "TcInit setpoint 40 (the plant is at 71, heating to 70)", s => s.ReInit(k => k.Setpoint = 40))
            .Expect(40.0, "heater kept by the re-Init; the new logic starts the at-setpoint countdown", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && s.Ctl.HiBand == 45 && s.Ctl.AtSetPtRemainMs == 500)
            .Expect(40.5, "released after AtSetPtTimeout, cool pending (above the new HiBand)", s => s.Ctl.Status == TcStatus.CoolPending && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .Expect(41.0, "CoolerON after DeadbandTimeout", s => s.Ctl.Status == TcStatus.CoolerON && s.Ctl.DoCooler)
            .Expect(60, "no fault", s => !s.Ctl.IsFault));

        // 3 cool-down
        c = Base(); c.Plant.Initial = 70; c.Seconds = 40;
        list.Add(new Scenario("cool-down", "S3 Start at 70 with setpoint 50: CoolPending -> CoolerON -> release at the setpoint after AtSetPtTimeout.", c)
            .Expect(0.1, "CoolPending on the first tick", s => s.Ctl.Status == TcStatus.CoolPending && !s.Ctl.DoCooler)
            .Expect(0.6, "CoolerON after DeadbandTimeout", s => s.Ctl.Status == TcStatus.CoolerON && s.Ctl.DoCooler && !s.Ctl.DoHeater)
            .Expect(20, "released at the setpoint, idle", s => s.SeenStatuses.Contains(TcStatus.TempAtSetPt) && s.Ctl.InitialHcFlag && !s.Ctl.IsFault));

        // 4 chatter immunity: single spike / single NaN in every state
        c = Flat(Single(Base())); c.Seconds = 16;
        P(c, 0, 30); P(c, 3.0, double.NaN); P(c, 3.1, 30); P(c, 3.2, 200); P(c, 3.3, 30); P(c, 3.4, 52); P(c, 3.5, 30);
        P(c, 6.0, 52); P(c, 8.0, double.NaN); P(c, 8.1, 52); P(c, 8.2, 30); P(c, 8.3, 52); P(c, 8.4, 200); P(c, 8.5, 52);
        P(c, 12.0, 70); P(c, 14.0, double.NaN); P(c, 14.1, 70);
        list.Add(new Scenario("chatter", "S4 One-sample NaN, spikes out of range and spikes across the band while heating, idle and cooling: no relay transition, averages untouched.", c)
            .Expect(3.0, "heating: a NaN sample holds the heater (warning only)", s => s.Ctl.DoHeater && s.Ctl.Status == TcStatus.HeaterON && s.Ctl.Warning == TcWarning.Temp1OutOfRange)
            .Expect(3.2, "heating: a 200 spike holds the heater", s => s.Ctl.DoHeater && s.Ctl.Status == TcStatus.HeaterON)
            .Expect(3.4, "heating: one sample at the setpoint only starts the countdown", s => s.Ctl.DoHeater && s.Ctl.AtSetPtRemainMs == 500)
            .Expect(3.5, "back below: countdown cleared, still heating", s => s.Ctl.DoHeater && s.Ctl.AtSetPtRemainMs == 0)
            .Expect(6.5, "released after 500 ms at the setpoint", s => s.Ctl.Status == TcStatus.TempAtSetPt && !s.Ctl.DoHeater)
            .Expect(8.0, "idle: a NaN sample changes nothing", s => s.Ctl.Status == TcStatus.TempAtSetPt && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .Expect(8.2, "idle: one sample below the band only starts the countdown", s => s.Ctl.Status == TcStatus.HeatPending && !s.Ctl.DoHeater)
            .Expect(8.3, "back in band: countdown cleared", s => s.Ctl.Status == TcStatus.TempAtSetPt && s.Ctl.DeadbandRemainMs == 0)
            .Expect(8.4, "idle: a 200 spike changes nothing", s => s.Ctl.Status == TcStatus.TempAtSetPt && !s.Ctl.DoCooler)
            .Expect(10.0, "average holds only in-range samples (52); four out-of-range events counted", s => Math.Abs(s.Ctl.Temp1Avg - 52) < 1e-9 && s.Ctl.Temp1OorEventsPerHour == 4)
            .Expect(12.5, "CoolerON after 500 ms above the band", s => s.Ctl.Status == TcStatus.CoolerON && s.Ctl.DoCooler)
            .Expect(14.0, "cooling: a NaN sample holds the cooler", s => s.Ctl.DoCooler && s.Ctl.Status == TcStatus.CoolerON)
            .Expect(16.0, "no fault anywhere", s => !s.Ctl.IsFault));

        // 5 flickering sensor: leaky accumulator
        c = Flat(Single(Base())); c.Seconds = 40;
        P(c, 0, 50);
        foreach (var g in new[] { 2.0, 3.0, 4.0 }) { P(c, g, double.NaN); P(c, g + 0.1, 50); }          // sparse 1-tick glitches
        for (int k = 0; k < 18; k++) { P(c, 5.0 + 0.4 * k, 200); P(c, 5.3 + 0.4 * k, 50); }             // 75 % duty: 3 ticks out, 1 in
        P(c, 12.0, 50);
        for (int k = 0; k < 135; k++) { P(c, 13.0 + 0.2 * k, 200); P(c, 13.1 + 0.2 * k, 50); }          // 50 % duty: 1 out, 1 in
        list.Add(new Scenario("flicker", "S5 Leaky accumulator (DRAIN 0.5, Amendment A): sparse glitches never fail; 75 % duty in/out of range fails at ~1.6 x ErrorTimeout; after a reset 50 % duty fails at ~4 x ErrorTimeout; OorEventsPerHour counts transitions.", c)
            .Expect(2.0, "glitch charges one tick", s => s.Ctl.Temp1OorAccumMs == 100 && s.Ctl.Warning == TcWarning.Temp1OutOfRange)
            .Expect(2.1, "and drains half a tick per in-range tick", s => s.Ctl.Temp1OorAccumMs == 50 && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(2.2, "empty after two", s => s.Ctl.Temp1OorAccumMs == 0)
            .Expect(4.5, "three glitches: three events, no failure", s => s.Ctl.Temp1OorEventsPerHour == 3 && !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 0)
            .Expect(7.7, "75 % duty: 1750 ms after 7 cycles (2.7 s)", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1750)
            .Expect(7.9, "1950 ms, not yet failed", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1950)
            .Expect(8.0, "75 % duty fails after 3.0 s (~1.6 x ErrorTimeout 2000)", s => s.Ctl.Status == TcStatus.Temp1FailHigh && s.Ctl.Temp1OorAccumMs == 2050)
            .At(12.0, "operator reset", s => s.Reset())
            .Expect(12.0, "reset: accumulator and events cleared", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 0 && s.Ctl.Temp1OorEventsPerHour == 0)
            .Expect(20.4, "50 % duty: 1950 ms after 38 cycles, not yet failed", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1950)
            .Expect(20.5, "drained to 1900", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1900)
            .Expect(20.6, "50 % duty fails after 7.6 s (~4 x ErrorTimeout 2000)", s => s.Ctl.Status == TcStatus.Temp1FailHigh && s.Ctl.Temp1OorAccumMs == 2000 && s.Ctl.Temp1OorEventsPerHour == 39)
            .Expect(40.0, "latched", s => s.Ctl.Status == TcStatus.Temp1FailHigh));

        // 5b flickering sensor below the boundary: never fails
        c = Flat(Single(Base())); c.Seconds = 40;
        P(c, 0, 50);
        for (int k = 0; k < 60; k++) { P(c, 2.0 + 0.3 * k, 200); P(c, 2.1 + 0.3 * k, 50); }             // 33 % duty: 1 out, 2 in
        P(c, 20.0, 50);
        for (int k = 0; k < 48; k++) { P(c, 21.0 + 0.4 * k, 200); P(c, 21.1 + 0.4 * k, 50); }           // 25 % duty: 1 out, 3 in
        list.Add(new Scenario("flicker-25", "S5b Below the one-third boundary: 33 % duty (1 out, 2 in) nets zero and 25 % duty drains to zero every cycle; the sensor never fails, every excursion is counted.", c)
            .Expect(2.0, "first excursion charges 100", s => s.Ctl.Temp1OorAccumMs == 100)
            .Expect(2.2, "two in-range ticks drain it to zero", s => s.Ctl.Temp1OorAccumMs == 0)
            .Expect(20.0, "33 % duty for 18 s: never above one tick, no failure, 60 events", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs <= 100 && s.Ctl.Temp1OorEventsPerHour == 60 && s.Ctl.Status == TcStatus.TempAtSetPt)
            .Expect(40.0, "25 % duty for 19 s: never above one tick, no failure, 108 events", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs <= 100 && s.Ctl.Temp1OorEventsPerHour == 108 && s.Ctl.Status == TcStatus.TempAtSetPt));

        // 6 control pause while the active sensor is out of range
        c = Flat(Single(Base())); c.Seconds = 6;
        P(c, 0, 30); P(c, 2.0, 52); P(c, 2.2, 200); P(c, 3.0, 52);
        list.Add(new Scenario("control-pause", "S6 Heating with the at-setpoint countdown running, then the sensor leaves range for 0.8 s: relay holds, countdown freezes, resumes on recovery.", c)
            .Expect(0.6, "HeaterON", s => s.Ctl.DoHeater)
            .Expect(2.1, "at-setpoint countdown running (400 ms left)", s => s.Ctl.DoHeater && s.Ctl.AtSetPtRemainMs == 400)
            .Expect(2.5, "paused: relay held, countdown frozen at 400, warning 1", s => s.Ctl.DoHeater && s.Ctl.Status == TcStatus.HeaterON && s.Ctl.AtSetPtRemainMs == 400 && s.Ctl.Warning == TcWarning.Temp1OutOfRange && double.IsNaN(s.Ctl.ControlTemp) == false)
            .Expect(2.9, "still paused after 800 ms out of range (accumulator 800)", s => s.Ctl.DoHeater && s.Ctl.AtSetPtRemainMs == 400 && s.Ctl.Temp1OorAccumMs == 800)
            .Expect(3.2, "resumed: 100 ms left, still heating", s => s.Ctl.DoHeater && s.Ctl.AtSetPtRemainMs == 100)
            .Expect(3.3, "released", s => !s.Ctl.DoHeater && s.Ctl.Status == TcStatus.TempAtSetPt)
            .Expect(6.0, "no fault", s => !s.Ctl.IsFault));

        // 7 failover
        c = Base(); c.Seconds = 120;
        list.Add(new Scenario("failover", "S7 Sensor 1 opens at 40 s: control continues, fails over to (offset-corrected) sensor 2 after ErrorTimeout, warning RunningOnTemp2, no fault. Sensor 2 sticks at 200 at 70 s: BothSensorsFailed. Reset at 90 s.", c)
            .At(40, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .Expect(40.0, "warning Temp1OutOfRange, control holds", s => s.Ctl.Warning == TcWarning.Temp1OutOfRange && !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 100)
            .Expect(41.8, "still on sensor 1 one tick before ErrorTimeout", s => s.Ctl.ActiveSensor == 1 && !s.Ctl.IsFault)
            .Expect(41.9, "failed over to sensor 2, no fault", s => s.Ctl.ActiveSensor == 2 && !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 2000)
            .At(50, "Sensor 1 back", s => s.Sensor1.Fault = SensorFault.None)
            .Expect(50.0, "sensor 1 stays failed: RunningOnTemp2", s => s.Ctl.ActiveSensor == 2 && s.Ctl.Warning == TcWarning.RunningOnTemp2)
            .Expect(60.0, "control on corrected sensor 2", s => Math.Abs(s.Ctl.ControlTemp - (s.Temp2Raw + 0.5)) < 1e-9 && !s.Ctl.IsFault)
            .At(70, "Sensor 2 stuck at 200", s => { s.Sensor2.Fault = SensorFault.StuckValue; s.Sensor2.StuckValue = 200; })
            .Expect(71.8, "no fault one tick before", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.Temp2OutOfRange)
            .Expect(71.9, "BothSensorsFailed, relays off", s => s.Ctl.Status == TcStatus.BothSensorsFailed && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .At(90, "Sensors healthy, reset", s => { s.Sensor1.Fault = SensorFault.None; s.Sensor2.Fault = SensorFault.None; s.Reset(); })
            .Expect(90.0, "reset: sensor 1 active again, no fault", s => s.Ctl.ActiveSensor == 1 && !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(120, "runs on", s => !s.Ctl.IsFault));

        // 8 disagreement, two stages
        c = Base(); c.Seconds = 60;
        list.Add(new Scenario("disagree", "S8 Sensor 2 drifts +8 at 40 s (tolerance 4): warning after TempCompareTimeout/10, agreement at 43 s clears it, drift again at 50 s ends in TempDisagreeFault after the full timeout.", c)
            .At(40, "Sensor 2 offset +8", s => s.Sensor2.Offset = 8.0)
            .Expect(40.1, "averages differ by more than the tolerance: disagreement observed", s => s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CompareRemainMs == 5000)
            .Expect(40.5, "no warning yet (400 ms of the 500 ms warning stage)", s => s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CompareRemainMs == 4600)
            .Expect(40.6, "TempDisagree warning after TempCompareTimeout/10, control continues", s => s.Ctl.Warning == TcWarning.TempDisagree && !s.Ctl.IsFault)
            .At(43, "Sensor 2 back", s => s.Sensor2.Offset = -0.2)
            .Expect(43.1, "averages still apart (4-sample window)", s => s.Ctl.Warning == TcWarning.TempDisagree)
            .Expect(43.2, "agreement clears the warning and the countdown at once", s => s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CompareRemainMs == 0)
            .At(50, "Sensor 2 offset +8 again", s => s.Sensor2.Offset = 8.0)
            .Expect(55.0, "one tick before the fault", s => s.Ctl.Warning == TcWarning.TempDisagree && !s.Ctl.IsFault && s.Ctl.CompareRemainMs == 100)
            .Expect(55.1, "TempDisagreeFault after the full 5000 ms", s => s.Ctl.Status == TcStatus.TempDisagreeFault && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .Expect(60, "latched, warning frozen", s => s.Ctl.Status == TcStatus.TempDisagreeFault && s.Ctl.Warning == TcWarning.TempDisagree));

        // 9 comparison gating
        c = Flat(Base()); c.Controller.TempCompareTimeoutMs = 2000; c.Seconds = 12;
        P(c, 0, 30, 40); P(c, 6.0, 52, 62); P(c, 7.5, 52, 200); P(c, 7.6, 52, 62);
        list.Add(new Scenario("compare-gating", "S9 Sensors disagree by 10 from the start: no comparison until Initial_HC_Flag; then warning; a range excursion restarts it from zero.", c)
            .Expect(5.0, "heating, flag clear, no comparison", s => s.Ctl.Status == TcStatus.HeaterON && !s.Ctl.InitialHcFlag && s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CompareRemainMs == 0)
            .Expect(6.5, "released: flag set", s => s.Ctl.Status == TcStatus.TempAtSetPt && s.Ctl.InitialHcFlag && s.Ctl.CompareRemainMs == 0)
            .Expect(6.6, "comparison observed on the next tick", s => s.Ctl.CompareRemainMs == 2000 && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(6.8, "warning after TempCompareTimeout/10", s => s.Ctl.Warning == TcWarning.TempDisagree)
            .Expect(7.5, "excursion on sensor 2 pauses the comparison", s => s.Ctl.Warning == TcWarning.Temp2OutOfRange && s.Ctl.CompareRemainMs == 0)
            .Expect(7.6, "restarted from zero", s => s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CompareRemainMs == 2000)
            .Expect(7.8, "warning again", s => s.Ctl.Warning == TcWarning.TempDisagree)
            .Expect(9.5, "one tick before the fault", s => !s.Ctl.IsFault && s.Ctl.CompareRemainMs == 100)
            .Expect(9.6, "TempDisagreeFault after the full timeout", s => s.Ctl.Status == TcStatus.TempDisagreeFault));

        // 10 relay feedback
        c = Base(); c.Heater.DelayTicks = 1; c.Controller.LoLimit = -50; c.Controller.HiLimit = 120; c.Seconds = 60;
        list.Add(new Scenario("relay-feedback", "S10 Heater DO answers one tick late: warning on each transition only. Cooler stuck closed at 30 s: CoolerFBMismatch at once, CoolerFBFault after RelayFeedbackTimeout. Reset at 40 s; FeedbackEnable off at 50 s silences a stuck cooler.", c)
            .Expect(0.6, "HeaterON, feedback still 0 but matches the previous command", s => s.Ctl.DoHeater && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(0.7, "one-tick lag: HeaterFBMismatch warning only", s => s.Ctl.Warning == TcWarning.HeaterFBMismatch && s.Ctl.HeaterFbRemainMs == 1000 && !s.Ctl.IsFault)
            .Expect(0.8, "matched again", s => s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.HeaterFbRemainMs == 0)
            .At(30, "Cooler stuck closed (the DO read-back shows it one tick later)", s => s.Cooler.StuckClosed = true)
            .Expect(30.1, "CoolerFBMismatch as soon as the read-back disagrees", s => s.Ctl.Warning == TcWarning.CoolerFBMismatch && s.Ctl.CoolerFbRemainMs == 1000)
            .Expect(31.0, "not yet a fault", s => !s.Ctl.IsFault && s.Ctl.CoolerFbRemainMs == 100)
            .Expect(31.1, "CoolerFBFault after RelayFeedbackTimeout, relays off", s => s.Ctl.Status == TcStatus.CoolerFBFault && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .At(40, "Cooler repaired, reset", s => { s.Cooler.StuckClosed = false; s.Reset(); })
            .Expect(40.1, "reset clears the fault; the repaired contact matches again", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning)
            .At(50, "TcInit FeedbackEnable 0", s => s.ReInit(k => k.FeedbackEnable = false))
            .At(51, "Cooler stuck closed again", s => s.Cooler.StuckClosed = true)
            .Expect(60, "feedback disabled: silent", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CoolerFbRemainMs == 0));

        // 11 config faults
        c = Base(); c.Seconds = 30;
        list.Add(new Scenario("config-fault", "S11 TcInit with a reversed band while running: ConfigFault (relays off); Reset does not clear it; the same setup with Enable = 0 is only ConfigInvalid; a valid Init clears everything.", c)
            .At(10, "TcInit DeadbandHi -1", s => s.ReInit(k => k.DeadbandHi = -1))
            .Expect(10.0, "ConfigFault, relays off", s => s.Ctl.Status == TcStatus.ConfigFault && !s.Ctl.DoHeater && !s.Ctl.DoCooler && s.Ctl.Warning == TcWarning.NoWarning)
            .At(15, "operator reset", s => s.Reset())
            .Expect(15.0, "Reset does not clear ConfigFault", s => s.Ctl.Status == TcStatus.ConfigFault)
            .At(20, "TcInit Enable 0 (band still reversed)", s => s.ReInit(k => k.TempCtrlEnable = false))
            .Expect(20.0, "disabled with ConfigInvalid warning", s => s.Ctl.Status == TcStatus.TempCtrlDisabled && s.Ctl.Warning == TcWarning.ConfigInvalid && !s.Ctl.DoHeater)
            .At(25, "TcInit valid, Enable 1", s => s.ReInit(k => { k.TempCtrlEnable = true; k.DeadbandHi = 5; }))
            .Expect(25.0, "running again, no warning", s => !s.Ctl.IsFault && s.Ctl.Status != TcStatus.TempCtrlDisabled && s.Ctl.Warning == TcWarning.NoWarning));

        // 12 enable / disable
        c = Base(); c.Controller.TempCtrlEnable = false; c.Controller.HiLimit = 150; c.Seconds = 40;   // the stuck heater cooks the plant to ~100 while disabled
        list.Add(new Scenario("enable-disable", "S12 Zone disabled: inert under an open sensor, a stuck sensor and a stuck relay; TcInit with Enable = 1 at 30 s starts control.", c)
            .At(5, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .At(10, "Sensor 2 stuck at 200", s => { s.Sensor2.Fault = SensorFault.StuckValue; s.Sensor2.StuckValue = 200; })
            .At(15, "Heater stuck closed", s => s.Heater.StuckClosed = true)
            .Expect(20, "disabled: status 0, no warning, relays 0, nothing accumulated", s => s.Ctl.Status == TcStatus.TempCtrlDisabled && s.Ctl.Warning == TcWarning.NoWarning
                && !s.Ctl.DoHeater && !s.Ctl.DoCooler && s.Ctl.Temp1OorAccumMs == 0 && s.Ctl.Temp2OorAccumMs == 0 && s.Ctl.Temp1OorEventsPerHour == 0 && double.IsNaN(s.Ctl.Temp1Avg))
            .At(30, "Everything healthy, TcInit Enable 1", s => { s.Sensor1.Fault = SensorFault.None; s.Sensor2.Fault = SensorFault.None; s.Heater.StuckClosed = false; s.ReInit(k => k.TempCtrlEnable = true); })
            .Expect(30.1, "control started", s => s.Ctl.Status != TcStatus.TempCtrlDisabled && !s.Ctl.IsFault)
            .Expect(40, "no fault", s => !s.Ctl.IsFault));

        // 13 reset
        c = Single(Base()); c.Seconds = 40;
        list.Add(new Scenario("reset", "S13 Single sensor opens at 10 s: Temp1FailHigh after ErrorTimeout. Reset at 20 s with the sensor still open clears everything and faults again after the full timeout. Sensor back at 25 s, reset at 30 s.", c)
            .At(10, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .Expect(11.8, "not yet failed", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1900)
            .Expect(11.9, "Temp1FailHigh", s => s.Ctl.Status == TcStatus.Temp1FailHigh && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .At(20, "operator reset (sensor still open)", s => s.Reset())
            .Expect(20.0, "reset clears fault, accumulator (one open tick charged again), averages, flag", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 100 && !s.Ctl.InitialHcFlag && double.IsNaN(s.Ctl.Temp1Avg) && s.Ctl.Temp1OorEventsPerHour == 1)
            .Expect(21.8, "counting again", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1900)
            .Expect(21.9, "faults again after the full timeout", s => s.Ctl.Status == TcStatus.Temp1FailHigh)
            .At(25, "Sensor 1 back", s => s.Sensor1.Fault = SensorFault.None)
            .At(30, "operator reset", s => s.Reset())
            .Expect(30.0, "clean", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.Temp1OorEventsPerHour == 0)
            .Expect(40, "runs on", s => !s.Ctl.IsFault && s.Ctl.InitialHcFlag));

        // 14 two zones
        c = Base(); c.Seconds = 60;
        c.Companion = Single(Base()); c.Companion.Controller.Setpoint = 80; c.Companion.Controller.HiLimit = 120; c.Companion.Controller.DeadbandTimeoutMs = 1000; c.Companion.Seconds = 60;
        list.Add(new Scenario("two-zones", "S14 Zone 0 (two sensors, setpoint 50) and zone 1 (one sensor, setpoint 80, other timeouts) stepped alternately in the same tick: each follows its own trajectory, identical to running it alone.", c)
            .Expect(1.0, "zone 1 has its own bands and slower deadband timeout", s => s.Companion!.Ctl.HiBand == 85 && s.Companion.Ctl.Status == TcStatus.HeatPending && s.Ctl.Status == TcStatus.HeaterON)
            .Expect(1.1, "zone 1 heater on after 1000 ms", s => s.Companion!.Ctl.Status == TcStatus.HeaterON)
            .At(30, "Zone 0 sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .Expect(35, "zone 0 failed over, zone 1 untouched", s => s.Ctl.ActiveSensor == 2 && s.Companion!.Ctl.Warning == TcWarning.NoWarning && !s.Companion.Ctl.IsFault)
            .Expect(60, "zone 1's trace equals a solo run of the same setup", s =>
            {
                using var solo = new Simulation(s.Config.Companion!.Clone(), s.Table, 2);
                for (int i = 0; i < s.Companion!.Ticks; i++) solo.Step();
                return solo.Trace.SequenceEqual(s.Companion.Trace) && solo.Trace.Count == s.Ticks;
            }));

        // 15 time
        c = Flat(Single(Base())); c.StartTickMs = 0xFFFFFFFFu - 300u; c.Seconds = 12;
        P(c, 0, 30); P(c, 2.0, 52); P(c, 8.0, 30);
        list.Add(new Scenario("time", "S15 Tick Count wraps at 2^32 during the first countdown; a backwards step of 60 s at 2.3 s expires nothing; a 600 s gap at 8.2 s completes the pending countdown.", c)
            .Expect(0.1, "counting across the 2^32 wrap", s => s.Ctl.Status == TcStatus.HeatPending && s.NowMs > 0xFFFFFF00u)
            .Expect(0.6, "HeaterON after the wrap", s => s.Ctl.Status == TcStatus.HeaterON && s.NowMs < 1000u)
            .Expect(2.2, "at-setpoint countdown at 300 ms", s => s.Ctl.AtSetPtRemainMs == 300)
            .At(2.3, "host clock steps back 60 s", s => s.ClockOffsetMs -= 60_000)
            .Expect(2.3, "backwards step: 0 ms elapsed, countdown unchanged", s => s.Ctl.AtSetPtRemainMs == 300 && s.Ctl.DoHeater)
            .Expect(2.6, "released one tick later than without the step", s => s.Ctl.Status == TcStatus.TempAtSetPt && !s.Ctl.DoHeater)
            .Expect(8.1, "pending, 400 ms left", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.DeadbandRemainMs == 400)
            .At(8.2, "host clock jumps ahead 600 s", s => s.ClockOffsetMs += 600_000)
            .Expect(8.2, "long gap completes the countdown", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(12, "no fault", s => !s.Ctl.IsFault));

        return list;
    }

    public static Scenario? Find(string name, SimConfig? baseConfig = null) =>
        BuiltIn(baseConfig).FirstOrDefault(s => string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
}
