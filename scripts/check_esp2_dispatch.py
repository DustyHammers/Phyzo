#!/usr/bin/env python3
"""Checks that every ESP2 line routine reaches the next line by a jump, never a call (F-57 / F-64).

The ESP2's threaded dispatch (phyzo-emu/src/esp2_core.cpp) ends each line routine (runT<...>, runStep, runNop) by
running the next line's routine as a guaranteed tail call ([[clang::musttail]]). This compiles esp2_core.cpp for each
architecture asked for, disassembles it, and for every line routine classifies each indirect branch by where its
target was loaded from: Decoded::run (the next line) or anything else (the ALU calls). Fails if a routine reaches the
next line with a call (arm64 blr, x86-64 call) or has no next-line jump at all.

usage: check_esp2_dispatch.py [--cxx clang++] [--objdump objdump] --arch arm64 --arch x86_64 [-- extra compiler flags]
On macOS, --arch uses -arch; elsewhere use --target (e.g. --target aarch64-linux-gnu) with extra flags as needed.
"""
import argparse, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "phyzo-emu", "src")

OFFSETS_CPP = r'''
#include <cstddef>
#include <cstdio>
#include "esp2_core.h"
int main() { std::printf("%zu %zu\n", offsetof(Esp2Core::Decoded, run), offsetof(Esp2Core::Decoded, aluFn)); }
'''


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"failed ({r.returncode}): {' '.join(cmd)}\n{r.stderr}")
    return r.stdout


def offsets(cxx, tmp):
    src, exe = os.path.join(tmp, "offsets.cpp"), os.path.join(tmp, "offsets")
    with open(src, "w") as f:
        f.write(OFFSETS_CPP)
    run([cxx, "-std=c++17", "-I" + SRC, src, "-o", exe])
    run_off, alu_off = (int(x) for x in run([exe]).split())
    return run_off, alu_off


def functions(dis, x86):
    """Instructions per function, comments removed (x86: '#' / '##'; arm64: '//' or ';')."""
    funcs, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.*)>:$', line.strip())
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        m = re.match(r'^\s*[0-9a-f]+:\s+(.*)$', line)
        if cur is not None and m:
            text = m.group(1)
            text = text.split('#', 1)[0] if x86 else re.split(r'//|;', text, maxsplit=1)[0]
            funcs[cur].append(text.strip())
    return funcs


def mem_offset(text):
    """The displacement of a memory operand: arm64 [xN, #0x38] / x86 0x38(%reg)."""
    if re.search(r'\(%\w*,%\w+,\d\)', text):
        return 'table'
    m = re.search(r'\[\w+, #(-?0x[0-9a-f]+|-?\d+)\]', text)
    if m:
        return int(m.group(1), 0)
    m = re.search(r'(-?0x[0-9a-f]+|-?\d+)\(%\w+\)', text)
    if m:
        return int(m.group(1), 0)
    if re.search(r'\(%\w+\)', text) or re.search(r'\[\w+\]', text):
        return 0
    return None


def source_of(ins, i, reg, x86):
    """Where the register used by the indirect branch at ins[i] was loaded from: an offset, 'table' or '?'."""
    for j in range(i - 1, -1, -1):
        t = ins[j].strip()
        m = re.match(r'(\w+)\s+(.*)', t)
        if not m:
            continue
        op, args = m.group(1), m.group(2)
        if x86:
            # AT&T: op src, dst
            parts = [a.strip() for a in re.split(r',(?![^(]*\))', args)]
            if parts and parts[-1] == reg and op.startswith('mov'):
                if len(parts) == 2 and re.search(r'\(%\w+(,%\w+,\d)?\)', parts[0]):
                    if re.search(r'\(%\w+,%\w+,\d\)', parts[0]):
                        return 'table'
                    return mem_offset(parts[0])
                return '?'
            if parts and parts[-1] == reg and not op.startswith(('cmp', 'test', 'push')):
                return '?'
        else:
            parts = [a.strip() for a in re.split(r',(?![^\[]*\])', args)]
            if parts and parts[0] == reg and op not in ('str', 'stp', 'stur', 'cmp', 'tst', 'cbz', 'cbnz', 'b', 'bl', 'br', 'blr', 'ret'):
                if op.startswith('ldr') or op == 'ldur':
                    mem = ','.join(parts[1:])
                    if re.search(r'\[\w+, [xw]\d+', mem):
                        return 'table'
                    return mem_offset(mem)
                return '?'
    return '?'


def check(dis, run_off, x86):
    funcs = functions(dis, x86)
    routines = {n: ins for n, ins in funcs.items() if re.search(r'Esp2Core::run(T<|Step|Nop)', n)}
    by_jump, by_call, none, other = [], [], [], {}
    for name, ins in sorted(routines.items()):
        nexts = []
        for i, t in enumerate(ins):
            if x86:
                m = re.match(r'(jmp\w*|call\w*)\s+\*(.*)$', t)
                if not m:
                    continue
                kind = 'jump' if m.group(1).startswith('jmp') else 'call'
                target = m.group(2).strip()
                src = mem_offset(target) if '(' in target else source_of(ins, i, target, True)
            else:
                m = re.match(r'(br|blr)\s+(x\d+)$', t)
                if not m:
                    continue
                kind = 'jump' if m.group(1) == 'br' else 'call'
                src = source_of(ins, i, m.group(2), False)
            if src == run_off:
                nexts.append(kind)
            else:
                key = f"{kind} via {'offset %d' % src if isinstance(src, int) else src}"
                other[key] = other.get(key, 0) + 1
        if not nexts:
            none.append(name)
        elif all(k == 'jump' for k in nexts):
            by_jump.append(name)
        else:
            by_call.append(name)
    return routines, by_jump, by_call, none, other


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cxx', default='clang++')
    ap.add_argument('--objdump', default='objdump')
    ap.add_argument('--arch', action='append', default=[], help='macOS -arch value (arm64, x86_64)')
    ap.add_argument('--target', action='append', default=[], help='clang --target triple')
    ap.add_argument('extra', nargs='*', help='extra compiler flags (after --)')
    a = ap.parse_args()
    builds = [('-arch', x) for x in a.arch] + [('--target=', t) for t in a.target]
    if not builds:
        ap.error('give at least one --arch or --target')
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        run_off, alu_off = offsets(a.cxx, tmp)
        print(f"Decoded::run at offset {run_off}, Decoded::aluFn at {alu_off}")
        for flag, value in builds:
            obj = os.path.join(tmp, f"esp2_{value}.o")
            cmd = [a.cxx, '-std=c++17', '-O3', '-DNDEBUG'] + ([flag + value] if flag.endswith('=') else [flag, value]) + ['-I' + SRC] + a.extra + \
                  ['-c', os.path.join(SRC, 'esp2_core.cpp'), '-o', obj]
            run(cmd)
            dis = run([a.objdump, '-d', '-C', '--no-show-raw-insn', obj])
            x86 = 'x86' in value or 'i386' in value or 'amd64' in value
            routines, by_jump, by_call, none, other = check(dis, run_off, x86)
            print(f"{value}: {len(routines)} line routines; next line by jump: {len(by_jump)}, by call: {len(by_call)}, "
                  f"not found: {len(none)}; other indirect branches: {other}")
            for n in by_call:
                print(f"  CALL: {n}")
            for n in none:
                print(f"  NO NEXT-LINE JUMP: {n}")
            if not routines or by_call or none:
                failed = True
    print('esp2 dispatch: ' + ('FAILED' if failed else 'every line routine jumps to the next line'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
