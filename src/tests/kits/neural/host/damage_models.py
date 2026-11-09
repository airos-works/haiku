#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""Checks that the Neural Kit's kernels stand models that are wrong.

    damage_models.py [--host <neural_host>] [--count N] [--seed N]
        <directory with models> <work directory>

A model file comes from anywhere: whatever it says, the kit has to load and
run it or turn it down, and must not read or write outside its memory doing
so. This takes the models make_models.py made, changes each in a few places
(a dimension, a stride, an axis, a scale, the order of the inputs, the
numbers of a constant that holds a shape) and runs the results with
neural_host. Each run has to end with status 0 (it ran) or 1 (refused);
anything else, or a report of a sanitizer, is a failure. Build neural_host
with CXXFLAGS="-O1 -g -fsanitize=address,undefined" for the latter.

Needs: pip install numpy flatbuffers ai-edge-litert
"""

import argparse
import glob
import os
import subprocess
import sys
import zlib

import flatbuffers
import numpy as np
from ai_edge_litert import schema_py_generated as schema

# values that are wrong more often than not
NUMBERS = (0, 1, 1, 2, 2, 3, 4, 5, 7, 8, 9, 16, 31, -1, -2, -3, 255, 1000,
    65536, 2 ** 31 - 1, -2 ** 31)
SIZES = (1, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 11, 16, 0, -1, 300)
SCALES = (0.0, -0.5, 1e-30, 1e30, float('nan'), float('inf'), 1.0, 0.004,
    3.5e-5, 250.0)


def damage(model, rng):
    """Changes the model in one to three places; returns what was done."""
    subgraph = model.subgraphs[0]
    done = []
    for _ in range(rng.integers(1, 4)):
        kind = rng.integers(0, 8)
        operator = subgraph.operators[rng.integers(len(subgraph.operators))]
        tensor = subgraph.tensors[rng.integers(len(subgraph.tensors))]

        if kind == 0 and tensor.shape is not None and len(tensor.shape) > 0:
            # another size; a constant then has the wrong amount of data,
            # unless the sizes still multiply to the same
            shape = [int(size) for size in tensor.shape]
            shape[rng.integers(len(shape))] = int(rng.choice(SIZES))
            tensor.shape = shape
            done.append('shape')
        elif kind == 1 and tensor.shape is not None:
            # another rank
            shape = [int(size) for size in tensor.shape]
            if len(shape) > 0 and rng.integers(2) == 0:
                del shape[rng.integers(len(shape))]
            else:
                shape.insert(rng.integers(len(shape) + 1), 1)
            tensor.shape = shape
            done.append('rank')
        elif kind == 2 and operator.builtinOptions is not None:
            # a parameter of an operator
            fields = [name for name, value
                in vars(operator.builtinOptions).items()
                if isinstance(value, (int, bool))]
            if fields:
                name = fields[rng.integers(len(fields))]
                value = getattr(operator.builtinOptions, name)
                if isinstance(value, bool):
                    value = not value
                else:
                    value = int(rng.choice(NUMBERS[:17]))
                setattr(operator.builtinOptions, name, value)
                done.append(name)
        elif kind == 3 and tensor.quantization is not None \
                and tensor.quantization.scale is not None:
            quantization = tensor.quantization
            choice = rng.integers(4)
            if choice == 0:
                quantization.scale = [float(rng.choice(SCALES))
                    for scale in quantization.scale]
            elif choice == 1:
                quantization.zeroPoint = [int(rng.choice(NUMBERS))
                    for point in quantization.scale]
            elif choice == 2:
                quantization.quantizedDimension = int(rng.choice(NUMBERS))
            else:
                quantization.scale = list(quantization.scale)[:-1] \
                    or [0.5, 0.25]
            done.append('quantization')
        elif kind == 4:
            # the numbers of a small constant of integers: a shape, axes,
            # paddings, where a slice begins
            constants = [index for index, candidate
                in enumerate(subgraph.tensors)
                if candidate.buffer != 0 and candidate.type in (
                    schema.TensorType.INT32, schema.TensorType.INT64)
                and model.buffers[candidate.buffer].data is not None
                and len(model.buffers[candidate.buffer].data) <= 64]
            if constants:
                index = constants[rng.integers(len(constants))]
                buffer = model.buffers[subgraph.tensors[index].buffer]
                dtype = np.int32 if subgraph.tensors[index].type \
                    == schema.TensorType.INT32 else np.int64
                data = bytes(buffer.data)
                data = data[:len(data) - len(data) % np.dtype(dtype).itemsize]
                values = np.frombuffer(data, dtype=dtype).copy()
                if len(values) > 0:
                    values[rng.integers(len(values))] = rng.choice(NUMBERS)
                    buffer.data = np.frombuffer(values.tobytes(),
                        dtype=np.uint8)
                    done.append('constant')
        elif kind == 5:
            # the inputs of an operator
            inputs = [int(index) for index in operator.inputs]
            choice = rng.integers(4)
            if choice == 0 and len(inputs) > 1:
                first, second = rng.choice(len(inputs), 2, replace=False)
                inputs[first], inputs[second] = inputs[second], inputs[first]
            elif choice == 1 and len(inputs) > 0:
                del inputs[-1]
            elif choice == 2 and len(inputs) > 0:
                inputs[rng.integers(len(inputs))] = -1
            elif len(inputs) > 0:
                inputs.append(inputs[rng.integers(len(inputs))])
            operator.inputs = inputs
            done.append('inputs')
        elif kind == 6:
            # another type, with or without data that fits it
            tensor.type = int(rng.choice((schema.TensorType.FLOAT32,
                schema.TensorType.UINT8, schema.TensorType.INT8,
                schema.TensorType.INT16, schema.TensorType.INT32,
                schema.TensorType.INT64, schema.TensorType.FLOAT16,
                schema.TensorType.BOOL)))
            done.append('type')
        elif kind == 7 and tensor.buffer != 0 \
                and model.buffers[tensor.buffer].data is not None:
            # a constant becomes something computed, or loses its end
            if rng.integers(2) == 0:
                tensor.buffer = 0
            else:
                buffer = model.buffers[tensor.buffer]
                buffer.data = np.asarray(buffer.data)[:len(buffer.data) // 2]
            done.append('buffer')
    return done


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='/tmp/neural-host/neural_host')
    parser.add_argument('--count', type=int, default=6)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('models')
    parser.add_argument('work')
    args = parser.parse_args()

    os.makedirs(args.work, exist_ok=True)
    for old in glob.glob(os.path.join(args.work, 'failed-*.tflite')):
        os.remove(old)
    path = os.path.join(args.work, 'damaged.tflite')
    output = os.path.join(args.work, 'outputs.bin')

    ran = refused = 0
    failures = []
    for source in sorted(glob.glob(os.path.join(args.models, '*.tflite'))):
        with open(source, 'rb') as file:
            original = file.read()
        rng = np.random.default_rng(
            zlib.crc32(os.path.basename(source).encode()) + args.seed)

        for number in range(args.count):
            model = schema.ModelT.InitFromPackedBuf(original, 0)
            done = damage(model, rng)
            if not done:
                continue
            builder = flatbuffers.Builder(1024)
            try:
                builder.Finish(model.Pack(builder), b'TFL3')
            except (TypeError, ValueError, OverflowError):
                # a number the file format has no room for
                continue
            with open(path, 'wb') as file:
                file.write(builder.Output())

            what = '%s #%d (%s)' % (os.path.basename(source), number,
                ', '.join(done))
            try:
                result = subprocess.run([args.host, 'run', path, 'random',
                    output], capture_output=True, text=True, timeout=120)
            except subprocess.TimeoutExpired:
                failures.append((what, 'took too long', path))
                result = None
            if result is not None:
                if 'Sanitizer' in result.stderr \
                        or 'runtime error' in result.stderr \
                        or result.returncode not in (0, 1):
                    failures.append((what, 'status %d\n%s'
                        % (result.returncode, result.stderr[-3000:]), path))
                elif result.returncode == 0:
                    ran += 1
                else:
                    refused += 1

            if failures and failures[-1][0] == what:
                # keep the model that did it
                kept = os.path.join(args.work, 'failed-%d.tflite'
                    % len(failures))
                os.replace(path, kept)
                print('FAILED: %s, kept as %s\n%s' % (what, kept,
                    failures[-1][1]))

    for leftover in (path, output):
        if os.path.exists(leftover):
            os.remove(leftover)
    print('%d damaged models ran, %d were refused, %d failed'
        % (ran, refused, len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
