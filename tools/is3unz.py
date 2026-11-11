"""InstallShield 3 .Z archive extractor (PKWARE DCL implode), pure Python.
usage: is3unz.py archive.Z outdir"""
import sys, os, struct

# --- PKWARE DCL explode (port of zlib contrib/blast.c) ---
class _St:
    def __init__(s, d): s.d=d; s.p=0; s.bitbuf=0; s.bitcnt=0
    def bits(s, need):
        val=s.bitbuf
        while s.bitcnt<need:
            if s.p>=len(s.d): raise EOFError
            val|=s.d[s.p]<<s.bitcnt; s.p+=1; s.bitcnt+=8
        s.bitbuf=val>>need; s.bitcnt-=need
        return val&((1<<need)-1)

def _construct(rep):
    lens=[]
    for b in rep:
        n=(b>>4)+1; l=b&15; lens+= [l]*n
    MAXBITS=13
    count=[0]*(MAXBITS+1)
    for l in lens: count[l]+=1
    offs=[0]*(MAXBITS+1)
    for i in range(1,MAXBITS): offs[i+1]=offs[i]+count[i]
    sym=[0]*len(lens)
    for i,l in enumerate(lens):
        if l: sym[offs[l]]=i; offs[l]+=1
    return count,sym

def _decode(s,h):
    count,sym=h
    code=first=index=0
    bitbuf=s.bitbuf; left=s.bitcnt; length=1
    while True:
        while left:
            code|=(bitbuf&1)^1; bitbuf>>=1
            c=count[length]
            if code<first+c:
                s.bitbuf=bitbuf; s.bitcnt=(s.bitcnt-length)&7
                return sym[index+(code-first)]
            index+=c; first+=c; first<<=1; code<<=1; length+=1; left-=1
        left=14-length  # MAXBITS+1-len
        if left==0: raise ValueError('ran out of codes')
        if s.p>=len(s.d): raise EOFError
        bitbuf=s.d[s.p]; s.p+=1
        if left>8: left=8

_litlen=bytes([11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,9,7,6,7,8,7,6,55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,38,7,9,8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,27,44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173])
_lenlen=bytes([2,35,36,53,38,23]); _distlen=bytes([2,20,53,230,247,151,248])
_base=[3,2,4,5,6,7,8,9,10,12,16,24,40,72,136,264]; _extra=[0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8]
_H=None
def explode(data):
    global _H
    if _H is None: _H=(_construct(_litlen),_construct(_lenlen),_construct(_distlen))
    hl,hlen,hd=_H
    s=_St(data); out=bytearray()
    lit=s.bits(8); dictb=s.bits(8)
    if lit>1 or not 4<=dictb<=6: raise ValueError('bad header')
    while True:
        if s.bits(1):
            sym=_decode(s,hlen); ln=_base[sym]+s.bits(_extra[sym])
            if ln==519: break
            sym=2 if ln==2 else dictb
            dist=(_decode(s,hd)<<sym)+s.bits(sym)+1
            if dist>len(out): raise ValueError('distance too far')
            st=len(out)-dist
            for i in range(ln): out.append(out[st+i])
        else:
            out.append(_decode(s,hl) if lit else s.bits(8))
    return bytes(out)

def main(arc,outdir):
    d=open(arc,'rb').read()
    assert d[:4]==b'\x13\x5d\x65\x8c', 'not IS3 Z'
    nfiles=struct.unpack_from('<H',d,0x0C)[0]
    dirofs=struct.unpack_from('<I',d,0x29)[0]; ndirs=struct.unpack_from('<H',d,0x31)[0]
    fofs=struct.unpack_from('<I',d,0x33)[0]
    # directories
    dirs=[]; p=dirofs
    for i in range(ndirs):
        nf,esz,nl=struct.unpack_from('<HHH',d,p); dirs.append(d[p+6:p+6+nl].decode('latin1')); p+=esz
    p=fofs
    for i in range(nfiles):
        _,di,orig,comp,off,date,time,attr,esz=struct.unpack_from('<BHIIIHHIH',d,p)
        nl=d[p+0x1D]; name=d[p+0x1E:p+0x1E+nl].decode('latin1')
        sub=dirs[di] if di<len(dirs) else ''
        raw=explode(d[off:off+comp])
        assert len(raw)==orig,(name,len(raw),orig)
        dst=os.path.join(outdir,sub,name); os.makedirs(os.path.dirname(dst),exist_ok=True)
        open(dst,'wb').write(raw); print(f'{os.path.join(sub,name)}  {orig}')
        p+=esz
if __name__=='__main__': main(sys.argv[1],sys.argv[2])
