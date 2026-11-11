"""The first icon group of a PE file (exe / dll) as an .ico file.
usage: exe_icon.py game.exe out.ico"""
import struct, sys


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    rsrc_rva = struct.unpack_from('<I', d, opt + 96 + 2 * 8)[0]
    secs = []
    for i in range(nsec):
        s = opt + optsz + 40 * i
        vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', d, s + 8)
        secs.append((va, max(vsize, rawsz), rawptr))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return rva - va + raw
        raise ValueError('rva %x' % rva)
    return d, off, rsrc_rva


def entries(d, off, base, rva):
    o = off(rva)
    nn, ni = struct.unpack_from('<HH', d, o + 12)
    out = []
    for k in range(nn + ni):
        name, ptr = struct.unpack_from('<II', d, o + 16 + 8 * k)
        out.append((name, ptr))
    return out


def main(exe, ico):
    d, off, root = load(exe)
    types = dict(entries(d, off, root, root))

    def leaf(type_id, name_id=None):
        sub = entries(d, off, root, root + (types[type_id] & 0x7fffffff))
        for name, ptr in sub:
            if name_id is None or name == name_id:
                langs = entries(d, off, root, root + (ptr & 0x7fffffff))
                data = root + langs[0][1]
                rva, size = struct.unpack_from('<II', d, off(data))
                return d[off(rva):off(rva) + size]
        raise KeyError(name_id)
    grp = leaf(14)
    n = struct.unpack_from('<H', grp, 4)[0]
    imgs = []
    for i in range(n):
        w, h, cc, r, planes, bits, size, iid = struct.unpack_from('<BBBBHHIH', grp, 6 + 14 * i)
        imgs.append((w, h, cc, planes, bits, leaf(3, iid)))
    out = struct.pack('<HHH', 0, 1, n)
    pos = 6 + 16 * n
    for w, h, cc, planes, bits, data in imgs:
        out += struct.pack('<BBBBHHII', w, h, cc, 0, planes, bits, len(data), pos)
        pos += len(data)
    for img in imgs:
        out += img[5]
    open(ico, 'wb').write(out)
    print('%d images -> %s' % (n, ico))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
