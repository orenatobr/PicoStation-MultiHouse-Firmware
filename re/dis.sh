#!/bin/bash
# Disassemble the original V3 image (Thumb): dis.sh <addr> [length-bytes]
# Flash addresses 0x10xxxxxx and the RAM image 0x200000c0.. (crt0 copy of flash 0x100183cc) both work.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
A=$((16#${1#0x})); L=${2:-128}
OBJDUMP=${OBJDUMP:-$HOME/.cache/picostation-fw/tc/bin/arm-none-eabi-objdump}
if [ $A -ge $((16#20000000)) ]; then ADJ=$((16#200000c0 - 16#183cc)); else ADJ=$((16#10000000)); fi
"$OBJDUMP" -D -b binary -marm -Mforce-thumb --adjust-vma=$ADJ --start-address=$A --stop-address=$((A+L)) "$D/mh_v3.bin" | sed -n '8,$p'
