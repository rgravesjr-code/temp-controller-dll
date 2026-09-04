using System.Runtime.InteropServices;

namespace TempSim.Core.Native;

/// <summary>P/Invoke surface of tempctl.dll / libtempctl.so (src/tempctl.h).</summary>
public static unsafe class TempCtlNative
{
    public const string Lib = "tempctl";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern uint TcVersion();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int TcInputCount();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int TcSignalCount();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int TcStep(int zone, int action, uint nowMs, float* input, int inLen, float* output, int outLen);

    public static int Step(int zone, TcAction action, uint nowMs, ReadOnlySpan<float> input, Span<float> output)
    {
        fixed (float* pi = input) fixed (float* po = output)
            return TcStep(zone, (int)action, nowMs, pi, input.Length, po, output.Length);
    }
}

public enum TcAction { Init = 0, Step = 1, Reset = 2 }

/// <summary>enum TcSignal in tempctl.h: index into the SGL array.</summary>
public enum TcSignal
{
    Setpoint = 0, DeadbandHi = 1, DeadbandLo = 2, HiLimit = 3, LoLimit = 4,
    ErrorTimeoutMs = 5, DeadbandTimeoutMs = 6, FilterPoints = 7, Temp2Enable = 8, Temp2Tolerance = 9, FeedbackEnable = 10,
    Temp1 = 11, Temp2 = 12, HeaterFeedback = 13, CoolerFeedback = 14,
    HeatingCmd = 15, CoolingCmd = 16,
    ErrorStatus = 17, TempStatus = 18, ControlTemp = 19, Temp1Filtered = 20, Temp2Filtered = 21,
    HiBand = 22, LoBand = 23, ErrorRemainMs = 24, DbRemainMs = 25, ActiveSensor = 26,
}

public static class TcConst
{
    public const int InputCount = 17;
    public const int SignalCount = 27;
    public const int MaxZones = 16;
    public const int Ok = 0, WarnConfig = 1, ErrArg = -1, ErrZone = -2, ErrAction = -3, ErrNotInit = -4;
}

[Flags]
public enum TcErrorBits : uint
{
    None = 0,
    T1Hi = 1 << 0, T1Lo = 1 << 1, T1Bad = 1 << 2,
    T2Hi = 1 << 3, T2Lo = 1 << 4, T2Bad = 1 << 5,
    Disagree = 1 << 6, HeaterFeedback = 1 << 7, CoolerFeedback = 1 << 8, Config = 1 << 9,
}

public enum TcTempStatus
{
    InBand = 0, HeatPending = 1, Heating = 2, CoolPending = 3, Cooling = 4,
    ErrorPending = 5, Stopped = 6, Degraded = 7, FilterWarmup = 8,
}
