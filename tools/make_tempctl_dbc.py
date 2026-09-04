"""
make_tempctl_dbc.py - write tempctl.dbc, the CAN database for the TempCtl
controller message, and (optionally) the CanTp tables derived from it.

One J1939 parameter group, PGN 65280 (0xFF00, Proprietary B), priority 6,
source address placeholder 0xFE (Vector convention: "any source", CanTp
msgDef column 4 supplies the real SA), sent as a J1939 BAM. The SG_ order is
exactly the TempCtl output array order (enum TcSignal in src/tempctl.h), so
the whole `out` array of TcStep goes into CanTp_PackSgl without reordering.

Wire scaling follows J1939 SPN conventions: temperatures 16-bit, 0.03125
deg/bit, -273 offset (valid raw 0..0xFAFF); times U32 ms; flags 2-bit
(0/1, 3 = not available); enums 4/8-bit; the error bit mask 16-bit. NaN
outputs (e.g. Temp2Filtered while Temp2 is disabled) are packed by CanTp as
the J1939 "not available" pattern (all ones).

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

TEMP = dict(bits=16, factor=0.03125, offset=-273, min=-273, max=1734.96875, unit='degC')
DELTA = dict(bits=16, factor=0.03125, offset=0, min=0, max=2007.96875, unit='degC')
MS32 = dict(bits=32, factor=1, offset=0, min=0, max=4211081215, unit='ms')
FLAG = dict(bits=2, factor=1, offset=0, min=0, max=1, unit='')

# (TcSignal enumerator, DBC signal name, layout, comment, value table)
SIGNALS = [
    ('TC_SETPOINT',         'Setpoint',          TEMP,  'Setpoint. Heating runs until >= Setpoint, cooling until <= Setpoint', None),
    ('TC_DEADBAND_HI',      'DeadbandHi',        DELTA, 'Offset above Setpoint: HiBand = Setpoint + DeadbandHi', None),
    ('TC_DEADBAND_LO',      'DeadbandLo',        DELTA, 'Offset below Setpoint: LoBand = Setpoint - DeadbandLo', None),
    ('TC_HI_LIMIT',         'HiLimit',           TEMP,  'Sensor above this for ErrorTimeout has failed (hi)', None),
    ('TC_LO_LIMIT',         'LoLimit',           TEMP,  'Sensor below this for ErrorTimeout has failed (lo)', None),
    ('TC_ERROR_TIMEOUT',    'ErrorTimeoutMs',    MS32,  'Limit / bad reading / disagreement / feedback countdown', None),
    ('TC_DEADBAND_TIMEOUT', 'DeadbandTimeoutMs', MS32,  'Time outside the band before a relay engages', None),
    ('TC_FILTER_POINTS',    'FilterPoints',      dict(bits=8, factor=1, offset=0, min=1, max=64, unit=''), 'Moving-average length per sensor', None),
    ('TC_TEMP2_ENABLE',     'Temp2Enable',       FLAG,  'Second sensor present: rationality check and failover', {0: 'Disabled', 1: 'Enabled'}),
    ('TC_TEMP2_TOLERANCE',  'Temp2Tolerance',    DELTA, '|Temp1Filtered - Temp2Filtered| above this for ErrorTimeout -> disagree', None),
    ('TC_FEEDBACK_ENABLE',  'FeedbackEnable',    FLAG,  'Compare relay feedback inputs with the commands', {0: 'Disabled', 1: 'Enabled'}),
    ('TC_TEMP1',            'Temp1',             TEMP,  'Sensor 1 raw reading (primary)', None),
    ('TC_TEMP2',            'Temp2',             TEMP,  'Sensor 2 raw reading', None),
    ('TC_HEATER_FEEDBACK',  'HeaterFeedback',    FLAG,  'Measured heater relay state', {0: 'Off', 1: 'On'}),
    ('TC_COOLER_FEEDBACK',  'CoolerFeedback',    FLAG,  'Measured cooler relay state', {0: 'Off', 1: 'On'}),
    ('TC_HEATING_CMD',      'HeatingCmd',        FLAG,  'Heating relay command', {0: 'Off', 1: 'On'}),
    ('TC_COOLING_CMD',      'CoolingCmd',        FLAG,  'Cooling relay command', {0: 'Off', 1: 'On'}),
    ('TC_ERROR_STATUS',     'ErrorStatus',       dict(bits=16, factor=1, offset=0, min=0, max=1023, unit=''),
        'Bit mask: b0 T1 hi, b1 T1 lo, b2 T1 bad, b3 T2 hi, b4 T2 lo, b5 T2 bad, b6 disagree, b7 heater feedback, b8 cooler feedback, b9 config', None),
    ('TC_TEMP_STATUS',      'TempStatus',        dict(bits=4, factor=1, offset=0, min=0, max=8, unit=''), 'Controller state',
        {0: 'InBand', 1: 'HeatPending', 2: 'Heating', 3: 'CoolPending', 4: 'Cooling', 5: 'ErrorPending', 6: 'Stopped', 7: 'Degraded', 8: 'FilterWarmup'}),
    ('TC_CONTROL_TEMP',     'ControlTemp',       TEMP,  'Filtered value of the active sensor that drives control', None),
    ('TC_TEMP1_FILTERED',   'Temp1Filtered',     TEMP,  'Moving average of Temp1', None),
    ('TC_TEMP2_FILTERED',   'Temp2Filtered',     TEMP,  'Moving average of Temp2 (not available when Temp2 is disabled)', None),
    ('TC_HI_BAND',          'HiBand',            TEMP,  'Setpoint + DeadbandHi', None),
    ('TC_LO_BAND',          'LoBand',            TEMP,  'Setpoint - DeadbandLo', None),
    ('TC_ERROR_REMAIN_MS',  'ErrorRemainMs',     MS32,  'Smallest running error countdown, 0 when none', None),
    ('TC_DB_REMAIN_MS',     'DbRemainMs',        MS32,  'Remaining deadband countdown, 0 when idle or running', None),
    ('TC_ACTIVE_SENSOR',    'ActiveSensor',      dict(bits=2, factor=1, offset=0, min=1, max=2, unit=''), 'Sensor whose filtered value is ControlTemp', {1: 'Sensor1', 2: 'Sensor2'}),
]

def layout():
    """Assign Intel start bits sequentially; signals of 8 bits or more start on a byte boundary."""
    rows, bit = [], 0
    for enum, name, ly, comment, vals in SIGNALS:
        if ly['bits'] >= 8 and bit % 8:
            bit += 8 - bit % 8
        rows.append(dict(enum=enum, name=name, start=bit, comment=comment, vals=vals, **ly))
        bit += ly['bits']
    length = (bit + 7) // 8
    return rows, length

def check_header(header_path):
    """The DBC order must be the TcSignal order: TC_INPUT_COUNT inputs + outputs, 0..TC_SIGNAL_COUNT-1."""
    text = open(header_path).read()
    enum = re.search(r'enum TcSignal \{(.*?)\};', text, re.S).group(1)
    idx = {m.group(1): int(m.group(2)) for m in re.finditer(r'(TC_[A-Z0-9_]+)\s*=\s*(\d+)', enum)}
    count = idx['TC_SIGNAL_COUNT']
    names = [s[0] for s in SIGNALS]
    problems = []
    if len(names) != count: problems.append(f'DBC has {len(names)} signals, header TC_SIGNAL_COUNT is {count}')
    for i, n in enumerate(names):
        if n not in idx: problems.append(f'{n} not in tempctl.h')
        elif idx[n] != i: problems.append(f'{n} is index {idx[n]} in tempctl.h but position {i} in the DBC')
    return problems

def fmt(x):
    return repr(float(x)) if isinstance(x, float) and not float(x).is_integer() else str(int(x))

def write_dbc(path, rows, length):
    L = []
    L.append('VERSION "TempCtl v2 - generated by tools/make_tempctl_dbc.py"')
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
    L.append(f'CM_ BO_ {DBC_ID} "TempCtl controller state (TcStep output array), PGN 65280 Proprietary B, J1939 BAM";')
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
        print(f'  {r["start"]:4d}|{r["bits"]:<2d} {r["name"]:<18s} x{fmt(r["factor"])} {fmt(r["offset"]):>5s} [{fmt(r["min"])}|{fmt(r["max"])}] {r["unit"]}')
    if a.tables:
        outdir = os.path.join(os.path.dirname(os.path.abspath(a.out)), 'tables')
        rc = subprocess.call([sys.executable, a.dbc2tables, a.out, '--message', MESSAGE, '--out', outdir])
        if rc: sys.exit(rc)
        print(f'tables written to {outdir}')

if __name__ == '__main__':
    main()
