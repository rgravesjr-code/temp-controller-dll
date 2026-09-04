namespace TempSim.Core;

/// <summary>Deterministic xorshift64* generator so runs are identical on every platform (no System.Random).</summary>
public sealed class Rng
{
    ulong _s;
    public Rng(ulong seed) { _s = seed == 0 ? 0x9E3779B97F4A7C15UL : seed; }
    public ulong NextU64()
    {
        _s ^= _s >> 12; _s ^= _s << 25; _s ^= _s >> 27;
        return _s * 0x2545F4914F6CDD1DUL;
    }
    /// <summary>Uniform in [-1, 1) using 53 bits, exact in double.</summary>
    public double NextSigned() => (NextU64() >> 11) * (1.0 / 4503599627370496.0) * 2.0 - 1.0;
}

/// <summary>
/// First-order thermal plant. Only +, -, *, / are used so the trajectory is bit-identical across platforms.
///   dT/dt = (Ambient - T) * LagPerSec + HeatRate * heaterOn - CoolRate * coolerOn
/// </summary>
public sealed class Plant
{
    public double Ambient { get; set; } = 20.0;
    public double LagPerSec { get; set; } = 0.02;     // 1/s toward ambient
    public double HeatRate { get; set; } = 6.0;       // deg/s with the heater on
    public double CoolRate { get; set; } = 6.0;       // deg/s with the cooler on
    public double Temperature { get; set; } = 20.0;

    public void Step(double dtSeconds, bool heaterOn, bool coolerOn)
    {
        double drive = (heaterOn ? HeatRate : 0.0) - (coolerOn ? CoolRate : 0.0);
        Temperature += dtSeconds * ((Ambient - Temperature) * LagPerSec + drive);
    }
}

public enum SensorFault { None, Open, StuckLast, StuckValue }

/// <summary>A thermocouple: offset, lag toward the plant, deterministic noise, override, and fault injection.</summary>
public sealed class SensorModel
{
    public double Offset { get; set; }
    public double NoiseAmplitude { get; set; }
    public double LagPerSec { get; set; } = 0.0;      // 0 = follows the plant instantly
    public SensorFault Fault { get; set; } = SensorFault.None;
    public double StuckValue { get; set; }
    /// <summary>When set, the reading is forced to <see cref="OverrideValue"/> (operator slider).</summary>
    public bool Override { get; set; }
    public double OverrideValue { get; set; }

    double _lagged = double.NaN, _last = double.NaN;

    public double Read(double plantTemp, double dtSeconds, Rng rng)
    {
        double truth = plantTemp + Offset;
        if (LagPerSec > 0.0 && !double.IsNaN(_lagged)) _lagged += dtSeconds * (truth - _lagged) * LagPerSec;
        else _lagged = truth;
        double v = _lagged;
        if (NoiseAmplitude > 0.0) v += rng.NextSigned() * NoiseAmplitude;
        if (Override) v = OverrideValue;
        switch (Fault)
        {
            case SensorFault.Open: return double.NaN;
            case SensorFault.StuckLast: return double.IsNaN(_last) ? v : _last;
            case SensorFault.StuckValue: return StuckValue;
        }
        _last = v;
        return v;
    }
}

/// <summary>A relay with a feedback contact: honest by default; can be stuck open/closed or slow to answer.</summary>
public sealed class RelayModel
{
    public bool StuckOpen { get; set; }      // never closes: feedback stays 0
    public bool StuckClosed { get; set; }    // never opens: feedback stays 1
    public int DelayTicks { get; set; }      // ticks between command and contact answer
    readonly Queue<bool> _pipeline = new();
    public bool Contact { get; private set; }

    /// <summary>Apply a command and return the physical state (drives the plant and the feedback input).</summary>
    public bool Apply(bool command)
    {
        _pipeline.Enqueue(command);
        while (_pipeline.Count > DelayTicks + 1) _pipeline.Dequeue();
        bool physical = _pipeline.Count > DelayTicks ? _pipeline.Peek() : Contact;
        if (StuckOpen) physical = false;
        if (StuckClosed) physical = true;
        Contact = physical;
        return physical;
    }
    public void Clear() { _pipeline.Clear(); Contact = false; }
}
