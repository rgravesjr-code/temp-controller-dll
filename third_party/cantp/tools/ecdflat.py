"""
ecdflat.py - LabVIEW "J1939Msg(V4).ctl" cluster <-> bytes, and .ecd database reader.

CanTp_DefineFlat() takes exactly the bytes LabVIEW's "Flatten To String"
produces for one J1939Msg(V4) cluster (big-endian, sizes prepended). An Eaton
.ecd CAN database is an encrypted LabVIEW flatten of an ARRAY of those
clusters, so this module can pull the per-message flattened bytes straight
out of an .ecd for tests and for non-LabVIEW callers, or build them from
plain Python values.

Cluster layout (all big-endian; str = I32 length + latin-1 bytes):
  message:  str name, U32 msgId, U32 pgn, U8 extended, I32 numBytes, str desc,
            DBL updateRate, DBL tolerance, I32 nCh, channel[nCh], U8 eatonIpy
  channel:  str name, I32 startBit, I32 nBits, U16 dataType (0 Signed, 1 Unsigned,
            2 IEEE Float), U16 byteOrder (0 Intel, 1 Motorola), DBL sf, DBL offset,
            DBL min, DBL max, DBL default, str unit, U16 lookup (0 none, 1 custom),
            I32 nLut, lut[nLut]{DBL value, str short, str alphaNum, str desc}, str chDesc

.ecd file: I32 (big-endian) length + encrypted string. Decrypt: reverse the
bytes; the first byte is then an ASCII digit = offset; swap each byte pair;
add the offset to every byte (mod 256). Plaintext = I32 count + count clusters.

    python tools/ecdflat.py J1939_3Msg.ecd                 # list messages
    python tools/ecdflat.py J1939_3Msg.ecd --message EC1_ISX --out EC1_ISX.flat  # bytes for DefineFlat
    python tools/ecdflat.py J1939_3Msg.ecd --message EC1_ISX --tables            # as CanTp msgdef/sigdef rows
"""
import argparse, struct, sys

# ------------------------------------------------------------------ reader
class R:
    def __init__(self, b, o=0): self.b, self.o = b, o
    def take(self, fmt):
        v = struct.unpack_from('>' + fmt, self.b, self.o); self.o += struct.calcsize('>' + fmt)
        return v[0] if len(v) == 1 else v
    def s(self):
        n = self.take('i')
        if n < 0 or self.o + n > len(self.b): raise ValueError('bad string length %d at %d' % (n, self.o - 4))
        v = self.b[self.o:self.o + n].decode('latin-1'); self.o += n
        return v

def parse_channel(r):
    c = {'name': r.s(), 'startBit': r.take('i'), 'nBits': r.take('i'), 'dataType': r.take('H'),
         'byteOrder': r.take('H'), 'sf': r.take('d'), 'offset': r.take('d'), 'min': r.take('d'),
         'max': r.take('d'), 'default': r.take('d'), 'unit': r.s(), 'lookup': r.take('H')}
    n = r.take('i')
    c['lut'] = [{'value': r.take('d'), 'short': r.s(), 'alphaNum': r.s(), 'desc': r.s()} for _ in range(n)]
    c['desc'] = r.s()
    return c

def parse_message(r):
    """One J1939Msg(V4) cluster; returns (dict, start_offset, end_offset)."""
    start = r.o
    m = {'name': r.s(), 'msgId': r.take('I'), 'pgn': r.take('I'), 'extended': r.take('B'),
         'numBytes': r.take('i'), 'desc': r.s(), 'updateRate': r.take('d'), 'tolerance': r.take('d')}
    n = r.take('i')
    m['channels'] = [parse_channel(r) for _ in range(n)]
    m['eatonIpy'] = r.take('B')
    return m, start, r.o

def parse_message_bytes(flat):
    """Parse exactly one flattened cluster; raises if bytes are left over."""
    r = R(flat)
    m, _, end = parse_message(r)
    if end != len(flat): raise ValueError('%d trailing bytes' % (len(flat) - end))
    return m

# ------------------------------------------------------------------ writer
def _s(v): b = v.encode('latin-1'); return struct.pack('>i', len(b)) + b

def flatten_channel(c):
    out = _s(c['name']) + struct.pack('>iiHHddddd', c['startBit'], c['nBits'], c['dataType'], c['byteOrder'],
                                      c['sf'], c['offset'], c['min'], c['max'], c['default'])
    out += _s(c.get('unit', '')) + struct.pack('>Hi', c.get('lookup', 0), len(c.get('lut', [])))
    for e in c.get('lut', []):
        out += struct.pack('>d', e['value']) + _s(e['short']) + _s(e['alphaNum']) + _s(e['desc'])
    return out + _s(c.get('desc', ''))

