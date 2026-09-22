namespace TempSim.Core;

/// <summary>Estimated bench physics, in SI units. Opt-in so the original release scenarios retain their trajectories.</summary>
public sealed class FixtureConfig
{
    public bool Enabled { get; set; }
    public bool MotorsRunning { get; set; } = true;
    public int MotorCount { get; set; } = 2;
    public double MotorRpm { get; set; } = 1800;
    public double MotorRatedRpm { get; set; } = 3000;
    public double MotorPowerW { get; set; } = 1500;
    public double MotorLoad { get; set; } = 0.6;
    public double MotorHeatFraction { get; set; } = 0.25;
    public double MotorResponseSeconds { get; set; } = 2;
    public bool PumpRunning { get; set; } = true;
    public double PumpFlowLpm { get; set; } = 12;
    public double PumpResponseSeconds { get; set; } = 1;
    public double HeaterPowerW { get; set; } = 6000;
    public double OilVolumeL { get; set; } = 8;
    public double UutOilVolumeL { get; set; } = 0.5;
    public double OilDensityKgL { get; set; } = 0.85;
    public double OilSpecificHeat { get; set; } = 2000;
    public double FixtureMassKg { get; set; } = 12;
    public double FixtureSpecificHeat { get; set; } = 500;
    public double PassiveConductance { get; set; } = 8;
    public double TransferFlowLpm { get; set; } = 4;
    public bool FanAutomatic { get; set; } = true;
    public bool FanRunning { get; set; } = true;
    public double FanRpm { get; set; } = 1600;
    public double FanRatedRpm { get; set; } = 2000;
    public double FanConductance { get; set; } = 120;
    public double FanResponseSeconds { get; set; } = 1.5;

    public void Validate()
    {
        foreach (var p in GetType().GetProperties())
            if (p.PropertyType == typeof(double) && (!double.IsFinite((double)p.GetValue(this)!) || (double)p.GetValue(this)! < 0))
                throw new ArgumentException($"{p.Name} must be a finite nonnegative value.");
        if (MotorCount < 1 || MotorCount > 3) throw new ArgumentException("Motor count must be 1, 2 or 3.");
        if (MotorLoad > 1 || MotorHeatFraction > 1) throw new ArgumentException("Load and heat fraction must be between 0 and 1.");
        if (MotorRatedRpm <= 0 || FanRatedRpm <= 0 || TransferFlowLpm <= 0 ||
            OilVolumeL <= 0 || OilDensityKgL <= 0 || OilSpecificHeat <= 0 || FixtureMassKg <= 0 || FixtureSpecificHeat <= 0)
            throw new ArgumentException("Rated speeds, transfer flow, masses and heat capacities must be positive.");
        if (MotorRpm > MotorRatedRpm || FanRpm > FanRatedRpm) throw new ArgumentException("Requested RPM must not exceed rated RPM.");
        if (UutOilVolumeL <= 0 || UutOilVolumeL >= OilVolumeL)
            throw new ArgumentException("UUT oil volume must be positive and smaller than total loop oil volume.");
        double capacity = OilVolumeL * OilDensityKgL * OilSpecificHeat + FixtureMassKg * FixtureSpecificHeat;
        if ((OilVolumeL - UutOilVolumeL) * OilDensityKgL * OilSpecificHeat < 1 ||
            UutOilVolumeL * OilDensityKgL * OilSpecificHeat + FixtureMassKg * FixtureSpecificHeat < 1)
            throw new ArgumentException("Each thermal mass must have at least 1 J/K heat capacity.");
        if (!double.IsFinite(capacity) || capacity < 1 || capacity > 1e12 || MotorPowerW > 1e9 || HeaterPowerW > 1e9 ||
            PassiveConductance > 1e9 || FanConductance > 1e9 || MotorRatedRpm > 1e6 || FanRatedRpm > 1e6 || PumpFlowLpm > 1e6)
            throw new ArgumentException("Parameters exceed the supported bench-model range (capacity 1–1e12 J/K; power/conductance ≤1e9; RPM/flow ≤1e6).");
    }
}

