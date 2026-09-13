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
  * Release 3 (v1.2.0):
        (h) every cluster of an Eaton .ecd database through CanTp_DefineFlat (tables,
            defaults, FlatSize walk), ECD vs DBC wire layout by channel name, and
            CanTp_Transfer with frame lengths vs cantools for the real J1939 messages.
  * Release 2:
        (e) multiplexed VW message (17 rows) vs cantools for 12 multiplexor values,
        (f) ISO-TP sessions in both directions against the `isotp` package (block
            sizes, STmin, 11- and 29-bit ids),
        (g) J1939 RTS/CTS sessions in both directions against a python reference
            peer (windows, re-request of a dropped packet), DT stream reassembled
            by pretty_j1939.

Usage:  python tests\oracle_test.py [path\to\cantp.dll] [--pylibs DIR] [--dbc FILE]
Needs cantools, pretty_j1939 and can-isotp (pip install cantools pretty_j1939 can-isotp).
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
ap.add_argument('--ecd', default=r'C:\CodeProjects\J1939ReferenceFiles\Conversion\J1939_NGHD_V130.ecd', help='Eaton .ecd database for section (h)')
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

# ============================================================ release 2 sections
lib.CanTp_DefineMux.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_SessionConfig.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_TxStart.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.c_uint32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_TxFeed.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_RxStep.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint32, C.c_uint64, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
lib.CanTp_MakeRecord.argtypes = [C.c_uint32, C.c_int32, C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32]
lib.CanTp_TxReset.argtypes = [C.c_int32]; lib.CanTp_RxReset.argtypes = [C.c_int32]
for f in (lib.CanTp_DefineMux, lib.CanTp_SessionConfig, lib.CanTp_TxStart, lib.CanTp_TxFeed, lib.CanTp_RxStep, lib.CanTp_MakeRecord, lib.CanTp_TxReset, lib.CanTp_RxReset): f.restype = C.c_int32
import time, math

def make_record(ident, ext, data):
    rec = (C.c_uint8 * 24)()
    d = (C.c_uint8 * 8)(*data)
    lib.CanTp_MakeRecord(ident, 1 if ext else 0, 0, d, len(data), 0, rec, 24)
    return bytes(rec)

