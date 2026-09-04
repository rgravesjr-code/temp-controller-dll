"""
oracle_test.py - TempCtl v2 end-to-end cross-check with independent implementations.

  * tempctl.dll   TcStep through ctypes: a scripted scenario (warm-up, setpoint
                  step, sensor failure with failover, feedback fault) checked
                  against expectations derived from the spec in tempctl.h.
  * cantp.dll     the vendored CanTp packs every tick's output array with the
                  tables from dbc/tables (generated from dbc/tempctl.dbc).
  * cantools      encodes the same physical values with dbc/tempctl.dbc; the
                  50-byte payload must match CanTp's reassembled BAM byte for
                  byte, and cantools' decode of CanTp's payload must equal
                  CanTp_Unpack's values.

Usage:  python tests\oracle_test.py [--tempctl build\win-x64\tempctl.dll] [--cantp third_party\cantp\cantp.dll] [--pylibs DIR]
Needs cantools (pip install cantools, or --pylibs pointing at a `pip install --target` dir).
Also doubles as a ctypes usage example of the TempCtl + CanTp chain.
"""
import argparse, ctypes as C, json, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
ap = argparse.ArgumentParser()
ap.add_argument('--tempctl', default=os.path.join(ROOT, 'build', 'win-x64', 'tempctl.dll'))
ap.add_argument('--cantp', default=os.path.join(ROOT, 'third_party', 'cantp', 'cantp.dll'))
ap.add_argument('--dbc', default=os.path.join(ROOT, 'dbc', 'tempctl.dbc'))
ap.add_argument('--tables', default=os.path.join(ROOT, 'dbc', 'tables', 'TempCtl.json'))
ap.add_argument('--pylibs', default=None)
args = ap.parse_args()
if args.pylibs: sys.path.insert(0, args.pylibs)
import cantools

# ---------------------------------------------------------------- libraries
tc = C.CDLL(os.path.abspath(args.tempctl))
tc.TcVersion.restype = C.c_uint32
tc.TcStep.argtypes = [C.c_int32, C.c_int32, C.c_uint32, C.POINTER(C.c_float), C.c_int32, C.POINTER(C.c_float), C.c_int32]
tc.TcStep.restype = C.c_int32
tc.TcSignalCount.restype = C.c_int32
tc.TcInputCount.restype = C.c_int32

cp = C.CDLL(os.path.abspath(args.cantp))
cp.CanTp_Version.restype = C.c_uint32
cp.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
cp.CanTp_OutputSize.argtypes = [C.c_int32]
cp.CanTp_PackSgl.argtypes = [C.c_int32, C.POINTER(C.c_float), C.c_int32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_int32)]
cp.CanTp_Unpack.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_int32)]
for f in (cp.CanTp_Define, cp.CanTp_OutputSize, cp.CanTp_PackSgl, cp.CanTp_Unpack): f.restype = C.c_int32

N_IN, N_OUT = tc.TcInputCount(), tc.TcSignalCount()
INIT, STEP, RESET = 0, 1, 2
(SETPOINT, DB_HI, DB_LO, HI_LIMIT, LO_LIMIT, ERR_TO, DB_TO, FILTER, T2_EN, T2_TOL, FB_EN,
 TEMP1, TEMP2, HTR_FB, CLR_FB, HEAT_CMD, COOL_CMD, ERR_STATUS, TEMP_STATUS, CTRL_TEMP,
 T1_FILT, T2_FILT, HI_BAND, LO_BAND, ERR_REMAIN, DB_REMAIN, ACTIVE) = range(27)
assert (N_IN, N_OUT) == (17, 27), (N_IN, N_OUT)

fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print('FAIL', msg)

# ---------------------------------------------------------------- tables / DBC
tab = json.load(open(args.tables))
msgdef = (C.c_double * 8)(*tab['msgdef'])
sigrows = tab['sigdefs']
sigdefs = (C.c_double * (8 * len(sigrows)))(*[v for row in sigrows for v in row])
SLOT = 0
rc = cp.CanTp_Define(SLOT, msgdef, 8, sigdefs, len(sigrows))
check(rc == 0, f'CanTp_Define rc={rc}')
check(len(sigrows) == N_OUT, f'table has {len(sigrows)} signals, controller has {N_OUT}')
OUT_SIZE = cp.CanTp_OutputSize(SLOT)
db = cantools.database.load_file(args.dbc, strict=True)
msg = db.get_message_by_name('TempCtl')
names = tab['signals']
check([s.name for s in msg.signals] == names, 'DBC / table signal order differ')
PAYLOAD_LEN = msg.length

