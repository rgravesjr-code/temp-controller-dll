"""
make_tempctl_dbc.py - write tempctl.dbc, the CAN database for the TempCtl
diagnostics message, and (optionally) the CanTp tables and the tempctl.ecd
database derived from the same signal model.

TempCtl v4 does no CAN itself. The host reads the zone's diagnostics with
TcGetDiag (28 doubles: the values control acts on, the countdowns, the
accumulators, the status / warning / relay mirrors, the lifecycle fields)
and packs them with CanTp. This script defines that message: one J1939
parameter group, PGN 65280 (0xFF00, Proprietary B), priority 6, source
address placeholder 0xFE (Vector convention: "any source", CanTp msgDef
column 4 supplies the real SA), sent as a J1939 BAM. The SG_ order is
exactly the TcGetDiag array order (TC_DIAG_* in src/tempctl.h), so the whole
diagArray goes into CanTp_Pack without reordering; the ECD channel order is
the same, so CanTp_DefineFlat on the ECD cluster gives the same slot.

Wire scaling follows J1939 SPN conventions: temperatures 16-bit, 0.03125
deg/bit, -273 offset (valid raw 0..0xFAFF); every millisecond diagnostic
U16, 1 ms/bit, valid 0..64255 (0xFAFF), 0xFFFF = not available, values above
64255 saturate ON THE WIRE ONLY (the controller's timers and TcGetDiag stay
full width); flags 2-bit (0/1, 3 = not available); enums 8-bit; counts
16-bit. NaN values (Temp2 fields while Temp2 is disabled, ControlTemp during
an open sensor, RunPermissive before the first evaluation) are packed by
CanTp as the J1939 "not available" pattern (all ones).

The v4 layout (44 bytes) is NOT wire-compatible with the v3 layout (55
bytes): the U16 timers move every later signal. A v3 DBC / ECD / table must
not be used to decode a v4 payload.

    python tools\make_tempctl_dbc.py                          -> dbc\tempctl.dbc
    python tools\make_tempctl_dbc.py --tables                 -> + dbc\tables\ via third_party\cantp\tools\dbc2tables.py
    python tools\make_tempctl_dbc.py --ecd                    -> + dbc\tempctl.ecd via third_party\cantp\tools\ecdflat.py (read back and compared)
    python tools\make_tempctl_dbc.py --check src\tempctl.h    -> verify the order against the header (default on)
"""
import argparse, json, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
CANTP_TOOLS = os.path.join(ROOT, 'third_party', 'cantp', 'tools')

MESSAGE   = 'TempCtl'
PGN       = 0xFF00
PRIORITY  = 6
SA        = 0xFE
CAN_ID    = (PRIORITY << 26) | (PGN << 8) | SA          # 0x18FF00FE
DBC_ID    = CAN_ID | 0x80000000                          # DBC extended-frame convention
CYCLE_MS  = 1000
EXPECTED_PAYLOAD = 44                                    # handoff 6.1: computed below and asserted

TEMP  = dict(bits=16, factor=0.03125, offset=-273, min=-273, max=1734.96875, unit='deg')
MS16  = dict(bits=16, factor=1, offset=0, min=0, max=64255, unit='ms')   # 0xFAFF; 0xFFFF = not available
FLAG  = dict(bits=2, factor=1, offset=0, min=0, max=1, unit='')
CNT16 = dict(bits=16, factor=1, offset=0, min=0, max=64254, unit='')
ENUM8 = dict(bits=8, factor=1, offset=0, min=0, max=250, unit='')

STATUS_NAMES = {0: 'TempCtrlDisabled', 1: 'TempAtSetPt', 2: 'HeaterON', 3: 'CoolerON', 4: 'HeatPending', 5: 'CoolPending',
                6: 'IdleStopped', 7: 'IdleStartBlocked', 8: 'OperatingConditionPending', 9: 'IdleOperatingConditionTripped',
                10: 'Temp1FailHigh', 11: 'Temp1FailLow', 12: 'BothSensorsFailed', 13: 'TempDisagreeFault',
                14: 'ConfigFault', 15: 'HeaterFBFault', 16: 'CoolerFBFault', 17: 'OperatingConditionFault'}
WARNING_NAMES = {0: 'NoWarning', 1: 'Temp1OutOfRange', 2: 'Temp2OutOfRange', 3: 'HeaterFBMismatch', 4: 'CoolerFBMismatch',
                 5: 'TempDisagree', 6: 'RunningOnTemp2', 7: 'ConfigInvalid', 8: 'OperatingConditionNotMet'}
