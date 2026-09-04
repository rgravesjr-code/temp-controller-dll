using System.Runtime.InteropServices;
using System.Text;
using TempSim.Core.Native;

namespace TempSim.Core;

/// <summary>
/// Linux SocketCAN (PF_CAN / CAN_RAW) through libc: puts the NI-XNET raw records CanTp produced onto a real
/// interface (Raspberry Pi bench, or any Linux box with a CAN adapter), and reads frames back for loop tests.
/// Windows: <see cref="Open"/> throws PlatformNotSupportedException.
/// </summary>
public sealed unsafe class SocketCan : IDisposable
{
    const int PF_CAN = 29, SOCK_RAW = 3, CAN_RAW = 1;
    const uint SIOCGIFINDEX = 0x8933;
    const uint CAN_EFF_FLAG = 0x80000000u, CAN_EFF_MASK = 0x1FFFFFFFu;
    const int FrameSize = 16;

    [DllImport("libc", SetLastError = true)] static extern int socket(int domain, int type, int protocol);
    [DllImport("libc", SetLastError = true)] static extern int ioctl(int fd, nuint request, void* arg);
    [DllImport("libc", SetLastError = true)] static extern int bind(int fd, void* addr, uint addrlen);
    [DllImport("libc", SetLastError = true)] static extern nint write(int fd, void* buf, nuint count);
    [DllImport("libc", SetLastError = true)] static extern nint read(int fd, void* buf, nuint count);
    [DllImport("libc", SetLastError = true)] static extern int close(int fd);
    [DllImport("libc", SetLastError = true)] static extern int poll(void* fds, nuint nfds, int timeout);

    [StructLayout(LayoutKind.Explicit, Size = 24)]
    struct SockAddrCan { [FieldOffset(0)] public ushort Family; [FieldOffset(4)] public int IfIndex; }
    [StructLayout(LayoutKind.Explicit, Size = 40)]
    struct IfReq { [FieldOffset(0)] public fixed byte Name[16]; [FieldOffset(16)] public int IfIndex; }
    [StructLayout(LayoutKind.Sequential)]
    struct PollFd { public int Fd; public short Events; public short REvents; }

    readonly int _fd;
    public string Interface { get; }
    public long FramesSent { get; private set; }

    SocketCan(int fd, string iface) { _fd = fd; Interface = iface; }

    public static SocketCan Open(string iface)
    {
        if (!OperatingSystem.IsLinux()) throw new PlatformNotSupportedException("SocketCAN is Linux only");
        int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (fd < 0) throw new IOException($"socket(PF_CAN) failed: errno {Marshal.GetLastWin32Error()}");
        var req = new IfReq();
        var name = Encoding.ASCII.GetBytes(iface);
        if (name.Length > 15) throw new ArgumentException("interface name too long");
        for (int i = 0; i < name.Length; i++) req.Name[i] = name[i];
        if (ioctl(fd, SIOCGIFINDEX, &req) < 0) { close(fd); throw new IOException($"SIOCGIFINDEX({iface}) failed: errno {Marshal.GetLastWin32Error()}"); }
        var addr = new SockAddrCan { Family = PF_CAN, IfIndex = req.IfIndex };
        if (bind(fd, &addr, (uint)sizeof(SockAddrCan)) < 0) { close(fd); throw new IOException($"bind({iface}) failed: errno {Marshal.GetLastWin32Error()}"); }
        return new SocketCan(fd, iface);
    }

    /// <summary>Send every classic-CAN record in a CanTp output buffer as one can_frame each.</summary>
    public void Send(ReadOnlySpan<byte> records)
    {
        byte* frame = stackalloc byte[FrameSize];
        for (int o = 0; o + Simulation.RecordSize <= records.Length; o += Simulation.RecordSize)
        {
            uint id = BitConverter.ToUInt32(records.Slice(o + 8, 4));
            int len = records[o + 15];
            if (len > 8) throw new NotSupportedException("CAN FD records are not sent by this simulator");
            new Span<byte>(frame, FrameSize).Clear();
            uint canId = (id & CAN_EFF_MASK) | ((id & CanTpNative.XnetExtendedIdFlag) != 0 ? CAN_EFF_FLAG : 0u);
            *(uint*)frame = canId;
            frame[4] = (byte)len;
            records.Slice(o + 16, len).CopyTo(new Span<byte>(frame + 8, 8));
            if (write(_fd, frame, FrameSize) != FrameSize) throw new IOException($"write({Interface}) failed: errno {Marshal.GetLastWin32Error()}");
            FramesSent++;
        }
    }

    /// <summary>Receive one frame as an NI-XNET raw record (24 bytes, timestamp 0). False on timeout.</summary>
    public bool TryReceive(Span<byte> record, int timeoutMs)
    {
        var pfd = new PollFd { Fd = _fd, Events = 1 /* POLLIN */ };
        if (poll(&pfd, 1, timeoutMs) <= 0) return false;
        byte* frame = stackalloc byte[FrameSize];
        if (read(_fd, frame, FrameSize) != FrameSize) return false;
        uint canId = *(uint*)frame;
        record.Clear();
        uint id = canId & CAN_EFF_MASK;
        if ((canId & CAN_EFF_FLAG) != 0) id |= CanTpNative.XnetExtendedIdFlag;
        BitConverter.TryWriteBytes(record.Slice(8, 4), id);
        record[15] = (byte)Math.Min(frame[4], (byte)8);
        new ReadOnlySpan<byte>(frame + 8, record[15]).CopyTo(record.Slice(16));
        return true;
    }

    public void Dispose() { if (_fd >= 0) close(_fd); }
}
