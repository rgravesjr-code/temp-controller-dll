"""
oracle_test.py - end-to-end check of the TempCtl v4 diagnostics on the CAN
wire, against an independent encoder:

  * tempctl.dll   TcInit / TcStart / TcCheckTemp / TcStop / TcReset / TcGetDiag
                  through ctypes: a scripted closed-loop run with a toy plant,
                  an open sensor, a relay-feedback fault, permissive losses
                  (one-tick trip, sustained loss to the fault), Stop and Reset.
  * cantp.dll     the vendored CanTp packs every tick's 28-value diagnostics
                  into a J1939 BAM (slot 0 defined from dbc/tables/TempCtl.json,
                  slot 1 from the flattened ECD cluster of dbc/tempctl.ecd via
                  CanTp_DefineFlat; both must produce identical bytes).
  * cantools      encodes the same physical values with dbc/tempctl.dbc; the
                  payloads must agree bit for bit on every signal, NaN must be
                  the J1939 not-available pattern, and cantools' decode of
                  CanTp's bytes must equal CanTp_Unpack.

Also covers the handoff 13.8 items: every millisecond diagnostic is U16 /
factor 1 / offset 0 in the DBC and the ECD, 0 / 1 / 64255 round-trip, 64256
and larger saturate to 64255 on the wire while TcGetDiag keeps the full
value, NaN is not-available, the payload is 44 bytes under PGN 65280 /
0x18FF00FE, and a table with the wrong signal count is refused.

Usage:  python tests\oracle_test.py [--tempctl build\win-x64\tempctl.dll] [--cantp third_party\cantp\cantp.dll] [--pylibs DIR]
Exit code 0 and "ALL OK" when every check passed.
"""
import argparse, ctypes as C, json, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
ap = argparse.ArgumentParser()
ap.add_argument('--tempctl', default=os.path.join(ROOT, 'build', 'win-x64', 'tempctl.dll'))
ap.add_argument('--cantp', default=os.path.join(ROOT, 'third_party', 'cantp', 'cantp.dll'))
ap.add_argument('--dbc', default=os.path.join(ROOT, 'dbc', 'tempctl.dbc'))
ap.add_argument('--tables', default=os.path.join(ROOT, 'dbc', 'tables', 'TempCtl.json'))
ap.add_argument('--ecd', default=os.path.join(ROOT, 'dbc', 'tempctl.ecd'))
ap.add_argument('--pylibs', default=None)
args = ap.parse_args()
if args.pylibs: sys.path.insert(0, args.pylibs)
sys.path.insert(0, os.path.join(ROOT, 'third_party', 'cantp', 'tools'))
import cantools
import ecdflat

EXPECTED_SETUP, EXPECTED_DIAG, EXPECTED_PAYLOAD, EXPECTED_ID = 18, 28, 44, 0x18FF00FE
MS_MAX = 64255.0

# ---------------------------------------------------------------- libraries
tc = C.CDLL(os.path.abspath(args.tempctl))
I32P = C.POINTER(C.c_int32)
tc.TcVersion.restype = C.c_int32
tc.TcSetupCount.restype = C.c_int32
tc.TcDiagCount.restype = C.c_int32
tc.TcInit.argtypes = [C.c_int32, C.c_uint32, C.POINTER(C.c_double), C.c_int32, I32P, I32P]
tc.TcStart.argtypes = [C.c_int32, C.c_uint32, C.c_int32, I32P, I32P]
tc.TcStop.argtypes = [C.c_int32, C.c_uint32, I32P, I32P, I32P, I32P]
tc.TcCheckTemp.argtypes = [C.c_int32, C.c_uint32, C.c_double, C.c_double, C.c_int32, C.c_int32, C.c_int32, I32P, I32P, I32P, I32P]
tc.TcReset.argtypes = [C.c_int32, C.c_uint32, I32P, I32P]
tc.TcGetDiag.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32]
for f in (tc.TcInit, tc.TcStart, tc.TcStop, tc.TcCheckTemp, tc.TcReset, tc.TcGetDiag): f.restype = C.c_int32

