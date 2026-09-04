"""
oracle_test.py - cross-check tempctl.dll against independent implementations.

  * J1939 BAM framing   -> reassembled by pretty_j1939's J1939TransportTracker
  * DBC signal packing  -> compared bit-for-bit with cantools encode()

Usage:  python tests\oracle_test.py [path\to\tempctl.dll] [--pylibs DIR]
Needs cantools and pretty_j1939 importable (pip install cantools pretty_j1939,
or --pylibs pointing at a `pip install --target` directory).
Also doubles as a ctypes usage example for calling the library from a PC.
"""
import argparse, ctypes as C, os, random, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument('dll', nargs='?', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'win-x64', 'tempctl.dll'))
ap.add_argument('--pylibs', default=None)
ap.add_argument('--iters', type=int, default=3000)
ap.add_argument('--seed', type=int, default=1)
args = ap.parse_args()
if args.pylibs: sys.path.insert(0, args.pylibs)

import cantools
from cantools.database.can import Message, Signal
from cantools.database.conversion import BaseConversion
from pretty_j1939.describe import J1939TransportTracker

lib = C.CDLL(os.path.abspath(args.dll))
lib.TcVersion.restype = C.c_uint32
lib.TcStep.argtypes = [C.c_int32, C.c_int32, C.c_uint32, C.POINTER(C.c_float), C.c_int32, C.POINTER(C.c_float), C.c_int32]
lib.TcEncodeFrames.argtypes = [C.POINTER(C.c_float), C.c_int32, C.c_uint32, C.c_uint8, C.c_uint8, C.c_uint64, C.c_uint64,
                               C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.TcJ1939Bam.argtypes = [C.c_uint32, C.c_uint8, C.c_uint8, C.POINTER(C.c_uint8), C.c_int32, C.c_uint64, C.c_uint64,
                           C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.TcCanPack.argtypes = [C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_float), C.c_int32,
                          C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
for f in (lib.TcStep, lib.TcEncodeFrames, lib.TcJ1939Bam, lib.TcCanPack): f.restype = C.c_int32

RAW = 24
def split_frames(buf):
    """-> list of (timestamp, arb_id, extended, payload bytes) from NI-XNET raw records."""
    out = []
    for o in range(0, len(buf), RAW):
        ts, ident, typ, flags, info, plen = struct.unpack_from('<QIBBBB', buf, o)
        assert typ == 0 and flags == 0 and info == 0
        out.append((ts, ident & 0x1FFFFFFF, bool(ident & 0x20000000), bytes(buf[o + 16:o + 16 + plen])))
    return out

fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print('FAIL', msg)

# ---------------------------------------------------------------- BAM oracle
def bam_oracle(payload, pgn, sa, prio):
    buf = (C.c_uint8 * (RAW * 256))()
    n = C.c_int32()
    rc = lib.TcJ1939Bam(pgn, sa, prio, (C.c_uint8 * len(payload))(*payload), len(payload), 0, 0, buf, len(buf), C.byref(n))
    check(rc == 0, f'TcJ1939Bam rc={rc}')
    frames = split_frames(bytes(buf[:n.value]))
    got = []
    tracker = J1939TransportTracker(real_time=False)
    def found(data, sa_, pgn_, is_last_packet): got.append((bytes(data), sa_, pgn_))
    for ts, ident, ext, data in frames:
        check(ext, 'J1939 frames must be extended')
        if len(payload) <= 8:
            got.append((data, ident & 0xFF, (ident >> 8) & 0x3FFFF))
        else:
            tracker.process(found, list(data), ident)
    check(len(got) == 1, f'reassembly count {len(got)} for len {len(payload)}')
    if got:
        d, s, p = got[0]
        if p < 0xF000: p &= 0x3FF00
        check(d == bytes(payload), f'payload mismatch len {len(payload)}: {d[:12].hex()} vs {bytes(payload[:12]).hex()}')
        check(s == sa and p == (pgn & 0x3FFFF if pgn >= 0xF000 else pgn & 0x3FF00), f'sa/pgn mismatch {s:#x}/{p:#x}')
    return frames

rng = random.Random(args.seed)
for ln in list(range(0, 40)) + [63, 64, 65, 100, 255, 256, 700, 1784, 1785]:
    payload = bytes(rng.randrange(256) for _ in range(ln))
    frames = bam_oracle(payload, 0xFF00 + rng.randrange(256), rng.randrange(254), 6)
    if ln > 8:
        check(len(frames) == 1 + (ln + 6) // 7, f'frame count {len(frames)} for len {ln}')
        check(frames[0][1] >> 26 == 7 and frames[1][1] >> 26 == 7, 'TP frames priority 7')
        check(frames[0][1] & 0x3FFFF00 == 0xECFF00, 'TP.CM id')
        check(all(f[1] & 0x3FFFF00 == 0xEBFF00 for f in frames[1:]), 'TP.DT id')
        # padding of the last packet is 0xFF
        last = frames[-1][3]
        used = ln - 7 * (len(frames) - 2)
        check(last[1 + used:] == b'\xFF' * (7 - used), 'last packet padding 0xFF')
bam_oracle(b'\x01\x02\x03', 0xEA00, 0x21, 6)   # PDU1 -> DA global
print('BAM oracle: ok' if fails == 0 else f'BAM oracle: {fails} failures')

# 11-signal controller message end to end
sig = (C.c_float * 11)(100, 0, 60, 40, 50, 23.5, 1000, 500, 0, 1, 0)
buf = (C.c_uint8 * (RAW * 8))(); n = C.c_int32()
check(lib.TcEncodeFrames(sig, 11, 0xFF00, 0x80, 6, 0, 0, buf, len(buf), C.byref(n)) == 0 and n.value == 192, 'TcEncodeFrames')
tracker = J1939TransportTracker(real_time=False); got = []
for ts, ident, ext, data in split_frames(bytes(buf[:192])):
    tracker.process(lambda d, s, p, is_last_packet: got.append(bytes(d)), list(data), ident)
check(len(got) == 1 and struct.unpack('<11f', got[0]) == tuple(sig), 'controller message floats round-trip')

# ---------------------------------------------------------- cantools oracle
def pack_c(sigdefs, framedefs, values):
    sd = (C.c_double * (9 * len(sigdefs)))(*[x for row in sigdefs for x in row])
    fd = (C.c_double * (4 * len(framedefs)))(*[x for row in framedefs for x in row])
    vv = (C.c_float * len(values))(*values)
    out = (C.c_uint8 * (RAW * len(framedefs)))(); n = C.c_int32()
    rc = lib.TcCanPack(sd, len(sigdefs), fd, len(framedefs), vv, len(values), 0, out, len(out), C.byref(n))
    return rc, split_frames(bytes(out[:n.value])) if rc == 0 else None

def motorola_ok(start, length):
    """cantools rejects Motorola layouts that spill past byte 7; mirror its rule."""
    byte, bit = start // 8, start % 8
    for _ in range(length - 1):
        bit -= 1
        if bit < 0: bit = 7; byte += 1
    return byte < 8

before = fails
compared = 0
for it in range(args.iters):
    big = rng.random() < 0.5
    vtype = rng.choice([0, 0, 1, 1, 2])
    if vtype == 2:
        length = 32
    else:
        length = rng.randrange(1, 33)
    if big:
        for _ in range(100):
            start = rng.randrange(64)
            if motorola_ok(start, length): break
        else:
            continue
    else:
        start = rng.randrange(0, 64 - length + 1)
    dlc = 8
    # exact-in-float32 values only, so C (float in) and cantools (double in) see the same number
    if vtype == 2:
        val = rng.choice([0.0, 1.5, -2.25, 1e10, -3.0e-3, 123456.0])
        factor, offset = 1.0, 0.0
    else:
        factor = rng.choice([1, 1, 1, 0.5, 0.25, 2, 4])
        offset = rng.choice([0, 0, -40, 10, 128])
        if vtype == 0: raw = rng.randrange(0, 2 ** min(length, 20))
        else:          raw = rng.randrange(-(2 ** (min(length, 20) - 1)), 2 ** (min(length, 20) - 1))
        val = raw * factor + offset
    msg = Message(frame_id=0x123, name='M', length=dlc, signals=[
        Signal(name='s', start=start, length=length, byte_order='big_endian' if big else 'little_endian',
               is_signed=(vtype == 1),
               conversion=BaseConversion.factory(scale=factor, offset=offset, is_float=(vtype == 2)))])
    try:
        expect = msg.encode({'s': val}, strict=False)
    except Exception as e:
        continue   # out of cantools' range for this layout; skip
    rc, frames = pack_c([[0, start, length, 1 if big else 0, vtype, factor, offset, 0, 0]], [[0x123, 0, dlc, 0]], [val])
    check(rc == 0, f'TcCanPack rc {rc} start={start} len={length} big={big} vtype={vtype}')
    if rc == 0:
        compared += 1
        check(frames[0][3] == expect, f'mismatch start={start} len={length} big={big} vtype={vtype} val={val}: got {frames[0][3].hex()} want {expect.hex()}')

# multi-signal frame from a real-looking DBC definition
db = cantools.database.load_string('''VERSION ""
NS_ :
BS_:
BU_: ECU
BO_ 2364540158 EEC1: 8 ECU
 SG_ EngineTorqueMode : 0|4@1+ (1,0) [0|15] "" Vector__XXX
 SG_ ActualEnginePercentTorque : 16|8@1+ (1,-125) [-125|125] "%" Vector__XXX
 SG_ EngineSpeed : 24|16@1+ (0.125,0) [0|8031.875] "rpm" Vector__XXX
 SG_ SourceAddress : 40|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ BigEndianTemp : 55|12@0- (0.1,0) [-200|200] "C" Vector__XXX
''')
m = db.get_message_by_name('EEC1')
vals = {'EngineTorqueMode': 3, 'ActualEnginePercentTorque': 42, 'EngineSpeed': 1500.125, 'SourceAddress': 0x80, 'BigEndianTemp': -17.5}
expect = m.encode(vals, strict=False)
sigdefs = []
for s in m.signals:
    sigdefs.append([0, s.start, s.length, 1 if s.byte_order == 'big_endian' else 0, 1 if s.is_signed else 0,
                    float(s.scale), float(s.offset), 0, 0])
rc, frames = pack_c(sigdefs, [[m.frame_id, 1, 8, 0]], [vals[s.name] for s in m.signals])
check(rc == 0 and frames[0][3] == expect, f'EEC1-like frame: got {frames and frames[0][3].hex()} want {expect.hex()}')
check(rc == 0 and frames[0][1] == m.frame_id and frames[0][2], 'EEC1 id/extended')
print(f'cantools oracle: {compared} random layouts compared, ' + ('ok' if fails == before else f'{fails - before} failures'))

# ---------------------------------------------------------------- controller smoke via ctypes
cfg = (C.c_float * 13)(100, 0, 60, 40, 50, 50, 1000, 500, 0, 0, 0, 0, 0)
out = (C.c_float * 13)()
check(lib.TcStep(0, 0, 0, cfg, 11, out, 13) == 0, 'init')
cfg[5] = 30
for t in range(100, 700, 100): lib.TcStep(0, 1, t, cfg, 11, out, 13)
check(out[9] == 1.0 and out[8] == 0.0 and out[10] == 0.0, f'heating after deadband timeout: {list(out)}')
print(f'library version {lib.TcVersion():#08x}')
print('ALL OK' if fails == 0 else f'{fails} FAILURES')
sys.exit(1 if fails else 0)
