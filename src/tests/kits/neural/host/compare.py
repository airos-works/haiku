#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""Checks the Neural Kit's CPU interpreter against LiteRT (TensorFlow Lite).

    compare.py [--host <neural_host>] [--seed N] [--seeds N] [--tolerance T]
        [--all] [--quiet] [--keep <directory>] [--inputs <file>]
        model.tflite...

For each model: random inputs are made, the model is run by LiteRT with its
reference kernels (no XNNPACK, so that quantized results are the canonical
ones) and by neural_host, and the outputs are compared: integer tensors
must be equal (or within --int-tolerance), float tensors within --tolerance
(relative to the largest reference value). With --all every tensor of the
model is compared, in the order the operators make them, which finds the
first operator that goes wrong. --seeds repeats all of it with as many
different inputs. --inputs gives the inputs instead of random ones: the
raw bytes of the model's inputs one after the other (for running a model
on what it is meant for: a network many layers deep may be much less
exact on noise).

A file <model>.json beside a model (make_models.py writes them) may give
the range of each float input ("inputs": [[low, high], ...]), that of int32
and int64 inputs ("int_ranges": {"<input number>": [low, high]}: inputs
that are indices must be in range) and say that
integer results may be off by some ("int_tolerance"). An output that has
the same value everywhere tests nothing and counts as a failure, unless
that file says "allow_constant".

Needs: pip install numpy ai-edge-litert
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from ai_edge_litert.interpreter import Interpreter, OpResolverType


def make_input(detail, rng, limits, int_limits=None):
    shape = tuple(detail['shape'])
    dtype = detail['dtype']
    if dtype == np.float32:
        return rng.uniform(limits[0], limits[1], shape).astype(np.float32)
    if dtype == np.bool_:
        return rng.integers(0, 2, shape).astype(np.bool_)
    if dtype in (np.uint8, np.int8, np.int16):
        info = np.iinfo(dtype)
        return rng.integers(info.min, info.max, shape, dtype=dtype,
            endpoint=True)
    if dtype in (np.int32, np.int64):
        if int_limits is not None:
            return rng.integers(int_limits[0], int_limits[1], shape,
                dtype=dtype, endpoint=True)
        return rng.integers(-1000, 1000, shape, dtype=dtype)
    raise SystemExit('input type %s not handled' % dtype)


def compare(name, reference, ours, args, settings, report):
    """Returns True when the tensors agree."""
    if reference.dtype == np.float32:
        ours = np.frombuffer(ours, dtype=np.float32).reshape(reference.shape)
        scale = max(float(np.max(np.abs(reference))), 1e-6)
        error = float(np.max(np.abs(reference - ours))) / scale \
            if reference.size else 0.0
        ok = error <= args.tolerance and np.all(np.isfinite(ours)
            == np.isfinite(reference))
        report('  %-52s float32 max relative error %.2e %s'
            % (name[:52], error, 'ok' if ok else 'MISMATCH'), ok)
        return ok

    ours = np.frombuffer(ours, dtype=reference.dtype).reshape(reference.shape)
    difference = np.abs(reference.astype(np.int64) - ours.astype(np.int64))
    worst = int(difference.max()) if difference.size else 0
    different = int(np.count_nonzero(difference))
    ok = worst <= max(args.int_tolerance, settings.get('int_tolerance', 0))
    report('  %-52s %-7s max difference %d, %d of %d differ %s'
        % (name[:52], reference.dtype, worst, different, difference.size,
            'ok' if ok else 'MISMATCH'), ok)
    return ok


def run_model(path, args, seed):
    """Returns True when the kit and the reference agree on the model."""
    # the tensors of a large model are large: what was written for a run
    # goes with it, unless it is to be kept
    directory = args.keep or tempfile.mkdtemp(prefix='neural-compare-')
    try:
        return run_model_in(path, args, seed, directory)
    finally:
        if not args.keep:
            shutil.rmtree(directory, ignore_errors=True)


