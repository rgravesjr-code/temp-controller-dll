using System.Runtime.InteropServices;

namespace TempSim.Core.Native;

/// <summary>P/Invoke surface of cantp.dll / libcantp.so (third_party/cantp/cantp.h).</summary>
public static unsafe class CanTpNative
{
    public const string Lib = "cantp";
    public const int MsgDefCols = 8, SigDefCols = 8, RecordMin = 24, NclHeaderSize = 12;
    public const uint XnetExtendedIdFlag = 0x20000000u;
    public const int Found = 1;

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern uint CanTp_Version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_Define(int slot, double* msgDef, int msgDefLen, double* sigDefs, int nSig);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_Clear(int slot);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_SignalCount(int slot);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_PayloadLength(int slot);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_FrameCount(int slot);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_OutputSize(int slot);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_PackSgl(int slot, float* values, int nValues, ulong timestamp100ns, ulong spacing100ns, byte* output, int outLen, int* bytesWritten);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_Unpack(int slot, byte* frames, int framesLen, double* values, int nValues, int* bytesConsumed);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_RxFeed(int slot, byte* frame, int frameLen, double* values, int nValues);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_RxReset(int slot);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_RecordSize(byte* rec, int avail);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int CanTp_NclHeader(byte* output, int outLen);

    public static int Define(int slot, ReadOnlySpan<double> msgDef, ReadOnlySpan<double> sigDefs, int nSig)
    {
        fixed (double* pm = msgDef) fixed (double* ps = sigDefs)
            return CanTp_Define(slot, pm, msgDef.Length, ps, nSig);
    }
    public static int PackSgl(int slot, ReadOnlySpan<float> values, ulong ts, ulong spacing, Span<byte> output, out int written)
    {
        int w, rc;
        fixed (float* pv = values) fixed (byte* po = output) rc = CanTp_PackSgl(slot, pv, values.Length, ts, spacing, po, output.Length, &w);
        written = w;
        return rc;
    }
    public static int Unpack(int slot, ReadOnlySpan<byte> frames, Span<double> values, out int consumed)
    {
        int c, rc;
        fixed (byte* pf = frames) fixed (double* pv = values) rc = CanTp_Unpack(slot, pf, frames.Length, pv, values.Length, &c);
        consumed = c;
        return rc;
    }
    public static int RxFeed(int slot, ReadOnlySpan<byte> frame, Span<double> values)
    {
        fixed (byte* pf = frame) fixed (double* pv = values) return CanTp_RxFeed(slot, pf, frame.Length, pv, values.Length);
    }
    public static int RecordSize(ReadOnlySpan<byte> rec)
    {
        fixed (byte* p = rec) return CanTp_RecordSize(p, rec.Length);
    }
    public static byte[] NclHeader()
    {
        var h = new byte[NclHeaderSize];
        fixed (byte* p = h) CanTp_NclHeader(p, h.Length);
        return h;
    }
}