cp = C.CDLL(os.path.abspath(args.cantp))
cp.CanTp_Version.restype = C.c_uint32
cp.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
cp.CanTp_DefineFlat.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.c_int32, C.c_int32]
cp.CanTp_GetDef.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
cp.CanTp_PayloadLength.argtypes = [C.c_int32]
cp.CanTp_SignalCount.argtypes = [C.c_int32]
cp.CanTp_OutputSize.argtypes = [C.c_int32]
cp.CanTp_Pack.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, I32P]
cp.CanTp_Unpack.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_double), C.c_int32, I32P]
for f in (cp.CanTp_Define, cp.CanTp_DefineFlat, cp.CanTp_GetDef, cp.CanTp_PayloadLength, cp.CanTp_SignalCount,
          cp.CanTp_OutputSize, cp.CanTp_Pack, cp.CanTp_Unpack): f.restype = C.c_int32

fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print('FAIL', msg)

N_SETUP, N_DIAG = tc.TcSetupCount(), tc.TcDiagCount()
if (N_SETUP, N_DIAG) != (EXPECTED_SETUP, EXPECTED_DIAG) or tc.TcVersion() >> 16 != 4:
    sys.exit(f'FAIL not a TempCtl v4 binary: version {tc.TcVersion():#x}, setup {N_SETUP}, diag {N_DIAG} '
             f'(expected 4.x.y, {EXPECTED_SETUP}, {EXPECTED_DIAG})')
if cp.CanTp_Version() < 0x010200:
    sys.exit(f'FAIL CanTp {cp.CanTp_Version():#x} is older than 1.2.0 (CanTp_DefineFlat needed)')

# setup indexes (TC_SETUP_*)
(ENABLE, UNITS, SETPOINT, DB_HI, DB_LO, HI_LIMIT, LO_LIMIT, ERR_TO, DB_TO, ASP_TO,
 T2_EN, T2_OFF, T2_TOL, CMP_TO, FILTER, FB_EN, FB_TO, OC_TO) = range(18)
# diag indexes (TC_DIAG_*)
(D_CTRL, D_ACTIVE, D_T1RAW, D_T2RAW, D_T2CORR, D_T1AVG, D_T2AVG, D_HIBAND, D_LOBAND, D_HCFLAG,
 D_DB_REM, D_ASP_REM, D_CMP_REM, D_HFB_REM, D_CFB_REM, D_T1ACC, D_T2ACC, D_T1EV, D_T2EV,
 D_STATUS, D_WARNING, D_DOHEAT, D_DOCOOL, D_FILTER, D_INIT, D_PERM, D_OC_REM, D_STARTED) = range(28)
MS_FIELDS = (D_DB_REM, D_ASP_REM, D_CMP_REM, D_HFB_REM, D_CFB_REM, D_T1ACC, D_T2ACC, D_OC_REM)
# status / warning codes
(ST_DISABLED, ST_AT_SETPT, ST_HEATER_ON, ST_COOLER_ON, ST_HEAT_PENDING, ST_COOL_PENDING,
 ST_IDLE_STOPPED, ST_IDLE_BLOCKED, ST_OC_PENDING, ST_IDLE_TRIPPED) = range(10)
ST_T1_FAIL_HIGH, ST_BOTH_FAILED, ST_HEATER_FB_FAULT, ST_OC_FAULT = 10, 12, 15, 17
WN_NONE, WN_T1_OOR, WN_HEATER_FB, WN_RUNNING_ON_T2, WN_OC_NOT_MET = 0, 1, 3, 6, 8

# ---------------------------------------------------------------- tables / DBC / ECD
tab = json.load(open(args.tables))
sigrows = tab['sigdefs']
if len(sigrows) != N_DIAG:
    sys.exit(f'FAIL {args.tables} has {len(sigrows)} signal rows, the controller has {N_DIAG} diagnostics: '
             f'stale table (a v3 table has 25 rows). Rerun tools\\make_tempctl_dbc.py --tables --ecd.')