def positions(n):
    """Byte positions carried as 8-bit signals: all of them up to 128 bytes, else a sample (first, last, spread)."""
    if n <= 128: return list(range(n))
    step = max(1, (n - 120) // 8)
    return sorted(set(range(60)) | set(range(n - 60, n)) | set(range(60, n - 60, step)))[:128]
def byte_message(slot, msgdef, n):
    """Selected payload bytes as unsigned 8-bit signals, so values and payload bytes correspond 1:1."""
    return define(slot, msgdef, [[8 * i, 8, 0, 0, 1, 0, 0, 0] for i in positions(n)])
def wire_payload(n, values):
    """What CanTp puts on the wire for those values: signal bytes, pad 0xFF elsewhere."""
    b = bytearray([0xFF] * n)
    for p, v in zip(positions(n), values): b[p] = int(v)
    return bytes(b)

class Session:
    """ctypes driver for one CanTp slot as sender or receiver."""
    def __init__(self, slot, n):
        self.slot, self.n = slot, n
        self.out = (C.c_uint8 * 8192)(); self.w = C.c_int32()
        self.vals = (C.c_double * max(1, n))()
    def tx_start(self, values, now):
        v = (C.c_double * len(values))(*values)
        rc = lib.CanTp_TxStart(self.slot, v, len(values), now, 0, 0, self.out, 8192, C.byref(self.w))
        return rc, bytes(self.out[:self.w.value])
    def tx_feed(self, rec, now):
        r = (C.c_uint8 * 24)(*rec) if rec is not None else None
        rc = lib.CanTp_TxFeed(self.slot, r, 24 if rec else 0, now, 0, 0, self.out, 8192, C.byref(self.w))
        return rc, bytes(self.out[:self.w.value])
    def rx_step(self, rec, now):
        r = (C.c_uint8 * 24)(*rec) if rec is not None else None
        rc = lib.CanTp_RxStep(self.slot, r, 24 if rec else 0, now, 0, self.vals, self.n, self.out, 8192, C.byref(self.w))
        return rc, bytes(self.out[:self.w.value]), list(self.vals)[:self.n]

# ------------------------------------------------------------ (e) multiplexed VW message vs cantools
before = fails
if os.path.exists(args.dbc):
    m = db.get_message_by_name('VW')
    t = dbc2tables.convert(m, 0x00)
    rc = define(4, t['msgdef'], t['sigdefs'])
    mx = (C.c_double * (2 * len(t['muxdefs'])))(*[x for r in t['muxdefs'] for x in r])
    check(rc == 0 and lib.CanTp_DefineMux(4, mx, len(t['muxdefs'])) == 0, '(e) VW define')
    for loc in [0, 15, 16, 31, 32, 47, 48, 63, 79, 95, 111, 127]:
        present = {s.name for s in m.signals if s.multiplexer_ids is None or loc in s.multiplexer_ids}
        vals = {}
        for s in m.signals:
            if s.name not in present: continue
            raw = 0 if s.name == 'AxleLocation' else rng.randrange(0, 2 ** s.length - 1)
            phys = raw * float(s.scale) + float(s.offset)
            lo = float(s.minimum) if s.minimum is not None else 0
            hi = float(s.maximum) if s.maximum is not None else phys
            vals[s.name] = loc if s.name == 'AxleLocation' else (min(max(phys, lo), hi) if hi > lo else phys)
        expect = m.encode(vals, strict=False, padding=True)
        # CanTp gets a value for every row; unselected rows must not appear on the wire
        rc, buf = pack(4, [vals.get(n, 12345.0) for n in t['signals']])
        payload = reassemble(buf, m.length)
        check(rc == 0 and payload[:m.length] == expect[:m.length], f'(e) VW loc {loc}: {payload.hex() if payload else None} vs {expect.hex()}')
        rc, got = unpack(4, buf, len(t['signals']))
        dec = m.decode(expect, decode_choices=False, scaling=True)
        for n, g in zip(t['signals'], got):
            if n in dec: check(abs(g - float(dec[n])) < 1e-9, f'(e) VW loc {loc} {n}: {g} vs {dec[n]}')
            else:        check(math.isnan(g), f'(e) VW loc {loc} {n}: expected NaN (not selected), got {g}')
    print(f'(e) multiplexed VW ({len(t["sigdefs"])} rows): {"ok" if fails == before else f"{fails - before} failures"}')
else:
    print('(e) skipped, DBC not found')

# ------------------------------------------------------------ (f) ISO-TP vs the `isotp` package (both directions)
before = fails
try:
    import isotp
    have_isotp = True
except ImportError:
    have_isotp = False
    print('(f) skipped: pip install can-isotp')
if have_isotp:
    def run_isotp(n, bs, stmin, tx_is_cantp, ext=False):
        txid, rxid = (0x18DA2180, 0x18DA8021) if ext else (0x7E0, 0x7E8)
        msgdef = [txid, 1 if ext else 0, n, 5, -1, 255, 0xCC, 0]
        check(byte_message(5, msgdef, n) == 0, f'(f) define n={n}')
        ses = (C.c_double * 6)(-1, -1, bs, stmin, 0, 0)
        lib.CanTp_SessionConfig(5, ses, 6)
        lib.CanTp_TxReset(5); lib.CanTp_RxReset(5)
        payload = bytes(rng.randrange(256) for _ in range(n))
        to_py, from_py = [], []                                    # frames toward the isotp layer / produced by it
        def rxfn(timeout):
            if to_py:
                i, d = to_py.pop(0)
                return isotp.CanMessage(arbitration_id=i, data=d, extended_id=ext)
            time.sleep(min(timeout, 0.002)); return None
        def txfn(msg): from_py.append((msg.arbitration_id, bytes(msg.data)))
        mode = isotp.AddressingMode.Normal_29bits if ext else isotp.AddressingMode.Normal_11bits
        # the python side is the peer: it transmits on rxid and listens on txid when CanTp sends, and vice versa
        addr = isotp.Address(mode, txid=rxid, rxid=txid) if tx_is_cantp else isotp.Address(mode, txid=txid, rxid=rxid)
        layer = isotp.TransportLayer(rxfn, txfn, addr, params={'blocksize': bs, 'stmin': stmin, 'tx_padding': 0xCC, 'rx_flowcontrol_timeout': 2000, 'rx_consecutive_frame_timeout': 2000})
        pos = positions(n); values = [payload[p] for p in pos]; payload = wire_payload(n, values)
        s = Session(5, len(pos))
        now = 1000
        if tx_is_cantp:
            rc, frames = s.tx_start(values, now)
            check(rc in (0, 2), f'(f) TxStart rc {rc}')
            pending = [frames[o:o + 24] for o in range(0, len(frames), 24)]
            got = None
            for _ in range(4000):
                for rec in pending:
                    ident = struct.unpack_from('<I', rec, 8)[0] & 0x1FFFFFFF
                    to_py.append((ident, rec[16:24]))
                pending = []
                layer.process()
                while from_py:
                    i, d = from_py.pop(0)
                    rc, frames = s.tx_feed(make_record(i, ext, d), now)
                    check(rc >= 0, f'(f) TxFeed rc {rc}')
                    pending += [frames[o:o + 24] for o in range(0, len(frames), 24)]
                if layer.available():
                    got = layer.recv(); break
                now += 1
                time.sleep(0.001)
            check(got == payload, f'(f) cantp->isotp n={n} bs={bs} stmin={stmin} ext={ext}: {None if got is None else len(got)} bytes')
        else:
            layer.send(payload)
            done = None
            for _ in range(4000):
                layer.process()
                while from_py:
                    i, d = from_py.pop(0)
                    rc, resp, vals = s.rx_step(make_record(i, ext, d), now)
                    check(rc >= 0, f'(f) RxStep rc {rc}')
                    for o in range(0, len(resp), 24):
                        ident = struct.unpack_from('<I', resp, o + 8)[0] & 0x1FFFFFFF
                        to_py.append((ident, resp[o + 16:o + 24]))
                    if rc == 1: done = wire_payload(n, vals)
                if done is not None and not layer.transmitting(): break
                now += 1
                time.sleep(0.001)
            check(done == payload, f'(f) isotp->cantp n={n} bs={bs} stmin={stmin} ext={ext}: {None if done is None else len(done)} bytes')
        layer.reset()
    for n, bs, st in [(7, 0, 0), (8, 0, 0), (20, 0, 0), (41, 2, 5), (100, 8, 0), (128, 3, 2)]:
        run_isotp(n, bs, st, True)
        run_isotp(n, bs, st, False)
    run_isotp(60, 4, 0, True, ext=True)
    run_isotp(60, 4, 0, False, ext=True)
    print(f'(f) ISO-TP vs isotp {isotp.__version__}: {"ok" if fails == before else f"{fails - before} failures"}')

# ------------------------------------------------------------ (g) J1939 RTS/CTS vs a python reference peer + pretty_j1939
before = fails
def rts_msgdef(n, sa=0x80, da=0x21, pgn=0xFF20):
    return [0x18000000 | (pgn << 8) | sa, 1, n, 4, sa, da, 255, 0]
def cm(ctrl, pgn, b1=0xFF, b2=0xFF, b3=0xFF, b4=0xFF):
    return bytes([ctrl, b1, b2, b3, b4, pgn & 0xFF, (pgn >> 8) & 0xFF, (pgn >> 16) & 0xFF])

def run_rts_tx(n, window, drop=None):
    """CanTp sends; a python receiver answers with CTS windows (optionally dropping one DT once) and an EndOfMsgAck.
    The DT stream is also reassembled by pretty_j1939."""
    sa, da, pgn = 0x80, 0x21, 0xFF20
    check(byte_message(6, rts_msgdef(n), n) == 0, f'(g) define n={n}')
    lib.CanTp_TxReset(6)
    pos = positions(n); values = [rng.randrange(256) for _ in pos]; payload = wire_payload(n, values)
    s = Session(6, len(pos))
    packets = (n + 6) // 7
    rc, frames = s.tx_start(values, 100)
    recs = records(frames)
    check(rc == 0 and len(recs) == 1 and recs[0][0] == (0x1CEC0000 | (da << 8) | sa) and recs[0][2] == cm(0x10, pgn, n & 0xFF, n >> 8, packets, 255), f'(g) RTS n={n}')
    buf = bytearray(n); seen = set(); got_all = []
    tracker = J1939TransportTracker(real_time=False)
    tracker.process(lambda d, s_, p, is_last_packet=True: got_all.append(bytes(d)), list(recs[0][2]), recs[0][0])
    nxt, dropped = 1, False
    for rnd in range(200):
        num = min(window, packets - nxt + 1)
        rc, frames = s.tx_feed(make_record(0x1CEC0000 | (sa << 8) | da, True, cm(0x11, pgn, num, nxt)), 200 + rnd)
        check(rc == 0, f'(g) CTS rc {rc}')
        dts = records(frames)
        check(len(dts) == num and all(r[0] == (0x1CEB0000 | (da << 8) | sa) for r in dts), f'(g) n={n} window {nxt}: {len(dts)} DT')
        for ident, ext, data, typ in dts:
            seq = data[0]
            if drop == seq and not dropped: dropped = True; continue
            off = (seq - 1) * 7; buf[off:off + min(7, n - off)] = data[1:1 + min(7, n - off)]; seen.add(seq)
            tracker.process(lambda d, s_, p, is_last_packet=True: got_all.append(bytes(d)), list(data), ident)
        missing = [k for k in range(1, packets + 1) if k not in seen]
        if missing and missing[0] <= nxt + num - 1: nxt = missing[0]; continue
        nxt += num
        if nxt > packets: break
    eoma = cm(0x13, pgn, n & 0xFF, n >> 8, packets)
    rc, frames = s.tx_feed(make_record(0x1CEC0000 | (sa << 8) | da, True, eoma), 900)
    check(rc == 2, f'(g) EndOfMsgAck -> DONE, got {rc}')
    # pretty_j1939 quirk: it keys the EndOfMsgAck on the RTS direction, so hand it the ack with the RTS id
    tracker.process(lambda d, s_, p, is_last_packet=True: got_all.append(bytes(d)), list(eoma), 0x1CEC0000 | (da << 8) | sa)
    check(bytes(buf) == payload, f'(g) cantp tx n={n} window={window} drop={drop}: reassembled payload differs')
    if drop is None:
        check(got_all and got_all[0][:n] == payload, f'(g) pretty_j1939 reassembly n={n}')

def run_rts_rx(n, window, drop=None):
    """A python sender (RTS, DT per CTS) talks to CanTp's receiver; CanTp must answer CTS, re-request a dropped DT, and EoMA."""
    sa, da, pgn = 0x80, 0x21, 0xFF20
    check(byte_message(7, rts_msgdef(n), n) == 0, f'(g) define rx n={n}')
    ses = (C.c_double * 6)(-1, -1, window, 0, 0, 0)
    lib.CanTp_SessionConfig(7, ses, 6); lib.CanTp_RxReset(7)
    pos = positions(n); values = [rng.randrange(256) for _ in pos]; payload = wire_payload(n, values)
    packets = (n + 6) // 7
    s = Session(7, len(pos))
    rc, resp, vals = s.rx_step(make_record(0x1CEC0000 | (da << 8) | sa, True, cm(0x10, pgn, n & 0xFF, n >> 8, packets, 255)), 100)
    dropped, found, ctss = False, None, 0
    for rnd in range(300):
        r = records(resp)
        check(len(r) == 1 and r[0][0] == (0x1CEC0000 | (sa << 8) | da), f'(g) rx n={n}: expected one TP.CM from the receiver')
        if not r: break
        ctrl, num, nxt = r[0][2][0], r[0][2][1], r[0][2][2]
        if ctrl == 0x13:
            check(found is not None and r[0][2][1] | (r[0][2][2] << 8) == n, f'(g) rx n={n}: EoMA'); break
        check(ctrl == 0x11, f'(g) rx n={n}: unexpected control {ctrl:#x}')
        ctss += 1
        resp = b''
        for seq in range(nxt, nxt + num):
            if drop == seq and not dropped: dropped = True; continue
            off = (seq - 1) * 7; chunk = payload[off:off + 7].ljust(7, b'\xff')
            rc, out, vals = s.rx_step(make_record(0x1CEB0000 | (da << 8) | sa, True, bytes([seq]) + chunk), 200 + rnd)
            check(rc >= 0, f'(g) rx DT rc {rc}')
            if rc == 1: found = wire_payload(n, vals)
            resp += out
    check(found == payload, f'(g) cantp rx n={n} window={window} drop={drop}: payload differs')
    return ctss

for n in [9, 10, 20, 63, 64, 200, 1785]:
    run_rts_tx(n, 255)
    run_rts_tx(n, 3)
    run_rts_rx(n, 0)
    run_rts_rx(n, 4)
run_rts_tx(60, 4, drop=6)
c = run_rts_rx(60, 3, drop=5)
check(c == 4, f'(g) rx with a dropped DT: expected 4 CTS (3 windows + re-request), got {c}')
print(f'(g) J1939 RTS/CTS: {"ok" if fails == before else f"{fails - before} failures"}')

# ------------------------------------------------------------ (h) v1.2.0: flattened J1939Msg(V4) clusters (.ecd) vs the DBC, Transfer + frame lengths
before = fails
import ecdflat
lib.CanTp_DefineFlat.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_int32, C.c_int32]
lib.CanTp_FlatSize.argtypes = [C.POINTER(C.c_uint8), C.c_int32]
lib.CanTp_GetDef.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_Defaults.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32]
lib.CanTp_FrameLengths.argtypes = [C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_uint8), C.c_int32]
lib.CanTp_Transfer.argtypes = [C.c_int32, C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_uint8), C.c_int32,
                               C.POINTER(C.c_uint8), C.c_int32, C.c_uint64, C.c_uint64, C.POINTER(C.c_int32), C.POINTER(C.c_int32)]
