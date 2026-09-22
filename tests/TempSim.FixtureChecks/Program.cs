using TempSim.Core;

int count = 0;
void Check(bool ok, string label) { if (!ok) throw new Exception(label); count++; Console.WriteLine("ok " + label); }
(Plant p, FixtureModel f) Run(FixtureConfig c, bool heat = false, bool cool = false, double start = 20, int units = 1, int ticks = 1000)
{
    var p = new Plant { Temperature = start, Ambient = units == 0 ? 68 : 20 };
    var f = new FixtureModel();
    for (int i = 0; i < ticks; i++) f.Step(0.1, p, c, heat, cool, units);
    return (p, f);
}
var idle = new FixtureConfig { MotorsRunning = false, PumpRunning = false };
Check(Run(idle).p.Temperature == 20, "ambient equilibrium");
Check(Run(idle, heat: true).p.Temperature == 20, "no oil flow means no heater delivery");
var heated = new FixtureConfig { MotorsRunning = false };
var a = Run(heated, heat: true);
Check(a.p.Temperature > 20 && a.f.HeaterHeatW > 0, "flowing oil delivers heater power");
var b = Run(heated, heat: true);
Check(a.p.Temperature == b.p.Temperature && a.f.FlowPhase == b.f.FlowPhase, "repeatable state and animation phase");
var one = Run(new FixtureConfig { MotorCount = 1 });
var three = Run(new FixtureConfig { MotorCount = 3 });
Check(three.p.Temperature > one.p.Temperature && Math.Abs(three.f.MotorHeatW / one.f.MotorHeatW - 3) < 1e-12, "three motors add three times the heat");
Check(Run(idle, cool: true, start: 80).p.Temperature < Run(idle, start: 80).p.Temperature, "automatic fan increases cooling");
Check(Run(idle, cool: true, start: 10).p.Temperature <= 20, "air cooling cannot cross ambient from below");
Check(Run(new FixtureConfig { FanAutomatic = false, FanRunning = true }).f.FanRpm > 1500, "manual fan runs without cooler command");
var fahrenheit = Run(heated, heat: true, start: 68, units: 0);
Check(Math.Abs((fahrenheit.p.Temperature - 32) * 5 / 9 - a.p.Temperature) < 1e-9, "SI physics agrees in Fahrenheit and Celsius");
var isolated = new FixtureConfig { MotorsRunning = false, PassiveConductance = 0, PumpResponseSeconds = 0 };
var energy = Run(isolated, heat: true, ticks: 100);
Check(Math.Abs(energy.p.Temperature - (20 + 4500.0 * 10 / 19600)) < 1e-9, "heater energy balances combined oil and fixture heat capacity");
Check(a.f.PumpAngle != 0 && a.f.MotorRpm == 0, "pump animation independent of motor command");
var coastConfig = new FixtureConfig(); var coast = Run(coastConfig);
coastConfig.MotorsRunning = false; coastConfig.PumpRunning = false;
for (int i = 0; i < 1000; i++) coast.f.Step(0.1, coast.p, coastConfig, false, false, 1);
Check(coast.f.MotorRpm < 0.001 && coast.f.FlowLpm < 0.001, "motors and pump coast down after stop");
foreach (var bad in new[] { new FixtureConfig { MotorCount = 4 }, new FixtureConfig { OilVolumeL = 0 }, new FixtureConfig { FanRpm = double.NaN }, new FixtureConfig { MotorLoad = 2 }, new FixtureConfig { MotorRpm = 4000 } })
{
    bool rejected = false; try { bad.Validate(); } catch (ArgumentException) { rejected = true; }
    Check(rejected, "invalid physical configuration rejected");
}
var configured = new SimConfig { Fixture = new FixtureConfig { Enabled = true, MotorCount = 3, FanAutomatic = false } };
Check(configured.Clone().Fixture.MotorCount == 3 && !configured.Clone().Fixture.FanAutomatic, "physics settings survive JSON round trip");
Check(Scenario.BuiltIn(configured).All(s => !s.Config.Fixture.Enabled), "controller scenarios retain the legacy plant");
var actuating = new FixtureConfig { PassiveConductance = 0, MotorResponseSeconds = 0, PumpResponseSeconds = 0 };
var active = Run(actuating);
Check(active.f.OutletTemperature > active.f.InletTemperature, "actuation heats outlet above inlet without heater");
Check(a.f.InletTemperature > a.f.OutletTemperature, "inline heater warms inlet first during warm-up");
var fastFlow = Run(new FixtureConfig { PassiveConductance = 0, MotorResponseSeconds = 0, PumpResponseSeconds = 0, PumpFlowLpm = 24 });
Check(fastFlow.f.DeltaTemperature < active.f.DeltaTemperature, "greater flow reduces actuation temperature rise");
var noFlow = Run(new FixtureConfig { PassiveConductance = 0, PumpRunning = false });
Check(noFlow.f.InletTemperature == 20 && noFlow.f.OutletTemperature > 20, "stopped oil retains actuation heat in UUT without dividing by zero");
double beforeStop = active.f.DeltaTemperature;
double storedMean = active.p.Temperature;
actuating.MotorsRunning = false;
for (int i = 0; i < 2000; i++) active.f.Step(0.1, active.p, actuating, false, false, 1);
Check(Math.Abs(active.f.DeltaTemperature) < beforeStop * 0.001, "inlet/outlet equalize after actuation stops");
Check(Math.Abs(active.p.Temperature - storedMean) < 1e-9, "recirculation conserves stored thermal energy");
var motorEnergy = Run(new FixtureConfig { PassiveConductance = 0, MotorResponseSeconds = 0 }, ticks: 100);
Check(Math.Abs(motorEnergy.p.Temperature - (20 + 270.0 * 10 / 19600)) < 1e-9, "UUT actuation energy counted exactly once");
Check(Math.Abs((fahrenheit.f.InletTemperature - 32) * 5 / 9 - a.f.InletTemperature) < 1e-9 &&
      Math.Abs((fahrenheit.f.OutletTemperature - 32) * 5 / 9 - a.f.OutletTemperature) < 1e-9, "both thermal nodes convert consistently to Fahrenheit");
foreach (double volume in new[] { 0.0, 8.0, 9.0 })
{
    bool rejected = false;
    try { new FixtureConfig { UutOilVolumeL = volume }.Validate(); } catch (ArgumentException) { rejected = true; }
    Check(rejected, "UUT hold-up volume must fit inside loop");
}
Console.WriteLine($"{count}/{count} fixture checks passed");
