"""
oracle_test.py - cross-check cantp.dll / libcantp.so against independent implementations.

  * DBC packing/unpacking  -> cantools encode()/decode(), bit-for-bit, on
        (a) 3000 random single-signal layouts (Intel/Motorola, 1..32 bit, signed,
            unsigned, float32, scaled) in 8-byte frames,
        (b) random multi-signal messages up to 64 bytes (non-overlapping),
        (c) real messages from a J1939 DBC converted with tools/dbc2tables.py
            (EC1 40 bytes, RC 19 bytes, EEC1 8 bytes when the file is present).
  * J1939 BAM framing      -> reassembled by pretty_j1939's J1939TransportTracker
        for every payload length 9..1785 that matters, plus CanTp's own RxFeed.

Usage:  python tests\oracle_test.py [path\to\cantp.dll] [--pylibs DIR] [--dbc FILE]
Needs cantools and pretty_j1939 (pip install cantools pretty_j1939).
Also a complete ctypes usage example for the library.
"""
import argparse, ctypes as C, os, random, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('lib', nargs='?', default=os.path.join(HERE, '..', 'build', 'win-x64', 'cantp.dll'))
ap.add_argument('--pylibs', default=None)
ap.add_argument('--iters', type=int, default=3000)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--dbc', default=r'C:\CodeProjects\J1939ReferenceFiles\Conversion\J1939_NGHD_V130.dbc')
args = ap.parse_args()
if args.pylibs: sys.path.insert(0, args.pylibs)
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))

import cantools
from cantools.database.can import Message, Signal
from cantools.database.conversion import BaseConversion
from pretty_j1939.describe import J1939TransportTracker
import dbc2tables

lib = C.CDLL(os.path.abspath(args.lib))
lib.CanTp_Version.restype = C.c_uint32
lib.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_Pack.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_Unpack.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_RxFeed.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_OutputSize.argtypes = [C.c_int32]
lib.CanTp_RecordSize.argtypes = [C.POINTER(C.c_uint8), C.c_int32]
for f in (lib.CanTp_Define, lib.CanTp_Pack, lib.CanTp_Unpack, lib.CanTp_RxFeed, lib.CanTp_OutputSize, lib.CanTp_RecordSize): f.restype = C.c_int32

fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        if fails <= 30: print('FAIL', msg)

def define(slot, msgdef, sigdefs):
    md = (C.c_double * 8)(*msgdef)
    sd = (C.c_double * (8 * max(1, len(sigdefs))))(*[x for r in sigdefs for x in r])
    return lib.CanTp_Define(slot, md, 8, sd, len(sigdefs))

def pack(slot, values):
    size = lib.CanTp_OutputSize(slot)
    out = (C.c_uint8 * size)(); n = C.c_int32()
    v = (C.c_double * max(1, len(values)))(*values)
    rc = lib.CanTp_Pack(slot, v, len(values), 0, 0, out, size, C.byref(n))
    return rc, bytes(out[:n.value])

def unpack(slot, buf, n):
    vals = (C.c_double * max(1, n))(); consumed = C.c_int32()
    b = (C.c_uint8 * len(buf))(*buf)
    rc = lib.CanTp_Unpack(slot, b, len(buf), vals, n, C.byref(consumed))
    return rc, list(vals)[:n]

def records(buf):
    out, o = [], 0
    while o < len(buf):
        plen = buf[o + 15]
        size = 24 + (((plen - 1) & ~7) if plen > 8 else 0)
        ts, ident = struct.unpack_from('<QI', buf, o)
        out.append((ident & 0x1FFFFFFF, bool(ident & 0x20000000), bytes(buf[o + 16:o + 16 + plen]), buf[o + 12]))
        o += size
    return out

def reassemble(buf, expect_len):
    """Payload bytes of a CanTp sequence: single frame, or BAM via pretty_j1939."""
    recs = records(buf)
    if len(recs) == 1:
        return recs[0][2]
    got = []
    tr = J1939TransportTracker(real_time=False)
    for ident, ext, data, typ in recs:
        tr.process(lambda d, sa, pgn, is_last_packet: got.append(bytes(d)), list(data), ident)
    return got[0] if got else None

# ------------------------------------------------------------ (a) random single-signal layouts
rng = random.Random(args.seed)
def motorola_ok(start, length, nbytes=8):
    byte, bit = start // 8, start % 8
    for _ in range(length - 1):
        bit -= 1
        if bit < 0: bit = 7; byte += 1
    return byte < nbytes

