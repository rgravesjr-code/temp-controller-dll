using System.Diagnostics;

namespace TempSim.Core;

/// <summary>One outstanding transfer: preserve frame spacing without queuing overlapping BAMs or blocking control.</summary>
internal sealed class PacedFrameSender(Action<byte[]> write) : IDisposable
{
    Task _pending = Task.CompletedTask;

    public void Send(byte[] records)
    {
        if (!_pending.IsCompleted)
            throw new InvalidOperationException("Previous CAN transfer has not finished. Use real-time pacing and a longer CAN message period.");
        _pending.GetAwaiter().GetResult(); // propagate any bus-write failure before accepting another message
        if (records.Length == 0 || records.Length % Simulation.RecordSize != 0)
            throw new ArgumentException("CAN output must contain complete records.");
        var frames = new List<byte[]>();
        var delays = new List<TimeSpan>();
        ulong first = BitConverter.ToUInt64(records, 0), previous = first;
        for (int o = 0; o < records.Length; o += Simulation.RecordSize)
        {
            ulong timestamp = BitConverter.ToUInt64(records, o);
            if (timestamp < previous) throw new ArgumentException("CAN record timestamps must be nondecreasing.");
            delays.Add(TimeSpan.FromTicks(checked((long)(timestamp - first))));
            frames.Add(records.AsSpan(o, Simulation.RecordSize).ToArray());
            previous = timestamp;
        }
        _pending = Task.Run(() =>
        {
            var clock = Stopwatch.StartNew();
            var lastSent = TimeSpan.Zero;
            for (int i = 0; i < frames.Count; i++)
            {
                var due = i == 0 ? TimeSpan.Zero : lastSent + delays[i] - delays[i - 1];
                TimeSpan wait;
                while ((wait = due - clock.Elapsed) > TimeSpan.Zero)
                    Thread.Sleep(Math.Max(1, (int)Math.Ceiling(wait.TotalMilliseconds)));
                write(frames[i]);
                lastSent = clock.Elapsed;
            }
        });
    }

    public void Dispose() => _pending.GetAwaiter().GetResult();
}