msgdef = (C.c_double * 8)(*tab['msgdef'])
sigdefs = (C.c_double * (8 * len(sigrows)))(*[v for row in sigrows for v in row])
SLOT, SLOT_ECD = 0, 1
rc = cp.CanTp_Define(SLOT, msgdef, 8, sigdefs, len(sigrows))
check(rc == 0, f'CanTp_Define rc={rc}')
OUT_SIZE = cp.CanTp_OutputSize(SLOT)
db = cantools.database.load_file(args.dbc, strict=True)
msg = db.get_message_by_name('TempCtl')
names = tab['signals']
check([s.name for s in msg.signals] == names, 'DBC / table signal order differ')
check(len(msg.signals) == N_DIAG, f'DBC has {len(msg.signals)} signals, controller {N_DIAG} (stale v3 DBC?)')
PAYLOAD_LEN = msg.length
check(PAYLOAD_LEN == EXPECTED_PAYLOAD, f'payload length {PAYLOAD_LEN} != {EXPECTED_PAYLOAD}')                     # 13.8.5
check(msg.frame_id == EXPECTED_ID and msg.is_extended_frame, f'CAN id {msg.frame_id:#x}')
check(cp.CanTp_PayloadLength(SLOT) == EXPECTED_PAYLOAD, 'CanTp payload length')
for s in msg.signals:                                                                                          # 13.8.1
    if s.unit == 'ms':
        check((s.length, float(s.scale), float(s.offset), float(s.minimum), float(s.maximum)) == (16, 1.0, 0.0, 0.0, MS_MAX),
              f'{s.name}: not a U16 ms signal 0..64255')
check(sum(1 for s in msg.signals if s.unit == 'ms') == len(MS_FIELDS), 'eight millisecond diagnostics expected')

# the ECD: same message, channels in TC_DIAG order; CanTp_DefineFlat must give the same slot as the tables
ecd_msgs = ecdflat.ecd_messages(args.ecd)
check(len(ecd_msgs) == 1 and ecd_msgs[0][0]['name'] == 'TempCtl', 'tempctl.ecd holds one TempCtl message')
ecd_m, ecd_flat = ecd_msgs[0]
check([c['name'] for c in ecd_m['channels']] == names, 'ECD channel order differs from TC_DIAG order (a reorder table would be needed)')   # 13.8.7
for c in ecd_m['channels']:                                                                                      # 13.8.1 (ECD)
    if c['unit'] == 'ms':
        check((c['nBits'], c['dataType'], c['byteOrder'], c['sf'], c['offset'], c['min'], c['max']) == (16, 1, 0, 1.0, 0.0, 0.0, MS_MAX), f'ECD {c["name"]} not U16 ms')
for c, s in zip(ecd_m['channels'], msg.signals):                                                                 # 13.8.4
    check((c['startBit'], c['nBits'], c['sf'], c['offset'], c['min'], c['max'], c['unit']) ==
          (s.start, s.length, float(s.scale), float(s.offset), float(s.minimum), float(s.maximum), s.unit or ''), f'ECD/DBC differ on {s.name}')
    lut = {int(e['value']): e['short'] for e in c['lut']}
    dbc_choices = {int(k): str(v) for k, v in (s.choices or {}).items()}
    check(lut == dbc_choices, f'ECD lookup / DBC value table differ on {s.name}: {lut} vs {dbc_choices}')
flatbuf = (C.c_uint8 * len(ecd_flat)).from_buffer_copy(ecd_flat)
rc = cp.CanTp_DefineFlat(SLOT_ECD, flatbuf, len(ecd_flat), -1, -1)
check(rc == 0, f'CanTp_DefineFlat rc={rc}')
def slot_def(slot):
    md = (C.c_double * 8)(); sd = (C.c_double * (8 * 64))()
    n = cp.CanTp_GetDef(slot, md, 8, sd, 64)
    return n, list(md), [list(sd[8 * i:8 * i + 8]) for i in range(max(n, 0))]
