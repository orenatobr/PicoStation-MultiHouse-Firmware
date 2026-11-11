"""Match V3 functions to the upstream baseline ELF by mnemonic-sequence hash (operands masked)."""
import sys, hashlib, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)

def sig(code):
    return hashlib.sha1(" ".join(i.mnemonic for i in md.disasm(code, 0)).encode()).hexdigest()

elf = ELFFile(open(sys.argv[1], 'rb'))
text = {}
for s in elf.iter_sections():
    if s.name in ('.text', '.boot2'):
        text[s.name] = (s['sh_addr'], s.data())
base = collections.defaultdict(list)
for sym in elf.get_section_by_name('.symtab').iter_symbols():
    if sym['st_info']['type'] == 'STT_FUNC' and sym['st_size'] > 8:
        a = sym['st_value'] & ~1
        for (sa, data) in text.values():
            if sa <= a < sa + len(data):
                base[sig(data[a - sa:a - sa + sym['st_size']])].append(sym.name)

img = open(sys.argv[2], 'rb').read()
hit = miss = 0
out = []
for line in open(sys.argv[3]):
    addr, size, name = line.rstrip().split('\t')
    a, size = int(addr, 16) - 0x10000000, int(size)
    if size <= 8: continue
    s = sig(img[a:a + size])
    if s in base:
        hit += 1; out.append((addr, size, base[s][0]))
    else:
        miss += 1; out.append((addr, size, None))
print(f"matched {hit}, unmatched {miss}")
with open(sys.argv[4], 'w') as f:
    for addr, size, nm in out: f.write(f"{addr}\t{size}\t{nm or '-'}\n")
