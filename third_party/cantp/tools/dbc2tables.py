"""
dbc2tables.py - turn DBC messages into CanTp message/signal tables.

This is the only place a .dbc is ever parsed; the library itself never reads
one (Scott's rule). Output per message, in the chosen formats:

  * JSON   <out>/<Message>.json   {"msgdef":[8 numbers], "sigdefs":[[8 numbers],...],
                                    "signals":[names], "units":[...], "comment":...}
  * CSV    <out>/<Message>.msg.csv (1 row x 8) and <Message>.sig.csv (nSig x 8),
           numeric only, ready for LabVIEW "Read Delimited Spreadsheet" -> 2-D DBL
           -> Reshape Array; <Message>.names.txt lists the signal order.
  * C      <out>/cantp_tables.h  static const double arrays for every message

Rules (see docs/DBC-CONVENTIONS.md):
  - transport: length <= 8 -> classic (0); > 8 with VFrameFormat J1939PG (or a
    29-bit id and no FD attribute) -> J1939 BAM (1); > 8 with VFrameFormat
    StandardCAN_FD / ExtendedCAN_FD -> CAN FD (2, or 3 when CANFD_BRS = 1).
  - id: DBC id with bit 31 stripped; SA placeholder 0xFE kept (any SA on
    receive) unless --sa overrides it.
  - start bit / byte order / sign / factor / offset / min / max verbatim from
    the DBC; SIG_VALTYPE_ 1/2 -> float32/float64.
  - multiplexed signals are skipped with a warning (release 2).

    python tools/dbc2tables.py J1939_NGHD_V130.dbc --message EC1 --message EEC1 --out tables/ [--sa 0x00]
    python tools/dbc2tables.py my.dbc --all --out tables/ --json --csv --c
"""
import argparse, json, os, sys
import cantools

TP_CLASSIC, TP_BAM, TP_FD, TP_FD_BRS = 0, 1, 2, 3

def msg_transport(m):
    attrs = m.dbc.attributes if m.dbc else {}
    vff = attrs.get('VFrameFormat')
    vff = vff.value if vff is not None else None
    # cantools resolves enum attributes to their string value where it can
    if isinstance(vff, int):
        choices = ["StandardCAN", "ExtendedCAN", "reserved", "J1939PG", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "StandardCAN_FD", "ExtendedCAN_FD"]
        vff = choices[vff] if 0 <= vff < len(choices) else None
    is_fd = bool(getattr(m, 'is_fd', False)) or (vff or '').endswith('_FD')
    if m.length <= 8 and not is_fd:
        return TP_CLASSIC
    if is_fd:
        brs = attrs.get('CANFD_BRS')
        return TP_FD_BRS if (brs is not None and int(brs.value) == 1) else TP_FD
    if vff == 'J1939PG' or m.is_extended_frame:
        return TP_BAM
    raise ValueError(f'{m.name}: length {m.length} > 8 but neither J1939PG nor CAN FD')

def convert(m, sa_override):
    tp = msg_transport(m)
    fid = m.frame_id & 0x1FFFFFFF
    ext = 1 if m.is_extended_frame else 0
    cycle = m.cycle_time or 0
    pad = 255 if tp == TP_BAM or ext else 0
    msgdef = [float(fid), float(ext), float(m.length), float(tp),
              float(sa_override if sa_override is not None else -1), 255.0, float(pad), float(cycle)]
    sigdefs, names, units = [], [], []
    for s in m.signals:
        if s.multiplexer_ids is not None or s.is_multiplexer:
            print(f'  warning: {m.name}.{s.name} is multiplexed - skipped', file=sys.stderr)
            continue
        vtype = 0
        if s.is_float:
            vtype = 2 if s.length == 32 else 3
        elif s.is_signed:
            vtype = 1
        mn = float(s.minimum) if s.minimum is not None else 0.0
        mx = float(s.maximum) if s.maximum is not None else 0.0
        sigdefs.append([float(s.start), float(s.length), 1.0 if s.byte_order == 'big_endian' else 0.0,
                        float(vtype), float(s.scale), float(s.offset), mn, mx])
        names.append(s.name); units.append(s.unit or '')
    return {'message': m.name, 'frame_id_dbc': m.frame_id, 'msgdef': msgdef, 'sigdefs': sigdefs,
            'signals': names, 'units': units, 'comment': m.comment or '',
            'transport': ['classic', 'j1939_bam', 'canfd', 'canfd_brs'][tp]}

def cident(s):
    return ''.join(c if c.isalnum() else '_' for c in s)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dbc')
    ap.add_argument('--message', action='append', default=[], help='message name (repeatable)')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--out', default='tables')
    ap.add_argument('--sa', type=lambda x: int(x, 0), default=None, help='source address override (J1939)')
    ap.add_argument('--json', action='store_true'); ap.add_argument('--csv', action='store_true'); ap.add_argument('--c', action='store_true')
    a = ap.parse_args()
    if not (a.json or a.csv or a.c): a.json = a.csv = a.c = True
    db = cantools.database.load_file(a.dbc, strict=False)
    msgs = db.messages if a.all else [db.get_message_by_name(n) for n in a.message]
    if not msgs: ap.error('give --message NAME (repeatable) or --all')
    os.makedirs(a.out, exist_ok=True)
    tables = []
    for m in msgs:
        try:
            t = convert(m, a.sa)
        except ValueError as e:
            print('  skipped:', e, file=sys.stderr); continue
        tables.append(t)
        if a.json:
            with open(os.path.join(a.out, f'{m.name}.json'), 'w') as f: json.dump(t, f, indent=1)
        if a.csv:
            with open(os.path.join(a.out, f'{m.name}.msg.csv'), 'w') as f: f.write(','.join(repr(x) for x in t['msgdef']) + '\n')
            with open(os.path.join(a.out, f'{m.name}.sig.csv'), 'w') as f:
                for row in t['sigdefs']: f.write(','.join(repr(x) for x in row) + '\n')
            with open(os.path.join(a.out, f'{m.name}.names.txt'), 'w') as f:
                for n, u in zip(t['signals'], t['units']): f.write(f'{n}\t{u}\n')
        print(f"{m.name}: id {m.frame_id & 0x1FFFFFFF:#x} len {m.length} {t['transport']} {len(t['sigdefs'])} signals")
    if a.c:
        with open(os.path.join(a.out, 'cantp_tables.h'), 'w') as f:
            f.write(f'/* generated by dbc2tables.py from {os.path.basename(a.dbc)} - do not edit */\n#pragma once\n\n')
            for t in tables:
                n = cident(t['message'])
                f.write(f"/* {t['message']}: {t['transport']}, {len(t['sigdefs'])} signals: {', '.join(t['signals'])} */\n")
                f.write(f"static const double {n}_msgdef[8] = {{ {', '.join(repr(x) for x in t['msgdef'])} }};\n")
                f.write(f"static const double {n}_sigdefs[{max(1, len(t['sigdefs']))}][8] = {{\n")
                for row, name in zip(t['sigdefs'], t['signals']):
                    f.write(f"    {{ {', '.join(repr(x) for x in row)} }}, /* {name} */\n")
                if not t['sigdefs']: f.write('    { 0 }\n')
                f.write('};\n')
                f.write(f"#define {n}_NSIG {len(t['sigdefs'])}\n\n")
    print(f'{len(tables)} message(s) written to {a.out}')

if __name__ == '__main__':
    main()
