"""
oracle_test.py - TempCtl v3 end-to-end cross-check with independent implementations.

  * tempctl.dll   TcInit / TcCheckTemp / TcReset / TcGetDiag through ctypes: a
                  scripted scenario (heat-up, at-setpoint release, sensor 1 open
                  with failover to sensor 2, reset, relay feedback fault, Temp2
                  disabled) checked against expectations from TEMPCTL-SPEC-v3.0.0.
  * cantp.dll     the vendored CanTp packs every tick's 25-value diagnostics
                  array (TcGetDiag) with the tables from dbc/tables (generated
                  from dbc/tempctl.dbc).
  * cantools      encodes the same physical values with dbc/tempctl.dbc; the
                  payload must match CanTp's reassembled BAM bit for bit, and
                  cantools' decode of CanTp's payload must equal CanTp_Unpack's
                  values.

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
I32P = C.POINTER(C.c_int32)
tc.TcVersion.restype = C.c_int32
tc.TcSetupCount.restype = C.c_int32
tc.TcDiagCount.restype = C.c_int32
tc.TcInit.argtypes = [C.c_int32, C.c_uint32, C.POINTER(C.c_double), C.c_int32, I32P, I32P]
tc.TcCheckTemp.argtypes = [C.c_int32, C.c_uint32, C.c_double, C.c_double, C.c_int32, C.c_int32, I32P, I32P, I32P, I32P]
tc.TcReset.argtypes = [C.c_int32, C.c_uint32, I32P, I32P]
tc.TcGetDiag.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32]
for f in (tc.TcInit, tc.TcCheckTemp, tc.TcReset, tc.TcGetDiag): f.restype = C.c_int32

cp = C.CDLL(os.path.abspath(args.cantp))
cp.CanTp_Version.restype = C.c_uint32
cp.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
cp.CanTp_OutputSize.argtypes = [C.c_int32]
cp.CanTp_Pack.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.c_uint64, C.c_uint64, C.POINTER(C.c_uint8), C.c_int32, I32P]
cp.CanTp_Unpack.argtypes = [C.c_int32, C.POINTER(C.c_uint8), C.c_int32, C.POINTER(C.c_double), C.c_int32, I32P]
for f in (cp.CanTp_Define, cp.CanTp_OutputSize, cp.CanTp_Pack, cp.CanTp_Unpack): f.restype = C.c_int32

N_SETUP, N_DIAG = tc.TcSetupCount(), tc.TcDiagCount()
assert (N_SETUP, N_DIAG) == (17, 25), (N_SETUP, N_DIAG)
assert tc.TcVersion() >> 16 == 3, hex(tc.TcVersion())

# setup indexes (TC_SETUP_*)
(ENABLE, UNITS, SETPOINT, DB_HI, DB_LO, HI_LIMIT, LO_LIMIT, ERR_TO, DB_TO, ASP_TO,
 T2_EN, T2_OFF, T2_TOL, CMP_TO, FILTER, FB_EN, FB_TO) = range(17)
# diag indexes (TC_DIAG_*)
(D_CTRL, D_ACTIVE, D_T1RAW, D_T2RAW, D_T2CORR, D_T1AVG, D_T2AVG, D_HIBAND, D_LOBAND, D_HCFLAG,
 D_DB_REM, D_ASP_REM, D_CMP_REM, D_HFB_REM, D_CFB_REM, D_T1ACC, D_T2ACC, D_T1EV, D_T2EV,
 D_STATUS, D_WARNING, D_DOHEAT, D_DOCOOL, D_FILTER, D_INIT) = range(25)
# status / warning codes
ST_DISABLED, ST_AT_SETPT, ST_HEATER_ON, ST_COOLER_ON, ST_HEAT_PENDING, ST_COOL_PENDING = 0, 1, 2, 3, 4, 5
ST_T1_FAIL_HIGH, ST_BOTH_FAILED, ST_HEATER_FB_FAULT = 10, 12, 15
WN_NONE, WN_T1_OOR, WN_HEATER_FB, WN_RUNNING_ON_T2 = 0, 1, 3, 6

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
check(len(sigrows) == N_DIAG, f'table has {len(sigrows)} signals, controller has {N_DIAG} diagnostics')
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
unpacked = (C.c_double * N_DIAG)()
consumed = C.c_int32()
ticks_checked = 0

def pack_and_compare(diag):
    global ticks_checked
    rc = cp.CanTp_Pack(SLOT, diag, N_DIAG, 0, 0, outbuf, OUT_SIZE, C.byref(written))
    check(rc == 0 and written.value == OUT_SIZE, f'Pack rc={rc} written={written.value}')
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
    # decode side: cantools decode of CanTp's bytes == CanTp_Unpack
    rc = cp.CanTp_Unpack(SLOT, outbuf, OUT_SIZE, unpacked, N_DIAG, C.byref(consumed))
    check(rc == 1 and consumed.value == OUT_SIZE, f'Unpack rc={rc}')
    dec = msg.decode(payload, decode_choices=False, scaling=True)
    for s, u in zip(msg.signals, unpacked):
        check(abs(dec[s.name] - u) < 1e-9, f'{s.name}: cantools {dec[s.name]} vs CanTp_Unpack {u}')
    ticks_checked += 1
    return payload

# ---------------------------------------------------------------- scenario
setup = (C.c_double * N_SETUP)()
diag = (C.c_double * N_DIAG)()
st, wn, dh, dc = C.c_int32(), C.c_int32(), C.c_int32(), C.c_int32()
ZONE = 0

def set_setup(**kw):
    for k, v in kw.items(): setup[globals()[k]] = v
set_setup(ENABLE=1, UNITS=1, SETPOINT=50, DB_HI=5, DB_LO=5, HI_LIMIT=90, LO_LIMIT=10, ERR_TO=2000, DB_TO=500, ASP_TO=1000,
          T2_EN=1, T2_OFF=0.5, T2_TOL=4, CMP_TO=5000, FILTER=4, FB_EN=1, FB_TO=1000)

def init(ms):
    rc = tc.TcInit(ZONE, ms, setup, N_SETUP, C.byref(st), C.byref(wn))
    check(rc == 0, f'TcInit rc={rc}')
    get_diag()

def get_diag():
    rc = tc.TcGetDiag(ZONE, diag, N_DIAG)
    check(rc == 0, f'TcGetDiag rc={rc}')
    pack_and_compare(diag)

def tick(ms, t1, t2, hfb, cfb):
    rc = tc.TcCheckTemp(ZONE, ms, t1, t2, hfb, cfb, C.byref(dh), C.byref(dc), C.byref(st), C.byref(wn))
    check(rc == 0, f'TcCheckTemp rc={rc} at {ms}')
    get_diag()
    check(diag[D_STATUS] == st.value and diag[D_WARNING] == wn.value and diag[D_DOHEAT] == dh.value and diag[D_DOCOOL] == dc.value,
          f'diag mirrors differ from the outputs at {ms}')
    return rc

init(0)
check(st.value == ST_AT_SETPT and wn.value == WN_NONE and diag[D_INIT] == 1 and diag[D_ACTIVE] == 1, 'init: idle, sensor 1')
check(math.isnan(diag[D_CTRL]) and math.isnan(diag[D_T1RAW]), 'init: no readings yet')

# toy plant with honest relay feedback (the DO answers the previous command)
temp, dt = 20.0, 0.1
heat = cool = 0
saw_heat_pending = saw_heater_on = saw_at_setpt = False
for i in range(1, 1201):                                      # 120 s
    ms = i * 100
    fail_t1 = 600 <= i < 700                                  # 10 s open sensor 1
    t1 = float('nan') if fail_t1 else temp + 0.3
    t2 = temp - 0.2                                           # + Temp2Offset 0.5 = temp + 0.3, so the two agree
    tick(ms, t1, t2, heat, cool)
    heat, cool = dh.value, dc.value
    temp += dt * (0.02 * (20.0 - temp) + 2.0 * heat - 2.0 * cool)   # 2 deg/s: the 1 s at-setpoint hold overshoots by 2, inside the 5 deg band
    saw_heat_pending |= st.value == ST_HEAT_PENDING
    saw_heater_on |= st.value == ST_HEATER_ON
    saw_at_setpt |= st.value == ST_AT_SETPT and i > 1
    if i == 1:   check(st.value == ST_HEAT_PENDING and dh.value == 0 and diag[D_DB_REM] == 500, f'tick 1: heat pending, got st={st.value} rem={diag[D_DB_REM]}')
    if i == 6:   check(st.value == ST_HEATER_ON and dh.value == 1, f'tick 6: heater on after 500 ms, got st={st.value}')
    if i == 300: check(abs(diag[D_CTRL] - 50) < 6, f'near setpoint at 30 s: {diag[D_CTRL]}')
    if i == 599: check(st.value < 10 and wn.value == WN_NONE and diag[D_HCFLAG] == 1, f'healthy before the open sensor: st={st.value} wn={wn.value}')
    if i == 605: check(st.value < 10 and wn.value == WN_T1_OOR and diag[D_T1ACC] == 600 and math.isnan(diag[D_CTRL]), f'open sensor: warning 1, accumulator 600 after 6 ticks, got wn={wn.value} acc={diag[D_T1ACC]}')
    if i == 618: check(diag[D_ACTIVE] == 1 and st.value < 10 and diag[D_T1ACC] == 1900, 'still on sensor 1 one tick before ErrorTimeout')
    if i == 619: check(diag[D_ACTIVE] == 2 and st.value < 10 and diag[D_T1ACC] == 2000, f'failover to sensor 2 at 2 s: active={diag[D_ACTIVE]} st={st.value}')
    if i == 700: check(wn.value == WN_RUNNING_ON_T2 and diag[D_ACTIVE] == 2, f'sensor 1 back in range: warning 6, still on sensor 2: wn={wn.value}')
    if i == 800: check(diag[D_ACTIVE] == 2 and st.value < 10 and abs(diag[D_CTRL] - (t2 + 0.5)) < 1e-9, 'control on corrected sensor 2, no fault')
check(saw_heat_pending and saw_heater_on and saw_at_setpt, 'the run went through HeatPending, HeaterON and TempAtSetPt')

rc = tc.TcReset(ZONE, 120100, C.byref(st), C.byref(wn)); check(rc == 0, 'TcReset rc')
get_diag()
check(diag[D_ACTIVE] == 1 and wn.value == WN_NONE and diag[D_T1ACC] == 0 and diag[D_HCFLAG] == 0 and math.isnan(diag[D_T1AVG]), 'reset restores sensor 1, clears history')

# relay feedback fault: heater DO stuck off while commanded on
set_setup(SETPOINT=80)
init(120100)                                                  # re-Init: relays were off (Reset), so both start off
for i in range(1, 60):
    ms = 120100 + i * 100
    tick(ms, temp + 0.3, temp - 0.2, 0, 0)
    if i == 6:  check(dh.value == 1 and st.value == ST_HEATER_ON, f'heating commanded at tick 6: st={st.value}')
    if i == 7:  check(wn.value == WN_HEATER_FB and diag[D_HFB_REM] == 1000 and dh.value == 1, f'feedback mismatch warning at once: wn={wn.value} rem={diag[D_HFB_REM]}')
    if i == 16: check(st.value == ST_HEATER_ON and diag[D_HFB_REM] == 100, f'one tick before the feedback fault: st={st.value} rem={diag[D_HFB_REM]}')
    if i == 17: check(st.value == ST_HEATER_FB_FAULT and dh.value == 0 and dc.value == 0 and wn.value == WN_HEATER_FB, f'heater feedback fault after 1000 ms: st={st.value}')
    if i == 17: frozen = list(diag)
    if i == 30: check(st.value == ST_HEATER_FB_FAULT and list(diag) == frozen, 'latched, diagnostics frozen since the fault tick')

# single-sensor mode: sensor 1 open -> Temp1FailHigh; Temp2 fields not available on the wire
set_setup(T2_EN=0, SETPOINT=50, FB_EN=0)
init(130000)
check(st.value == ST_AT_SETPT, 'Init after a fault clears it')
for i in range(1, 25):
    tick(130000 + i * 100, float('nan') if i > 3 else 50.0, 123.0, 0, 0)
    check(math.isnan(diag[D_T2RAW]) and math.isnan(diag[D_T2CORR]) and math.isnan(diag[D_T2AVG]), 'Temp2 fields NaN when disabled')
    if i == 22: check(st.value == ST_AT_SETPT, 'still a state one tick before')
    if i == 23: check(st.value == ST_T1_FAIL_HIGH and wn.value == WN_T1_OOR, f'single sensor NaN stream -> Temp1FailHigh after 2000 ms: st={st.value}')

print(f'tempctl {tc.TcVersion():#x}, cantp {cp.CanTp_Version():#x}, cantools {cantools.__version__}: '
      f'{ticks_checked} diagnostics arrays packed and compared bit-for-bit, {PAYLOAD_LEN}-byte BAM of {OUT_SIZE // 24} frames')
print('ALL OK' if fails == 0 else f'{fails} FAILURE(S)')
sys.exit(1 if fails else 0)
