"""Test helper: a copy of STELLAR.DAT whose DESIGN/STATES.INI starts with
other commands, in testroot/cd (the rest of the CD hard-linked).  Lets a
hidden test run jump straight into an away mission or a space combat.

usage: teststart.py "Cmd 1" "Cmd 2" ...     e.g.  teststart.py "SpaceCombat 1 SpaceCombat1Won SpaceCombat1Lost"
       (no commands: the original start)"""
import os, re, sys, struct, zipfile, zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
src = os.path.join(ROOT, 'game', 'cd', 'GENS', 'STELLAR.DAT')
dst = os.path.join(ROOT, 'testroot', 'cd', 'GENS', 'STELLAR.DAT')
CRLF = chr(13) + chr(10)

d = bytearray(open(src, 'rb').read())
z = zipfile.ZipFile(src)
info = [i for i in z.infolist() if i.filename.upper() == 'DESIGN/STATES.INI'][0]
text = z.read(info.filename).decode('latin1')
if len(sys.argv) > 1:
    a = text.index('[Start]')
    b = text.index('[', a + 1)
    text = text[:a] + '[Start]' + CRLF + ''.join(c + CRLF for c in sys.argv[1:]) + CRLF + text[b:]
# trailing ;comments are ignored by the game: dropping them makes room
text = re.sub('[ ' + chr(9) + ']*;[^' + CRLF + ']*', '', text)
new = text.encode('latin1')
co = zlib.compressobj(9, zlib.DEFLATED, -15)
comp = co.compress(new) + co.flush()
if len(comp) > info.compress_size:
    sys.exit('patched STATES.INI too big (%d > %d compressed)' % (len(comp), info.compress_size))
lh = info.header_offset
nl, el = struct.unpack_from('<HH', d, lh + 26)
crc = zlib.crc32(new) & 0xffffffff
struct.pack_into('<III', d, lh + 14, crc, len(comp), len(new))
d[lh + 30 + nl + el:lh + 30 + nl + el + len(comp)] = comp
cd = d.rfind(b'PK\x05\x06')
n, cdsize, cdoff = struct.unpack_from('<HII', d, cd + 10)
p = cdoff
for k in range(n):
    fnl, exl, cml = struct.unpack_from('<HHH', d, p + 28)
    if bytes(d[p + 46:p + 46 + fnl]).decode('latin1').upper() == 'DESIGN/STATES.INI':
        struct.pack_into('<III', d, p + 16, crc, len(comp), len(new))
    p += 46 + fnl + exl + cml
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, 'wb').write(d)
print('start:', sys.argv[1:] or 'original')
