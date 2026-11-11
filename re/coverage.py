#!/usr/bin/env python3
"""Summarize how much of the original V3 code the rebuilt ELF reproduces.

usage: coverage.py <fuzzy.tsv>      (output of fuzzy.py <rebuilt.elf> mh_v3.bin functions.tsv fuzzy.tsv)
Buckets V3 functions by size weighted best mnemonic similarity against the rebuilt firmware, split
into the region that holds the Multi House specific card layer and everything else. The flash copy
of the RAM image (0x100183cc..0x10021274) is skipped, its code is counted at its RAM address.
"""
import sys

RAM_COPY = (0x100183cc, 0x10021274)
MH = (0x10021274, 0x10030190)
buckets = [(0.95, '>= 0.95'), (0.85, '0.85 - 0.95'), (0.60, '0.60 - 0.85'), (0.0, '< 0.60')]


def classify(r):
    for lo, name in buckets:
        if r >= lo:
            return name


def main():
    stats = {}
    for line in open(sys.argv[1]):
        a, size, ratio, _ = line.rstrip('\n').split('\t')
        a, size, ratio = int(a, 16), int(size), float(ratio)
        if RAM_COPY[0] <= a < RAM_COPY[1] or size < 12:
            continue
        region = 'multi house region' if MH[0] <= a < MH[1] else 'rest of image'
        stats.setdefault(region, {}).setdefault(classify(ratio), 0)
        stats[region][classify(ratio)] += size
    for region, d in stats.items():
        tot = sum(d.values())
        print(f'{region}: {tot} bytes')
        for _, name in buckets:
            v = d.get(name, 0)
            print(f'  {name:12} {v:7} bytes  {100 * v / tot:5.1f}%')


main()