def reassemble_bam(buf):
    """NI-XNET raw records of one BAM -> (payload bytes, ids). Independent of CanTp's own unpacker."""
    ids, chunks, size = [], [], None
    o = 0
    while o < len(buf):
        ts, ident, typ, flags, info, plen = struct.unpack_from('<QIBBBB', buf, o)
        data = bytes(buf[o + 16:o + 16 + plen]); o += 24
        pgn = (ident >> 8) & 0x3FF00
        ids.append(ident & 0x1FFFFFFF)
        if pgn == 0xEC00:                       # TP.CM BAM
            check(data[0] == 0x20, 'TP.CM control byte')
            size = data[1] | (data[2] << 8)
            check(data[5] | (data[6] << 8) | (data[7] << 16) == 0xFF00, 'TP.CM PGN')
        elif pgn == 0xEB00:                     # TP.DT
            check(data[0] == len(chunks) + 1, 'TP.DT sequence')
            chunks.append(data[1:8])
    return b''.join(chunks)[:size], ids

def phys_to_cantools(values):
    """Physical values as cantools expects them: NaN -> skipped (checked separately), clamp to the DBC range,
    round to the raw resolution the same way CanTp does (nearest)."""
    d = {}
    for s, v in zip(msg.signals, values):
        if math.isnan(v): v = float(s.minimum)          # placeholder; those bits are masked out
        v = min(max(v, float(s.minimum)), float(s.maximum))
        raw = round((v - float(s.offset)) / float(s.scale))
        d[s.name] = raw * float(s.scale) + float(s.offset)
    return d

def na_pattern_ok(payload, sig):
    raw = int.from_bytes(payload, 'little') >> sig.start
    return raw & ((1 << sig.length) - 1) == (1 << sig.length) - 1

# ---------------------------------------------------------------- one tick: controller -> CanTp -> oracle
outbuf = (C.c_uint8 * OUT_SIZE)()
written = C.c_int32()
unpacked = (C.c_double * N_OUT)()
consumed = C.c_int32()
ticks_checked = 0