ONOFF = {0: 'Off', 1: 'On'}

# (TC_DIAG_* name in tempctl.h, DBC signal name, layout, comment, value table)
SIGNALS = [
    ('TC_DIAG_CONTROL_TEMP',            'ControlTemp',          TEMP,  'Raw value control acts on (sensor 2 offset-corrected); not available before the first active CheckTemp or while the reading is NaN', None),
    ('TC_DIAG_ACTIVE_SENSOR',           'ActiveSensor',         dict(bits=4, factor=1, offset=0, min=1, max=2, unit=''), 'Sensor in control', {1: 'Sensor1', 2: 'Sensor2'}),
    ('TC_DIAG_TEMP1_RAW',               'Temp1Raw',             TEMP,  'Last temp1 as supplied (mirrored while stopped as well)', None),
    ('TC_DIAG_TEMP2_RAW',               'Temp2Raw',             TEMP,  'Last temp2 as supplied (not available when Temp2Enable = 0)', None),
    ('TC_DIAG_TEMP2_CORRECTED',         'Temp2Corrected',       TEMP,  'temp2 + Temp2Offset', None),
    ('TC_DIAG_TEMP1_AVG',               'Temp1Avg',             TEMP,  'Moving average of in-range temp1 samples (comparison only)', None),
    ('TC_DIAG_TEMP2_AVG',               'Temp2Avg',             TEMP,  'Moving average of in-range corrected temp2 samples', None),
    ('TC_DIAG_HI_BAND',                 'HiBand',               TEMP,  'Setpoint + DeadbandHi', None),
    ('TC_DIAG_LO_BAND',                 'LoBand',               TEMP,  'Setpoint - DeadbandLo', None),
    ('TC_DIAG_INITIAL_HC_FLAG',         'InitialHcFlag',        FLAG,  'First heat-up or cool-down complete (gates the sensor comparison)', {0: 'WarmUp', 1: 'Complete'}),
    ('TC_DIAG_DEADBAND_REMAIN_MS',      'DeadbandRemainMs',     MS16,  'ms before a relay engages; 0 when not counting; 64255 = 64255 ms or more', None),
    ('TC_DIAG_AT_SETPT_REMAIN_MS',      'AtSetPtRemainMs',      MS16,  'ms before the running relay drops; 0 when not counting; saturates at 64255', None),
    ('TC_DIAG_COMPARE_REMAIN_MS',       'CompareRemainMs',      MS16,  'ms to the disagreement fault; 0 when not counting; saturates at 64255', None),
    ('TC_DIAG_HEATER_FB_REMAIN_MS',     'HeaterFbRemainMs',     MS16,  'ms to the heater feedback fault; 0 when not counting; saturates at 64255', None),
    ('TC_DIAG_COOLER_FB_REMAIN_MS',     'CoolerFbRemainMs',     MS16,  'ms to the cooler feedback fault; 0 when not counting; saturates at 64255', None),
    ('TC_DIAG_TEMP1_OOR_ACCUM_MS',      'Temp1OorAccumMs',      MS16,  'Sensor 1 out-of-range leaky accumulator; the sensor fails at ErrorTimeout; saturates at 64255', None),
    ('TC_DIAG_TEMP2_OOR_ACCUM_MS',      'Temp2OorAccumMs',      MS16,  'Sensor 2 out-of-range leaky accumulator; saturates at 64255', None),
    ('TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR', 'Temp1OorEventsPerHour', CNT16, 'Sensor 1 in-range to out-of-range transitions in the last 60 minutes', None),
    ('TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR', 'Temp2OorEventsPerHour', CNT16, 'Sensor 2 in-range to out-of-range transitions in the last 60 minutes', None),
    ('TC_DIAG_STATUS_MIRROR',           'Status',               ENUM8, 'Status code of the last Init/Start/Stop/CheckTemp/Reset (0..9 states, 10..17 faults)', STATUS_NAMES),
    ('TC_DIAG_WARNING_MIRROR',          'Warning',              ENUM8, 'Warning code of the last Init/Start/Stop/CheckTemp/Reset (lowest active code)', WARNING_NAMES),
    ('TC_DIAG_DO_HEATER_MIRROR',        'DoHeater',             FLAG,  'Internal heater relay command after the last call', ONOFF),
    ('TC_DIAG_DO_COOLER_MIRROR',        'DoCooler',             FLAG,  'Internal cooler relay command after the last call', ONOFF),
    ('TC_DIAG_APPLIED_FILTER_POINTS',   'AppliedFilterPoints',  dict(bits=8, factor=1, offset=0, min=1, max=64, unit=''), 'FilterPoints in use (1..64; an invalid setup value becomes 4)', None),
    ('TC_DIAG_ZONE_INITIALIZED',        'ZoneInitialized',      FLAG,  '1 once a setup has been loaded', {0: 'NoSetup', 1: 'Loaded'}),
    ('TC_DIAG_RUN_PERMISSIVE',          'RunPermissive',        FLAG,  'Last run permissive evaluated by an enabled, non-faulted Start or CheckTemp; not available after Init/Reset until then', {0: 'False', 1: 'True'}),
    ('TC_DIAG_OPERATING_CONDITION_REMAIN_MS', 'OperatingConditionRemainMs', MS16, 'ms before a pending permissive loss becomes OperatingConditionFault; 0 when not pending; saturates at 64255', None),
    ('TC_DIAG_CONTROLLER_STARTED',      'ControllerStarted',    FLAG,  '1 while a Start is accepted and control may run; 0 when disabled, stopped, blocked, pending, tripped or faulted', {0: 'Stopped', 1: 'Started'}),
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
    # the value tables must name every status / warning code the header defines
    st = {int(m.group(2)) for m in re.finditer(r'#define\s+TC_ST_(?!FAULT_FIRST)([A-Z0-9_]+)\s+(\d+)', text)}
    wn = {int(m.group(2)) for m in re.finditer(r'#define\s+TC_WN_([A-Z0-9_]+)\s+(\d+)', text)}
    if st != set(STATUS_NAMES): problems.append(f'status codes differ: header {sorted(st)} vs DBC {sorted(STATUS_NAMES)}')
    if wn != set(WARNING_NAMES): problems.append(f'warning codes differ: header {sorted(wn)} vs DBC {sorted(WARNING_NAMES)}')
    return problems

def fmt(x):
    return repr(float(x)) if isinstance(x, float) and not float(x).is_integer() else str(int(x))

def write_dbc(path, rows, length):
    L = []
    L.append('VERSION "TempCtl v4 diagnostics - generated by tools/make_tempctl_dbc.py"')
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
    L.append(f'CM_ BO_ {DBC_ID} "TempCtl v4 zone diagnostics (TcGetDiag array, 28 values), PGN 65280 Proprietary B, J1939 BAM; not wire-compatible with the v3 (55-byte) layout";')
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
    # every millisecond diagnostic is U16, factor 1, offset 0, 0..64255 (handoff 13.8.1)
    for s in m.signals:
        if s.unit == 'ms':
            assert (s.length, float(s.scale), float(s.offset), float(s.minimum), float(s.maximum)) == (16, 1.0, 0.0, 0.0, 64255.0), s.name
    return m

# ---------------------------------------------------------------- ECD (Eaton .ecd = encrypted LabVIEW flatten of J1939Msg(V4)[])
def ecdflat():
    sys.path.insert(0, CANTP_TOOLS)
    import ecdflat as ef
    return ef

def ecd_message(rows, length):
    """The J1939Msg(V4) cluster for the message, channels in TC_DIAG_* order (Intel, unsigned)."""
    chans = []
    for r in rows:
        lut = [{'value': float(k), 'short': v, 'alphaNum': v, 'desc': v} for k, v in sorted(r['vals'].items())] if r['vals'] else []
        chans.append({'name': r['name'], 'startBit': r['start'], 'nBits': r['bits'], 'dataType': 1, 'byteOrder': 0,
                      'sf': float(r['factor']), 'offset': float(r['offset']), 'min': float(r['min']), 'max': float(r['max']),
                      'default': 0.0, 'unit': r['unit'], 'lookup': 1 if lut else 0, 'lut': lut, 'desc': r['comment']})
    return {'name': MESSAGE, 'msgId': CAN_ID, 'pgn': PGN, 'extended': 1, 'numBytes': length,
            'desc': 'TempCtl v4 zone diagnostics (TcGetDiag array), generated by tools/make_tempctl_dbc.py',
            'updateRate': float(CYCLE_MS), 'tolerance': 0.0, 'channels': chans, 'eatonIpy': 0}

def ecd_encrypt(plain, off=3):
    """Inverse of ecdflat.ecd_decrypt: subtract the offset, swap byte pairs, prepend the offset digit, reverse, prefix the length."""
    body = bytearray((b - off) & 0xFF for b in plain)
    for i in range(0, len(body) - 1, 2): body[i], body[i + 1] = body[i + 1], body[i]
    enc = (bytes([48 + off]) + bytes(body))[::-1]
    return struct.pack('>i', len(enc)) + enc

def write_ecd(path, rows, length):
    ef = ecdflat()
    m = ecd_message(rows, length)
    plain = struct.pack('>i', 1) + ef.flatten_message(m)
    data = ecd_encrypt(plain)
    assert ef.ecd_decrypt(data) == plain, 'ecd_encrypt is not the inverse of ecd_decrypt'
    with open(path, 'wb') as f: f.write(data)
    return m

def verify_ecd(path, rows, length, tables_json=None):
    """Read the .ecd back with CanTp's reader and compare every channel with the DBC rows (handoff 6.1, 13.8.4);
    with the dbc2tables JSON given, also check that CanTp_DefineFlat's derived rows equal the table rows (13.8.7)."""
    ef = ecdflat()
    msgs = ef.ecd_messages(path)
    assert len(msgs) == 1, f'{len(msgs)} messages in the ECD'
    m, flat = msgs[0]
    assert ef.flatten_message(m) == flat, 'ECD cluster does not round-trip'
    assert (m['name'], m['msgId'], m['pgn'], m['extended'], m['numBytes']) == (MESSAGE, CAN_ID, PGN, 1, length), 'ECD message header'
    assert len(m['channels']) == len(rows), f'ECD has {len(m["channels"])} channels, DBC {len(rows)}'
    for c, r in zip(m['channels'], rows):
        got = (c['name'], c['startBit'], c['nBits'], c['dataType'], c['byteOrder'], c['sf'], c['offset'], c['min'], c['max'], c['unit'])
        want = (r['name'], r['start'], r['bits'], 1, 0, float(r['factor']), float(r['offset']), float(r['min']), float(r['max']), r['unit'])
        assert got == want, f'ECD channel {c["name"]} differs from the DBC: {got} vs {want}'
        lut = {int(e['value']): e['short'] for e in c['lut']}
        assert lut == (r['vals'] or {}), f'ECD lookup table of {c["name"]} differs from the DBC value table'
    if tables_json:
        t = json.load(open(tables_json))
        msgdef, sigdefs = ef.to_tables(m)
        assert msgdef == t['msgdef'], f'ECD-derived msgdef {msgdef} != table {t["msgdef"]}'
        assert sigdefs == t['sigdefs'], 'ECD-derived signal rows differ from the dbc2tables rows (channel order or scaling)'
        assert [c['name'] for c in m['channels']] == t['signals'], 'ECD channel order differs from the table order'
    return len(flat)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(ROOT, 'dbc', 'tempctl.dbc'))
    ap.add_argument('--check', default=os.path.join(ROOT, 'src', 'tempctl.h'), help='tempctl.h to verify the order against ("" to skip)')
    ap.add_argument('--tables', action='store_true', help='also run CanTp dbc2tables.py into <out dir>/tables')
    ap.add_argument('--ecd', action='store_true', help='also write <out dir>/tempctl.ecd and read it back for comparison')
    ap.add_argument('--dbc2tables', default=os.path.join(CANTP_TOOLS, 'dbc2tables.py'))
    a = ap.parse_args()

    rows, length = layout()
    assert length == EXPECTED_PAYLOAD, f'payload is {length} bytes, the v4 contract says {EXPECTED_PAYLOAD}'
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
        print(f'  {r["start"]:4d}|{r["bits"]:<2d} {r["name"]:<28s} x{fmt(r["factor"])} {fmt(r["offset"]):>5s} [{fmt(r["min"])}|{fmt(r["max"])}] {r["unit"]}')
    outdir = os.path.join(os.path.dirname(os.path.abspath(a.out)), 'tables')
    if a.tables:
        rc = subprocess.call([sys.executable, a.dbc2tables, a.out, '--message', MESSAGE, '--out', outdir])
        if rc: sys.exit(rc)
        print(f'tables written to {outdir}')
    if a.ecd:
        ecd_path = os.path.join(os.path.dirname(os.path.abspath(a.out)), 'tempctl.ecd')
        write_ecd(ecd_path, rows, length)
        tj = os.path.join(outdir, f'{MESSAGE}.json')
        n = verify_ecd(ecd_path, rows, length, tj if os.path.exists(tj) else None)
        print(f'{ecd_path}: 1 message, {len(rows)} channels in TC_DIAG order, {n}-byte flattened cluster; read back and compared with the DBC'
              + (' and the tables' if os.path.exists(tj) else ''))

if __name__ == '__main__':
    main()
