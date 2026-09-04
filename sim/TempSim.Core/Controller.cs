using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// Typed wrapper around one TempCtl zone: fill <see cref="In"/>, call <see cref="Init"/>/<see cref="Step"/>/<see cref="Reset"/>,
/// read <see cref="Out"/>. The arrays are exactly the SGL arrays TcStep sees.
/// </summary>
public sealed class Controller
{
    public int Zone { get; }
    public float[] In { get; } = new float[TcConst.SignalCount];   // sized like Out so the same array could be used in place
    public float[] Out { get; } = new float[TcConst.SignalCount];
    public int LastRc { get; private set; }
    public bool Initialized { get; private set; }

    public Controller(int zone = 0)
    {
        NativeLoader.Register();
        Zone = zone;
        if (TempCtlNative.TcSignalCount() != TcConst.SignalCount || TempCtlNative.TcInputCount() != TcConst.InputCount)
            throw new InvalidOperationException("tempctl library signal layout does not match this simulator");
    }

    public float this[TcSignal s] { get => Out[(int)s]; }
    public void Set(TcSignal s, float v) => In[(int)s] = v;
    public float Get(TcSignal s) => In[(int)s];

    public int Init(uint nowMs) { LastRc = Call(TcAction.Init, nowMs); Initialized = true; return LastRc; }
    public int Reset(uint nowMs) { LastRc = Call(TcAction.Reset, nowMs); return LastRc; }
    public int Step(uint nowMs)
    {
        if (!Initialized) throw new InvalidOperationException("Init first");
        LastRc = Call(TcAction.Step, nowMs);
        return LastRc;
    }
    int Call(TcAction a, uint nowMs)
    {
        int rc = TempCtlNative.Step(Zone, a, nowMs, In.AsSpan(0, TcConst.InputCount), Out);
        if (rc < 0) throw new InvalidOperationException($"TcStep returned {rc}");
        return rc;
    }

    // ---- typed outputs ----
    public bool HeatingCmd => Out[(int)TcSignal.HeatingCmd] > 0.5f;
    public bool CoolingCmd => Out[(int)TcSignal.CoolingCmd] > 0.5f;
    public TcErrorBits ErrorStatus => (TcErrorBits)(uint)Out[(int)TcSignal.ErrorStatus];
    public TcTempStatus TempStatus => (TcTempStatus)(int)Out[(int)TcSignal.TempStatus];
    public float ControlTemp => Out[(int)TcSignal.ControlTemp];
    public float Temp1Filtered => Out[(int)TcSignal.Temp1Filtered];
    public float Temp2Filtered => Out[(int)TcSignal.Temp2Filtered];
    public float HiBand => Out[(int)TcSignal.HiBand];
    public float LoBand => Out[(int)TcSignal.LoBand];
    public float ErrorRemainMs => Out[(int)TcSignal.ErrorRemainMs];
    public float DbRemainMs => Out[(int)TcSignal.DbRemainMs];
    public int ActiveSensor => (int)Out[(int)TcSignal.ActiveSensor];

    public static string Describe(TcErrorBits bits)
    {
        if (bits == TcErrorBits.None) return "none";
        var parts = new List<string>();
        foreach (TcErrorBits b in Enum.GetValues<TcErrorBits>())
            if (b != TcErrorBits.None && bits.HasFlag(b)) parts.Add(b.ToString());
        return string.Join("|", parts);
    }
}
