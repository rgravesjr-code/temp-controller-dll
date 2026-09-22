using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// A scripted run: a config, timed events (applied before the tick whose time reaches them) and timed
/// expectations (checked after that tick). The built-in ones are the required scenarios of the TempCtl v3.0.0
/// handoff (section 10, carried into v4 with the explicit Start) plus the 14 lifecycle / permissive scenarios of the
/// v4.0.0 handoff (section 14); their expectations are the release gate of TempSim.Cli.
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
                onEvent?.Invoke($"{t,7:0.0} s  {(ok ? "ok  " : "FAIL")} {label}   [status {Controller.Describe(sim.Ctl.Status)}, warning {Controller.Describe(sim.Ctl.Warning)}, relays {(sim.Ctl.DoHeater ? 1 : 0)}/{(sim.Ctl.DoCooler ? 1 : 0)}, started {(sim.Ctl.Started ? 1 : 0)}, perm {(sim.RunPermissive ? 1 : 0)}]");
            }
        }
        return result;
    }

    // ---- built-in scenarios (TEMPCTL-v3.0.0-HANDOFF section 10 under the v4 lifecycle, TEMPCTL-v4.0.0 handoff section 14) ------
    public static IReadOnlyList<Scenario> BuiltIn(SimConfig? baseConfig = null)
    {
        SimConfig Base() { var config = (baseConfig ?? new SimConfig()).Clone(); config.Fixture.Enabled = false; config.StartOnInit = true; config.RunPermissive = true; return config; }
        static SimConfig Single(SimConfig c) { c.Controller.Temp2Enable = false; c.Controller.FeedbackEnable = false; return c; }
        /// exact readings: no sensor offsets / noise, no Temp2Offset, values from a profile
        static SimConfig Flat(SimConfig c) { c.Sensor1.Offset = 0; c.Sensor2.Offset = 0; c.Controller.Temp2Offset = 0; c.Profile.Clear(); return c; }
        /// the zone is only configured at t = 0; a Start event is needed (v4 R10.2)
        static SimConfig Stopped(SimConfig c) { c.StartOnInit = false; return c; }
        static void P(SimConfig c, double at, double t1, double? t2 = null) => c.Profile.Add(new SimConfig.ProfilePoint(at, t1, t2 ?? t1));
        var list = new List<Scenario>();
        SimConfig c;

        // 1 single-sensor heat-up
        c = Single(Base()); c.Seconds = 60;
        list.Add(new Scenario("heat-up", "S1 Single sensor, Start at t = 0 out of band: HeatPending -> HeaterON -> at-setpoint countdown -> TempAtSetPt -> in-band idle.", c)
            .Expect(0.1, "HeatPending on the first tick", s => s.Ctl.Status == TcStatus.HeatPending && !s.Ctl.DoHeater && s.Ctl.DeadbandRemainMs == 500 && s.Ctl.Started)
            .Expect(0.5, "still pending 100 ms before the deadband timeout", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.DeadbandRemainMs == 100)
            .Expect(0.6, "HeaterON after DeadbandTimeout", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && !s.Ctl.InitialHcFlag)
            .Expect(20, "reached the setpoint, released after AtSetPtTimeout, idle in band", s => s.SeenStatuses.Contains(TcStatus.TempAtSetPt) && s.Ctl.InitialHcFlag && !s.Ctl.IsFault)
            .Expect(60, "no fault, no warning through the cycling", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning));

        // 2 (v4 form of S2, handoff v4 scenario 11) active reconfiguration: Stop -> apply zeros -> Init leaves the zone stopped; Start required
        c = Base(); c.Seconds = 60;
        list.Add(new Scenario("reconfigure-stop-init", "V4-11 Setpoint 50 -> 70 at 3 s while heating through the host sequence Stop -> zero DOs -> TcInit: relays dropped, IdleStopped until Start at 5 s; setpoint 40 at 40 s with the automatic re-Start: CoolPending -> CoolerON.", c)
            .Expect(0.6, "heating", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && s.HeaterOn)
            .At(3.0, "Stop -> apply zero DOs -> TcInit setpoint 70 (no re-Start)", s => s.ReInit(k => k.Setpoint = 70, restart: false))
            .Expect(3.0, "IdleStopped: heater dropped, physical relay off, bands moved, not started", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.HeaterOn && s.Ctl.HiBand == 75 && !s.Ctl.Started && s.Ctl.AtSetPtRemainMs == 0)
            .Expect(4.9, "Init never starts control: still IdleStopped two seconds later", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && s.Ctl.DeadbandRemainMs == 0)
            .At(5.0, "Start", s => s.Start())
            .Expect(5.0, "Start accepted: HeatPending with a fresh countdown", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started && s.Ctl.DeadbandRemainMs == 500)
            .Expect(5.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(30, "reached 70 and released", s => s.SeenStatuses.Contains(TcStatus.TempAtSetPt) && s.Ctl.HiBand == 75 && !s.Ctl.IsFault)
            .At(40.0, "Stop -> zero DOs -> TcInit setpoint 40 -> Start (the plant is near 70)", s => s.ReInit(k => k.Setpoint = 40))
            .Expect(40.0, "re-started: CoolPending above the new HiBand, relays off until the countdown", s => s.Ctl.Started && s.Ctl.Status == TcStatus.CoolPending && s.Ctl.HiBand == 45 && !s.Ctl.DoHeater && !s.Ctl.DoCooler && s.Ctl.DeadbandRemainMs == 500)
            .Expect(40.6, "CoolerON after DeadbandTimeout", s => s.Ctl.Status == TcStatus.CoolerON && s.Ctl.DoCooler)
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
            .At(12.0, "operator reset (Stop -> Reset -> Start)", s => s.Reset())
            .Expect(12.0, "reset: accumulator and events cleared, running again", s => !s.Ctl.IsFault && s.Ctl.Started && s.Ctl.Temp1OorAccumMs == 0 && s.Ctl.Temp1OorEventsPerHour == 0)
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
            .Expect(90.0, "reset: sensor 1 active again, no fault, running", s => s.Ctl.ActiveSensor == 1 && !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.Started)
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
        list.Add(new Scenario("relay-feedback", "S10 Heater DO answers one tick late: warning on each transition only. Cooler stuck closed at 30 s: CoolerFBMismatch at once, CoolerFBFault after RelayFeedbackTimeout. Reset at 40 s; FeedbackEnable off at 50 s (Stop -> Init -> Start) silences a stuck cooler.", c)
            .Expect(0.6, "HeaterON, feedback still 0 but matches the previous command", s => s.Ctl.DoHeater && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(0.7, "one-tick lag: HeaterFBMismatch warning only", s => s.Ctl.Warning == TcWarning.HeaterFBMismatch && s.Ctl.HeaterFbRemainMs == 1000 && !s.Ctl.IsFault)
            .Expect(0.8, "matched again", s => s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.HeaterFbRemainMs == 0)
            .At(30, "Cooler stuck closed (the DO read-back shows it one tick later)", s => s.Cooler.StuckClosed = true)
            .Expect(30.1, "CoolerFBMismatch as soon as the read-back disagrees", s => s.Ctl.Warning == TcWarning.CoolerFBMismatch && s.Ctl.CoolerFbRemainMs == 1000)
            .Expect(31.0, "not yet a fault", s => !s.Ctl.IsFault && s.Ctl.CoolerFbRemainMs == 100)
            .Expect(31.1, "CoolerFBFault after RelayFeedbackTimeout, relays off", s => s.Ctl.Status == TcStatus.CoolerFBFault && !s.Ctl.DoHeater && !s.Ctl.DoCooler)
            .At(40, "Cooler repaired, reset", s => { s.Cooler.StuckClosed = false; s.Reset(); })
            .Expect(40.1, "reset clears the fault; the repaired contact matches again", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.Started)
            .At(50, "Stop -> TcInit FeedbackEnable 0 -> Start", s => s.ReInit(k => k.FeedbackEnable = false))
            .At(51, "Cooler stuck closed again", s => s.Cooler.StuckClosed = true)
            .Expect(60, "feedback disabled: silent", s => !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning && s.Ctl.CoolerFbRemainMs == 0 && s.Ctl.Started));

        // 11 config faults
        c = Base(); c.Seconds = 30;
        list.Add(new Scenario("config-fault", "S11 TcInit with a reversed band while running: ConfigFault (relays off); Reset does not clear it; the same setup with Enable = 0 is only ConfigInvalid; a valid Init (+ Start) clears everything.", c)
            .At(10, "TcInit DeadbandHi -1", s => s.ReInit(k => k.DeadbandHi = -1))
            .Expect(10.0, "ConfigFault, relays off, Start refused", s => s.Ctl.Status == TcStatus.ConfigFault && !s.Ctl.DoHeater && !s.Ctl.DoCooler && s.Ctl.Warning == TcWarning.NoWarning && !s.Ctl.Started)
            .At(15, "operator reset", s => s.Reset())
            .Expect(15.0, "Reset does not clear ConfigFault", s => s.Ctl.Status == TcStatus.ConfigFault)
            .At(20, "TcInit Enable 0 (band still reversed)", s => s.ReInit(k => k.TempCtrlEnable = false))
            .Expect(20.0, "disabled with ConfigInvalid warning", s => s.Ctl.Status == TcStatus.TempCtrlDisabled && s.Ctl.Warning == TcWarning.ConfigInvalid && !s.Ctl.DoHeater)
            .At(25, "TcInit valid, Enable 1 (+ Start)", s => s.ReInit(k => { k.TempCtrlEnable = true; k.DeadbandHi = 5; }))
            .Expect(25.0, "running again, no warning", s => !s.Ctl.IsFault && s.Ctl.IsActive && s.Ctl.Started && s.Ctl.Warning == TcWarning.NoWarning));

        // 12 enable / disable
        c = Base(); c.Controller.TempCtrlEnable = false; c.Controller.HiLimit = 150; c.Seconds = 40;   // the stuck heater cooks the plant to ~100 while disabled
        list.Add(new Scenario("enable-disable", "S12 Zone disabled: inert under an open sensor, a stuck sensor and a stuck relay, a Start is refused; TcInit with Enable = 1 (+ Start) at 30 s starts control.", c)
            .At(5, "Sensor 1 open", s => s.Sensor1.Fault = SensorFault.Open)
            .At(10, "Sensor 2 stuck at 200", s => { s.Sensor2.Fault = SensorFault.StuckValue; s.Sensor2.StuckValue = 200; })
            .At(15, "Heater stuck closed", s => s.Heater.StuckClosed = true)
            .At(18, "Start on the disabled zone", s => s.Start())
            .Expect(20, "disabled: status 0, no warning, relays 0, nothing accumulated, not started", s => s.Ctl.Status == TcStatus.TempCtrlDisabled && s.Ctl.Warning == TcWarning.NoWarning
                && !s.Ctl.DoHeater && !s.Ctl.DoCooler && s.Ctl.Temp1OorAccumMs == 0 && s.Ctl.Temp2OorAccumMs == 0 && s.Ctl.Temp1OorEventsPerHour == 0 && double.IsNaN(s.Ctl.Temp1Avg) && !s.Ctl.Started)
            .At(30, "Everything healthy, TcInit Enable 1 (+ Start)", s => { s.Sensor1.Fault = SensorFault.None; s.Sensor2.Fault = SensorFault.None; s.Heater.StuckClosed = false; s.ReInit(k => k.TempCtrlEnable = true); })
            .Expect(30.1, "control started", s => s.Ctl.IsActive && s.Ctl.Started && !s.Ctl.IsFault)
            .Expect(40, "no fault", s => !s.Ctl.IsFault));

        // 13 reset
        c = Single(Base()); c.Seconds = 40;
        list.Add(new Scenario("reset", "S13 Single sensor opens at 10 s: Temp1FailHigh after ErrorTimeout. Reset (+ Start) at 20 s with the sensor still open clears everything and faults again after the full timeout. Sensor back at 25 s, reset at 30 s.", c)
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

        // ---- TempCtl v4.0.0 handoff section 14: lifecycle and run permissive ----------------------------------------
        // V4-1 enabled Init, idle: no control before Start
        c = Stopped(Flat(Single(Base()))); c.Seconds = 20; P(c, 0, 30);
        list.Add(new Scenario("idle-before-start", "V4-1 Enabled Init only: IdleStopped, relays off, the raw reading mirrored, nothing evaluated for 10 s (30 is below the band and would heat); Start at 10 s: HeatPending -> HeaterON.", c)
            .Expect(0.1, "IdleStopped on the first tick, nothing counting, raw mirrored", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.Ctl.Started && s.Ctl.DeadbandRemainMs == 0 && s.Ctl.Temp1Raw == 30)
            .Expect(5.0, "still idle: no averaging, no accumulation, no warning, permissive stored", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning && double.IsNaN(s.Ctl.Temp1Avg) && s.Ctl.Temp1OorAccumMs == 0 && s.Ctl.RunPermissive == 1)
            .Expect(9.9, "CheckTemp alone never starts control", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.HeaterOn)
            .At(10.0, "Start", s => s.Start())
            .Expect(10.0, "Start accepted: HeatPending with the full countdown", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started && s.Ctl.DeadbandRemainMs == 500)
            .Expect(10.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && s.HeaterOn)
            .Expect(20, "no fault", s => !s.Ctl.IsFault));

        // V4-2 Start then an ordinary heat / cool trajectory
        c = Stopped(Base()); c.Seconds = 60;
        list.Add(new Scenario("start-heat-cool", "V4-2 Plant at 20, setpoint 50, idle until Start at 2 s: HeatPending -> HeaterON -> TempAtSetPt; setpoint 30 at 40 s (Stop -> Init -> Start): CoolPending -> CoolerON.", c)
            .Expect(1.0, "idle before Start", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.Ctl.Started)
            .At(2.0, "Start", s => s.Start())
            .Expect(2.0, "HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started && s.Ctl.DeadbandRemainMs == 500)
            .Expect(2.4, "100 ms left", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.DeadbandRemainMs == 100)
            .Expect(2.5, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(25, "reached the setpoint and released", s => s.SeenStatuses.Contains(TcStatus.TempAtSetPt) && s.Ctl.InitialHcFlag && !s.Ctl.IsFault)
            .At(40.0, "Stop -> zero DOs -> TcInit setpoint 30 -> Start", s => s.ReInit(k => k.Setpoint = 30))
            .Expect(40.0, "CoolPending above the new band, started", s => s.Ctl.Started && s.Ctl.Status == TcStatus.CoolPending && s.Ctl.HiBand == 35 && s.Ctl.DeadbandRemainMs == 500)
            .Expect(40.6, "CoolerON", s => s.Ctl.Status == TcStatus.CoolerON && s.Ctl.DoCooler && s.CoolerOn)
            .Expect(60, "no fault", s => !s.Ctl.IsFault && s.SeenStatuses.Contains(TcStatus.CoolerON)));

        // V4-3 manual Stop from HeatPending, HeaterON, CoolPending and CoolerON
        c = Flat(Single(Base())); c.Seconds = 8; P(c, 0, 30); P(c, 3.0, 70);
        list.Add(new Scenario("stop-from-active", "V4-3 Stop from HeatPending (0.3 s), HeaterON (2 s), CoolPending (3.3 s) and CoolerON (5 s): both commands 0 at once, the modelled relays off, IdleStopped, no fault, no restart without Start.", c)
            .Expect(0.1, "HeatPending", s => s.Ctl.Status == TcStatus.HeatPending)
            .At(0.3, "Stop from HeatPending", s => s.Stop())
            .Expect(0.3, "IdleStopped, countdown cancelled", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.HeaterOn && s.Ctl.DeadbandRemainMs == 0 && !s.Ctl.Started && s.Ctl.Warning == TcWarning.NoWarning)
            .At(1.0, "Start", s => s.Start())
            .Expect(1.0, "fresh HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.DeadbandRemainMs == 500)
            .Expect(1.6, "HeaterON, relay closed", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater && s.HeaterOn)
            .At(2.0, "Stop from HeaterON", s => s.Stop())
            .Expect(2.0, "IdleStopped, heater command and relay off at once", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.HeaterOn && !s.Ctl.Started)
            .Expect(2.9, "stays stopped", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.HeaterOn)
            .At(3.0, "Start (the reading is 70 now)", s => s.Start())
            .Expect(3.0, "CoolPending", s => s.Ctl.Status == TcStatus.CoolPending && s.Ctl.DeadbandRemainMs == 500)
            .At(3.3, "Stop from CoolPending", s => s.Stop())
            .Expect(3.3, "IdleStopped", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoCooler && s.Ctl.DeadbandRemainMs == 0)
            .At(4.0, "Start", s => s.Start())
            .Expect(4.6, "CoolerON, relay closed", s => s.Ctl.Status == TcStatus.CoolerON && s.Ctl.DoCooler && s.CoolerOn)
            .At(5.0, "Stop from CoolerON", s => s.Stop())
            .Expect(5.0, "IdleStopped, cooler command and relay off at once", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoCooler && !s.CoolerOn && !s.Ctl.Started)
            .Expect(8.0, "no fault, no restart", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.IsFault && !s.Ctl.DoHeater && !s.Ctl.DoCooler && s.Ctl.Warning == TcWarning.NoWarning));

        // V4-4 blocked Start
        c = Stopped(Flat(Single(Base()))); c.RunPermissive = false; c.Seconds = 12; P(c, 0, 30);
        list.Add(new Scenario("blocked-start", "V4-4 Start with the permissive false: IdleStartBlocked with warning 8, no countdown, never a fault; the permissive returns at 6 s: warning clears, status stays, no auto-start; Start at 8 s runs.", c)
            .At(1.0, "Start with runPermissive = 0", s => s.Start())
            .Expect(1.0, "IdleStartBlocked, warning OperatingConditionNotMet, relays 0, no countdown", s => s.Ctl.Status == TcStatus.IdleStartBlocked && s.Ctl.Warning == TcWarning.OperatingConditionNotMet && !s.Ctl.Started && !s.Ctl.DoHeater && s.Ctl.OperatingConditionRemainMs == 0 && s.Ctl.RunPermissive == 0)
            .Expect(5.0, "four seconds of false permissive: still blocked, no fault, no countdown", s => s.Ctl.Status == TcStatus.IdleStartBlocked && !s.Ctl.IsFault && s.Ctl.OperatingConditionRemainMs == 0 && s.Ctl.Warning == TcWarning.OperatingConditionNotMet)
            .At(6.0, "permissive true", s => s.RunPermissive = true)
            .Expect(6.0, "warning clears live, status 7 latched, nothing starts", s => s.Ctl.Status == TcStatus.IdleStartBlocked && s.Ctl.Warning == TcWarning.NoWarning && !s.Ctl.Started && !s.Ctl.DoHeater && s.Ctl.RunPermissive == 1)
            .Expect(7.9, "no auto-start", s => s.Ctl.Status == TcStatus.IdleStartBlocked && !s.Ctl.Started)
            .At(8.0, "Start", s => s.Start())
            .Expect(8.0, "accepted: HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(8.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(12, "no fault", s => !s.Ctl.IsFault));

        // V4-5 one-tick permissive loss
        c = Flat(Single(Base())); c.Seconds = 10; P(c, 0, 30);
        list.Add(new Scenario("permissive-trip", "V4-5 Heating; the permissive drops for one tick at 3.0 s: both outputs off on that sample, OperatingConditionPending with the full 3000 ms; back at 3.1 s: IdleOperatingConditionTripped, no fault, no restart; Start at 6 s.", c)
            .Expect(0.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.HeaterOn)
            .At(3.0, "permissive false", s => s.RunPermissive = false)
            .Expect(3.0, "first false sample: relays off at once, pending, full timeout, Started 0, warning 8", s => s.Ctl.Status == TcStatus.OperatingConditionPending && !s.Ctl.DoHeater && !s.HeaterOn && s.Ctl.OperatingConditionRemainMs == 3000 && !s.Ctl.Started && s.Ctl.Warning == TcWarning.OperatingConditionNotMet && s.Ctl.RunPermissive == 0)
            .At(3.1, "permissive true", s => s.RunPermissive = true)
            .Expect(3.1, "recovered before the timeout: tripped, countdown cancelled, warning clears", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && s.Ctl.OperatingConditionRemainMs == 0 && s.Ctl.Warning == TcWarning.NoWarning && !s.Ctl.DoHeater)
            .Expect(5.9, "no automatic restart, no fault", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && !s.Ctl.IsFault && !s.Ctl.DoHeater && !s.Ctl.Started)
            .At(6.0, "Start", s => s.Start())
            .Expect(6.0, "explicit Start clears the trip: HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started && s.Ctl.DeadbandRemainMs == 500)
            .Expect(6.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(10, "no fault", s => !s.Ctl.IsFault));

        // V4-6 recovery during the countdown
        c = Flat(Single(Base())); c.Seconds = 14; P(c, 0, 30);
        list.Add(new Scenario("permissive-recover", "V4-6 Permissive lost at 3.0 s, back at 6.0 s = one tick before the 3000 ms timeout: the countdown is cancelled, IdleOperatingConditionTripped, no fault, no restart until Start at 10 s.", c)
            .At(3.0, "permissive false", s => s.RunPermissive = false)
            .Expect(3.0, "pending, 3000 ms", s => s.Ctl.Status == TcStatus.OperatingConditionPending && s.Ctl.OperatingConditionRemainMs == 3000 && !s.Ctl.DoHeater)
            .Expect(5.8, "200 ms left", s => s.Ctl.Status == TcStatus.OperatingConditionPending && s.Ctl.OperatingConditionRemainMs == 200)
            .Expect(5.9, "100 ms left", s => s.Ctl.Status == TcStatus.OperatingConditionPending && s.Ctl.OperatingConditionRemainMs == 100 && s.Ctl.Warning == TcWarning.OperatingConditionNotMet)
            .At(6.0, "permissive true (the tick that would have faulted)", s => s.RunPermissive = true)
            .Expect(6.0, "cancelled: tripped, no fault", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && s.Ctl.OperatingConditionRemainMs == 0 && !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(9.9, "no automatic restart", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && !s.Ctl.DoHeater && !s.Ctl.Started)
            .At(10.0, "Start", s => s.Start())
            .Expect(10.0, "HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started)
            .Expect(10.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON)
            .Expect(14, "no fault", s => !s.Ctl.IsFault));

        // V4-7 sustained permissive loss: fault at the exact timeout
        c = Flat(Single(Base())); c.Seconds = 14; P(c, 0, 30);
        list.Add(new Scenario("permissive-fault", "V4-7 Permissive lost at 3.0 s and held: OperatingConditionFault exactly 3000 ms later (6.0 s), latched through recovery and a Start; Reset at 10 s (Stop -> Reset -> Start) runs again.", c)
            .At(3.0, "permissive false", s => s.RunPermissive = false)
            .Expect(3.0, "pending, relays off", s => s.Ctl.Status == TcStatus.OperatingConditionPending && !s.Ctl.DoHeater && s.Ctl.OperatingConditionRemainMs == 3000)
            .Expect(5.9, "100 ms before the fault", s => s.Ctl.Status == TcStatus.OperatingConditionPending && s.Ctl.OperatingConditionRemainMs == 100)
            .Expect(6.0, "OperatingConditionFault on the exact tick, warning 8 frozen", s => s.Ctl.Status == TcStatus.OperatingConditionFault && s.Ctl.Warning == TcWarning.OperatingConditionNotMet && !s.Ctl.DoHeater && s.Ctl.OperatingConditionRemainMs == 0 && !s.Ctl.Started)
            .At(8.0, "permissive true", s => s.RunPermissive = true)
            .Expect(8.0, "latched: recovery changes nothing", s => s.Ctl.Status == TcStatus.OperatingConditionFault && s.Ctl.Warning == TcWarning.OperatingConditionNotMet)
            .At(9.0, "Start on the faulted zone", s => s.Start())
            .Expect(9.0, "Start refused, fault kept", s => s.Ctl.Status == TcStatus.OperatingConditionFault && !s.Ctl.Started)
            .At(10.0, "operator reset (Stop -> Reset -> Start)", s => s.Reset())
            .Expect(10.0, "fault cleared, running: HeatPending", s => !s.Ctl.IsFault && s.Ctl.Started && s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(10.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON)
            .Expect(14, "no fault", s => !s.Ctl.IsFault));

        // V4-8 Stop during pending
        c = Flat(Single(Base())); c.Seconds = 12; P(c, 0, 30);
        list.Add(new Scenario("stop-while-pending", "V4-8 Permissive lost at 3.0 s, Stop at 4.0 s: the escalation is cancelled, the cause is kept (IdleOperatingConditionTripped), warning 8 while the permissive is false, no fault at 6.0 s; Start at 9 s.", c)
            .At(3.0, "permissive false", s => s.RunPermissive = false)
            .Expect(3.0, "pending", s => s.Ctl.Status == TcStatus.OperatingConditionPending)
            .At(4.0, "Stop while pending", s => s.Stop())
            .Expect(4.0, "tripped: countdown cancelled, cause kept, warning 8 retained", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && s.Ctl.OperatingConditionRemainMs == 0 && s.Ctl.Warning == TcWarning.OperatingConditionNotMet && !s.Ctl.DoHeater && !s.Ctl.Started)
            .Expect(7.0, "no fault where the timeout would have expired", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && !s.Ctl.IsFault && s.Ctl.Warning == TcWarning.OperatingConditionNotMet)
            .At(8.0, "permissive true", s => s.RunPermissive = true)
            .Expect(8.0, "warning clears, cause kept", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && s.Ctl.Warning == TcWarning.NoWarning)
            .At(9.0, "Start", s => s.Start())
            .Expect(9.0, "HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started)
            .Expect(12, "no fault", s => !s.Ctl.IsFault));

        // V4-9 fault priority on the first false-permissive tick
        c = Flat(Single(Base())); c.Seconds = 10; P(c, 0, 50); P(c, 3.0, double.NaN);
        list.Add(new Scenario("permissive-fault-priority", "V4-9 Sensor open from 3.0 s (ErrorTimeout 2000 fails it at 4.9 s); the permissive drops on that same tick: the sensor fault wins, the permissive path never runs, later permissive changes and a Start change nothing.", c)
            .Expect(4.8, "one tick before the sensor failure", s => !s.Ctl.IsFault && s.Ctl.Temp1OorAccumMs == 1900 && s.Ctl.Status == TcStatus.TempAtSetPt)
            .At(4.9, "permissive false on the tick the sensor fails", s => s.RunPermissive = false)
            .Expect(4.9, "Temp1FailHigh wins over the operating-condition path", s => s.Ctl.Status == TcStatus.Temp1FailHigh && s.Ctl.Warning == TcWarning.Temp1OutOfRange && !s.Ctl.DoHeater && s.Ctl.OperatingConditionRemainMs == 0 && !s.Ctl.Started)
            .At(6.0, "permissive true", s => s.RunPermissive = true)
            .At(7.0, "Start", s => s.Start())
            .Expect(7.0, "latched sensor fault, Start refused", s => s.Ctl.Status == TcStatus.Temp1FailHigh && s.Ctl.Warning == TcWarning.Temp1OutOfRange && !s.Ctl.Started)
            .Expect(10, "still latched", s => s.Ctl.Status == TcStatus.Temp1FailHigh));

        // V4-10 Reset then CheckTemp without Start: stays idle
        c = Flat(Single(Base())); c.Seconds = 16; P(c, 0, 50); P(c, 3.0, double.NaN); P(c, 8.0, 30);
        list.Add(new Scenario("reset-stays-idle", "V4-10 Sensor fault at 4.9 s; Reset at 8 s without a re-Start: IdleStopped, CheckTemp alone (reading 30, below the band) never resumes control; Start at 12 s does.", c)
            .Expect(4.9, "Temp1FailHigh", s => s.Ctl.Status == TcStatus.Temp1FailHigh)
            .At(8.0, "Reset (no Start)", s => s.Reset(restart: false))
            .Expect(8.0, "IdleStopped, clean, not started", s => s.Ctl.Status == TcStatus.IdleStopped && s.Ctl.Warning == TcWarning.NoWarning && !s.Ctl.Started && !s.Ctl.DoHeater && s.Ctl.Temp1OorAccumMs == 0 && double.IsNaN(s.Ctl.RunPermissive) == false)
            .Expect(11.9, "four seconds of CheckTemp below the band: still IdleStopped, raw mirrored, nothing counting", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && s.Ctl.DeadbandRemainMs == 0 && s.Ctl.Temp1Raw == 30 && double.IsNaN(s.Ctl.Temp1Avg))
            .At(12.0, "Start", s => s.Start())
            .Expect(12.0, "HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started)
            .Expect(12.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(16, "no fault", s => !s.Ctl.IsFault));

        // V4-12 active non-fault Reset: Stop -> zero DOs -> Reset; CheckTemp alone does not resume
        c = Flat(Single(Base())); c.Seconds = 10; P(c, 0, 30);
        list.Add(new Scenario("reset-stop-first", "V4-12 Heating; operator Reset at 3 s through Stop -> zero DOs -> TcReset without a re-Start: heater and relay off, history cleared, IdleStopped; CheckTemp alone does not resume; Start at 6 s.", c)
            .Expect(0.6, "HeaterON, relay closed", s => s.Ctl.Status == TcStatus.HeaterON && s.HeaterOn)
            .At(3.0, "Stop -> zero DOs -> Reset (no Start)", s => s.Reset(restart: false))
            .Expect(3.0, "IdleStopped, relay off, history cleared", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && !s.HeaterOn && !s.Ctl.Started && double.IsNaN(s.Ctl.Temp1Avg) && !s.Ctl.InitialHcFlag && double.IsNaN(s.Ctl.RunPermissive) == false)
            .Expect(5.9, "still IdleStopped", s => s.Ctl.Status == TcStatus.IdleStopped && !s.Ctl.DoHeater && s.Ctl.DeadbandRemainMs == 0)
            .At(6.0, "Start", s => s.Start())
            .Expect(6.0, "HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started)
            .Expect(6.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .Expect(10, "no fault", s => !s.Ctl.IsFault));

        // V4-13 Start while pending (false keeps the countdown, true restarts) and while tripped
        c = Flat(Single(Base())); c.Seconds = 14; P(c, 0, 30);
        list.Add(new Scenario("start-while-pending", "V4-13 Permissive lost at 3 s; Start with it still false at 4 s keeps the pending state and the countdown; permissive true + Start at 5 s restarts and clears the countdown; a second loss at 8 s recovers at 8.5 s (tripped) and Start at 9 s restarts.", c)
            .At(3.0, "permissive false", s => s.RunPermissive = false)
            .Expect(3.0, "pending 3000", s => s.Ctl.Status == TcStatus.OperatingConditionPending && s.Ctl.OperatingConditionRemainMs == 3000)
            .At(4.0, "Start with runPermissive = 0", s => s.Start())
            .Expect(4.0, "refused: pending kept, countdown continues (2000 left), not started", s => s.Ctl.Status == TcStatus.OperatingConditionPending && s.Ctl.OperatingConditionRemainMs == 2000 && !s.Ctl.Started && s.Ctl.Warning == TcWarning.OperatingConditionNotMet)
            .At(5.0, "permissive true + Start", s => { s.RunPermissive = true; s.Start(); })
            .Expect(5.0, "explicit restart: HeatPending, countdown cleared", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started && s.Ctl.OperatingConditionRemainMs == 0 && s.Ctl.Warning == TcWarning.NoWarning)
            .Expect(5.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON && s.Ctl.DoHeater)
            .At(8.0, "permissive false", s => s.RunPermissive = false)
            .Expect(8.0, "pending again", s => s.Ctl.Status == TcStatus.OperatingConditionPending && !s.Ctl.DoHeater)
            .At(8.5, "permissive true", s => s.RunPermissive = true)
            .Expect(8.5, "tripped", s => s.Ctl.Status == TcStatus.IdleOperatingConditionTripped && !s.Ctl.Started)
            .At(9.0, "Start while tripped", s => s.Start())
            .Expect(9.0, "accepted: HeatPending", s => s.Ctl.Status == TcStatus.HeatPending && s.Ctl.Started)
            .Expect(9.6, "HeaterON", s => s.Ctl.Status == TcStatus.HeaterON)
            .Expect(14, "no fault", s => !s.Ctl.IsFault));

        // V4-14 two zones with independent lifecycle / permissive states
        c = Base(); c.Seconds = 30;
        c.Companion = Single(Base()); c.Companion.Controller.Setpoint = 80; c.Companion.Controller.HiLimit = 120; c.Companion.Seconds = 30;
        list.Add(new Scenario("two-zones-lifecycle", "V4-14 Zone 0 loses its permissive at 5 s (pending, fault at 8 s) while zone 1 keeps heating; zone 1 is stopped at 6 s and started again at 10 s; zone 0 is reset (+ Start) at 20 s with the permissive back: each zone's lifecycle is its own.", c)
            .Expect(1.0, "both heating", s => s.Ctl.Status == TcStatus.HeaterON && s.Companion!.Ctl.Status == TcStatus.HeaterON && s.Ctl.Started && s.Companion.Ctl.Started)
            .At(5.0, "zone 0 permissive false", s => s.RunPermissive = false)
            .Expect(5.0, "zone 0 pending, zone 1 unaffected", s => s.Ctl.Status == TcStatus.OperatingConditionPending && !s.Ctl.DoHeater && s.Companion!.Ctl.Status == TcStatus.HeaterON && s.Companion.Ctl.DoHeater && s.Companion.Ctl.Started)
            .At(6.0, "zone 1 Stop", s => s.Companion!.Stop())
            .Expect(6.0, "zone 1 IdleStopped, zone 0 still pending", s => s.Companion!.Ctl.Status == TcStatus.IdleStopped && !s.Companion.Ctl.DoHeater && !s.Companion.HeaterOn && s.Ctl.Status == TcStatus.OperatingConditionPending)
            .Expect(8.0, "zone 0 OperatingConditionFault, zone 1 idle", s => s.Ctl.Status == TcStatus.OperatingConditionFault && s.Companion!.Ctl.Status == TcStatus.IdleStopped)
            .At(10.0, "zone 1 Start", s => s.Companion!.Start())
            .Expect(10.0, "zone 1 HeatPending, zone 0 still faulted", s => s.Companion!.Ctl.Status == TcStatus.HeatPending && s.Companion.Ctl.Started && s.Ctl.Status == TcStatus.OperatingConditionFault)
            .Expect(10.6, "zone 1 HeaterON", s => s.Companion!.Ctl.Status == TcStatus.HeaterON)
            .At(15.0, "zone 0 permissive true", s => s.RunPermissive = true)
            .Expect(15.0, "zone 0 fault latched", s => s.Ctl.Status == TcStatus.OperatingConditionFault)
            .At(20.0, "zone 0 reset (Stop -> Reset -> Start)", s => s.Reset())
            .Expect(20.0, "zone 0 running again, zone 1 untouched", s => !s.Ctl.IsFault && s.Ctl.Started && s.Companion!.Ctl.Started && !s.Companion.Ctl.IsFault)
            .Expect(30, "no fault on either zone", s => !s.Ctl.IsFault && !s.Companion!.Ctl.IsFault && s.Ctl.Started && s.Companion.Ctl.Started));

        return list;
    }

    public static Scenario? Find(string name, SimConfig? baseConfig = null) =>
        BuiltIn(baseConfig).FirstOrDefault(s => string.Equals(s.Name, name, StringComparison.OrdinalIgnoreCase));
}
