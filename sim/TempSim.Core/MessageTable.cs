using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// One CanTp message definition: the 8-column msgdef row, the nSig x 8 sigdefs rows and the signal names / units in
/// row order. Loaded either from CanTp's tools/dbc2tables.py output (&lt;Message&gt;.json) or from the generated
/// tempctl.ecd (an encrypted LabVIEW flatten of J1939Msg(V4) clusters, TempCtl v4); an ECD-loaded table keeps the
/// flattened cluster and defines its CanTp slot with CanTp_DefineFlat.
/// </summary>
public sealed class MessageTable
{
    [JsonPropertyName("message")] public string Message { get; set; } = "";
    [JsonPropertyName("msgdef")] public double[] MsgDef { get; set; } = Array.Empty<double>();
    [JsonPropertyName("sigdefs")] public double[][] SigDefs { get; set; } = Array.Empty<double[]>();
    [JsonPropertyName("signals")] public string[] Signals { get; set; } = Array.Empty<string>();
    [JsonPropertyName("units")] public string[] Units { get; set; } = Array.Empty<string>();
    [JsonPropertyName("transport")] public string Transport { get; set; } = "";
    /// <summary>The flattened J1939Msg(V4) cluster when the table came from an .ecd (null for a JSON table).</summary>
    [JsonIgnore] public byte[]? Flat { get; private set; }
    [JsonIgnore] public string Source { get; private set; } = "";

    public int SignalCount => SigDefs.Length;
    public uint CanId => (uint)MsgDef[0];
    public int Length => (int)MsgDef[2];

    /// <summary>Load a .json table or a .ecd database (by extension).</summary>
    public static MessageTable Load(string path) =>
        path.EndsWith(".ecd", StringComparison.OrdinalIgnoreCase) ? LoadEcd(path) : LoadJson(path);

    public static MessageTable LoadJson(string jsonPath)
    {
        var t = JsonSerializer.Deserialize<MessageTable>(File.ReadAllText(jsonPath))
                ?? throw new InvalidDataException("empty table " + jsonPath);
        if (t.MsgDef.Length != CanTpNative.MsgDefCols) throw new InvalidDataException("msgdef must have 8 columns");
        foreach (var r in t.SigDefs) if (r.Length != CanTpNative.SigDefCols) throw new InvalidDataException("sigdef rows must have 8 columns");
        t.Source = Path.GetFileName(jsonPath);
        return t;
    }

    /// <summary>The bundled TempCtl table (dbc/tables/TempCtl.json copied next to the executable).</summary>
    public static string DefaultPath => Path.Combine(AppContext.BaseDirectory, "TempCtl.json");
    /// <summary>The bundled tempctl.ecd (dbc/tempctl.ecd copied next to the executable), or null when absent.</summary>
    public static string? DefaultEcdPath { get { var p = Path.Combine(AppContext.BaseDirectory, "tempctl.ecd"); return File.Exists(p) ? p : null; } }

    /// <summary>Load into a CanTp slot; source address override replaces the DBC's 0xFE placeholder.</summary>
    public void Define(int slot, int sourceAddress = -1)
    {
        NativeLoader.Register();
        int rc;
        if (Flat != null)
        {
            rc = CanTpNative.DefineFlat(slot, Flat, -1, sourceAddress);
            if (rc != 0) throw new InvalidOperationException($"CanTp_DefineFlat failed: {rc} ({Source})");
            // the slot CanTp derived from the cluster must be the table this object describes
            var md = new double[CanTpNative.MsgDefCols]; var sd = new double[CanTpNative.SigDefCols * Math.Max(SigDefs.Length, 1)];
            int n = CanTpNative.GetDef(slot, md, sd);
            if (n != SigDefs.Length) throw new InvalidOperationException($"CanTp_DefineFlat made {n} signals from {Source}, expected {SigDefs.Length}");
            for (int i = 0; i < SigDefs.Length; i++)
                for (int k = 0; k < CanTpNative.SigDefCols; k++)
                    if (sd[i * CanTpNative.SigDefCols + k] != SigDefs[i][k]) throw new InvalidOperationException($"CanTp_DefineFlat row {i} ({Signals[i]}) column {k} differs from the ECD channel");
            return;
        }
        var msg = (double[])MsgDef.Clone();
        if (sourceAddress >= 0) msg[4] = sourceAddress;
        var flat = new double[SigDefs.Length * CanTpNative.SigDefCols];
        for (int i = 0; i < SigDefs.Length; i++) Array.Copy(SigDefs[i], 0, flat, i * CanTpNative.SigDefCols, CanTpNative.SigDefCols);
        rc = CanTpNative.Define(slot, msg, flat, SigDefs.Length);
        if (rc != 0) throw new InvalidOperationException($"CanTp_Define failed: {rc}");
    }

