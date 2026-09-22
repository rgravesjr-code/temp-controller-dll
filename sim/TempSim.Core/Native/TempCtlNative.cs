using System.Runtime.InteropServices;

namespace TempSim.Core.Native;

/// <summary>P/Invoke surface of tempctl.dll / libtempctl.so (src/tempctl.h, TempCtl v4).</summary>
public static unsafe class TempCtlNative
{
    public const string Lib = "tempctl";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int TcVersion();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int TcSetupCount();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int TcDiagCount();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcInit(int zone, uint nowMs, double* setupArray, int setupLen, int* status, int* warning);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcStart(int zone, uint nowMs, int runPermissive, int* status, int* warning);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcStop(int zone, uint nowMs, int* doHeater, int* doCooler, int* status, int* warning);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcCheckTemp(int zone, uint nowMs, double temp1, double temp2, int diHeaterFB, int diCoolerFB,
                                         int runPermissive, int* doHeater, int* doCooler, int* status, int* warning);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcReset(int zone, uint nowMs, int* status, int* warning);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcGetDiag(int zone, double* diagArray, int diagLen);

    public static int Init(int zone, uint nowMs, ReadOnlySpan<double> setup, out int status, out int warning)
    {
        int st, wn, rc;
        fixed (double* p = setup) rc = TcInit(zone, nowMs, p, setup.Length, &st, &wn);
        status = st; warning = wn;
        return rc;
    }
    public static int Start(int zone, uint nowMs, int runPermissive, out int status, out int warning)
    {
        int st, wn;
        int rc = TcStart(zone, nowMs, runPermissive, &st, &wn);
        status = st; warning = wn;
        return rc;
    }
    public static int Stop(int zone, uint nowMs, out int doHeater, out int doCooler, out int status, out int warning)
    {
        int dh, dc, st, wn;
        int rc = TcStop(zone, nowMs, &dh, &dc, &st, &wn);
        doHeater = dh; doCooler = dc; status = st; warning = wn;
        return rc;
    }
    public static int CheckTemp(int zone, uint nowMs, double temp1, double temp2, int hfb, int cfb, int runPermissive,
                                out int doHeater, out int doCooler, out int status, out int warning)
    {
        int dh, dc, st, wn;
        int rc = TcCheckTemp(zone, nowMs, temp1, temp2, hfb, cfb, runPermissive, &dh, &dc, &st, &wn);
        doHeater = dh; doCooler = dc; status = st; warning = wn;
        return rc;
    }
    public static int Reset(int zone, uint nowMs, out int status, out int warning)
    {
        int st, wn;
        int rc = TcReset(zone, nowMs, &st, &wn);
        status = st; warning = wn;
        return rc;
    }
    public static int GetDiag(int zone, Span<double> diag)
    {
        fixed (double* p = diag) return TcGetDiag(zone, p, diag.Length);
    }
}

/// <summary>TC_SETUP_* in tempctl.h: index into the setup array of TcInit (0..16 as in v3, 17 new in v4).</summary>
public enum TcSetup
{
    TempCtrlEnable = 0, TempUnits = 1, Setpoint = 2, DeadbandHi = 3, DeadbandLo = 4, HiLimit = 5, LoLimit = 6,
    ErrorTimeout = 7, DeadbandTimeout = 8, AtSetPtTimeout = 9, Temp2Enable = 10, Temp2Offset = 11, Temp2Tolerance = 12,
    TempCompareTimeout = 13, FilterPoints = 14, FeedbackEnable = 15, RelayFeedbackTimeout = 16,
    OperatingConditionTimeout = 17,
}

/// <summary>TC_DIAG_* in tempctl.h: index into the diagnostics array of TcGetDiag (0..24 as in v3, 25..27 new in v4).</summary>
public enum TcDiag
{
    ControlTemp = 0, ActiveSensor = 1, Temp1Raw = 2, Temp2Raw = 3, Temp2Corrected = 4, Temp1Avg = 5, Temp2Avg = 6,
    HiBand = 7, LoBand = 8, InitialHcFlag = 9, DeadbandRemainMs = 10, AtSetPtRemainMs = 11, CompareRemainMs = 12,
    HeaterFbRemainMs = 13, CoolerFbRemainMs = 14, Temp1OorAccumMs = 15, Temp2OorAccumMs = 16,
    Temp1OorEventsPerHour = 17, Temp2OorEventsPerHour = 18, StatusMirror = 19, WarningMirror = 20,
    DoHeaterMirror = 21, DoCoolerMirror = 22, AppliedFilterPoints = 23, ZoneInitialized = 24,
    RunPermissive = 25, OperatingConditionRemainMs = 26, ControllerStarted = 27,
}

/// <summary>TC_ST_* status codes: 0..5 active-control states, 6..9 lifecycle states, 10 and above are faults.</summary>
public enum TcStatus
{
    TempCtrlDisabled = 0, TempAtSetPt = 1, HeaterON = 2, CoolerON = 3, HeatPending = 4, CoolPending = 5,
    IdleStopped = 6, IdleStartBlocked = 7, OperatingConditionPending = 8, IdleOperatingConditionTripped = 9,
    Temp1FailHigh = 10, Temp1FailLow = 11, BothSensorsFailed = 12, TempDisagreeFault = 13, ConfigFault = 14,
    HeaterFBFault = 15, CoolerFBFault = 16, OperatingConditionFault = 17,
}

/// <summary>TC_WN_* warning codes (one value, the lowest active code).</summary>
public enum TcWarning
{
    NoWarning = 0, Temp1OutOfRange = 1, Temp2OutOfRange = 2, HeaterFBMismatch = 3, CoolerFBMismatch = 4,
    TempDisagree = 5, RunningOnTemp2 = 6, ConfigInvalid = 7, OperatingConditionNotMet = 8,
}

public static class TcConst
{
    public const int Major = 4;
    public const int SetupCount = 18;
    public const int DiagCount = 28;
    public const int MaxZones = 16;
    public const int FaultFirst = 10;
    public const int Ok = 0, ErrArg = -1, ErrZone = -2;
    public static bool IsFault(TcStatus s) => (int)s >= FaultFirst;
    /// <summary>Started and controlling (TempAtSetPt .. CoolPending).</summary>
    public static bool IsActive(TcStatus s) => (int)s >= 1 && (int)s <= 5;
    /// <summary>Enabled but not controlling: IdleStopped, IdleStartBlocked, OperatingConditionPending, IdleOperatingConditionTripped.</summary>
    public static bool IsIdle(TcStatus s) => (int)s >= 6 && (int)s <= 9;
    public static string Name(TcStatus s) => Enum.IsDefined(s) ? s.ToString() : $"Status{(int)s}";
    public static string Name(TcWarning w) => Enum.IsDefined(w) ? w.ToString() : $"Warning{(int)w}";
}
