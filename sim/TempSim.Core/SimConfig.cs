using System.Text.Json;
using System.Text.Json.Serialization;

namespace TempSim.Core;

/// <summary>Everything a run needs, JSON-serialisable (the CLI's --config file and the WPF app's saved settings).</summary>
public sealed class SimConfig
{
    public int PeriodMs { get; set; } = 100;
    public int Seconds { get; set; } = 120;
    public ulong Seed { get; set; } = 1;

    public PlantConfig Plant { get; set; } = new();
    public SensorConfig Sensor1 { get; set; } = new() { Offset = 0.3 };
    public SensorConfig Sensor2 { get; set; } = new() { Offset = -0.2 };
    public RelayConfig Heater { get; set; } = new();
    public RelayConfig Cooler { get; set; } = new();
    public ControllerConfig Controller { get; set; } = new();
    public CanConfig Can { get; set; } = new();

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
    public sealed class ControllerConfig
    {
        public float Setpoint { get; set; } = 50;
        public float DeadbandHi { get; set; } = 5;
        public float DeadbandLo { get; set; } = 5;
        public float HiLimit { get; set; } = 90;
        public float LoLimit { get; set; } = 10;
        public float ErrorTimeoutMs { get; set; } = 2000;
        public float DeadbandTimeoutMs { get; set; } = 500;
        public float FilterPoints { get; set; } = 4;
        public bool Temp2Enable { get; set; } = true;
        public float Temp2Tolerance { get; set; } = 4;
        public bool FeedbackEnable { get; set; } = true;
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
    };
    public static SimConfig Load(string path) => JsonSerializer.Deserialize<SimConfig>(File.ReadAllText(path), s_json) ?? new SimConfig();
    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, s_json));
    public SimConfig Clone() => JsonSerializer.Deserialize<SimConfig>(JsonSerializer.Serialize(this, s_json), s_json)!;
}