for f in (lib.CanTp_DefineFlat, lib.CanTp_FlatSize, lib.CanTp_GetDef, lib.CanTp_Defaults, lib.CanTp_FrameLengths, lib.CanTp_Transfer): f.restype = C.c_int32

def define_flat(slot, flat, transport=-1, sa=-1):
    b = (C.c_uint8 * len(flat))(*flat)
    return lib.CanTp_DefineFlat(slot, b, len(flat), transport, sa)

def getdef(slot, nsig):
    md = (C.c_double * 8)(); sd = (C.c_double * (8 * max(1, nsig)))()
    n = lib.CanTp_GetDef(slot, md, 8, sd, nsig)
    return n, list(md), [list(sd[i * 8:(i + 1) * 8]) for i in range(min(n, nsig))]

def transfer_write(slot, values):
    size = lib.CanTp_OutputSize(slot); nfr = lib.CanTp_FrameCount(slot)
    out = (C.c_uint8 * size)(); lens = (C.c_uint8 * nfr)(); used = C.c_int32(); nf = C.c_int32()
    v = (C.c_double * max(1, len(values)))(*values)
    rc = lib.CanTp_Transfer(slot, 0, v, len(values), out, size, lens, nfr, 0, 0, C.byref(used), C.byref(nf))
    return rc, bytes(out[:used.value]), list(lens[:nf.value])

