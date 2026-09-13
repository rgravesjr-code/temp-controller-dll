"""Minimal ELF inspector (ELF32 and ELF64, little-endian): machine, type, soname,
NEEDED, exported / imported dynamic symbols, glibc version needs, section sizes."""
import struct, sys

def main(path, sections=False):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF', 'not ELF'
    assert d[5] == 1, 'need little-endian ELF'
    is64 = d[4] == 2
    e_type, e_machine = struct.unpack_from('<HH', d, 16)
    if is64:
        e_shoff, = struct.unpack_from('<Q', d, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
    else:
        e_shoff, = struct.unpack_from('<I', d, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2E)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if is64:
            name, typ, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', d, off)
        else:
            name, typ, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from('<IIIIIIIIII', d, off)
        secs.append(dict(name=name, type=typ, addr=addr, off=offset, size=size, link=link, entsize=entsize))
    shstr = secs[e_shstrndx]
    def cstr(base, o):
        e = d.index(b'\0', base + o); return d[base + o:e].decode()
    for s in secs: s['sname'] = cstr(shstr['off'], s['name'])
    by = {s['sname']: s for s in secs}
    print('class', 'ELF64' if is64 else 'ELF32', 'type', {2: 'EXEC', 3: 'DYN'}.get(e_type, e_type),
          'machine', {3: 'x86', 62: 'x86_64', 40: 'ARM', 183: 'AArch64'}.get(e_machine, e_machine))
    dyn = by.get('.dynamic'); dynstr = by.get('.dynstr')
    if dyn and dynstr:
        ent = 16 if is64 else 8
        for i in range(dyn['size'] // ent):
            tag, val = struct.unpack_from('<qQ' if is64 else '<iI', d, dyn['off'] + ent * i)
            if tag == 1: print('NEEDED', cstr(dynstr['off'], val))
            if tag == 14: print('SONAME', cstr(dynstr['off'], val))
    dsym = by.get('.dynsym')
    if dsym and dynstr:
        exp, imp = [], []
        ent = 24 if is64 else 16
        for i in range(dsym['size'] // ent):
            if is64:
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', d, dsym['off'] + ent * i)
            else:
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', d, dsym['off'] + ent * i)
            nm = cstr(dynstr['off'], st_name)
            if not nm: continue
            (exp if st_shndx != 0 else imp).append(nm)
        print('EXPORTS', sorted(exp))
        print('IMPORTS', sorted(imp))
    vn = by.get('.gnu.version_r')
    if vn and dynstr:
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
    if sections:
        for s in sorted(secs, key=lambda s: -s['size'])[:12]:
            print('SECTION %-24s %8d bytes' % (s['sname'], s['size']))

if __name__ == '__main__':
    main(sys.argv[1], sections='--sections' in sys.argv[2:])