compared = 0
for it in range(args.iters):
    big = rng.random() < 0.5
    vtype = rng.choice([0, 0, 1, 1, 2])
    length = 32 if vtype == 2 else rng.randrange(1, 33)
    if big:
        for _ in range(100):
            start = rng.randrange(64)
            if motorola_ok(start, length): break
        else: continue
    else:
        start = rng.randrange(0, 64 - length + 1)
    if vtype == 2:
        val = rng.choice([0.0, 1.5, -2.25, 1e10, -3.0e-3, 123456.0]); factor, offset = 1.0, 0.0
    else:
        factor = rng.choice([1, 1, 1, 0.5, 0.25, 2, 4]); offset = rng.choice([0, 0, -40, 10, 128])
        raw = rng.randrange(0, 2 ** min(length, 20)) if vtype == 0 else rng.randrange(-(2 ** (min(length, 20) - 1)), 2 ** (min(length, 20) - 1))
        val = raw * factor + offset
    msg = Message(frame_id=0x123, name='M', length=8, signals=[
        Signal(name='s', start=start, length=length, byte_order='big_endian' if big else 'little_endian',
               is_signed=(vtype == 1), conversion=BaseConversion.factory(scale=factor, offset=offset, is_float=(vtype == 2)))])
    try:
        expect = msg.encode({'s': val}, strict=False)
    except Exception:
        continue
    rc = define(0, [0x123, 0, 8, 0, -1, 255, 0, 0], [[start, length, 1 if big else 0, vtype, factor, offset, 0, 0]])
    check(rc == 0, f'define rc {rc} start={start} len={length} big={big} vtype={vtype}')
    if rc != 0: continue
    rc, buf = pack(0, [val])
    check(rc == 0 and buf[16:24] == expect, f'pack mismatch start={start} len={length} big={big} vtype={vtype} val={val}: {buf[16:24].hex()} vs {expect.hex()}')
    rc, got = unpack(0, buf, 1)
    dec = msg.decode(expect)['s']
    check(rc == 1 and abs(got[0] - float(dec)) < 1e-9 * max(1, abs(float(dec))), f'unpack mismatch {got} vs {dec}')
    compared += 1
print(f'(a) single-signal layouts: {compared} compared, {"ok" if fails == 0 else f"{fails} failures"}')