n0, md0, sd0 = slot_def(SLOT)
n1, md1, sd1 = slot_def(SLOT_ECD)
check(n0 == n1 == N_DIAG, f'slot signal counts {n0} / {n1}')
check(md0 == md1, f'msgdef from the tables {md0} != from the ECD {md1}')
check(sd0 == sd1, 'signal rows from the tables and from the ECD differ')
check(cp.CanTp_OutputSize(SLOT_ECD) == OUT_SIZE and cp.CanTp_PayloadLength(SLOT_ECD) == EXPECTED_PAYLOAD, 'ECD slot sizes')

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
    """Physical values as cantools expects them: NaN -> skipped (checked separately), clamp to the DBC range
    (this is the wire saturation), round to the raw resolution the same way CanTp does (nearest)."""
    d = {}
    for s, v in zip(msg.signals, values):
        if math.isnan(v): v = float(s.minimum)          # placeholder; those bits are masked out
        v = min(max(v, float(s.minimum)), float(s.maximum))
        raw = round((v - float(s.offset)) / float(s.scale))
        d[s.name] = raw * float(s.scale) + float(s.offset)
    return d

def raw_of(payload, sig):
    return (int.from_bytes(payload, 'little') >> sig.start) & ((1 << sig.length) - 1)

def na_pattern_ok(payload, sig):
    return raw_of(payload, sig) == (1 << sig.length) - 1

# ---------------------------------------------------------------- one array: CanTp (both slots) -> oracle
outbuf = (C.c_uint8 * OUT_SIZE)()
outbuf2 = (C.c_uint8 * OUT_SIZE)()
written = C.c_int32()
unpacked = (C.c_double * N_DIAG)()
consumed = C.c_int32()
arrays_checked = 0

def pack_and_compare(diag):
    global arrays_checked
    rc = cp.CanTp_Pack(SLOT, diag, N_DIAG, 0, 0, outbuf, OUT_SIZE, C.byref(written))
    check(rc == 0 and written.value == OUT_SIZE, f'Pack rc={rc} written={written.value}')
    rc = cp.CanTp_Pack(SLOT_ECD, diag, N_DIAG, 0, 0, outbuf2, OUT_SIZE, C.byref(written))
    check(rc == 0 and bytes(outbuf2) == bytes(outbuf), 'ECD-defined slot packs different bytes than the table-defined slot')
    payload, ids = reassemble_bam(bytes(outbuf))
    check(len(payload) == PAYLOAD_LEN, f'payload {len(payload)} != {PAYLOAD_LEN}')
    check(ids[0] == 0x1CECFFFE and all(i == 0x1CEBFFFE for i in ids[1:]), f'ids {[hex(i) for i in ids]}')
    values = list(diag)
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
    # the millisecond fields on the wire: min(value, 64255), NaN = 0xFFFF (13.8.2, 13.8.3)
    for i in MS_FIELDS:
        s, v = msg.signals[i], values[i]
        want = 0xFFFF if math.isnan(v) else int(min(v, MS_MAX))
        check(raw_of(payload, s) == want, f'{s.name}: wire raw {raw_of(payload, s)} for value {v}, expected {want}')
    # decode side: cantools decode of CanTp's bytes == CanTp_Unpack (NaN fields come back as the max = not available)
    rc = cp.CanTp_Unpack(SLOT, outbuf, OUT_SIZE, unpacked, N_DIAG, C.byref(consumed))
    check(rc == 1 and consumed.value == OUT_SIZE, f'Unpack rc={rc}')
    dec = msg.decode(payload, decode_choices=False, scaling=True)
    for s, u in zip(msg.signals, unpacked):
        check(abs(dec[s.name] - u) < 1e-9, f'{s.name}: cantools {dec[s.name]} vs CanTp_Unpack {u}')
    arrays_checked += 1
    return payload

