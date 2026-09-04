"""Minimal ELF64 inspector: machine, type, soname, NEEDED, exported dynamic symbols, glibc version needs."""
import struct, sys
def main(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF', 'not ELF'
    assert d[4] == 2 and d[5] == 1, 'need ELF64 little-endian'
    e_type, e_machine = struct.unpack_from('<HH', d, 16)
    e_shoff, = struct.unpack_from('<Q', d, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', d, off)
        secs.append(dict(name=name, type=typ, addr=addr, off=offset, size=size, link=link, entsize=entsize))
    shstr = secs[e_shstrndx]
    def cstr(base, o):
        e = d.index(b'\0', base + o); return d[base + o:e].decode()
    for s in secs: s['sname'] = cstr(shstr['off'], s['name'])
    by = {s['sname']: s for s in secs}
    print('type', {2: 'EXEC', 3: 'DYN'}.get(e_type, e_type), 'machine', {62: 'x86_64', 40: 'ARM', 183: 'AArch64'}.get(e_machine, e_machine))
    dyn = by.get('.dynamic'); dynstr = by.get('.dynstr')
    if dyn:
        for i in range(dyn['size'] // 16):
            tag, val = struct.unpack_from('<qQ', d, dyn['off'] + 16 * i)
            if tag == 1: print('NEEDED', cstr(dynstr['off'], val))
            if tag == 14: print('SONAME', cstr(dynstr['off'], val))
    dsym = by.get('.dynsym')
    if dsym:
        exp, imp = [], []
        for i in range(dsym['size'] // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', d, dsym['off'] + 24 * i)
            nm = cstr(dynstr['off'], st_name)
            if not nm: continue
            bind = st_info >> 4
            (exp if st_shndx != 0 else imp).append(nm)
        print('EXPORTS', sorted(exp))
        print('IMPORTS', sorted(imp))
    vn = by.get('.gnu.version_r')
    if vn:
        o = vn['off']; vers = set()
        while True:
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from('<HHIII', d, o)
            a = o + vn_aux
            for _ in range(vn_cnt):
                vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from('<IHHII', d, a)
                vers.add(cstr(dynstr['off'], vna_name)); a += vna_next
            if not vn_next: break
            o += vn_next
        print('GLIBC_VERSIONS_NEEDED', sorted(vers))
if __name__ == '__main__': main(sys.argv[1])
