using System.Text.Json;
using System.Text.Json.Serialization;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// One CanTp message definition as written by CanTp's tools/dbc2tables.py (&lt;Message&gt;.json):
/// the 8-column msgdef row, the nSig x 8 sigdefs rows and the signal names/units in row order.
/// </summary>
public sealed class MessageTable
{
    [JsonPropertyName("message")] public string Message { get; set; } = "";
    [JsonPropertyName("msgdef")] public double[] MsgDef { get; set; } = Array.Empty<double>();
    [JsonPropertyName("sigdefs")] public double[][] SigDefs { get; set; } = Array.Empty<double[]>();
    [JsonPropertyName("signals")] public string[] Signals { get; set; } = Array.Empty<string>();
    [JsonPropertyName("units")] public string[] Units { get; set; } = Array.Empty<string>();
    [JsonPropertyName("transport")] public string Transport { get; set; } = "";

    public int SignalCount => SigDefs.Length;
    public uint CanId => (uint)MsgDef[0];
    public int Length => (int)MsgDef[2];

    public static MessageTable Load(string jsonPath)
    {
        var t = JsonSerializer.Deserialize<MessageTable>(File.ReadAllText(jsonPath))
                ?? throw new InvalidDataException("empty table " + jsonPath);
        if (t.MsgDef.Length != CanTpNative.MsgDefCols) throw new InvalidDataException("msgdef must have 8 columns");
        foreach (var r in t.SigDefs) if (r.Length != CanTpNative.SigDefCols) throw new InvalidDataException("sigdef rows must have 8 columns");
        return t;
    }

    /// <summary>The bundled TempCtl table (dbc/tables/TempCtl.json copied next to the executable).</summary>
    public static string DefaultPath => Path.Combine(AppContext.BaseDirectory, "TempCtl.json");

    /// <summary>Load into a CanTp slot; source address override replaces the DBC's 0xFE placeholder.</summary>
    public void Define(int slot, int sourceAddress = -1)
    {
        NativeLoader.Register();
        var msg = (double[])MsgDef.Clone();
        if (sourceAddress >= 0) msg[4] = sourceAddress;
        var flat = new double[SigDefs.Length * CanTpNative.SigDefCols];
        for (int i = 0; i < SigDefs.Length; i++) Array.Copy(SigDefs[i], 0, flat, i * CanTpNative.SigDefCols, CanTpNative.SigDefCols);
        int rc = CanTpNative.Define(slot, msg, flat, SigDefs.Length);
        if (rc != 0) throw new InvalidOperationException($"CanTp_Define failed: {rc}");
    }

    /// <summary>Resolution (factor) of a signal, for display rounding.</summary>
    public double Factor(int i) => SigDefs[i][4];
}