# ------------------------------------------------------------ (b) random multi-signal messages up to 64 bytes
before = fails; nb = 0
for it in range(300):
    nbytes = rng.choice([8, 8, 12, 16, 20, 24, 32, 48, 64])
    used = bytearray(nbytes)                     # occupied bits per byte
    sigs, defs, vals = [], [], {}
    for k in range(rng.randrange(1, 12)):
        for _ in range(50):
            big = rng.random() < 0.4
            vtype = rng.choice([0, 0, 0, 1, 2])
            length = 32 if vtype == 2 else rng.randrange(1, 25)
            start = rng.randrange(nbytes * 8)
            # collect the bit positions this signal would occupy
            if big:
                if not motorola_ok(start, length, nbytes): continue
                pos, byte, bit = [], start // 8, start % 8
                for j in range(length):
                    pos.append(byte * 8 + bit); bit -= 1
                    if bit < 0: bit = 7; byte += 1
            else:
                if start + length > nbytes * 8: continue
                pos = list(range(start, start + length))
            if any(used[p // 8] & (1 << (p % 8)) for p in pos): continue
            for p in pos: used[p // 8] |= 1 << (p % 8)
            factor = rng.choice([1, 0.1, 0.03125, 2]) if vtype != 2 else 1
            offset = rng.choice([0, -40, -273]) if vtype != 2 else 0
            name = f's{k}'
            sigs.append(Signal(name=name, start=start, length=length, byte_order='big_endian' if big else 'little_endian',
                               is_signed=(vtype == 1), conversion=BaseConversion.factory(scale=factor, offset=offset, is_float=(vtype == 2))))
            defs.append([start, length, 1 if big else 0, vtype, factor, offset, 0, 0])
            if vtype == 2: vals[name] = rng.choice([0.0, 2.5, -7.75, 1e6])
            else:
                raw = rng.randrange(0, 2 ** length) if vtype == 0 else rng.randrange(-(2 ** (length - 1)), 2 ** (length - 1))
                vals[name] = raw * factor + offset
            break
    if not sigs: continue
    fd = nbytes > 8
    msg = Message(frame_id=0x1ABCDEF0 if fd else 0x321, is_extended_frame=fd, name='M', length=nbytes, signals=sigs, is_fd=fd)
    try:
        expect = msg.encode(vals, strict=False, padding=False)
    except Exception as e:
        continue
    tp = 2 if fd else 0
    rc = define(1, [msg.frame_id, 1 if fd else 0, nbytes, tp, -1, 255, 0, 0], defs)
    check(rc == 0, f'(b) define rc {rc}')
    if rc != 0: continue
    rc, buf = pack(1, [vals[s.name] for s in sigs])
    recs = records(buf)
    check(rc == 0 and recs[0][2][:nbytes] == expect, f'(b) pack mismatch n={nbytes}: {recs[0][2][:nbytes].hex()} vs {expect.hex()}')
    rc, got = unpack(1, buf, len(sigs))
    dec = msg.decode(expect, decode_choices=False)
    for s, g in zip(sigs, got):
        d = float(dec[s.name])
        check(abs(g - d) < 1e-6 * max(1, abs(d)), f'(b) unpack {s.name}: {g} vs {d}')
    nb += 1
print(f'(b) multi-signal messages: {nb} compared, {"ok" if fails == before else f"{fails - before} failures"}')

# ------------------------------------------------------------ (c) real DBC messages through dbc2tables
before = fails
if os.path.exists(args.dbc):
    db = cantools.database.load_file(args.dbc, strict=False)
    for name in ['EEC1', 'EC1', 'RC', 'ET1', 'TCFG']:
        try: m = db.get_message_by_name(name)
        except KeyError: continue
        t = dbc2tables.convert(m, 0x00)                     # SA 0x00 replaces the 0xFE placeholder
        rc = define(2, t['msgdef'], t['sigdefs'])
        check(rc == 0, f'(c) {name} define rc {rc}')
        if rc != 0: continue
        for trial in range(20):
            vals = {}
            for s in m.signals:
                if s.name not in t['signals']: continue
                lo = float(s.minimum) if s.minimum is not None else 0
                hi = float(s.maximum) if s.maximum is not None else (2 ** s.length - 1) * float(s.scale) + float(s.offset)
                raw = rng.randrange(0, 2 ** s.length - 1) if trial else 0          # avoid all-ones (NA) so decode is exact
                phys = raw * float(s.scale) + float(s.offset)
                vals[s.name] = min(max(phys, lo), hi) if hi > lo else phys
            expect = m.encode(vals, strict=False, padding=True)
            rc, buf = pack(2, [vals[n] for n in t['signals']])
            payload = reassemble(buf, m.length)
            check(rc == 0 and payload is not None and payload[:m.length] == expect[:m.length],
                  f'(c) {name} trial {trial}: {payload[:16].hex() if payload else None} vs {expect[:16].hex()}')
            if payload is None: continue
            rc, got = unpack(2, buf, len(t['signals']))
            dec = m.decode(expect, decode_choices=False, scaling=True)
            for n, g in zip(t['signals'], got):
                d = float(dec[n])
                check(abs(g - d) < 1e-6 * max(1, abs(d)), f'(c) {name}.{n}: {g} vs {d}')
            if m.length > 8:
                # ids: TP.CM 1CECFF00, TP.DT 1CEBFF00, count
                recs = records(buf)
                check(recs[0][0] == 0x1CECFF00 and all(r[0] == 0x1CEBFF00 for r in recs[1:]), f'(c) {name} TP ids')
                check(len(recs) == 1 + (m.length + 6) // 7, f'(c) {name} frame count {len(recs)}')
        print(f'(c) {name}: {m.length} bytes, {len(t["sigdefs"])} signals, 20 random value sets')
    print(f'(c) real DBC messages: {"ok" if fails == before else f"{fails - before} failures"}')
else:
    print(f'(c) skipped, DBC not found: {args.dbc}')

# ------------------------------------------------------------ (d) BAM lengths vs pretty_j1939 + RxFeed
before = fails
for ln in list(range(9, 40)) + [63, 64, 65, 100, 255, 256, 700, 1784, 1785]:
    pgn = 0xFF00 + rng.randrange(256); sa = rng.randrange(254)
    defs = [[(ln - 1) * 8, 8, 0, 0, 1, 0, 0, 0], [0, 8, 0, 0, 1, 0, 0, 0]]
    rc = define(3, [0x18000000 | (pgn << 8) | 0xFE, 1, ln, 1, sa, 255, 255, 0], defs)
    check(rc == 0, f'(d) define len {ln} rc {rc}')
    rc, buf = pack(3, [ln & 0xFF, 0x5A])
    payload = reassemble(buf, ln)
    check(payload is not None and len(payload) == ln and payload[0] == 0x5A and payload[-1] == (ln & 0xFF), f'(d) len {ln} reassembly')
    recs = records(buf)
    check(recs[0][0] == (0x1CECFF00 | sa) and recs[0][2][1] | (recs[0][2][2] << 8) == ln, f'(d) len {ln} TP.CM')
    # RxFeed one record at a time, with a foreign frame between each
    vals = (C.c_double * 2)(); o = 0; found = 0
    junk = bytes(24)
    while o < len(buf):
        size = lib.CanTp_RecordSize((C.c_uint8 * 24)(*buf[o:o + 24]), 24)
        rec = (C.c_uint8 * size)(*buf[o:o + size])
        if lib.CanTp_RxFeed(3, rec, size, vals, 2) == 1: found += 1
        lib.CanTp_RxFeed(3, (C.c_uint8 * 24)(*junk), 24, vals, 2)
        o += size
    check(found == 1 and vals[0] == (ln & 0xFF) and vals[1] == 0x5A, f'(d) len {ln} RxFeed')
print(f'(d) BAM lengths: {"ok" if fails == before else f"{fails - before} failures"}')

print(f'library version {lib.CanTp_Version():#08x}')
print('ALL OK' if fails == 0 else f'{fails} FAILURES')
sys.exit(1 if fails else 0)