# ---------------------------------------------------------------- 13.8.2 / 13.8.3: U16 boundary vectors, independent of the controller
sane = [50.0, 1, 50.0, 49.5, 50.0, 50.0, 50.0, 55.0, 45.0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, ST_AT_SETPT, WN_NONE, 0, 0, 4, 1, 1, 0, 1]
for v in (0.0, 1.0, 64254.0, 64255.0, 64256.0, 65535.0, 65536.0, 100000.0, 4294967295.0, 1e12, float('nan')):
    arr = list(sane)
    for i in MS_FIELDS: arr[i] = v
    payload = pack_and_compare((C.c_double * N_DIAG)(*arr))
    for i in MS_FIELDS:
        raw = raw_of(payload, msg.signals[i])
        if math.isnan(v):      check(raw == 0xFFFF, f'{names[i]}: NaN -> {raw:#x}')
        elif v <= MS_MAX:      check(raw == int(v), f'{names[i]}: {v} -> {raw}')
        else:                  check(raw == 0xFAFF, f'{names[i]}: {v} should saturate to 0xFAFF, got {raw:#x}')
# RunPermissive NaN (before the first evaluation) is a 2-bit not-available (3)
arr = list(sane); arr[D_PERM] = float('nan'); arr[D_STARTED] = 0; arr[D_STATUS] = ST_IDLE_STOPPED
payload = pack_and_compare((C.c_double * N_DIAG)(*arr))
check(raw_of(payload, msg.signals[D_PERM]) == 3, 'RunPermissive NaN -> 3')
# every status and warning code survives the trip
for code in range(0, 18):
    arr = list(sane); arr[D_STATUS] = code; arr[D_WARNING] = min(code, 8)
    pack_and_compare((C.c_double * N_DIAG)(*arr))
boundary_arrays = arrays_checked

# ---------------------------------------------------------------- scenario: controller -> CanTp -> oracle on every call
setup = (C.c_double * N_SETUP)()
diag = (C.c_double * N_DIAG)()
st, wn, dh, dc = C.c_int32(), C.c_int32(), C.c_int32(), C.c_int32()
ZONE = 0
OC_TIMEOUT = 3000

def set_setup(**kw):
    for k, v in kw.items(): setup[globals()[k]] = v
set_setup(ENABLE=1, UNITS=1, SETPOINT=50, DB_HI=5, DB_LO=5, HI_LIMIT=90, LO_LIMIT=10, ERR_TO=2000, DB_TO=500, ASP_TO=1000,
          T2_EN=1, T2_OFF=0.5, T2_TOL=4, CMP_TO=5000, FILTER=4, FB_EN=1, FB_TO=1000, OC_TO=OC_TIMEOUT)

def get_diag():
    rc = tc.TcGetDiag(ZONE, diag, N_DIAG)
    check(rc == 0, f'TcGetDiag rc={rc}')
    pack_and_compare(diag)
    check(diag[D_STATUS] == st.value and diag[D_WARNING] == wn.value, 'status / warning mirrors differ from the last call')

def init(ms):
    rc = tc.TcInit(ZONE, ms, setup, N_SETUP, C.byref(st), C.byref(wn))
    check(rc == 0, f'TcInit rc={rc}')
    get_diag()

def start(ms, perm):
    rc = tc.TcStart(ZONE, ms, perm, C.byref(st), C.byref(wn))
    check(rc == 0, f'TcStart rc={rc}')
    get_diag()

def stop(ms):
    rc = tc.TcStop(ZONE, ms, C.byref(dh), C.byref(dc), C.byref(st), C.byref(wn))
    check(rc == 0 and dh.value == 0 and dc.value == 0, f'TcStop rc={rc} dh={dh.value} dc={dc.value}')
    get_diag()

def reset(ms):
    rc = tc.TcReset(ZONE, ms, C.byref(st), C.byref(wn))
    check(rc == 0, f'TcReset rc={rc}')
    get_diag()

