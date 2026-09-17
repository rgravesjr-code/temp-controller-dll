using System.Text.Json;
using System.Text.Json.Serialization;

namespace TempSim.Core;

/// <summary>Everything a run needs, JSON-serialisable (the CLI's --config file and the WPF app's saved settings).</summary>
public sealed class SimConfig
{
    public int PeriodMs { get; set; } = 100;
    public int Seconds { get; set; } = 120;
    public ulong Seed { get; set; } = 1;
    /// <summary>Value of the millisecond tick at t = 0 (LabVIEW Tick Count is free-running; set near 2^32 to test the wrap).</summary>
    public uint StartTickMs { get; set; } = 0;

    public PlantConfig Plant { get; set; } = new();
    public SensorConfig Sensor1 { get; set; } = new() { Offset = 0.3 };
    public SensorConfig Sensor2 { get; set; } = new() { Offset = -0.2 };
    public RelayConfig Heater { get; set; } = new();
    public RelayConfig Cooler { get; set; } = new();
    public ControllerConfig Controller { get; set; } = new();
    public CanConfig Can { get; set; } = new();
    /// <summary>
    /// Scripted temperature profile. When present the sensors read these values instead of the plant (step-hold:
    /// each point applies from its time until the next). NaN is an open sensor. Sensor offset / noise / faults still apply.
    /// </summary>
    public List<ProfilePoint> Profile { get; set; } = new();
    /// <summary>A second, independent zone (zone 1) stepped in the same loop with its own setup (scenario "two-zones").</summary>
    public SimConfig? Companion { get; set; }

    public sealed class ProfilePoint
    {
        public double AtSeconds { get; set; }
        public double Temp1 { get; set; }
        public double Temp2 { get; set; }
        public ProfilePoint() { }
        public ProfilePoint(double at, double t1, double t2) { AtSeconds = at; Temp1 = t1; Temp2 = t2; }
    }
    public sealed class PlantConfig
    {
        public double Ambient { get; set; } = 20.0;
        public double Initial { get; set; } = 20.0;
        public double LagPerSec { get; set; } = 0.02;
        public double HeatRate { get; set; } = 6.0;
        public double CoolRate { get; set; } = 6.0;
    }
    public sealed class SensorConfig
    {
        public double Offset { get; set; }
        public double NoiseAmplitude { get; set; }
        public double LagPerSec { get; set; }
    }
    public sealed class RelayConfig
    {
        public int DelayTicks { get; set; }
        public bool StuckOpen { get; set; }
        public bool StuckClosed { get; set; }
    }
    /// <summary>The 17 TcInit setup values (TC_SETUP_* order in ToSetupArray).</summary>
    public sealed class ControllerConfig
    {
        public bool TempCtrlEnable { get; set; } = true;
        public int TempUnits { get; set; } = 1;                  // 0 = degF, 1 = degC (label only)
        public double Setpoint { get; set; } = 50;
        public double DeadbandHi { get; set; } = 5;
        public double DeadbandLo { get; set; } = 5;
        public double HiLimit { get; set; } = 90;
        public double LoLimit { get; set; } = 10;
        public double ErrorTimeoutMs { get; set; } = 2000;
        public double DeadbandTimeoutMs { get; set; } = 500;
        public double AtSetPtTimeoutMs { get; set; } = 500;
        public bool Temp2Enable { get; set; } = true;
        public double Temp2Offset { get; set; } = 0.5;           // the default sensors sit at +0.3 / -0.2: corrected sensor 2 == sensor 1
        public double Temp2Tolerance { get; set; } = 4;
        public double TempCompareTimeoutMs { get; set; } = 5000;
        public double FilterPoints { get; set; } = 4;
        public bool FeedbackEnable { get; set; } = true;
        public double RelayFeedbackTimeoutMs { get; set; } = 1000;

        public double[] ToSetupArray() => new[]
        {
            TempCtrlEnable ? 1.0 : 0.0, TempUnits, Setpoint, DeadbandHi, DeadbandLo, HiLimit, LoLimit,
            ErrorTimeoutMs, DeadbandTimeoutMs, AtSetPtTimeoutMs, Temp2Enable ? 1.0 : 0.0, Temp2Offset, Temp2Tolerance,
            TempCompareTimeoutMs, FilterPoints, FeedbackEnable ? 1.0 : 0.0, RelayFeedbackTimeoutMs,
        };
    }
    public sealed class CanConfig
    {
        public int SourceAddress { get; set; } = 0x80;
        public int SpacingMs { get; set; } = 50;
        /// <summary>Linux SocketCAN interface to transmit on (e.g. "can1"); null = records only.</summary>
        public string? Interface { get; set; }
    }

    static readonly JsonSerializerOptions s_json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
        NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,     // NaN in profiles
    };
    public static SimConfig Load(string path) => JsonSerializer.Deserialize<SimConfig>(File.ReadAllText(path), s_json) ?? new SimConfig();
    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, s_json));
    public SimConfig Clone() => JsonSerializer.Deserialize<SimConfig>(JsonSerializer.Serialize(this, s_json), s_json)!;
}