/// <summary>Two mixed thermal masses: supply oil (inlet), and UUT oil plus fixture (outlet).
/// Recirculation transfers energy between them; actuation heats the UUT, heater heats the supply,
/// and passive/fan losses act at the UUT. Plant.Temperature is the energy-weighted mean.
/// Fixed substeps and arithmetic-only dynamics keep the model deterministic across targets.</summary>
public sealed class FixtureModel
{
    public double MotorRpm { get; private set; }
    public double FanRpm { get; private set; }
    public double FlowLpm { get; private set; }
    public double MotorHeatW { get; private set; }
    public double HeaterHeatW { get; private set; }
    public double CoolingW { get; private set; }
    public double MotorAngle { get; private set; }
    public double FanAngle { get; private set; }
    public double PumpAngle { get; private set; }
    public double FlowPhase { get; private set; }
    public double InletTemperature { get; private set; } = double.NaN;
    public double OutletTemperature { get; private set; } = double.NaN;
    public double DeltaTemperature => OutletTemperature - InletTemperature;

    public void SetTemperature(double temperature) => InletTemperature = OutletTemperature = temperature;

    static double Follow(double value, double target, double dt, double tau) => tau <= 0 ? target : value + (target - value) * dt / (tau + dt);

    public void Step(double dt, Plant plant, FixtureConfig c, bool heat, bool cool, int units)
    {
        c.Validate();
        int steps = Math.Max(1, (int)Math.Ceiling(dt / 0.05));
        double h = dt / steps;
        if (double.IsNaN(InletTemperature)) SetTemperature(plant.Temperature);
        double inletCapacity = (c.OilVolumeL - c.UutOilVolumeL) * c.OilDensityKgL * c.OilSpecificHeat;
        double outletCapacity = c.UutOilVolumeL * c.OilDensityKgL * c.OilSpecificHeat + c.FixtureMassKg * c.FixtureSpecificHeat;
        for (int i = 0; i < steps; i++)
        {
            MotorRpm = Follow(MotorRpm, c.MotorsRunning ? c.MotorRpm : 0, h, c.MotorResponseSeconds);
            FlowLpm = Follow(FlowLpm, c.PumpRunning ? c.PumpFlowLpm : 0, h, c.PumpResponseSeconds);
            FanRpm = Follow(FanRpm, (c.FanAutomatic ? cool : c.FanRunning) ? c.FanRpm : 0, h, c.FanResponseSeconds);
            MotorHeatW = c.MotorCount * c.MotorPowerW * c.MotorLoad * c.MotorHeatFraction * MotorRpm / c.MotorRatedRpm;
            HeaterHeatW = heat ? c.HeaterPowerW * FlowLpm / (FlowLpm + c.TransferFlowLpm) : 0;
            double conductance = c.PassiveConductance + c.FanConductance * FanRpm / c.FanRatedRpm;
            double toC = units == 0 ? 5.0 / 9.0 : 1;
            double inletC = (InletTemperature - plant.Ambient) * toC;
            double outletC = (OutletTemperature - plant.Ambient) * toC;
            double flowConductance = FlowLpm / 60 * c.OilDensityKgL * c.OilSpecificHeat;
            // Coupled backward-Euler solve: stable at zero/large flow, conserves internal transfer.
            double a = h * flowConductance / inletCapacity;
            double b = h * flowConductance / outletCapacity;
            double loss = h * conductance / outletCapacity;
            double supply = inletC + h * HeaterHeatW / inletCapacity;
            double uut = outletC + h * MotorHeatW / outletCapacity;
            double denominator = 1 + a + b + loss + a * loss;
            double nextIn = ((1 + b + loss) * supply + a * uut) / denominator;
            double nextOut = ((1 + a) * uut + b * supply) / denominator;
            CoolingW = conductance * nextOut;
            InletTemperature = plant.Ambient + nextIn / toC;
            OutletTemperature = plant.Ambient + nextOut / toC;
            plant.Temperature = (InletTemperature * inletCapacity + OutletTemperature * outletCapacity) / (inletCapacity + outletCapacity);
            // Deliberately slow illustrative rotations to avoid visual aliasing at real motor speeds.
            MotorAngle = (MotorAngle + h * MotorRpm * 0.12) % 360;
            FanAngle = (FanAngle + h * FanRpm * 0.16) % 360;
            PumpAngle = (PumpAngle + h * FlowLpm * 15) % 360;
            FlowPhase = (FlowPhase + h * FlowLpm / 30) % 1;
        }
    }
}
