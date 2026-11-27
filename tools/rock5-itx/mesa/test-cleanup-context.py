#!/usr/bin/env python3
"""Compile Mesa's actual csf_cleanup_context() against a lost-context fixture.

usage: test-cleanup-context.py --source MESA_TREE

A lost context's teardown must not abort (airos-works/haiku#209): its last job
ended in an error, which the Haiku wait reports, and the kernel may refuse the
destroys. Before the fix the asserts in the function abort here (exit 134).
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
args = parser.parse_args()
text = (args.source / 'src/gallium/drivers/panfrost/pan_csf.c').read_text()

marker = '\nGENX(csf_cleanup_context)('
assert text.count(marker) == 1, 'csf_cleanup_context not found'
at = text.index(marker)
start = text.rfind('\n', 0, at - 1) + 1
end = text.index('{', at) + 1
depth = 1
while depth:
    depth += {'{': 1, '}': -1}.get(text[end], 0)
    end += 1

with tempfile.TemporaryDirectory(prefix='mesa-cleanup-context-') as directory:
    root = Path(directory)
    (root / 'cleanup-function.inc').write_text(text[start:end] + '\n')
    binary = root / 'cleanup-context-test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', '-I' + str(root),
                    str(HERE / 'cleanup-context-test.c'), '-o', str(binary)],
                   check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            timeout=10, env=dict(os.environ,
                                                 UBSAN_OPTIONS='halt_on_error=1'))
    print(result.stdout + result.stderr, end='')
    if result.returncode != 0 or 'MESA_HAIKU_CLEANUP_CONTEXT_TEST_PASS' not in result.stdout:
        raise SystemExit('FAILED (exit %d)' % result.returncode)
    print('pass')
