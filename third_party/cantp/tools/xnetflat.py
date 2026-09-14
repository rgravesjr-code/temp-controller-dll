"""xnetflat.py - the flattened LabVIEW "XNET Frame CAN" cluster array in Python.

Mirrors what LabVIEW's Flatten To String / Unflatten From String do for a 1-D
array of the NI-XNET "XNET Frame CAN" cluster (big-endian, sizes prepended),
which is the frame format of CanTp_TransferXnet / CanTp_RecordsToXnet /
CanTp_XnetToRecords (CanTp v1.3.0). Use it to build or read that format from
any non-LabVIEW caller, and as the independent reference in the oracle test.

Per frame (27 + payload bytes), in the cluster's type order:
    I64 timestamp seconds since 1904-01-01 UTC, U64 fraction (2^-64 s)
    I32 payload length, U8 payload[]
    U32 identifier (bare 11/29-bit), U8 type, U8 extended?, U8 echo?
The array is an I32 count followed by the frames.

CLI:  python xnetflat.py file.bin          -> lists the frames
      python xnetflat.py file.bin --records out.bin  -> NI-XNET raw records
"""
import argparse, datetime, struct, sys

LV_EPOCH_OFFSET_S = 9561628800          # 1601-01-01 -> 1904-01-01
LV_EPOCH = datetime.datetime(1904, 1, 1, tzinfo=datetime.timezone.utc)
TYPES = {0: 'CAN Data', 1: 'CAN Remote', 2: 'CAN Bus Error', 8: 'CAN 2.0 Data', 16: 'CAN FD Data',
         24: 'CAN FD+BRS Data', 192: 'J1939 Data', 224: 'Delay', 225: 'Log Trigger', 226: 'Start Trigger'}


def time_to_labview(ts100ns):
    """100 ns since 1601 -> (seconds since 1904, 2^-64 fraction); 0 -> (0, 0)."""
    if ts100ns == 0:
        return 0, 0
    s, rem = divmod(ts100ns, 10_000_000)
    return s - LV_EPOCH_OFFSET_S, (rem << 64) // 10_000_000


def time_from_labview(seconds, fraction):
    """(seconds since 1904, 2^-64 fraction) -> 100 ns since 1601, rounded; (0, 0) -> 0."""
    if seconds == 0 and fraction == 0:
        return 0
    s = seconds + LV_EPOCH_OFFSET_S
    if s < 0:
        return 0
    sub = (fraction * 10_000_000 + (1 << 63)) >> 64
    return s * 10_000_000 + sub


def labview_datetime(seconds, fraction):
    return LV_EPOCH + datetime.timedelta(seconds=seconds + fraction / 2.0 ** 64)


def flatten_frame(ident, payload, ext, ftype=0, echo=False, ts100ns=0):
    sec, frac = time_to_labview(ts100ns)
    return (struct.pack('>qQi', sec, frac, len(payload)) + bytes(payload)
            + struct.pack('>IBBB', ident & 0x1FFFFFFF, ftype, 1 if ext else 0, 1 if echo else 0))


def flatten(frames):
    """frames: iterable of dicts {id, payload, ext, type, echo, ts100ns} (missing keys default)."""
    frames = list(frames)
    out = struct.pack('>i', len(frames))
    for f in frames:
        out += flatten_frame(f['id'], f['payload'], f.get('ext', f['id'] > 0x7FF), f.get('type', 0),
                             f.get('echo', False), f.get('ts100ns', 0))
    return out


def unflatten(buf):
    """-> list of dicts {id, payload, ext, type, echo, seconds, fraction, ts100ns}; ValueError when malformed."""
    if len(buf) < 4:
        raise ValueError('shorter than the count')
    (count,) = struct.unpack_from('>i', buf, 0)
    if count < 0:
        raise ValueError('negative count')
    off, frames = 4, []
    for _ in range(count):
        if len(buf) - off < 27:
            raise ValueError('truncated frame')
        sec, frac, plen = struct.unpack_from('>qQi', buf, off)
        if plen < 0 or plen > 1785 or len(buf) - off - 27 < plen:
            raise ValueError('bad payload length %d' % plen)
        payload = bytes(buf[off + 20:off + 20 + plen])
        ident, ftype, ext, echo = struct.unpack_from('>IBBB', buf, off + 20 + plen)
        frames.append(dict(id=ident, payload=payload, ext=bool(ext), type=ftype, echo=bool(echo),
                           seconds=sec, fraction=frac, ts100ns=time_from_labview(sec, frac)))
        off += 27 + plen
    return frames


def record_size(plen):
    return 24 + (((plen - 1) & ~7) if plen > 8 else 0)


def frames_to_records(frames):
    """NI-XNET raw records (the CanTp_Pack format) for frames of <= 64 bytes, type byte verbatim."""
    out = b''
    for f in frames:
        plen = len(f['payload'])
        if plen > 64:
            raise ValueError('a %d-byte frame has no record form' % plen)
        rec = struct.pack('<QIBBBB', f.get('ts100ns', 0), (f['id'] & 0x1FFFFFFF) | (0x20000000 if f.get('ext') else 0),
                          f.get('type', 0), 0, 0, plen) + bytes(f['payload'])
        out += rec + b'\0' * (record_size(plen) - len(rec))
    return out


def records_to_frames(buf):
    out, o = [], 0
    while o < len(buf):
        if len(buf) - o < 24:
            raise ValueError('truncated record')
        ts, raw, ftype, _f, _i, plen = struct.unpack_from('<QIBBBB', buf, o)
        size = record_size(plen)
        if len(buf) - o < size:
            raise ValueError('truncated record')
        out.append(dict(id=raw & 0x1FFFFFFF, ext=bool(raw & 0x20000000), type=ftype, echo=False,
                        payload=bytes(buf[o + 16:o + 16 + plen]), ts100ns=ts))
        o += size
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', help='flattened array of XNET Frame CAN (as saved by LabVIEW)')
    ap.add_argument('--records', help='write the frames as NI-XNET raw records to this file')
    a = ap.parse_args()
    buf = open(a.file, 'rb').read()
    frames = unflatten(buf)
    print('%d frame(s), %d bytes' % (len(frames), len(buf)))
    for i, f in enumerate(frames):
        print('  [%d] id 0x%08X %s %s%s  %s  %d bytes: %s' % (
            i, f['id'], 'ext' if f['ext'] else 'std', TYPES.get(f['type'], 'type %d' % f['type']),
            ' echo' if f['echo'] else '', labview_datetime(f['seconds'], f['fraction']).strftime('%Y-%m-%d %H:%M:%S.%f UTC'),
            len(f['payload']), f['payload'].hex(' ')))
    if a.records:
        open(a.records, 'wb').write(frames_to_records(frames))
        print('wrote', a.records)


if __name__ == '__main__':
    main()
