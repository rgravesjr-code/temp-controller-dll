"""
pi_session_loop.py - live SocketCAN test of the CanTp session transports (J1939 RTS/CTS and
ISO-TP) between two Linux nodes, each running libcantp.so through ctypes and python-can.

    receiver:  python3 pi_session_loop.py --role rx --iface can0 --so /tmp/libcantp.so [--transport rts|isotp] [--rounds 3]
    sender:    python3 pi_session_loop.py --role tx --iface can1 --so /tmp/libcantp.so [--transport rts|isotp] [--rounds 3]

Start the receiver first. The sender packs a 60-byte message (byte-valued signals from a
seeded generator, so both sides know the expected values), runs CanTp_TxStart / CanTp_TxFeed
against the frames coming back from the bus; the receiver runs CanTp_RxStep and sends its
CTS / EndOfMsgAck / flow-control frames. Exit code 0 = every round decoded identically.
"""
import argparse, ctypes as C, random, struct, sys, time
import can  # python-can

ap = argparse.ArgumentParser()
ap.add_argument('--role', choices=['tx', 'rx'], required=True)
ap.add_argument('--so', default='./libcantp.so')
ap.add_argument('--iface', default='can0')
ap.add_argument('--transport', choices=['rts', 'isotp'], default='rts')
ap.add_argument('--rounds', type=int, default=3)
ap.add_argument('--bs', type=int, default=4, help='receiver block size / CTS window')
ap.add_argument('--timeout', type=float, default=20.0)
a = ap.parse_args()

lib = C.CDLL(a.so)
lib.CanTp_Version.restype = C.c_uint32
lib.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_SessionConfig.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_TxStart.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.c_uint32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_TxFeed.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_RxStep.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint32, C.c_uint64, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_MakeRecord.argtypes = [C.c_uint32, C.c_int32, C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32]
lib.CanTp_TxReset.argtypes = [C.c_int32]; lib.CanTp_RxReset.argtypes = [C.c_int32]
for f in (lib.CanTp_Define, lib.CanTp_SessionConfig, lib.CanTp_TxStart, lib.CanTp_TxFeed, lib.CanTp_RxStep, lib.CanTp_MakeRecord, lib.CanTp_TxReset, lib.CanTp_RxReset): f.restype = C.c_int32

N = 60
SA, DA, PGN = 0x80, 0x21, 0xFF30
if a.transport == 'rts':
    msgdef = [0x18000000 | (PGN << 8) | SA, 1, N, 4, SA, DA, 255, 0]
else:
    msgdef = [0x7E0, 0, N, 5, -1, 255, 0xCC, 0]          # flow control on 0x7E8 (default)
sigdefs = [[8 * i, 8, 0, 0, 1, 0, 0, 0] for i in range(N)]
md = (C.c_double * 8)(*msgdef); sd = (C.c_double * (8 * N))(*[x for r in sigdefs for x in r])
rc = lib.CanTp_Define(0, md, 8, sd, N)
assert rc == 0, rc
ses = (C.c_double * 6)(-1, -1, a.bs, 0, 0, 0)
lib.CanTp_SessionConfig(0, ses, 6)
print(f'cantp {lib.CanTp_Version():#08x} {a.role} on {a.iface}, {a.transport}, {N} bytes, bs {a.bs}')

bus = can.Bus(interface='socketcan', channel=a.iface)
out = (C.c_uint8 * 8192)(); w = C.c_int32()
vals = (C.c_double * N)()
t0 = time.monotonic()
def now_ms(): return int((time.monotonic() - t0) * 1000) & 0xFFFFFFFF

def send_records(buf, n):
    for o in range(0, n, 24):
        ident = struct.unpack_from('<I', buf, o + 8)[0]
        plen = buf[o + 15]
        bus.send(can.Message(arbitration_id=ident & 0x1FFFFFFF, is_extended_id=bool(ident & 0x20000000), data=bytes(buf[o + 16:o + 16 + plen])))

def to_record(msg):
    rec = (C.c_uint8 * 24)()
    d = (C.c_uint8 * 8)(*msg.data[:8])
    lib.CanTp_MakeRecord(msg.arbitration_id, 1 if msg.is_extended_id else 0, 0, d, len(msg.data), 0, rec, 24)
    return rec

ok = 0
for rnd in range(a.rounds):
    expect = [random.Random(1000 + rnd).randrange(256) for _ in range(N)]
    if a.role == 'tx':
        time.sleep(0.5)
        lib.CanTp_TxReset(0)
        v = (C.c_double * N)(*expect)
        rc = lib.CanTp_TxStart(0, v, N, now_ms(), 0, 0, out, 8192, C.byref(w))
        send_records(bytes(out[:w.value]), w.value)
        deadline = time.monotonic() + a.timeout
        frames_sent = w.value // 24
        while rc == 0 and time.monotonic() < deadline:
            msg = bus.recv(timeout=0.05)
            rec = to_record(msg) if msg is not None else None
            rc = lib.CanTp_TxFeed(0, rec, 24 if rec else 0, now_ms(), 0, 0, out, 8192, C.byref(w))
            if w.value: send_records(bytes(out[:w.value]), w.value); frames_sent += w.value // 24
        print(f'round {rnd}: TxFeed final rc {rc} ({"DONE" if rc == 2 else "not done"}), {frames_sent} frames sent')
        ok += rc == 2
    else:
        lib.CanTp_RxReset(0)
        deadline = time.monotonic() + a.timeout * 2
        rc, frames_in, frames_out = 0, 0, 0
        while rc != 1 and time.monotonic() < deadline:
            msg = bus.recv(timeout=0.05)
            rec = to_record(msg) if msg is not None else None
            if rec is not None: frames_in += 1
            rc = lib.CanTp_RxStep(0, rec, 24 if rec else 0, now_ms(), 0, vals, N, out, 8192, C.byref(w))
            if w.value: send_records(bytes(out[:w.value]), w.value); frames_out += w.value // 24
            if rc < 0: print(f'  RxStep {rc}'); rc = 0
        got = [int(x) for x in vals]
        match = rc == 1 and got == expect
        print(f'round {rnd}: {"decoded OK" if match else "MISMATCH / timeout"} ({frames_in} frames in, {frames_out} responses out)')
        ok += match
bus.shutdown()
print(f'{ok}/{a.rounds} rounds OK')
sys.exit(0 if ok == a.rounds else 1)
