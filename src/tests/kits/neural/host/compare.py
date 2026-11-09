#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""Checks the Neural Kit's CPU interpreter against LiteRT (TensorFlow Lite).

    compare.py [--host <neural_host>] [--seed N] [--tolerance T] [--all]
        [--keep <directory>] model.tflite...

For each model: random inputs are made, the model is run by LiteRT with its
reference kernels (no XNNPACK, so that quantized results are the canonical
ones) and by neural_host, and the outputs are compared: integer tensors
must be equal (or within --int-tolerance), float tensors within --tolerance
(relative to the largest reference value). With --all every tensor of the
model is compared, in the order the operators make them, which finds the
first operator that goes wrong.

Needs: pip install numpy ai-edge-litert
"""

import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType


def make_input(detail, rng):
    shape = tuple(detail['shape'])
    dtype = detail['dtype']
    if dtype == np.float32:
        return rng.uniform(-1.0, 1.0, shape).astype(np.float32)
    if dtype == np.uint8:
        return rng.integers(0, 256, shape, dtype=np.uint8)
    if dtype == np.int8:
        return rng.integers(-128, 128, shape, dtype=np.int8)
    if dtype == np.int32:
        return rng.integers(-1000, 1000, shape, dtype=np.int32)
    raise SystemExit('input type %s not handled' % dtype)


def compare(name, reference, ours, args):
    """Returns True when the tensors agree."""
    if reference.dtype == np.float32:
        ours = np.frombuffer(ours, dtype=np.float32).reshape(reference.shape)
        scale = max(float(np.max(np.abs(reference))), 1e-6)
        error = float(np.max(np.abs(reference - ours))) / scale \
            if reference.size else 0.0
        ok = error <= args.tolerance and np.all(np.isfinite(ours))
        print('  %-52s float32 max relative error %.2e %s'
            % (name[:52], error, 'ok' if ok else 'MISMATCH'))
        return ok

    ours = np.frombuffer(ours, dtype=reference.dtype).reshape(reference.shape)
    difference = np.abs(reference.astype(np.int64) - ours.astype(np.int64))
    worst = int(difference.max()) if difference.size else 0
    different = int(np.count_nonzero(difference))
    ok = worst <= args.int_tolerance
    print('  %-52s %-7s max difference %d, %d of %d differ %s'
        % (name[:52], reference.dtype, worst, different, difference.size,
            'ok' if ok else 'MISMATCH'))
    return ok


def run_model(path, args):
    print(path)
    interpreter = Interpreter(model_path=path,
        experimental_op_resolver_type=OpResolverType.BUILTIN_REF,
        experimental_preserve_all_tensors=args.all)
    interpreter.allocate_tensors()

    rng = np.random.default_rng(args.seed)
    inputs = interpreter.get_input_details()
    outputs = interpreter.get_output_details()

    data = []
    for detail in inputs:
        value = make_input(detail, rng)
        interpreter.set_tensor(detail['index'], value)
        data.append(value.tobytes())
    interpreter.invoke()

    directory = args.keep or tempfile.mkdtemp(prefix='neural-compare-')
    os.makedirs(directory, exist_ok=True)
    input_path = os.path.join(directory, 'inputs.bin')
    output_path = os.path.join(directory, 'outputs.bin')
    with open(input_path, 'wb') as file:
        file.write(b''.join(data))

    command = [args.host, 'run', path, input_path, output_path]
    if args.all:
        command += ['--dump', directory]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print('  neural_host failed: %s' % result.stderr.strip())
        return False
    print('  ' + result.stderr.strip().splitlines()[0])

    ok = True
    with open(output_path, 'rb') as file:
        ours = file.read()
    offset = 0
    for detail in outputs:
        reference = interpreter.get_tensor(detail['index'])
        size = reference.size * reference.dtype.itemsize
        if not compare(detail['name'], reference, ours[offset:offset + size],
                args):
            ok = False
        offset += size

    if args.all:
        # every tensor an operator makes, in execution order
        for op in interpreter._get_ops_details():
            for index in op['outputs']:
                dump = os.path.join(directory, '%d.bin' % index)
                if not os.path.exists(dump):
                    continue
                reference = interpreter.get_tensor(index)
                with open(dump, 'rb') as file:
                    tensor = file.read()
                if len(tensor) != reference.size * reference.dtype.itemsize:
                    print('  op %d %s tensor %d: size differs'
                        % (op['index'], op['op_name'], index))
                    ok = False
                    continue
                if not compare('op %d %s -> %d' % (op['index'],
                        op['op_name'], index), reference, tensor, args):
                    ok = False
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='/tmp/neural-host/neural_host')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--tolerance', type=float, default=1e-4)
    parser.add_argument('--int-tolerance', type=int, default=0)
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--keep')
    parser.add_argument('models', nargs='+')
    args = parser.parse_args()

    failed = [model for model in args.models if not run_model(model, args)]
    if failed:
        print('FAILED: ' + ' '.join(failed))
        return 1
    print('all models agree')
    return 0


if __name__ == '__main__':
    sys.exit(main())