def transfer_read(slot, buf, lens, n):
    vals = (C.c_double * max(1, n))(); used = C.c_int32(); nf = C.c_int32()
    b = (C.c_uint8 * len(buf))(*buf); l = (C.c_uint8 * max(1, len(lens)))(*lens)
    rc = lib.CanTp_Transfer(slot, 1, vals, n, b, len(buf), l, len(lens), 0, 0, C.byref(used), C.byref(nf))
    return rc, list(vals)[:n], used.value, nf.value

ecd_path = args.ecd
if os.path.exists(ecd_path) and os.path.exists(args.dbc):
    msgs = ecdflat.ecd_messages(ecd_path)
    db = cantools.database.load_file(args.dbc, strict=False)
    # (h1) every cluster in the database: DefineFlat succeeds, GetDef == the python mirror, FlatSize walks the array
    with open(ecd_path, 'rb') as f: plain = ecdflat.ecd_decrypt(f.read())
    off, walked = 4, 0
    n_ok = 0
    motorola_rejected = []
    for m, flat in msgs:
        b = (C.c_uint8 * (len(plain) - off))(*plain[off:])
        sz = lib.CanTp_FlatSize(b, len(plain) - off)
        check(sz == len(flat), f'(h) FlatSize {m["name"]}: {sz} vs {len(flat)}')
        off += len(flat); walked += 1
        rc = define_flat(3, flat)
        if rc == -5 and any(c['byteOrder'] == 1 for c in m['channels']):
            # Motorola start bits in this .ecd do not all follow the DBC sawtooth rule the library applies
            # (ETC2 TransCurrentRange: ECD 48|16 vs DBC 55|16@0); the library rejects what does not fit.
            motorola_rejected.append(m['name']); continue
        check(rc == 0, f'(h) DefineFlat {m["name"]} rc {rc}')
        if rc != 0: continue
        n, md, sd = getdef(3, len(m['channels']))
        emd, esd = ecdflat.to_tables(m)
        check(n == len(m['channels']) and md == emd, f'(h) {m["name"]} msgdef {md} vs {emd}')
        check(sd == esd, f'(h) {m["name"]} sigdefs differ')
        dflt = (C.c_double * max(1, n))()
        lib.CanTp_Defaults(3, dflt, n)
        check(list(dflt)[:n] == [c['default'] for c in m['channels']], f'(h) {m["name"]} defaults')
        n_ok += 1
    check(off == len(plain), f'(h) FlatSize walk ended at {off} of {len(plain)}')
    print(f'(h1) {n_ok} of {len(msgs)} .ecd clusters defined through DefineFlat, tables and defaults match'
          + (f'; rejected (Motorola start bit does not fit under DBC numbering): {", ".join(motorola_rejected)}' if motorola_rejected else ''))

    # (h2) ECD cluster vs DBC message of the same name: same wire layout, then pack via Transfer vs cantools
    by_name = {m['name']: (m, flat) for m, flat in msgs}
    compared = 0
    for name in ['EEC1', 'EC1', 'RC', 'ET1', 'TCFG', 'CCVS', 'EEC2', 'AMB', 'LFE', 'VD', 'DM1', 'HOURS']:
        if name not in by_name: continue
        try: dm = db.get_message_by_name(name)
        except KeyError: continue
        m, flat = by_name[name]
        t = dbc2tables.convert(dm, None)
        rc = define_flat(4, flat)
        check(rc == 0, f'(h2) {name} DefineFlat rc {rc}')
        if rc != 0: continue
        n, md, sd = getdef(4, len(m['channels']))
        check(md[0] == t['msgdef'][0] and md[2] == t['msgdef'][2], f'(h2) {name}: id/len {md[:3]} vs {t["msgdef"][:3]}')
        # channel geometry by name (the ECD may order channels differently from the DBC)
        dbc_by_name = {sname: row for sname, row in zip(t['signals'], t['sigdefs'])}
        geo_ok = 0
        for c, row in zip(m['channels'], sd):
            d = dbc_by_name.get(c['name'])
            if d is None: continue
            check(row[:6] == d[:6], f'(h2) {name}.{c["name"]}: {row[:6]} vs DBC {d[:6]}')
            geo_ok += 1
        # pack every channel through Transfer (with frame lengths) and compare with cantools on the DBC message
        names = [c['name'] for c in m['channels']]
        if all(nm in dbc_by_name for nm in names) and not dm.is_multiplexed():
            for trial in range(10):
                vals = {}
                for s in dm.signals:
                    if s.name not in names: continue
                    lo = float(s.minimum) if s.minimum is not None else 0
                    hi = float(s.maximum) if s.maximum is not None else (2 ** s.length - 1) * float(s.scale) + float(s.offset)
                    raw = rng.randrange(0, 2 ** s.length - 1) if trial else 0
                    phys = raw * float(s.scale) + float(s.offset)
                    vals[s.name] = min(max(phys, lo), hi) if hi > lo else phys
                for s in dm.signals:
                    if s.name not in vals: vals[s.name] = 0
                expect = dm.encode(vals, strict=False, padding=True)
                rc, buf, lens = transfer_write(4, [vals[nm] for nm in names])
                check(rc == 0, f'(h2) {name} Transfer write rc {rc}')
                if rc != 0: break
                recs = records(buf)
                check(len(lens) == len(recs) and all(l == len(r[2]) for l, r in zip(lens, recs)), f'(h2) {name} frame lengths {lens}')
                payload = reassemble(buf, m['numBytes'])
                check(payload is not None and payload[:m['numBytes']] == expect[:m['numBytes']], f'(h2) {name} trial {trial} payload mismatch')
                rc, got, used, nf = transfer_read(4, buf, lens, len(names))
                check(rc == 1 and used == len(buf) and nf == len(recs), f'(h2) {name} Transfer read rc {rc} used {used} nf {nf}')
                dec = dm.decode(expect, decode_choices=False, scaling=True)
                for nm, g in zip(names, got):
                    d = float(dec[nm])
                    check(abs(g - d) < 1e-6 * max(1, abs(d)), f'(h2) {name}.{nm}: {g} vs {d}')
            compared += 1
            print(f'(h2) {name}: {m["numBytes"]} bytes, {len(names)} channels ({geo_ok} matched to the DBC by name), 10 value sets via Transfer')
        else:
            print(f'(h2) {name}: geometry checked for {geo_ok} channels (no full value comparison: multiplexed or names differ)')
    print(f'(h) .ecd clusters: {"ok" if fails == before else f"{fails - before} failures"}')
else:
    print(f'(h) skipped, .ecd or DBC not found: {ecd_path} / {args.dbc}')

print(f'library version {lib.CanTp_Version():#08x}')
print('ALL OK' if fails == 0 else f'{fails} FAILURES')
sys.exit(1 if fails else 0)
