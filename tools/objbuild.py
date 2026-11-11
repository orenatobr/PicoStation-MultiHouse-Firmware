#!/usr/bin/env python3
"""Compile one source file with the exact flags of the firmware build (from compile_commands.json).

usage: objbuild.py <build-dir> <source.cpp|c> [out.o]
Run a full tools/build.sh once first so generated headers (pio, pinout) exist.
"""
import json, os, shlex, subprocess, sys

build, src = sys.argv[1], os.path.abspath(sys.argv[2])
out = sys.argv[3] if len(sys.argv) > 3 else '/tmp/objbuild/' + os.path.basename(src) + '.o'
os.makedirs(os.path.dirname(out), exist_ok=True)
db = json.load(open(os.path.join(build, 'compile_commands.json')))
# any C++ or C entry works as a flag template
want_cpp = src.endswith(('.cpp', '.cc'))
tmpl = next(e for e in db if e['file'].endswith(('.cpp',) if want_cpp else ('.c',)) and 'third_party/SD-fatfs' not in e['file'])
args = shlex.split(tmpl['command'])
clean, skip = [], False
for i, a in enumerate(args):
    if skip: skip = False; continue
    if a in ('-o', '-MF', '-MT'): skip = True; continue
    if a in ('-c', '-MD') or a == tmpl['file']: continue
    clean.append(a)
cmd = clean + ['-c', src, '-o', out]
r = subprocess.run(cmd, cwd=tmpl['directory'])
sys.exit(r.returncode if r.returncode else print(out) or 0)