    /// <summary>Resolution (factor) of a signal, for display rounding.</summary>
    public double Factor(int i) => SigDefs[i][4];

    /// <summary>
    /// Compare two definitions of the same message signal by signal (id, length, transport, every sigdef column, names);
    /// returns null when identical, otherwise the first difference. Used to refuse a stale tempctl.ecd / TempCtl.json pair.
    /// </summary>
    public string? DifferenceFrom(MessageTable other)
    {
        if (SignalCount != other.SignalCount) return $"{Source} has {SignalCount} signals, {other.Source} has {other.SignalCount}";
        for (int k = 0; k < CanTpNative.MsgDefCols; k++)
            if (k != 4 && MsgDef[k] != other.MsgDef[k]) return $"msgdef column {k}: {MsgDef[k]} vs {other.MsgDef[k]}";   // column 4 = SA placeholder / override
        for (int i = 0; i < SignalCount; i++)
        {
            if (!string.Equals(Signals[i], other.Signals[i], StringComparison.Ordinal)) return $"signal {i}: {Signals[i]} vs {other.Signals[i]} (channel order)";
            for (int k = 0; k < CanTpNative.SigDefCols; k++)
                if (SigDefs[i][k] != other.SigDefs[i][k]) return $"{Signals[i]} column {k}: {SigDefs[i][k]} vs {other.SigDefs[i][k]}";
        }
        return null;
    }

    // ---------------------------------------------------------------- .ecd reader (layout: CanTp tools/ecdflat.py)
    /// <summary>
    /// Read the TempCtl message of an .ecd: decrypt (reverse; first byte = offset digit; swap byte pairs; add the offset),
    /// then parse the big-endian LabVIEW flatten of an array of J1939Msg(V4) clusters. The CanTp rows are derived the way
    /// CanTp_DefineFlat derives them (Intel channels; a Motorola start bit is converted from the NI-XNET form).
    /// </summary>
    public static MessageTable LoadEcd(string path, string message = "TempCtl")
    {
        var plain = Decrypt(File.ReadAllBytes(path));
        var r = new Reader(plain);
        int count = r.I32();
        for (int m = 0; m < count; m++)
        {
            int start = r.Pos;
            string name = r.Str(); uint msgId = r.U32(); uint pgn = r.U32(); byte extended = r.U8(); int numBytes = r.I32();
            string desc = r.Str(); double updateRate = r.F64(); r.F64();
            int nCh = r.I32();
            var sig = new List<double[]>(nCh); var names = new List<string>(nCh); var units = new List<string>(nCh);
            for (int c = 0; c < nCh; c++)
            {
                string cname = r.Str(); int startBit = r.I32(); int nBits = r.I32(); ushort dataType = r.U16(); ushort byteOrder = r.U16();
                double sf = r.F64(), offset = r.F64(), min = r.F64(), max = r.F64(); r.F64();
                string unit = r.Str(); r.U16(); int nLut = r.I32();
                for (int l = 0; l < nLut; l++) { r.F64(); r.Str(); r.Str(); r.Str(); }
                r.Str();
                double vtype = dataType switch { 0 => 1.0, 1 => 0.0, _ => nBits == 32 ? 2.0 : 3.0 };
                int dbcStart = byteOrder != 0 ? MotorolaToDbc(startBit, nBits) : startBit;
                sig.Add(new[] { dbcStart, nBits, byteOrder != 0 ? 1.0 : 0.0, vtype, sf, offset, min, max });
                names.Add(cname); units.Add(unit);
            }
            r.U8();
            int end = r.Pos;
            if (!string.Equals(name, message, StringComparison.Ordinal) && !(count == 1)) continue;
            uint ident = msgId & 0x1FFFFFFF;
            bool ext = extended != 0 || msgId >= 0x80000000u || ident > 0x7FF;
            bool pdu1 = ext && ((ident >> 16) & 0xFF) < 0xF0;
            uint ps = (ident >> 8) & 0xFF;
            bool realDa = pdu1 && ps != 0xFF && ps != 0xFE;
            int transport = !ext ? (numBytes <= 8 ? 0 : 2) : realDa ? (numBytes <= 8 ? 0 : 4) : 1;
            double da = pdu1 && transport != 1 ? ps : 255;
            double pad = ext ? 255 : 0;
            double sa = !ext || (ident & 0xFF) == 0xFE ? -1 : ident & 0xFF;
            var t = new MessageTable
            {
                Message = name,
                MsgDef = new[] { (double)ident, ext ? 1.0 : 0.0, numBytes, transport, sa, da, pad, updateRate > 0 ? updateRate : 0.0 },
                SigDefs = sig.ToArray(), Signals = names.ToArray(), Units = units.ToArray(),
                Transport = new[] { "classic", "j1939_bam", "canfd", "canfd_brs", "j1939_rts", "isotp" }[transport],
                Flat = plain.AsSpan(start, end - start).ToArray(),
                Source = Path.GetFileName(path),
            };
            return t;
        }
        throw new InvalidDataException($"{path}: no message '{message}' among {count}");
    }

