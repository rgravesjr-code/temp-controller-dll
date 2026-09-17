using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// Typed wrapper around one TempCtl v3 zone: fill <see cref="Setup"/> (or <see cref="LoadSetup"/>), call
/// <see cref="Init"/> / <see cref="CheckTemp"/> / <see cref="Reset"/>, read the outputs and <see cref="Diag"/>
/// (refreshed with TcGetDiag after every call). The arrays are exactly what the library sees.
/// </summary>
public sealed class Controller
{
    public int Zone { get; }
    public double[] Setup { get; } = new double[TcConst.SetupCount];
    public double[] Diag { get; } = new double[TcConst.DiagCount];
    public int LastRc { get; private set; }
    public bool Initialized { get; private set; }
    public TcStatus Status { get; private set; }
    public TcWarning Warning { get; private set; }
    public bool DoHeater { get; private set; }
    public bool DoCooler { get; private set; }

    public Controller(int zone = 0)
    {
        NativeLoader.Register();
        Zone = zone;
        if (TempCtlNative.TcSetupCount() != TcConst.SetupCount || TempCtlNative.TcDiagCount() != TcConst.DiagCount)
            throw new InvalidOperationException("tempctl library array sizes do not match this simulator");
        if (TempCtlNative.TcVersion() >> 16 != 3)
            throw new InvalidOperationException($"tempctl {NativeLoader.Describe()} is not a v3 library");
    }

    public double this[TcDiag d] => Diag[(int)d];
    public void SetSetup(TcSetup s, double v) => Setup[(int)s] = v;
    public double GetSetup(TcSetup s) => Setup[(int)s];
    public void LoadSetup(SimConfig.ControllerConfig c) => c.ToSetupArray().CopyTo(Setup, 0);

    /// <summary>TcInit: load / replace the setup (a running zone keeps its relays when the new setup is valid and enabled).</summary>
    public int Init(uint nowMs)
    {
        LastRc = TempCtlNative.Init(Zone, nowMs, Setup, out int st, out int wn);
        Check(LastRc, "TcInit");
        Status = (TcStatus)st; Warning = (TcWarning)wn;
        Initialized = true;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcCheckTemp: one control tick.</summary>
    public int CheckTemp(uint nowMs, double temp1, double temp2, bool heaterFb, bool coolerFb)
    {
        LastRc = TempCtlNative.CheckTemp(Zone, nowMs, temp1, temp2, heaterFb ? 1 : 0, coolerFb ? 1 : 0,
                                         out int dh, out int dc, out int st, out int wn);
        Check(LastRc, "TcCheckTemp");
        DoHeater = dh != 0; DoCooler = dc != 0; Status = (TcStatus)st; Warning = (TcWarning)wn;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcReset: clear faults and history, keep the setup, relays off.</summary>
    public int Reset(uint nowMs)
    {
        LastRc = TempCtlNative.Reset(Zone, nowMs, out int st, out int wn);
        Check(LastRc, "TcReset");
        Status = (TcStatus)st; Warning = (TcWarning)wn;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcGetDiag into <see cref="Diag"/>; the relay mirrors become <see cref="DoHeater"/> / <see cref="DoCooler"/>.</summary>
    public void RefreshDiag()
    {
        int rc = TempCtlNative.GetDiag(Zone, Diag);
        Check(rc, "TcGetDiag");
        DoHeater = Diag[(int)TcDiag.DoHeaterMirror] > 0.5;
        DoCooler = Diag[(int)TcDiag.DoCoolerMirror] > 0.5;
    }

    static void Check(int rc, string call) { if (rc < 0) throw new InvalidOperationException($"{call} returned {rc}"); }

    // ---- typed diagnostics ----
    public bool IsFault => TcConst.IsFault(Status);
    public double ControlTemp => Diag[(int)TcDiag.ControlTemp];
    public int ActiveSensor => (int)Diag[(int)TcDiag.ActiveSensor];
    public double Temp1Raw => Diag[(int)TcDiag.Temp1Raw];
    public double Temp2Raw => Diag[(int)TcDiag.Temp2Raw];
    public double Temp2Corrected => Diag[(int)TcDiag.Temp2Corrected];
    public double Temp1Avg => Diag[(int)TcDiag.Temp1Avg];
    public double Temp2Avg => Diag[(int)TcDiag.Temp2Avg];
    public double HiBand => Diag[(int)TcDiag.HiBand];
    public double LoBand => Diag[(int)TcDiag.LoBand];
    public bool InitialHcFlag => Diag[(int)TcDiag.InitialHcFlag] > 0.5;
    public double DeadbandRemainMs => Diag[(int)TcDiag.DeadbandRemainMs];
    public double AtSetPtRemainMs => Diag[(int)TcDiag.AtSetPtRemainMs];
    public double CompareRemainMs => Diag[(int)TcDiag.CompareRemainMs];
    public double HeaterFbRemainMs => Diag[(int)TcDiag.HeaterFbRemainMs];
    public double CoolerFbRemainMs => Diag[(int)TcDiag.CoolerFbRemainMs];
    public double Temp1OorAccumMs => Diag[(int)TcDiag.Temp1OorAccumMs];
    public double Temp2OorAccumMs => Diag[(int)TcDiag.Temp2OorAccumMs];
    public int Temp1OorEventsPerHour => (int)Diag[(int)TcDiag.Temp1OorEventsPerHour];
    public int Temp2OorEventsPerHour => (int)Diag[(int)TcDiag.Temp2OorEventsPerHour];
    public int AppliedFilterPoints => (int)Diag[(int)TcDiag.AppliedFilterPoints];
    public bool ZoneInitialized => Diag[(int)TcDiag.ZoneInitialized] > 0.5;

    public static string Describe(TcStatus s) => $"{TcConst.Name(s)} ({(int)s})";
    public static string Describe(TcWarning w) => $"{TcConst.Name(w)} ({(int)w})";
}