def pack_and_compare(out):
    global ticks_checked
    rc = cp.CanTp_PackSgl(SLOT, out, N_OUT, 0, 0, outbuf, OUT_SIZE, C.byref(written))
    check(rc == 0 and written.value == OUT_SIZE, f'PackSgl rc={rc} written={written.value}')
    payload, ids = reassemble_bam(bytes(outbuf))
    check(len(payload) == PAYLOAD_LEN, f'payload {len(payload)} != {PAYLOAD_LEN}')
    check(ids[0] == 0x1CECFFFE and all(i == 0x1CEBFFFE for i in ids[1:]), f'ids {[hex(i) for i in ids]}')
    values = list(out)
    expect = msg.encode(phys_to_cantools(values), scaling=True, padding=False, strict=True)
    # Compare only bits that carry a signal value: CanTp fills unused bits with the J1939 pad (1s)
    # where cantools leaves 0s, and NaN signals are packed as all ones (checked separately).
    mask = bytearray(PAYLOAD_LEN)
    for s, v in zip(msg.signals, values):
        if math.isnan(v):
            check(na_pattern_ok(payload, s), f'{s.name}: NaN not packed as not-available')
            continue
        for b in range(s.start, s.start + s.length): mask[b // 8] |= 1 << (b % 8)
    got = bytes(p & m for p, m in zip(payload, mask))
    exp = bytes(e & m for e, m in zip(expect, mask))
    check(got == exp, f'payload differs from cantools\n  cantp   {payload.hex()}\n  cantools{expect.hex()}')
    # decode side: cantools decode of CanTp's bytes == CanTp_Unpack
    rc = cp.CanTp_Unpack(SLOT, outbuf, OUT_SIZE, unpacked, N_OUT, C.byref(consumed))
    check(rc == 1 and consumed.value == OUT_SIZE, f'Unpack rc={rc}')
    dec = msg.decode(payload, decode_choices=False, scaling=True)
    for s, u in zip(msg.signals, unpacked):
        check(abs(dec[s.name] - u) < 1e-9, f'{s.name}: cantools {dec[s.name]} vs CanTp_Unpack {u}')
    ticks_checked += 1
    return payload

# ---------------------------------------------------------------- scenario
sig = (C.c_float * N_OUT)()
out = (C.c_float * N_OUT)()
def set_cfg(**kw):
    for k, v in kw.items(): sig[globals()[k]] = v
set_cfg(SETPOINT=50, DB_HI=5, DB_LO=5, HI_LIMIT=90, LO_LIMIT=10, ERR_TO=2000, DB_TO=500,
        FILTER=4, T2_EN=1, T2_TOL=4, FB_EN=1, TEMP1=20, TEMP2=20.5)
def step(ms, action=STEP, **kw):
    set_cfg(**kw)
    rc = tc.TcStep(0, action, ms, sig, N_IN, out, N_OUT)
    check(rc >= 0, f'TcStep rc={rc} at {ms}')
    pack_and_compare(out)
    return rc

step(0, INIT)
check(out[ACTIVE] == 1 and out[TEMP_STATUS] == 8, 'init: sensor 1, filter warm-up')
# toy plant with honest relay feedback
temp, dt = 20.0, 0.1
heat = cool = 0
for i in range(1, 1201):                                      # 120 s
    ms = i * 100
    # relays answer the previous command
    set_cfg(HTR_FB=heat, CLR_FB=cool)
    fail_t1 = 600 <= i < 700                                  # 10 s open sensor 1
    step(ms, TEMP1=(float('nan') if fail_t1 else temp + 0.3), TEMP2=temp - 0.2)
    heat, cool = int(out[HEAT_CMD]), int(out[COOL_CMD])
    temp += dt * (0.02 * (20.0 - temp) + 6.0 * heat - 6.0 * cool)
    if i == 40:  check(out[TEMP_STATUS] in (1, 2), f'heat pending/heating after warm-up, got {out[TEMP_STATUS]}')
    if i == 300: check(abs(out[CTRL_TEMP] - 50) < 6, f'near setpoint at 30 s: {out[CTRL_TEMP]}')
    if i == 605: check(out[HEAT_CMD] == 0 and out[COOL_CMD] == 0 and out[TEMP_STATUS] == 5, 'relays off, error pending on open sensor')
    if i == 625: check(int(out[ERR_STATUS]) & 4 and out[ACTIVE] == 2 and out[TEMP_STATUS] == 7, f'failover to sensor 2 after 2 s: err={int(out[ERR_STATUS])} active={out[ACTIVE]} st={out[TEMP_STATUS]}')
    if i == 800: check(out[ACTIVE] == 2 and int(out[ERR_STATUS]) == 4, 'sensor 1 stays failed (latched)')
step(120200, RESET)
check(out[ACTIVE] == 1 and int(out[ERR_STATUS]) == 0, 'reset clears the failed sensor')
# feedback fault: heater stuck off while commanded on
set_cfg(SETPOINT=80, HTR_FB=0)
for i in range(1, 60):
    step(120200 + i * 100, TEMP1=temp, TEMP2=temp)
    if i == 8:  check(out[HEAT_CMD] == 1, 'heating commanded')
    if i == 20: check(out[TEMP_STATUS] == 5, 'feedback mismatch pending')
check(int(out[ERR_STATUS]) & 0x80 and out[HEAT_CMD] == 1, f'heater feedback bit latched, operation continues: {int(out[ERR_STATUS])}')
# Temp2 disabled -> Temp2Filtered NaN -> packed as not available
step(130000, T2_EN=0)
check(math.isnan(out[T2_FILT]), 'Temp2Filtered NaN when disabled')

print(f'tempctl {tc.TcVersion():#x}, cantp {cp.CanTp_Version():#x}, cantools {cantools.__version__}: '
      f'{ticks_checked} ticks packed and compared bit-for-bit, {PAYLOAD_LEN}-byte BAM of {OUT_SIZE // 24} frames')
print('ALL OK' if fails == 0 else f'{fails} FAILURE(S)')
sys.exit(1 if fails else 0)
