"""
make_tempctl_dbc.py - write tempctl.dbc, the CAN database for the TempCtl
diagnostics message, and (optionally) the CanTp tables derived from it.

TempCtl v3 does no CAN itself. The host reads the zone's diagnostics with
TcGetDiag (25 doubles: the values control acts on, the countdowns, the
accumulators, the status / warning / relay mirrors) and packs them with
CanTp. This script defines that message: one J1939 parameter group, PGN
65280 (0xFF00, Proprietary B), priority 6, source address placeholder 0xFE
(Vector convention: "any source", CanTp msgDef column 4 supplies the real
SA), sent as a J1939 BAM. The SG_ order is exactly the TcGetDiag array
order (TC_DIAG_* in src/tempctl.h), so the whole diagArray goes into
CanTp_Pack without reordering.

Wire scaling follows J1939 SPN conventions: temperatures 16-bit, 0.03125
deg/bit, -273 offset (valid raw 0..0xFAFF); times U32 ms; flags 2-bit
(0/1, 3 = not available); enums 4/8-bit; counts 16-bit. NaN values (e.g.
Temp2Raw while Temp2 is disabled, ControlTemp during an open sensor) are
packed by CanTp as the J1939 "not available" pattern (all ones).

    python tools\make_tempctl_dbc.py                          -> dbc\tempctl.dbc
    python tools\make_tempctl_dbc.py --tables                 -> + dbc\tables\ via third_party\cantp\tools\dbc2tables.py
    python tools\make_tempctl_dbc.py --check src\tempctl.h    -> verify the order against the header (default on)
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))

MESSAGE   = 'TempCtl'
PGN       = 0xFF00
PRIORITY  = 6
SA        = 0xFE
CAN_ID    = (PRIORITY << 26) | (PGN << 8) | SA          # 0x18FF00FE
DBC_ID    = CAN_ID | 0x80000000                          # DBC extended-frame convention
CYCLE_MS  = 1000

TEMP  = dict(bits=16, factor=0.03125, offset=-273, min=-273, max=1734.96875, unit='deg')
MS32  = dict(bits=32, factor=1, offset=0, min=0, max=4211081215, unit='ms')
FLAG  = dict(bits=2, factor=1, offset=0, min=0, max=1, unit='')
CNT16 = dict(bits=16, factor=1, offset=0, min=0, max=64254, unit='')
ENUM8 = dict(bits=8, factor=1, offset=0, min=0, max=250, unit='')

STATUS_NAMES = {0: 'TempCtrlDisabled', 1: 'TempAtSetPt', 2: 'HeaterON', 3: 'CoolerON', 4: 'HeatPending', 5: 'CoolPending',
                10: 'Temp1FailHigh', 11: 'Temp1FailLow', 12: 'BothSensorsFailed', 13: 'TempDisagreeFault',
                14: 'ConfigFault', 15: 'HeaterFBFault', 16: 'CoolerFBFault'}
WARNING_NAMES = {0: 'NoWarning', 1: 'Temp1OutOfRange', 2: 'Temp2OutOfRange', 3: 'HeaterFBMismatch', 4: 'CoolerFBMismatch',
                 5: 'TempDisagree', 6: 'RunningOnTemp2', 7: 'ConfigInvalid'}
ONOFF = {0: 'Off', 1: 'On'}

# (TC_DIAG_* name in tempctl.h, DBC signal name, layout, comment, value table)
SIGNALS = [
    ('TC_DIAG_CONTROL_TEMP',            'ControlTemp',          TEMP,  'Raw value control acts on (sensor 2 offset-corrected); not available before the first CheckTemp or while the reading is NaN', None),
    ('TC_DIAG_ACTIVE_SENSOR',           'ActiveSensor',         dict(bits=4, factor=1, offset=0, min=1, max=2, unit=''), 'Sensor in control', {1: 'Sensor1', 2: 'Sensor2'}),
    ('TC_DIAG_TEMP1_RAW',               'Temp1Raw',             TEMP,  'Last temp1 as supplied', None),
    ('TC_DIAG_TEMP2_RAW',               'Temp2Raw',             TEMP,  'Last temp2 as supplied (not available when Temp2Enable = 0)', None),
    ('TC_DIAG_TEMP2_CORRECTED',         'Temp2Corrected',       TEMP,  'temp2 + Temp2Offset', None),
    ('TC_DIAG_TEMP1_AVG',               'Temp1Avg',             TEMP,  'Moving average of in-range temp1 samples (comparison only)', None),
    ('TC_DIAG_TEMP2_AVG',               'Temp2Avg',             TEMP,  'Moving average of in-range corrected temp2 samples', None),
    ('TC_DIAG_HI_BAND',                 'HiBand',               TEMP,  'Setpoint + DeadbandHi', None),
    ('TC_DIAG_LO_BAND',                 'LoBand',               TEMP,  'Setpoint - DeadbandLo', None),
    ('TC_DIAG_INITIAL_HC_FLAG',         'InitialHcFlag',        FLAG,  'First heat-up or cool-down complete (gates the sensor comparison)', {0: 'WarmUp', 1: 'Complete'}),
    ('TC_DIAG_DEADBAND_REMAIN_MS',      'DeadbandRemainMs',     MS32,  'ms before a relay engages; 0 when not counting', None),
    ('TC_DIAG_AT_SETPT_REMAIN_MS',      'AtSetPtRemainMs',      MS32,  'ms before the running relay drops; 0 when not counting', None),
    ('TC_DIAG_COMPARE_REMAIN_MS',       'CompareRemainMs',      MS32,  'ms to the disagreement fault; 0 when not counting', None),
    ('TC_DIAG_HEATER_FB_REMAIN_MS',     'HeaterFbRemainMs',     MS32,  'ms to the heater feedback fault; 0 when not counting', None),
    ('TC_DIAG_COOLER_FB_REMAIN_MS',     'CoolerFbRemainMs',     MS32,  'ms to the cooler feedback fault; 0 when not counting', None),
    ('TC_DIAG_TEMP1_OOR_ACCUM_MS',      'Temp1OorAccumMs',      MS32,  'Sensor 1 out-of-range leaky accumulator; the sensor fails at ErrorTimeout', None),
    ('TC_DIAG_TEMP2_OOR_ACCUM_MS',      'Temp2OorAccumMs',      MS32,  'Sensor 2 out-of-range leaky accumulator', None),
    ('TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR', 'Temp1OorEventsPerHour', CNT16, 'Sensor 1 in-range to out-of-range transitions in the last 60 minutes', None),
    ('TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR', 'Temp2OorEventsPerHour', CNT16, 'Sensor 2 in-range to out-of-range transitions in the last 60 minutes', None),
    ('TC_DIAG_STATUS_MIRROR',           'Status',               ENUM8, 'Status code of the last CheckTemp (0..5 states, 10..16 faults)', STATUS_NAMES),
    ('TC_DIAG_WARNING_MIRROR',          'Warning',              ENUM8, 'Warning code of the last CheckTemp (lowest active code)', WARNING_NAMES),
    ('TC_DIAG_DO_HEATER_MIRROR',        'DoHeater',             FLAG,  'Last heater relay command', ONOFF),
    ('TC_DIAG_DO_COOLER_MIRROR',        'DoCooler',             FLAG,  'Last cooler relay command', ONOFF),
    ('TC_DIAG_APPLIED_FILTER_POINTS',   'AppliedFilterPoints',  dict(bits=8, factor=1, offset=0, min=1, max=64, unit=''), 'FilterPoints in use (1..64; an invalid setup value becomes 4)', None),
    ('TC_DIAG_ZONE_INITIALIZED',        'ZoneInitialized',      FLAG,  '1 once a setup has been loaded', {0: 'NoSetup', 1: 'Loaded'}),
]

def layout():
    """Assign Intel start bits sequentially; signals of 8 bits or more start on a byte boundary."""
    rows, bit = [], 0
    for name_h, name, ly, comment, vals in SIGNALS:
        if ly['bits'] >= 8 and bit % 8:
            bit += 8 - bit % 8
        rows.append(dict(define=name_h, name=name, start=bit, comment=comment, vals=vals, **ly))
        bit += ly['bits']
    length = (bit + 7) // 8
    return rows, length

def check_header(header_path):
    """The DBC order must be the TcGetDiag order: TC_DIAG_* 0..TC_DIAG_COUNT-1 in tempctl.h."""
    text = open(header_path).read()
    idx = {m.group(1): int(m.group(2)) for m in re.finditer(r'#define\s+(TC_DIAG_[A-Z0-9_]+)\s+(\d+)', text)}
    count = idx.pop('TC_DIAG_COUNT')
    names = [s[0] for s in SIGNALS]
    problems = []
    if len(names) != count: problems.append(f'DBC has {len(names)} signals, header TC_DIAG_COUNT is {count}')
    for i, n in enumerate(names):
        if n not in idx: problems.append(f'{n} not in tempctl.h')
        elif idx[n] != i: problems.append(f'{n} is index {idx[n]} in tempctl.h but position {i} in the DBC')
    for n, i in idx.items():
        if n not in names: problems.append(f'{n} (index {i}) in tempctl.h but not in the DBC')
    return problems

def fmt(x):
    return repr(float(x)) if isinstance(x, float) and not float(x).is_integer() else str(int(x))

def write_dbc(path, rows, length):
    L = []
    L.append('VERSION "TempCtl v3 diagnostics - generated by tools/make_tempctl_dbc.py"')
    L.append('')
    L.append('NS_ :')
    for kw in ['NS_DESC_', 'CM_', 'BA_DEF_', 'BA_', 'VAL_', 'CAT_DEF_', 'CAT_', 'FILTER', 'BA_DEF_DEF_', 'EV_DATA_', 'ENVVAR_DATA_',
               'SGTYPE_', 'SGTYPE_VAL_', 'BA_DEF_SGTYPE_', 'BA_SGTYPE_', 'SIG_TYPE_REF_', 'VAL_TABLE_', 'SIG_GROUP_', 'SIG_VALTYPE_',
               'SIGTYPE_VALTYPE_', 'BO_TX_BU_', 'BA_DEF_REL_', 'BA_REL_', 'BA_DEF_DEF_REL_', 'BU_SG_REL_', 'BU_EV_REL_', 'BU_BO_REL_', 'SG_MUL_VAL_']:
        L.append(f'\t{kw}')
    L.append('')
    L.append('BS_:')
    L.append('')
    L.append('BU_: TempCtl')
    L.append('')
    L.append(f'BO_ {DBC_ID} {MESSAGE}: {length} TempCtl')
    for r in rows:
        L.append(f' SG_ {r["name"]} : {r["start"]}|{r["bits"]}@1+ ({fmt(r["factor"])},{fmt(r["offset"])}) [{fmt(r["min"])}|{fmt(r["max"])}] "{r["unit"]}" Vector__XXX')
    L.append('')
    L.append('')
    L.append(f'CM_ BO_ {DBC_ID} "TempCtl v3 zone diagnostics (TcGetDiag array), PGN 65280 Proprietary B, J1939 BAM";')
    for r in rows:
        L.append(f'CM_ SG_ {DBC_ID} {r["name"]} "{r["comment"]}";')
    L.append('BA_DEF_ BO_  "VFrameFormat" ENUM  "StandardCAN","ExtendedCAN","reserved","J1939PG";')
    L.append('BA_DEF_ BO_  "GenMsgCycleTime" INT 0 3600000;')
    L.append('BA_DEF_ BO_  "GenMsgSendType" ENUM  "Cyclic","NotUsed","NotUsed","NotUsed","NotUsed","NotUsed","NotUsed","IfActive","NoMsgSendType";')
    L.append('BA_DEF_  "ProtocolType" STRING ;')
    L.append('BA_DEF_DEF_  "VFrameFormat" "J1939PG";')
    L.append('BA_DEF_DEF_  "GenMsgCycleTime" 0;')
    L.append('BA_DEF_DEF_  "GenMsgSendType" "NoMsgSendType";')
    L.append('BA_DEF_DEF_  "ProtocolType" "J1939";')
    L.append('BA_ "ProtocolType" "J1939";')
    L.append(f'BA_ "VFrameFormat" BO_ {DBC_ID} 3;')
    L.append(f'BA_ "GenMsgCycleTime" BO_ {DBC_ID} {CYCLE_MS};')
    L.append(f'BA_ "GenMsgSendType" BO_ {DBC_ID} 0;')
    for r in rows:
        if r['vals']:
            L.append(f'VAL_ {DBC_ID} {r["name"]} ' + ' '.join(f'{k} "{v}"' for k, v in sorted(r['vals'].items(), reverse=True)) + ' ;')
    L.append('')
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', newline='\r\n') as f:
        f.write('\n'.join(L))

def verify_with_cantools(path, rows, length):
    import cantools
    db = cantools.database.load_file(path, strict=True)
    m = db.get_message_by_name(MESSAGE)
    assert m.frame_id == DBC_ID & 0x1FFFFFFF, hex(m.frame_id)
    assert m.is_extended_frame and m.length == length
    assert [s.name for s in m.signals] == [r['name'] for r in rows], 'cantools changed the signal order'
    assert m.dbc.attributes['VFrameFormat'].value in (3, 'J1939PG')
    # bit-level overlap check
    used = [None] * (length * 8)
    for s in m.signals:
        for b in range(s.start, s.start + s.length):
            assert used[b] is None, f'{s.name} overlaps {used[b]} at bit {b}'
            used[b] = s.name
    return m

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(ROOT, 'dbc', 'tempctl.dbc'))
    ap.add_argument('--check', default=os.path.join(ROOT, 'src', 'tempctl.h'), help='tempctl.h to verify the order against ("" to skip)')
    ap.add_argument('--tables', action='store_true', help='also run CanTp dbc2tables.py into <out dir>/tables')
    ap.add_argument('--dbc2tables', default=os.path.join(ROOT, 'third_party', 'cantp', 'tools', 'dbc2tables.py'))
    a = ap.parse_args()

    rows, length = layout()
    if a.check:
        problems = check_header(a.check)
        if problems:
            for p in problems: print('ERROR:', p)
            sys.exit(1)
    write_dbc(a.out, rows, length)
    m = verify_with_cantools(a.out, rows, length)
    frames = 1 if length <= 8 else 1 + (length + 6) // 7
    print(f'{a.out}: {MESSAGE} id {m.frame_id:#x} (PGN {PGN} SA {SA:#x}) {length} bytes, {len(rows)} signals, BAM = {frames} frames')
    for r in rows:
        print(f'  {r["start"]:4d}|{r["bits"]:<2d} {r["name"]:<22s} x{fmt(r["factor"])} {fmt(r["offset"]):>5s} [{fmt(r["min"])}|{fmt(r["max"])}] {r["unit"]}')
    if a.tables:
        outdir = os.path.join(os.path.dirname(os.path.abspath(a.out)), 'tables')
        rc = subprocess.call([sys.executable, a.dbc2tables, a.out, '--message', MESSAGE, '--out', outdir])
        if rc: sys.exit(rc)
        print(f'tables written to {outdir}')

if __name__ == '__main__':
    main()
