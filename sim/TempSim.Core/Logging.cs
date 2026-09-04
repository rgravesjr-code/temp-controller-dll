using System.Text;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>Writes the per-tick state table (LF line endings, invariant culture, identical on every platform).</summary>
public sealed class CsvLogger : IDisposable
{
    readonly StreamWriter _w;
    public string Path { get; }
    public int Rows { get; private set; }
    public CsvLogger(string path)
    {
        Path = path;
        _w = new StreamWriter(path, false, new UTF8Encoding(false)) { NewLine = "\n" };
        _w.WriteLine(Simulation.CsvHeader);
    }
    public void Log(Simulation s) { _w.WriteLine(s.CsvRow()); Rows++; }
    public void Dispose() => _w.Dispose();
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
