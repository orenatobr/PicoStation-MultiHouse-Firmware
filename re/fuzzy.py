"""Fuzzy-match V3 functions against the upstream ELF symbols (mnemonic sequence similarity)."""
import sys, difflib, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
def mn(code): return [i.mnemonic for i in md.disasm(code, 0)]
elf = ELFFile(open(sys.argv[1], 'rb'))
secs = [(s['sh_addr'], s.data()) for s in elf.iter_sections() if s['sh_type']=='SHT_PROGBITS' and s['sh_flags'] & 2 and s['sh_size']>0 and s.name!='.rodata']
base = []
for sym in elf.get_section_by_name('.symtab').iter_symbols():
    if sym['st_info']['type']=='STT_FUNC' and sym['st_size']>12:
        a = sym['st_value'] & ~1
        for sa, d in secs:
            if sa <= a < sa+len(d):
                base.append((sym.name, sym['st_size'], mn(d[a-sa:a-sa+sym['st_size']]))); break
img = open(sys.argv[2],'rb').read()
res = []
for line in open(sys.argv[3]):
    addr, size, _ = line.rstrip().split('\t'); A=int(addr,16); a=(A-0x10000000) if A<0x20000000 else (A-0x200000c0+0x183cc); size=int(size)
    if size <= 12: continue
    m = mn(img[a:a+size]); best=(0,None)
    for name, bs, bm in base:
        if bs < size*0.6 or bs > size*1.6: continue
        r = difflib.SequenceMatcher(None, m, bm, autojunk=False).ratio()
        if r > best[0]: best=(r,name)
    res.append((addr,size,best[0],best[1]))
with open(sys.argv[4],'w') as f:
    for addr,size,r,n in res: f.write(f"{addr}\t{size}\t{r:.2f}\t{n}\n")
good=[x for x in res if x[2]>=0.85]
print(len(res),'funcs;',len(good),'with ratio>=0.85;', sum(x[1] for x in good),'bytes of',sum(x[1] for x in res))
