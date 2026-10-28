# usage: dxref.py full.s lo hi  -> list (func, insn addr, target) where adrp+add/ldr resolves into [lo,hi)
import sys,re
lo=int(sys.argv[2],16); hi=int(sys.argv[3],16)
regs={}; func=None
rf=re.compile(r'^([0-9a-f]{16}) <(.*)>:')
ra=re.compile(r'^\s*([0-9a-f]+):\s+adrp\s+(x\d+), 0x([0-9a-f]+)')
rd=re.compile(r'^\s*([0-9a-f]+):\s+(add|ldr|ldrb|ldrh|ldur)\s+[xwqb]\d+, \[?(x\d+)(?:, #(-?0x[0-9a-f]+|-?\d+))?')
for line in open(sys.argv[1]):
    m=rf.match(line)
    if m: func=m.group(2); regs={}; continue
    m=ra.match(line)
    if m: regs[m.group(2)]=int(m.group(3),16); continue
    m=rd.match(line)
    if m and m.group(3) in regs:
        off=m.group(4); off=int(off,0) if off else 0
        t=regs[m.group(3)]+off
        if lo<=t<hi: print(func, m.group(1), hex(t))