def run_model_in(path, args, seed, directory):
    lines = []

    def report(line, ok=True):
        # with --quiet only what went wrong is shown, after the model's name
        if args.quiet and ok:
            lines.append(line)
            return
        for earlier in lines:
            print(earlier)
        del lines[:]
        print(line)

    report(path)
    settings = {}
    if os.path.exists(path + '.json'):
        with open(path + '.json') as file:
            settings = json.load(file)

    interpreter = Interpreter(model_path=path,
        experimental_op_resolver_type=OpResolverType.BUILTIN_REF,
        experimental_preserve_all_tensors=args.all)
    interpreter.allocate_tensors()

    rng = np.random.default_rng(seed)
    inputs = interpreter.get_input_details()
    outputs = interpreter.get_output_details()

    # The detection operator leaves the slots it has no box for as they
    # are; here they are to be zero. Zero them in the reference first.
    detection = any(op['op_name'] == 'TFLite_Detection_PostProcess'
        for op in interpreter._get_ops_details())
    if detection:
        for detail in outputs:
            interpreter.set_tensor(detail['index'],
                np.zeros(detail['shape'], dtype=detail['dtype']))

    data = []
    limits = settings.get('inputs', [])
    given = None
    if args.inputs:
        with open(args.inputs, 'rb') as file:
            given = file.read()
    for number, detail in enumerate(inputs):
        if given is not None:
            # the caller's inputs, one after the other
            size = int(np.prod(detail['shape'])) \
                * np.dtype(detail['dtype']).itemsize
            value = np.frombuffer(given[:size], dtype=detail['dtype']) \
                .reshape(detail['shape'])
            given = given[size:]
        else:
            value = make_input(detail, rng,
                limits[number] if number < len(limits) else (-1.0, 1.0),
                settings.get('int_ranges', {}).get(str(number)))
        interpreter.set_tensor(detail['index'], value)
        data.append(value.tobytes())
    if given:
        report('  %s does not hold the inputs of this model' % args.inputs,
            False)
        return False
    interpreter.invoke()

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
        report('  neural_host failed: %s' % result.stderr.strip(), False)
        return False
    report('  ' + result.stderr.strip().splitlines()[0])
    if 'runtime error' in result.stderr or 'Sanitizer' in result.stderr:
        # a build with -fsanitize found something
        report('  ' + result.stderr.strip()[-3000:], False)
        return False

    ok = True
    with open(output_path, 'rb') as file:
        ours = file.read()
    offset = 0
    for detail in outputs:
        reference = interpreter.get_tensor(detail['index'])
        size = reference.size * reference.dtype.itemsize
        if not compare(detail['name'], reference, ours[offset:offset + size],
                args, settings, report):
            ok = False
        offset += size

        # a result that is the same everywhere (all saturated, say) would
        # agree whatever the kernel did
        if reference.size > 8 and not settings.get('allow_constant') \
                and not detection and len(np.unique(reference)) == 1:
            report('  %-52s is %s everywhere: tests nothing'
                % (detail['name'][:52], reference.flat[0]), False)
            ok = False

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
                    report('  op %d %s tensor %d: size differs'
                        % (op['index'], op['op_name'], index), False)
                    ok = False
                    continue
                if not compare('op %d %s -> %d' % (op['index'],
                        op['op_name'], index), reference, tensor, args,
                        settings, report):
                    ok = False
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='/tmp/neural-host/neural_host')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--seeds', type=int, default=1)
    parser.add_argument('--quiet', action='store_true')
    parser.add_argument('--tolerance', type=float, default=1e-4)
    parser.add_argument('--int-tolerance', type=int, default=0)
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--keep')
    parser.add_argument('--inputs')
    parser.add_argument('models', nargs='+')
    args = parser.parse_args()

    failed = []
    for model in args.models:
        for seed in range(args.seed, args.seed + args.seeds):
            if not run_model(model, args, seed):
                failed.append(model)
                break
    if failed:
        print('FAILED (%d of %d): %s' % (len(failed), len(args.models),
            ' '.join(failed)))
        return 1
    print('all %d models agree' % len(args.models))
    return 0


if __name__ == '__main__':
    sys.exit(main())
