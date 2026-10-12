#!/usr/bin/env python3
"""Runs a CMake build's tests under another architecture of a universal (macOS) build: `arch -<arch> <test command>`.

ctest itself has no launcher option before CMake 3.29, so this reads the test list from `ctest --show-only=json-v1`
and runs each test's command as ctest would (its working directory and ENVIRONMENT property), under `arch`. `arch`
refuses to start a binary that has no slice for the architecture, so a test that runs has run on it.

usage: ctest_arch.py <build dir> <arch> [--timeout seconds]     e.g. ctest_arch.py build/emu x86_64
"""
import argparse, json, os, subprocess, sys, time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('build')
    ap.add_argument('arch')
    ap.add_argument('--timeout', type=float, default=1500)
    a = ap.parse_args()
    info = json.loads(subprocess.run(['ctest', '--test-dir', a.build, '--show-only=json-v1'], check=True,
                                     capture_output=True, text=True).stdout)
    tests = info.get('tests', [])
    if not tests:
        sys.exit(f'no tests in {a.build}')
    failed = []
    for t in tests:
        props = {p['name']: p['value'] for p in t.get('properties', [])}
        if props.get('DISABLED'):
            continue
        env = dict(os.environ)
        for kv in props.get('ENVIRONMENT', []):
            k, _, v = kv.partition('=')
            env[k] = v
        cmd = ['arch', '-' + a.arch] + t['command']
        t0 = time.monotonic()
        try:
            r = subprocess.run(cmd, cwd=props.get('WORKING_DIRECTORY', a.build), env=env, capture_output=True,
                               text=True, timeout=a.timeout)
            ok, out = r.returncode == 0, r.stdout + r.stderr
        except subprocess.TimeoutExpired as e:
            text = lambda x: x.decode(errors='replace') if isinstance(x, bytes) else (x or '')
            ok, out = False, f'timeout after {a.timeout} s\n' + text(e.stdout) + text(e.stderr)
        print(f"{'Passed' if ok else '***Failed'}  {t['name']} ({a.arch}, {time.monotonic() - t0:.1f} s)", flush=True)
        if not ok:
            failed.append(t['name'])
            print(out[-6000:], flush=True)
    print(f"{len(tests) - len(failed)}/{len(tests)} tests passed under {a.arch}")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
