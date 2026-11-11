#!/usr/bin/env python3
"""Compare functions of a built ELF with the original V3 image and report similarity.

The ledger file (re/ledger.tsv) has one function per line, tab separated:
    symbol   v3_addr   [v3_size]   [note]
`symbol` is matched exactly against the ELF symbol table (mangled name for C++).
`v3_addr` is the address in the original image (flash 0x10xxxxxx or RAM 0x200000c0..).
`v3_size` defaults to the symbol size of the rebuilt function.

usage: ledger.py <elf> <v3.bin> <ledger.tsv|ledger-dir> [--show SYMBOL] [--markdown OUT.md]
Similarity is difflib's ratio over the instruction mnemonic sequence (operands ignored), so it
tolerates register allocation and literal pool differences but not control flow differences.
"""
import os, sys, difflib, argparse
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = False


def v3_off(a):
    if 0x10000000 <= a < 0x10041000: return a - 0x10000000
    if 0x200000c0 <= a < 0x200000c0 + 0x8ea8: return a - 0x200000c0 + 0x183cc
    raise ValueError(hex(a))


def mnems(code):
    return [i.mnemonic for i in md.disasm(code, 0)]


def load_elf(path):
    """Return (symbols, body) for an executable or a relocatable object.

    symbols: name -> (key, size); body(key, size) -> bytes. For ET_REL the key is
    (section index, offset), for ET_EXEC it is the load address.
    """
    elf = ELFFile(open(path, 'rb'))
    rel = elf.header['e_type'] == 'ET_REL'
    secs = [(s['sh_addr'], s.data()) for s in elf.iter_sections()
            if s['sh_type'] == 'SHT_PROGBITS' and s['sh_flags'] & 2 and s['sh_size']]
    syms = {}
    for s in elf.get_section_by_name('.symtab').iter_symbols():
        if s['st_info']['type'] == 'STT_FUNC' and s['st_size']:
            if rel:
                syms[s.name] = ((s['st_shndx'], s['st_value'] & ~1), s['st_size'])
            else:
                syms[s.name] = (s['st_value'] & ~1, s['st_size'])

    def body(key, size):
        if rel:
            idx, off = key
            return elf.get_section(idx).data()[off:off + size]
        for sa, d in secs:
            if sa <= key < sa + len(d):
                return d[key - sa:key - sa + size]
        raise KeyError(hex(key))
    return syms, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf'); ap.add_argument('v3'); ap.add_argument('ledger')
    ap.add_argument('--show'); ap.add_argument('--markdown')
    a = ap.parse_args()
    syms, body = load_elf(a.elf)
    img = open(a.v3, 'rb').read()
    rows = []
    paths = ([os.path.join(a.ledger, f) for f in sorted(os.listdir(a.ledger)) if f.endswith('.tsv')]
             if os.path.isdir(a.ledger) else [a.ledger])
    lines = [ln for q in paths for ln in open(q)]
    for line in lines:
        if not line.strip() or line.startswith('#'): continue
        f = (line.rstrip('\n').split('\t') + ['', '', ''])[:4]
        sym, addr, size, note = f
        addr = int(addr, 16)
        if sym not in syms:
            rows.append((sym, addr, 0, 0, None, note, None)); continue
        saddr, ssize = syms[sym]
        vsize = int(size, 0) if size else ssize
        mine = mnems(body(saddr, ssize))
        theirs = mnems(img[v3_off(addr):v3_off(addr) + vsize])
        r = difflib.SequenceMatcher(None, mine, theirs, autojunk=False)
        rows.append((sym, addr, ssize, vsize, r.ratio(), note, (mine, theirs, r)))
        if a.show == sym:
            for tag, i1, i2, j1, j2 in r.get_opcodes():
                if tag == 'equal': continue
                print(f'{tag:8} mine[{i1}:{i2}]={mine[i1:i2]} v3[{j1}:{j2}]={theirs[j1:j2]}')
    if a.show: return
    out = []
    for sym, addr, ms, vs, ratio, note, _ in rows:
        out.append(f'{sym:48} {addr:#010x} mine={ms:5} v3={vs:5} ' + ('MISSING' if ratio is None else f'{ratio:5.2f}') + ('  ' + note if note else ''))
    print('\n'.join(out))
    done = [r for r in rows if r[4] is not None]
    if done:
        w = sum(r[3] * r[4] for r in done) / max(1, sum(r[3] for r in done))
        print(f'\n{len(done)}/{len(rows)} functions built, size-weighted similarity {w:.3f}')
    if a.markdown:
        with open(a.markdown, 'w') as f:
            f.write('| Function | V3 address | Rebuilt bytes | V3 bytes | Similarity | Note |\n|---|---|---|---|---|---|\n')
            for sym, addr, ms, vs, ratio, note, _ in rows:
                f.write(f'| `{sym}` | `{addr:#010x}` | {ms} | {vs} | {"missing" if ratio is None else f"{ratio:.2f}"} | {note} |\n')


if __name__ == '__main__':
    main()
