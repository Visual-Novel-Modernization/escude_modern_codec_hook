import struct,sys,os,collections
K=0x65AC9365;M=0xffffffff
def f(x):
    t=x^K
    return (t ^ ((8*(t^((2*t)&M)))&M) ^ ((t^(t>>1))>>3))&M
def parse(path):
    with open(path,'rb') as fh:
        h=fh.read(20)
        assert h[:8]==b'ESC-ARC2',h[:8]
        a,b,c=struct.unpack('<3I',h[8:])
        v20=f(a); count=v20^b; v21=f(v20); nsz=v21^c
        raw=bytearray(fh.read(12*count))
        names=fh.read(nsz)
    ents=[]
    for i in range(count):
        o,off,sz=[struct.unpack_from('<I',raw,12*i+4*k)[0] for k in range(3)]
        pass
    # decrypt stream sequentially across all dwords
    v=v21
    out=[]
    for i in range(3*count):
        v=f(v)
        out.append(struct.unpack_from('<I',raw,4*i)[0]^v)
    for i in range(count):
        no,off,sz=out[3*i:3*i+3]
        e=names.index(b'\0',no); ents.append((names[no:e].decode('cp932','replace'),off,sz))
    return ents,count,nsz
if __name__=='__main__':
    for p in sys.argv[1:]:
        ents,c,n=parse(p)
        ext=collections.Counter(os.path.splitext(e[0])[1].lower() for e in ents)
        print(os.path.basename(p),c,n,dict(ext)); print('  ',ents[:3])
