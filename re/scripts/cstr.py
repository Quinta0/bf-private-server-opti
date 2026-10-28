import sys,struct
f=open(sys.argv[1],'rb').read()
phoff=struct.unpack_from('<Q',f,0x20)[0]; phnum=struct.unpack_from('<H',f,0x38)[0]
segs=[]
for i in range(phnum):
    o=phoff+i*56
    t,fl,off,va,pa,fsz,msz,al=struct.unpack_from('<IIQQQQQQ',f,o)
    if t==1: segs.append((va,off,fsz))
def v2o(va):
    for s,o,z in segs:
        if s<=va<s+z: return va-s+o
def cs(va):
    o=v2o(va); e=f.index(b'\0',o); return f[o:e]
mode=sys.argv[2]
for a in sys.argv[3:]:
    va=int(a,16)
    if mode=='s': print(hex(va), cs(va))
    elif mode=='q': o=v2o(va); print(hex(va), hex(struct.unpack_from('<Q',f,o)[0]))
    elif mode=='b': o=v2o(va); print(hex(va), f[o:o+64].hex())
