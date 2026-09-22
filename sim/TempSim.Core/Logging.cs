using System.Text;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>Writes the per-tick state table (LF line endings, invariant culture, identical on every platform).</summary>
public sealed class CsvLogger : IDisposable
{
    readonly StreamWriter _w;
    StreamWriter? _fixture;
    public string Path { get; }
    public int Rows { get; private set; }
    public CsvLogger(string path)
    {
        Path = path;
        _w = new StreamWriter(path, false, new UTF8Encoding(false)) { NewLine = "\n" };
        _w.WriteLine(Simulation.CsvHeader);
    }
    public void Log(Simulation s)
    {
        _w.WriteLine(s.CsvRow()); Rows++;
        if (s.Config.Fixture.Enabled && _fixture == null)
        {
            _fixture = new StreamWriter(System.IO.Path.ChangeExtension(Path, ".fixture.csv"), false, new UTF8Encoding(false)) { NewLine = "\n" };
            _fixture.WriteLine("t_s,enabled,temp_units,inlet,outlet,delta_out_minus_in,flow_lpm,actuation_heat_w,heater_heat_w,cooling_w");
        }
        if (_fixture != null)
        {
            bool enabled = s.Config.Fixture.Enabled;
            var f = s.Fixture;
            static string F(double v) => Math.Round(v, 4).ToString("0.####", System.Globalization.CultureInfo.InvariantCulture);
            _fixture.WriteLine(string.Join(',', new[] { s.TimeSeconds, enabled ? 1.0 : 0, s.Config.Controller.TempUnits,
                enabled ? f.InletTemperature : double.NaN, enabled ? f.OutletTemperature : double.NaN,
                enabled ? f.DeltaTemperature : double.NaN, enabled ? f.FlowLpm : double.NaN,
                enabled ? f.MotorHeatW : double.NaN, enabled ? f.HeaterHeatW : double.NaN,
                enabled ? f.CoolingW : double.NaN }.Select(F)));
        }
    }
    public void Dispose() { _w.Dispose(); _fixture?.Dispose(); }
}

/// <summary>NI-XNET logfile (.ncl): 12-byte header from CanTp_NclHeader, then the raw records of every tick.</summary>
public sealed class NclWriter : IDisposable
{
    readonly FileStream _f;
    public string Path { get; }
    public long Records { get; private set; }
    public NclWriter(string path)
    {
        Path = path;
        _f = new FileStream(path, FileMode.Create, FileAccess.Write);
        _f.Write(CanTpNative.NclHeader());
    }
    public void Log(Simulation s) { _f.Write(s.Frames); Records += s.Frames.Length / Simulation.RecordSize; }
    public void Dispose() => _f.Dispose();
}
