"""Print Ghidra-decompiled functions with DAT_xxxxxxxx literals replaced by the dword read from the image.

usage: show.py <all.c> <image.bin> <addr> [<addr> ...]
"""
import re, struct, sys

def load(all_c):
    parts = re.split(r'(?m)^// ==== ', open(all_c).read())
    return {p.split(' ', 1)[0]: p for p in parts[1:]}

def off(a):
    if 0x10000000 <= a < 0x10041000: return a - 0x10000000
    if 0x200000c0 <= a < 0x200000c0 + 0x8ea8: return a - 0x200000c0 + 0x183cc
    return None

def resolve(txt, img):
    def sub(m):
        a = int(m.group(1), 16); o = off(a)
        if o is None or o + 4 > len(img): return m.group(0)
        return '0x%x/*%s*/' % (struct.unpack_from('<I', img, o)[0], m.group(0))
    return re.sub(r'\bDAT_([0-9a-fA-F]{8})\b', sub, txt)

if __name__ == '__main__':
    idx = load(sys.argv[1]); img = open(sys.argv[2], 'rb').read()
    for a in sys.argv[3:]:
        print('//====', resolve(idx.get(a, 'MISSING ' + a), img))