def tick(ms, t1, t2, hfb, cfb, perm=1):
    rc = tc.TcCheckTemp(ZONE, ms, t1, t2, hfb, cfb, perm, C.byref(dh), C.byref(dc), C.byref(st), C.byref(wn))
    check(rc == 0, f'TcCheckTemp rc={rc} at {ms}')
    get_diag()
    check(diag[D_DOHEAT] == dh.value and diag[D_DOCOOL] == dc.value, f'relay mirrors differ from the outputs at {ms}')
    return rc

init(0)
check(st.value == ST_IDLE_STOPPED and wn.value == WN_NONE and diag[D_INIT] == 1 and diag[D_ACTIVE] == 1, 'init: idle stopped, sensor 1')
check(math.isnan(diag[D_CTRL]) and math.isnan(diag[D_T1RAW]) and math.isnan(diag[D_PERM]) and diag[D_STARTED] == 0, 'init: no readings, no permissive yet, not started')
tick(50, 20.3, 19.8, 0, 0)
check(st.value == ST_IDLE_STOPPED and dh.value == 0 and diag[D_T1RAW] == 20.3 and diag[D_PERM] == 1 and diag[D_STARTED] == 0, 'CheckTemp before Start: inert, raw mirrored')
start(100, 0)
check(st.value == ST_IDLE_BLOCKED and wn.value == WN_OC_NOT_MET and diag[D_PERM] == 0 and diag[D_OC_REM] == 0, 'blocked Start')
stop(100)
check(st.value == ST_IDLE_STOPPED and wn.value == WN_NONE, 'Stop acknowledges the blocked Start')
start(100, 1)
check(st.value == ST_AT_SETPT and diag[D_STARTED] == 1 and diag[D_PERM] == 1 and diag[D_DOHEAT] == 0, 'accepted Start: provisional TempAtSetPt, relays off')

