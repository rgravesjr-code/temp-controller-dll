using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// Typed wrapper around one TempCtl v4 zone: fill <see cref="Setup"/> (or <see cref="LoadSetup"/>), call
/// <see cref="Init"/> / <see cref="Start"/> / <see cref="CheckTemp"/> / <see cref="Stop"/> / <see cref="Reset"/>,
/// read the outputs and <see cref="Diag"/> (refreshed with TcGetDiag after every call). The arrays are exactly
/// what the library sees.
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
        int ver = TempCtlNative.TcVersion();
        if (ver >> 16 != TcConst.Major)
            throw new InvalidOperationException($"tempctl {NativeLoader.Describe()} is not a v{TcConst.Major} library (TcStart / TcStop, {TcConst.SetupCount} setup values, {TcConst.DiagCount} diagnostics); this simulator drives TempCtl v{TcConst.Major} only");
        if (TempCtlNative.TcSetupCount() != TcConst.SetupCount || TempCtlNative.TcDiagCount() != TcConst.DiagCount)
            throw new InvalidOperationException($"tempctl reports {TempCtlNative.TcSetupCount()} setup / {TempCtlNative.TcDiagCount()} diagnostics values, this simulator expects {TcConst.SetupCount} / {TcConst.DiagCount}");
    }

    public double this[TcDiag d] => Diag[(int)d];
    public void SetSetup(TcSetup s, double v) => Setup[(int)s] = v;
    public double GetSetup(TcSetup s) => Setup[(int)s];
    public void LoadSetup(SimConfig.ControllerConfig c) => c.ToSetupArray().CopyTo(Setup, 0);

    /// <summary>TcInit: load / replace the setup. Leaves the zone IdleStopped: control runs only after <see cref="Start"/> (R10.2).</summary>
    public int Init(uint nowMs)
    {
        LastRc = TempCtlNative.Init(Zone, nowMs, Setup, out int st, out int wn);
        Check(LastRc, "TcInit");
        Status = (TcStatus)st; Warning = (TcWarning)wn;
        Initialized = true;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcStart: accepted when enabled, not faulted and runPermissive is true; refused (IdleStartBlocked) otherwise (R10.3).</summary>
    public int Start(uint nowMs, bool runPermissive)
    {
        LastRc = TempCtlNative.Start(Zone, nowMs, runPermissive ? 1 : 0, out int st, out int wn);
        Check(LastRc, "TcStart");
        Status = (TcStatus)st; Warning = (TcWarning)wn;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcStop: both relay commands 0 at once, no fault; the host applies the returned zeros (R10.4).</summary>
    public int Stop(uint nowMs)
    {
        LastRc = TempCtlNative.Stop(Zone, nowMs, out int dh, out int dc, out int st, out int wn);
        Check(LastRc, "TcStop");
        DoHeater = dh != 0; DoCooler = dc != 0; Status = (TcStatus)st; Warning = (TcWarning)wn;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcCheckTemp: one control tick with the live run permissive.</summary>
    public int CheckTemp(uint nowMs, double temp1, double temp2, bool heaterFb, bool coolerFb, bool runPermissive)
    {
        LastRc = TempCtlNative.CheckTemp(Zone, nowMs, temp1, temp2, heaterFb ? 1 : 0, coolerFb ? 1 : 0, runPermissive ? 1 : 0,
                                         out int dh, out int dc, out int st, out int wn);
        Check(LastRc, "TcCheckTemp");
        DoHeater = dh != 0; DoCooler = dc != 0; Status = (TcStatus)st; Warning = (TcWarning)wn;
        RefreshDiag();
        return LastRc;
    }

    /// <summary>TcReset: clear faults and history, keep the setup, relays off; leaves the zone IdleStopped (R10.5).</summary>
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
    public bool IsActive => TcConst.IsActive(Status);
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
    /// <summary>Last permissive evaluated by an enabled, non-faulted Start or CheckTemp: 0, 1, or NaN after Init / Reset.</summary>
    public double RunPermissive => Diag[(int)TcDiag.RunPermissive];
    public double OperatingConditionRemainMs => Diag[(int)TcDiag.OperatingConditionRemainMs];
    /// <summary>1 while a Start is accepted and control may run.</summary>
    public bool Started => Diag[(int)TcDiag.ControllerStarted] > 0.5;

    public static string Describe(TcStatus s) => $"{TcConst.Name(s)} ({(int)s})";
    public static string Describe(TcWarning w) => $"{TcConst.Name(w)} ({(int)w})";
}
