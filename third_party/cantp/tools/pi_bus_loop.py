"""
pi_bus_loop.py - live SocketCAN loop test for libcantp.so (Linux, e.g. Raspberry Pi).

Packs a multipacket J1939 BAM message with CanTp_Pack, writes each NI-XNET raw
record as a real CAN frame on the interface (python-can, paced 50 ms per
J1939-21), reads the bus back through a second socket, feeds every received
frame into CanTp_RxFeed, and compares the decoded values with the originals.
Other traffic on the bus is simply ignored by the decoder (that is part of
what is being tested).

    python3 pi_bus_loop.py --so /tmp/libcantp.so --iface can1 [--sa 0x80] [--rounds 3]

Exit code 0 = every round decoded identically.
"""
import argparse, ctypes as C, struct, sys, time, threading

import can  # python-can

ap = argparse.ArgumentParser()
ap.add_argument('--so', default='./libcantp.so')
ap.add_argument('--iface', default='can1')
ap.add_argument('--sa', type=lambda x: int(x, 0), default=0x80)
ap.add_argument('--rounds', type=int, default=3)
ap.add_argument('--gap', type=float, default=0.05)
a = ap.parse_args()

lib = C.CDLL(a.so)
lib.CanTp_Version.restype = C.c_uint32
lib.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_Pack.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_RxFeed.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_MakeRecord.argtypes = [C.c_uint32, C.c_int32, C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32]
lib.CanTp_OutputSize.argtypes = [C.c_int32]
for f in (lib.CanTp_Define, lib.CanTp_Pack, lib.CanTp_RxFeed, lib.CanTp_MakeRecord, lib.CanTp_OutputSize): f.restype = C.c_int32

# --- message: 40-byte Proprietary B PG with mixed scaled signals (like a temp controller state) ---
PGN = 0xFF10
msgdef = (C.c_double * 8)(0x18000000 | (PGN << 8) | 0xFE, 1, 40, 1, a.sa, 255, 255, 100)
sigs = [
    # start len order type factor offset min max
    (0,   16, 0, 0, 0.03125, -273, -273, 1735),   # temp 1
    (16,  16, 0, 0, 0.03125, -273, -273, 1735),   # temp 2
    (32,  16, 0, 0, 1, 0, 0, 65535),              # timeout ms
    (48,  8,  0, 0, 1, -125, -125, 125),          # signed-by-offset
    (56,  2,  0, 0, 1, 0, 0, 3),                  # 2-bit flag
    (58,  2,  0, 0, 1, 0, 0, 3),
    (64,  32, 0, 2, 1, 0, 0, 0),                  # float32
    (96,  16, 1, 1, 0.1, 0, 0, 0),                # Motorola signed
    (312, 8,  0, 0, 1, 0, 0, 255),                # last byte (Intel start = LSB of byte 39)
]
sigdefs = (C.c_double * (8 * len(sigs)))(*[x for row in sigs for x in row])
rc = lib.CanTp_Define(0, msgdef, 8, sigdefs, len(sigs))
assert rc == 0, rc
size = lib.CanTp_OutputSize(0)
print(f'libcantp {lib.CanTp_Version():#08x}, message PGN {PGN:#x} SA {a.sa:#x}: {size // 24} frames per message')

def records(buf):
    for o in range(0, len(buf), 24):
        ts, ident, typ, flags, info, plen = struct.unpack_from('<QIBBBB', buf, o)
        yield ident & 0x1FFFFFFF, bool(ident & 0x20000000), bytes(buf[o + 16:o + 16 + plen])

bus_tx = can.Bus(interface='socketcan', channel=a.iface)
bus_rx = can.Bus(interface='socketcan', channel=a.iface)   # separate socket: sees our own frames via kernel loopback

fails = 0
for rnd in range(a.rounds):
    vals = [20.5 + rnd, 21.0 - rnd, 5000 + rnd, -7 + rnd, 1, 2, 1.5 * (rnd + 1), -12.3, 200 + rnd]
    v = (C.c_double * len(vals))(*vals)
    out = (C.c_uint8 * size)(); n = C.c_int32()
    assert lib.CanTp_Pack(0, v, len(vals), 0, 0, out, size, C.byref(n)) == 0
    frames = list(records(bytes(out)))

    decoded = (C.c_double * len(vals))()
    got = []
    stop = threading.Event()
    def rx():
        rec = (C.c_uint8 * 24)()
        while not stop.is_set():
            m = bus_rx.recv(timeout=0.2)
            if m is None: continue
            data = (C.c_uint8 * 8)(*m.data[:8])
            lib.CanTp_MakeRecord(m.arbitration_id, 1 if m.is_extended_id else 0, 0, data, len(m.data), 0, rec, 24)
            if lib.CanTp_RxFeed(0, rec, 24, decoded, len(vals)) == 1:
                got.append(list(decoded)); stop.set()
    t = threading.Thread(target=rx, daemon=True); t.start()
    time.sleep(0.05)
    t0 = time.time()
    for ident, ext, data in frames:
        bus_tx.send(can.Message(arbitration_id=ident, is_extended_id=ext, data=data))
        time.sleep(a.gap)
    t.join(timeout=2.0)
    stop.set()
    if not got:
        print(f'round {rnd}: FAIL no message reassembled'); fails += 1; continue
    d = got[0]
    ok = all(abs(x - y) < 1e-3 for x, y in zip(d, vals))
    # float32 and Motorola signed round-trips are exact for these values
    print(f'round {rnd}: {"ok" if ok else "FAIL"}  sent {vals}  got {[round(x, 3) for x in d]}  ({(time.time() - t0) * 1000:.0f} ms on {a.iface})')
    fails += 0 if ok else 1

bus_tx.shutdown(); bus_rx.shutdown()
print('ALL OK' if fails == 0 else f'{fails} FAILURES')
sys.exit(1 if fails else 0)