# toy plant with honest relay feedback (the DO answers the previous command)
temp, dt = 20.0, 0.1
heat = cool = 0
saw_heat_pending = saw_heater_on = saw_at_setpt = False
for i in range(2, 1201):                                      # 120 s, tick 1 was the stopped tick above
    ms = i * 100
    fail_t1 = 600 <= i < 700                                  # 10 s open sensor 1
    perm = 0 if (850 <= i < 853 or i >= 950) else 1           # one-tick-class loss at 85 s, sustained loss from 95 s
    if i == 856: start(ms, 1)                                 # explicit restart after the trip recovered
    t1 = float('nan') if fail_t1 else temp + 0.3
    t2 = temp - 0.2                                           # + Temp2Offset 0.5 = temp + 0.3, so the two agree
    tick(ms, t1, t2, heat, cool, perm)
    heat, cool = dh.value, dc.value
    temp += dt * (0.02 * (20.0 - temp) + 2.0 * heat - 2.0 * cool)   # 2 deg/s: the 1 s at-setpoint hold overshoots by 2, inside the 5 deg band
    saw_heat_pending |= st.value == ST_HEAT_PENDING
    saw_heater_on |= st.value == ST_HEATER_ON
    saw_at_setpt |= st.value == ST_AT_SETPT and i > 2
    if i == 2:   check(st.value == ST_HEAT_PENDING and dh.value == 0 and diag[D_DB_REM] == 500, f'first active tick: heat pending, got st={st.value} rem={diag[D_DB_REM]}')
    if i == 7:   check(st.value == ST_HEATER_ON and dh.value == 1, f'heater on after 500 ms, got st={st.value}')
    if i == 300: check(abs(diag[D_CTRL] - 50) < 6, f'near setpoint at 30 s: {diag[D_CTRL]}')
    if i == 599: check(st.value < 10 and wn.value == WN_NONE and diag[D_HCFLAG] == 1, f'healthy before the open sensor: st={st.value} wn={wn.value}')
    if i == 605: check(st.value < 10 and wn.value == WN_T1_OOR and diag[D_T1ACC] == 600 and math.isnan(diag[D_CTRL]), f'open sensor: warning 1, accumulator 600 after 6 ticks, got wn={wn.value} acc={diag[D_T1ACC]}')
    if i == 618: check(diag[D_ACTIVE] == 1 and st.value < 10 and diag[D_T1ACC] == 1900, 'still on sensor 1 one tick before ErrorTimeout')
    if i == 619: check(diag[D_ACTIVE] == 2 and st.value < 10 and diag[D_T1ACC] == 2000, f'failover to sensor 2 at 2 s: active={diag[D_ACTIVE]} st={st.value}')
    if i == 700: check(wn.value == WN_RUNNING_ON_T2 and diag[D_ACTIVE] == 2, f'sensor 1 back in range: warning 6, still on sensor 2: wn={wn.value}')
    if i == 800: check(diag[D_ACTIVE] == 2 and st.value < 10 and abs(diag[D_CTRL] - (t2 + 0.5)) < 1e-9, 'control on corrected sensor 2, no fault')
    if i == 849: check(diag[D_STARTED] == 1 and st.value < 6, f'running before the permissive loss: st={st.value}')
    if i == 850: check(st.value == ST_OC_PENDING and dh.value == 0 and dc.value == 0 and diag[D_OC_REM] == OC_TIMEOUT and diag[D_STARTED] == 0 and diag[D_PERM] == 0
                       and wn.value == WN_RUNNING_ON_T2, f'permissive lost: relays off at once, pending, full timeout; warning 6 masks 8: st={st.value} wn={wn.value} rem={diag[D_OC_REM]}')
    if i == 852: check(st.value == ST_OC_PENDING and diag[D_OC_REM] == OC_TIMEOUT - 200, f'countdown running: rem={diag[D_OC_REM]}')
    if i == 853: check(st.value == ST_IDLE_TRIPPED and diag[D_OC_REM] == 0 and diag[D_PERM] == 1 and dh.value == 0, f'recovered before the timeout: tripped, no fault, no restart: st={st.value}')
    if i == 855: check(st.value == ST_IDLE_TRIPPED and dh.value == 0 and diag[D_STARTED] == 0, 'still tripped: recovery never restarts')
    if i == 856: check(st.value < 6 and diag[D_STARTED] == 1, f'explicit Start restarted control: st={st.value}')
    if i == 950: check(st.value == ST_OC_PENDING and dh.value == 0 and dc.value == 0 and diag[D_OC_REM] == OC_TIMEOUT, 'sustained loss: pending')
    if i == 979: check(st.value == ST_OC_PENDING and diag[D_OC_REM] == 100, f'one tick before the operating-condition fault: rem={diag[D_OC_REM]}')
    if i == 980: check(st.value == ST_OC_FAULT and wn.value == WN_RUNNING_ON_T2 and diag[D_OC_REM] == 0 and dh.value == 0, f'OperatingConditionFault on the exact tick: st={st.value} wn={wn.value}')
    if i == 980: frozen_oc = list(diag)
    if i == 1100: check(st.value == ST_OC_FAULT and list(diag) == frozen_oc, 'latched, diagnostics frozen since the fault tick')
check(saw_heat_pending and saw_heater_on and saw_at_setpt, 'the run went through HeatPending, HeaterON and TempAtSetPt')

reset(120100)
check(st.value == ST_IDLE_STOPPED and diag[D_ACTIVE] == 1 and wn.value == WN_NONE and diag[D_T1ACC] == 0 and diag[D_HCFLAG] == 0 and math.isnan(diag[D_T1AVG])
      and math.isnan(diag[D_PERM]) and diag[D_STARTED] == 0, 'reset: stopped, sensor 1 restored, history cleared, permissive NaN')
tick(120200, temp + 0.3, temp - 0.2, 0, 0)
check(st.value == ST_IDLE_STOPPED and dh.value == 0, 'CheckTemp after Reset does not resume control')