    static byte[] Decrypt(byte[] data)
    {
        int n = BinaryPrimitives.ReadInt32BigEndian(data);
        if (n < 2 || 4 + n > data.Length) throw new InvalidDataException("not an .ecd file (bad length prefix)");
        var rev = data.AsSpan(4, n).ToArray(); Array.Reverse(rev);
        int off = rev[0] - 48;
        if (off < 0 || off > 9) throw new InvalidDataException("not an .ecd file (bad offset digit)");
        var body = rev.AsSpan(1).ToArray();
        for (int i = 0; i + 1 < body.Length; i += 2) (body[i], body[i + 1]) = (body[i + 1], body[i]);
        for (int i = 0; i < body.Length; i++) body[i] = (byte)(body[i] + off);
        return body;
    }

    /// <summary>ECD / NI-XNET Motorola start bit -> DBC MSB (sawtooth) start bit (mirrors CanTp flat.c / ecdflat.py).</summary>
    static int MotorolaToDbc(int start, int nbits)
    {
        if (start < 0 || nbits < 1) return -1;
        int lsb = start + (nbits >= 8 ? 8 * (nbits / 8 - 1) : 0);
        int b = lsb / 8, bit = lsb % 8;
        for (int i = 0; i < nbits - 1; i++) { bit++; if (bit > 7) { bit = 0; b--; } }
        return b >= 0 ? b * 8 + bit : -1;
    }

    sealed class Reader
    {
        readonly byte[] _b; public int Pos;
        public Reader(byte[] b) { _b = b; }
        ReadOnlySpan<byte> Take(int n) { if (Pos + n > _b.Length) throw new InvalidDataException("truncated .ecd cluster"); var s = _b.AsSpan(Pos, n); Pos += n; return s; }
        public int I32() => BinaryPrimitives.ReadInt32BigEndian(Take(4));
        public uint U32() => BinaryPrimitives.ReadUInt32BigEndian(Take(4));
        public ushort U16() => BinaryPrimitives.ReadUInt16BigEndian(Take(2));
        public byte U8() => Take(1)[0];
        public double F64() => BitConverter.Int64BitsToDouble(BinaryPrimitives.ReadInt64BigEndian(Take(8)));
        public string Str() { int n = I32(); if (n < 0) throw new InvalidDataException("bad string length"); return Encoding.Latin1.GetString(Take(n)); }
    }

    /// <summary>
    /// The shipped tempctl.ecd next to the executable must describe exactly the shipped TempCtl.json; a stale pair
    /// (v3 ECD with a v4 table, reordered channels) is refused before any scenario runs. Returns a one-line summary.
    /// </summary>
    public static string CheckShippedEcd(MessageTable jsonTable, string? ecdPath = null)
    {
        ecdPath ??= DefaultEcdPath;
        if (ecdPath == null) return "tempctl.ecd: not present next to the executable (CanTp defined from TempCtl.json only)";
        var ecd = LoadEcd(ecdPath);
        var diff = ecd.DifferenceFrom(jsonTable);
        if (diff != null) throw new InvalidOperationException($"{Path.GetFileName(ecdPath)} does not match {jsonTable.Source}: {diff}. Regenerate both with tools\\make_tempctl_dbc.py --tables --ecd.");
        return $"{Path.GetFileName(ecdPath)}: {ecd.SignalCount} channels in TC_DIAG order, {ecd.Length} bytes, identical to {jsonTable.Source} (CanTp_DefineFlat gives the same slot)";
    }
}