def flatten_message(m):
    out = _s(m['name']) + struct.pack('>IIBi', m['msgId'], m['pgn'], m['extended'], m['numBytes'])
    out += _s(m.get('desc', '')) + struct.pack('>ddi', m.get('updateRate', -1.0), m.get('tolerance', 0.0), len(m['channels']))
    for c in m['channels']: out += flatten_channel(c)
    return out + struct.pack('>B', m.get('eatonIpy', 0))

# ------------------------------------------------------------------ .ecd
def ecd_decrypt(data):
    n = struct.unpack_from('>i', data, 0)[0]
    enc = data[4:4 + n]
    rev = enc[::-1]
    off = rev[0] - 48
    body = bytearray(rev[1:])
    for i in range(0, len(body) - 1, 2): body[i], body[i + 1] = body[i + 1], body[i]
    return bytes((b + off) & 0xFF for b in body)

def ecd_messages(path):
    """[(dict, flat_bytes)] for every message in an .ecd file."""
    with open(path, 'rb') as f: plain = ecd_decrypt(f.read())
    r = R(plain)
    count = r.take('i')
    out = []
    for _ in range(count):
        m, a, b = parse_message(r)
        out.append((m, plain[a:b]))
    if r.o != len(plain): raise ValueError('%d trailing bytes after %d messages' % (len(plain) - r.o, count))
    return out

# ------------------------------------------------------------------ CanTp view (mirrors cantp.c define_flat)
TP_CLASSIC, TP_BAM, TP_FD, TP_FD_BRS, TP_RTS, TP_ISOTP = 0, 1, 2, 3, 4, 5
def to_tables(m, transport=-1, sa=-1):
    """The msgdef/sigdefs rows CanTp_DefineFlat derives (for comparison with CanTp_GetDef)."""
    ident = m['msgId'] & 0x1FFFFFFF
    ext = 1 if (m['extended'] or m['msgId'] >= 0x80000000 or ident > 0x7FF) else 0   # an id above 0x7FF cannot be 11-bit
    n = m['numBytes']
    pdu1 = ext and ((ident >> 16) & 0xFF) < 0xF0
    ps = (ident >> 8) & 0xFF
    real_da = pdu1 and ps not in (0xFF, 0xFE)
    if transport < 0:
        if not ext: transport = TP_CLASSIC if n <= 8 else TP_FD
        elif real_da: transport = TP_CLASSIC if n <= 8 else TP_RTS
        else: transport = TP_BAM
    da = ps if (pdu1 and transport != TP_BAM) else 255
    pad = 0xCC if transport == TP_ISOTP else (255 if ext else 0)
    cycle = m['updateRate'] if m['updateRate'] > 0 else 0.0
    if ext and sa >= 0: ident = (ident & ~0xFF) | sa
    eff_sa = -1 if (not ext or (sa < 0 and (ident & 0xFF) == 0xFE)) else (ident & 0xFF)   # what CanTp_GetDef reports
    msgdef = [float(ident), float(ext), float(n), float(transport), float(eff_sa), float(da), float(pad), float(cycle)]
    sig = []
    for c in m['channels']:
        vtype = {0: 1, 1: 0, 2: 2 if c['nBits'] == 32 else 3}[c['dataType']]
        sig.append([float(c['startBit']), float(c['nBits']), 1.0 if c['byteOrder'] else 0.0, float(vtype),
                    c['sf'], c['offset'], c['min'], c['max']])
    return msgdef, sig

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ecd')
    ap.add_argument('--message', help='message name')
    ap.add_argument('--out', help='write the flattened cluster bytes of --message to this file')
    ap.add_argument('--tables', action='store_true', help='print the CanTp rows DefineFlat derives')
    a = ap.parse_args()
    msgs = ecd_messages(a.ecd)
    if not a.message:
        for m, flat in msgs:
            print('%-28s id %08X pgn %05X ext %d len %3d ch %3d flat %5d B' % (m['name'], m['msgId'], m['pgn'], m['extended'], m['numBytes'], len(m['channels']), len(flat)))
        print('%d messages' % len(msgs)); return
    for m, flat in msgs:
        if m['name'] == a.message: break
    else: sys.exit('no message ' + a.message)
    assert flatten_message(m) == flat, 'round trip mismatch'
    if a.out:
        with open(a.out, 'wb') as f: f.write(flat)
        print('wrote %d bytes to %s' % (len(flat), a.out))
    print('%s: id %08X len %d %d channels' % (m['name'], m['msgId'], m['numBytes'], len(m['channels'])))
    for c in m['channels']:
        print('  %-36s start %4d bits %2d type %d order %d sf %g off %g [%g..%g] dflt %g %s' % (
            c['name'], c['startBit'], c['nBits'], c['dataType'], c['byteOrder'], c['sf'], c['offset'], c['min'], c['max'], c['default'], c['unit']))
    if a.tables:
        md, sd = to_tables(m)
        print('msgdef', md)
        for r in sd: print('sigdef', r)

if __name__ == '__main__':
    main()
