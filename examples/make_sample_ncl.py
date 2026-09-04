"""
make_sample_ncl.py - run the controller against a toy thermal plant and write
the resulting J1939 BAM traffic as an NI-XNET logfile (.ncl) plus a CSV trace.

    python examples\make_sample_ncl.py [--dll build\win-x64\tempctl.dll] [--out examples\sample_tempctl.ncl]

Open the .ncl in NI-XNET Bus Monitor (File > Open Log) to see the frames.
"""
import argparse, ctypes as C, csv, os, struct, time

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('--dll', default=os.path.join(HERE, '..', 'build', 'win-x64', 'tempctl.dll'))
ap.add_argument('--out', default=os.path.join(HERE, 'sample_tempctl.ncl'))
ap.add_argument('--period-ms', type=int, default=100)
ap.add_argument('--seconds', type=int, default=120)
a = ap.parse_args()

lib = C.CDLL(os.path.abspath(a.dll))
lib.TcStep.argtypes = [C.c_int32, C.c_int32, C.c_uint32, C.POINTER(C.c_float), C.c_int32, C.POINTER(C.c_float), C.c_int32]
lib.TcEncodeFrames.argtypes = [C.POINTER(C.c_float), C.c_int32, C.c_uint32, C.c_uint8, C.c_uint8, C.c_uint64, C.c_uint64,
                               C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.TcNclHeader.argtypes = [C.POINTER(C.c_uint8), C.c_int32]

INIT, STEP, RESET = 0, 1, 2
NI_EPOCH_OFFSET_S = 11644473600          # 1601-01-01 -> 1970-01-01
def ni_ts(unix_s): return int((unix_s + NI_EPOCH_OFFSET_S) * 1e7)

# HiLimit LoLimit HiDb LoDb Setpoint Actual ErrTO DbTO Cool Heat Err
sig = (C.c_float * 13)(90, 10, 55, 45, 50, 20, 3000, 500, 0, 0, 0, 0, 0)
out = (C.c_float * 13)()
frames = (C.c_uint8 * (24 * 8))(); n = C.c_int32()
hdr = (C.c_uint8 * 12)(); lib.TcNclHeader(hdr, 12)

# toy plant: first-order lag toward ambient, heater/cooler push it
temp, ambient, dt = 20.0, 20.0, a.period_ms / 1000.0
t0 = time.time()
rc = lib.TcStep(0, INIT, 0, sig, 11, out, 13)
assert rc >= 0, rc

with open(a.out, 'wb') as f, open(os.path.splitext(a.out)[0] + '.csv', 'w', newline='') as cf:
    f.write(bytes(hdr))
    w = csv.writer(cf); w.writerow(['t_s', 'temp', 'setpoint', 'heat', 'cool', 'err', 'err_remain_ms', 'db_remain_ms'])
    steps = a.seconds * 1000 // a.period_ms
    for i in range(1, steps + 1):
        ms = i * a.period_ms
        if i == steps // 2:                                     # live setpoint change half way: move the deadbands with it
            sig[4], sig[3], sig[2] = 30.0, 25.0, 35.0
        stuck = steps * 3 // 4 <= i < steps * 3 // 4 + 40        # 4 s stuck sensor above HiLimit -> fault after 3 s
        if i == steps * 3 // 4 + 50:
            lib.TcStep(0, RESET, ms, sig, 11, out, 13)           # operator reset 1 s after the sensor recovers
        sig[5] = 95.0 if stuck else temp
        rc = lib.TcStep(0, STEP, ms, sig, 11, out, 13)
        assert rc >= 0, rc
        heat, cool = out[9], out[8]
        # plant update
        drive = 8.0 * heat - 8.0 * cool
        temp += dt * (0.05 * (ambient - temp) + drive)
        # transmit the full state as one BAM (1 TP.CM + 7 TP.DT), 50 ms between packets
        ts = ni_ts(t0 + ms / 1000.0)
        lib.TcEncodeFrames(out, 11, 0xFF00, 0x80, 6, ts, 50 * 10_000, frames, len(frames), C.byref(n))
        f.write(bytes(frames[:n.value]))
        w.writerow([ms / 1000.0, round(temp, 2), sig[4], int(heat), int(cool), int(out[10]), int(out[11]), int(out[12])])

size = os.path.getsize(a.out)
print(f'wrote {a.out}: {size} bytes = 12 header + {(size - 12) // 24} raw frames ({steps} BAM messages)')
print(f'trace: {os.path.splitext(a.out)[0] + ".csv"}')