# relay feedback fault: heater DO stuck off while commanded on (Init leaves the zone stopped; Start is explicit)
set_setup(SETPOINT=80)
init(120200); check(st.value == ST_IDLE_STOPPED, 're-Init: stopped')
start(120200, 1)
for i in range(1, 60):
    ms = 120200 + i * 100
    tick(ms, temp + 0.3, temp - 0.2, 0, 0)
    if i == 6:  check(dh.value == 1 and st.value == ST_HEATER_ON, f'heating commanded at tick 6: st={st.value}')
    if i == 7:  check(wn.value == WN_HEATER_FB and diag[D_HFB_REM] == 1000 and dh.value == 1, f'feedback mismatch warning at once: wn={wn.value} rem={diag[D_HFB_REM]}')
    if i == 16: check(st.value == ST_HEATER_ON and diag[D_HFB_REM] == 100, f'one tick before the feedback fault: st={st.value} rem={diag[D_HFB_REM]}')
    if i == 17: check(st.value == ST_HEATER_FB_FAULT and dh.value == 0 and dc.value == 0 and wn.value == WN_HEATER_FB, f'heater feedback fault after 1000 ms: st={st.value}')
    if i == 17: frozen = list(diag)
    if i == 30: check(st.value == ST_HEATER_FB_FAULT and list(diag) == frozen, 'latched, diagnostics frozen since the fault tick')
stop(126200); check(st.value == ST_HEATER_FB_FAULT, 'Stop keeps a fault')

# single-sensor mode: sensor 1 open -> Temp1FailHigh; Temp2 fields not available on the wire; Stop from active
set_setup(T2_EN=0, SETPOINT=50, FB_EN=0)
init(130000)
check(st.value == ST_IDLE_STOPPED, 'Init after a fault clears it, leaves the zone stopped')
start(130000, 1)
for i in range(1, 25):
    tick(130000 + i * 100, float('nan') if i > 3 else 50.0, 123.0, 0, 0)
    check(math.isnan(diag[D_T2RAW]) and math.isnan(diag[D_T2CORR]) and math.isnan(diag[D_T2AVG]), 'Temp2 fields NaN when disabled')
    if i == 22: check(st.value == ST_AT_SETPT, 'still a state one tick before')
    if i == 23: check(st.value == ST_T1_FAIL_HIGH and wn.value == WN_T1_OOR, f'single sensor NaN stream -> Temp1FailHigh after 2000 ms: st={st.value}')
reset(133000); start(133000, 1)
for i in range(1, 8): tick(133000 + i * 100, 30.0, 30.0, dh.value, dc.value)
check(st.value == ST_HEATER_ON and dh.value == 1, f'heating again after Reset + Start: st={st.value}')
stop(133800)
check(st.value == ST_IDLE_STOPPED and diag[D_DOHEAT] == 0 and diag[D_STARTED] == 0 and diag[D_PERM] == 1, 'Stop from HeaterON: relays 0, IdleStopped, permissive unchanged')

# controller-side saturation: a 100 s operating-condition timeout is 100000 in TcGetDiag and 64255 on the wire (13.8.2)
set_setup(OC_TO=100000)
init(140000); start(140000, 1)
for i in range(1, 8): tick(140000 + i * 100, 30.0, 30.0, dh.value, dc.value)
check(dh.value == 1, 'heating before the saturation test')
tick(140800, 30.0, 30.0, 1, 0, perm=0)
check(st.value == ST_OC_PENDING and diag[D_OC_REM] == 100000, f'TcGetDiag keeps the full 100000 ms: {diag[D_OC_REM]}')
payload = pack_and_compare(diag)
check(raw_of(payload, msg.signals[D_OC_REM]) == 0xFAFF, 'OperatingConditionRemainMs saturates to 0xFAFF on the wire')

print(f'tempctl {tc.TcVersion():#x}, cantp {cp.CanTp_Version():#x}, cantools {cantools.__version__}: '
      f'{arrays_checked} diagnostics arrays packed and compared bit-for-bit ({boundary_arrays} boundary vectors), '
      f'{PAYLOAD_LEN}-byte BAM of {OUT_SIZE // 24} frames, tables and ECD define identical slots')
print('ALL OK' if fails == 0 else f'{fails} FAILURE(S)')
sys.exit(1 if fails else 0)
