#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""Makes small TensorFlow Lite models that test the Neural Kit's kernels.

    make_models.py <directory>

Each model has one or a few operators with random weights; compare.py runs
them through LiteRT's reference kernels and through the kit. Together they
cover every operator the kit has with every type it takes, and the cases
that are easy to get wrong: odd strides, padding and dilation, weights with
a scale per channel, shapes that broadcast, both ways to suppress
detections.

The models are written as flat buffers directly (with the schema that comes
with LiteRT), so that no TensorFlow is needed. Beside each model goes a
<model>.json that tells compare.py the range of its inputs.

Needs: pip install numpy flatbuffers ai-edge-litert
"""

import json
import math
import os
import sys
import zlib

import flatbuffers
from flatbuffers import flexbuffers
import numpy as np
from ai_edge_litert import schema_py_generated as schema

BuiltinOperator = schema.BuiltinOperator
BuiltinOptions = schema.BuiltinOptions
ActivationFunctionType = schema.ActivationFunctionType
Padding = schema.Padding

TYPES = {
    'float32': schema.TensorType.FLOAT32,
    'float16': schema.TensorType.FLOAT16,
    'uint8': schema.TensorType.UINT8,
    'int8': schema.TensorType.INT8,
    'int16': schema.TensorType.INT16,
    'int32': schema.TensorType.INT32,
    'int64': schema.TensorType.INT64,
    'bool': schema.TensorType.BOOL,
}

NONE = ActivationFunctionType.NONE
RELU = ActivationFunctionType.RELU
RELU6 = ActivationFunctionType.RELU6
RELU1 = ActivationFunctionType.RELU_N1_TO_1
SAME = Padding.SAME
VALID = Padding.VALID

MODELS = []


class Graph:
    """A model being put together: tensors, operators, inputs."""

    def __init__(self, name):
        self.name = name
        self.rng = np.random.default_rng(zlib.crc32(name.encode()))
        self.tensors = []
        self.buffers = [b'']
        self.codes = []
        self.operators = []
        self.inputs = []
        self.ranges = []
        self.int_ranges = {}
        self.int_tolerance = 0
        self.allow_constant = False

    def _add_tensor(self, shape, dtype, quant, data=None, name=None):
        tensor = schema.TensorT()
        tensor.shape = [int(size) for size in shape]
        tensor.type = TYPES[dtype]
        tensor.name = name or 't%d' % len(self.tensors)
        tensor.buffer = 0
        if data is not None:
            tensor.buffer = len(self.buffers)
            self.buffers.append(np.ascontiguousarray(data).tobytes())
        if quant is not None:
            parameters = schema.QuantizationParametersT()
            if len(quant) == 3:
                scales, zero_points, axis = quant
                parameters.scale = [float(scale) for scale in scales]
                parameters.zeroPoint = [int(point) for point in zero_points]
                parameters.quantizedDimension = int(axis)
            else:
                parameters.scale = [float(quant[0])]
                parameters.zeroPoint = [int(quant[1])]
            tensor.quantization = parameters
        self.tensors.append(tensor)
        return len(self.tensors) - 1

    def input(self, shape, dtype='float32', quant=None, low=-1.0, high=1.0,
            int_range=None):
        """A tensor the caller fills in; floats are drawn from low..high,
        int32 and int64 from int_range (both ends included) if given."""
        index = self._add_tensor(shape, dtype, quant)
        if int_range is not None:
            self.int_ranges[str(len(self.inputs))] = list(int_range)
        self.inputs.append(index)
        self.ranges.append([low, high])
        return index

    def const(self, array, quant=None):
        array = np.asarray(array)
        return self._add_tensor(array.shape, str(array.dtype), quant, array)

    def tensor(self, shape, dtype='float32', quant=None):
        return self._add_tensor(shape, dtype, quant)

    def op(self, code, inputs, outputs, options=None, custom_name=None,
            custom_options=None):
        key = (code, custom_name)
        if key not in self.codes:
            self.codes.append(key)
        operator = schema.OperatorT()
        operator.opcodeIndex = self.codes.index(key)
        operator.inputs = [int(index) for index in inputs]
        operator.outputs = [int(index) for index in outputs]
        if options is not None:
            kind = type(options).__name__[:-1]
            operator.builtinOptionsType = getattr(BuiltinOptions, kind)
            operator.builtinOptions = options
        if custom_options is not None:
            operator.customOptions = list(custom_options)
        self.operators.append(operator)

    def save(self, directory, outputs):
        model = schema.ModelT()
        model.version = 3
        model.description = 'Neural Kit test: ' + self.name
        model.operatorCodes = []
        for code, custom_name in self.codes:
            operator_code = schema.OperatorCodeT()
            operator_code.deprecatedBuiltinCode = min(code, 127)
            operator_code.builtinCode = code
            operator_code.version = 1
            if custom_name is not None:
                operator_code.customCode = custom_name
            model.operatorCodes.append(operator_code)

        subgraph = schema.SubGraphT()
        subgraph.name = self.name
        subgraph.tensors = self.tensors
        subgraph.operators = self.operators
        subgraph.inputs = self.inputs
        subgraph.outputs = [int(index) for index in outputs]
        model.subgraphs = [subgraph]

        model.buffers = []
        for data in self.buffers:
            buffer = schema.BufferT()
            if data:
                buffer.data = np.frombuffer(data, dtype=np.uint8)
            model.buffers.append(buffer)

        builder = flatbuffers.Builder(1024)
        builder.Finish(model.Pack(builder), b'TFL3')
        path = os.path.join(directory, self.name + '.tflite')
        with open(path, 'wb') as file:
            file.write(builder.Output())
        with open(path + '.json', 'w') as file:
            json.dump({'inputs': self.ranges,
                'int_ranges': self.int_ranges,
                'int_tolerance': self.int_tolerance,
                'allow_constant': self.allow_constant}, file)


def model(function):
    """Registers a function that makes models: it yields or returns
    (graph, outputs) pairs."""
    MODELS.append(function)
    return function


def options(kind, **fields):
    result = getattr(schema, kind + 'T')()
    for name, value in fields.items():
        if not hasattr(result, name):
            raise AttributeError('%s has no %s' % (kind, name))
        setattr(result, name, value)
    return result


def output_size(padding, size, filter_size, stride, dilation=1):
    effective = (filter_size - 1) * dilation + 1
    if padding == SAME:
        return (size + stride - 1) // stride
    return (size + stride - effective) // stride


# what the stored values of the quantized test tensors mean
QUANT = {
    'uint8': (0.02, 121),
    'int8': (0.02, -7),
}

# how far stored values lie from the zero point on average
SPREAD = 74.0


def integer_range(dtype):
    info = np.iinfo(dtype)
    return info.min, info.max


def random_weights(graph, shape, dtype, channel_axis=None, per_channel=False):
    """Weights of a convolution: (tensor, list of scales per channel)."""
    if dtype == 'float32':
        values = graph.rng.uniform(-1, 1, shape).astype(np.float32)
        return graph.const(values), None
    if dtype == 'uint8':
        values = graph.rng.integers(0, 256, shape, dtype=np.uint8)
        scale = 0.011
        return graph.const(values, (scale, 119)), [scale]
    values = graph.rng.integers(-127, 128, shape, dtype=np.int8)
    if per_channel:
        channels = shape[channel_axis]
        scales = graph.rng.uniform(0.004, 0.02, channels)
        return graph.const(values, (scales, [0] * channels, channel_axis)), \
            [float(np.float32(scale)) for scale in scales]
    scale = 0.011
    return graph.const(values, (scale, 0)), [scale]


def accumulator_quant(dtype, input_scale, weight_scales, terms):
    """A quantization of the output of a sum of `terms` products that
    uses the range of the type without leaving it too often."""
    deviation = math.sqrt(terms) * SPREAD * SPREAD
    scale = input_scale * float(np.mean(weight_scales)) * deviation / 45.0
    return (scale, 133 if dtype == 'uint8' else -9)


def random_bias(graph, dtype, channels, input_scale, weight_scales, terms):
    if dtype == 'float32':
        return graph.const(graph.rng.uniform(-1, 1, channels)
            .astype(np.float32))
    deviation = int(math.sqrt(terms) * SPREAD * SPREAD)
    values = graph.rng.integers(-deviation, deviation + 1, channels,
        dtype=np.int32)
    if len(weight_scales) == 1:
        return graph.const(values, (input_scale * weight_scales[0], 0))
    scales = [input_scale * scale for scale in weight_scales]
    return graph.const(values, (scales, [0] * channels, 0))


def data_input(graph, shape, dtype, low=-1.0, high=1.0):
    return graph.input(shape, dtype, QUANT.get(dtype), low, high)


# -- convolutions ----------------------------------------------------------

def conv_model(name, dtype, input_shape, filters, kernel, stride=(1, 1),
        dilation=(1, 1), padding=SAME, activation=NONE, per_channel=False,
        groups=1):
    graph = Graph(name)
    batch, height, width, channels = input_shape
    input = data_input(graph, input_shape, dtype)
    weight_shape = (filters, kernel[0], kernel[1], channels // groups)
    weights, scales = random_weights(graph, weight_shape, dtype, 0,
        per_channel)
    terms = kernel[0] * kernel[1] * (channels // groups)
    quant = None
    input_scale = QUANT[dtype][0] if dtype in QUANT else None
    if dtype != 'float32':
        quant = accumulator_quant(dtype, input_scale, scales, terms)
    bias = random_bias(graph, dtype, filters, input_scale, scales, terms)
    output = graph.tensor((batch,
        output_size(padding, height, kernel[0], stride[0], dilation[0]),
        output_size(padding, width, kernel[1], stride[1], dilation[1]),
        filters), dtype, quant)
    graph.op(BuiltinOperator.CONV_2D, [input, weights, bias], [output],
        options('Conv2DOptions', padding=padding, strideH=stride[0],
            strideW=stride[1], dilationHFactor=dilation[0],
            dilationWFactor=dilation[1], fusedActivationFunction=activation))
    return graph, [output]


@model
def conv_models():
    for dtype in ('float32', 'uint8', 'int8'):
        tag = 'conv_' + dtype
        per_channel = dtype == 'int8'
        yield conv_model(tag + '_3x3', dtype, (1, 9, 11, 5), 7, (3, 3),
            per_channel=per_channel)
        yield conv_model(tag + '_1x1', dtype, (2, 6, 7, 19), 13, (1, 1),
            activation=RELU6, per_channel=per_channel)
        yield conv_model(tag + '_1x1_s2', dtype, (1, 9, 8, 16), 9, (1, 1),
            stride=(2, 3), per_channel=per_channel)
        yield conv_model(tag + '_3x3_s2_same', dtype, (1, 10, 13, 3), 8,
            (3, 3), stride=(2, 2), activation=RELU, per_channel=per_channel)
        yield conv_model(tag + '_5x3_s3x2_valid', dtype, (2, 17, 12, 4), 6,
            (5, 3), stride=(3, 2), padding=VALID, per_channel=per_channel)
        yield conv_model(tag + '_3x3_d2x3_same', dtype, (1, 12, 14, 6), 5,
            (3, 3), dilation=(2, 3), activation=RELU1,
            per_channel=per_channel)
        yield conv_model(tag + '_3x3_d2_valid', dtype, (1, 11, 11, 3), 4,
            (3, 3), dilation=(2, 2), padding=VALID,
            per_channel=per_channel)
        yield conv_model(tag + '_4x4_s2_same', dtype, (1, 7, 9, 2), 5,
            (4, 4), stride=(2, 2), per_channel=per_channel)
        yield conv_model(tag + '_groups', dtype, (1, 8, 8, 12), 9, (3, 3),
            groups=3, per_channel=per_channel)
    # signed weights with one scale for all channels
    yield conv_model('conv_int8_per_tensor', 'int8', (1, 8, 9, 6), 10,
        (3, 3), stride=(2, 1))
    # enough channels for every stretch of the inner loops
    yield conv_model('conv_uint8_wide', 'uint8', (1, 5, 5, 67), 37, (1, 1))
    yield conv_model('conv_float32_wide', 'float32', (1, 5, 5, 67), 37,
        (3, 3))


def depthwise_model(name, dtype, input_shape, multiplier, kernel,
        stride=(1, 1), dilation=(1, 1), padding=SAME, activation=NONE,
        per_channel=False):
    graph = Graph(name)
    batch, height, width, channels = input_shape
    filters = channels * multiplier
    input = data_input(graph, input_shape, dtype)
    weights, scales = random_weights(graph,
        (1, kernel[0], kernel[1], filters), dtype, 3, per_channel)
    terms = kernel[0] * kernel[1]
    quant = None
    input_scale = QUANT[dtype][0] if dtype in QUANT else None
    if dtype != 'float32':
        quant = accumulator_quant(dtype, input_scale, scales, terms)
    bias = random_bias(graph, dtype, filters, input_scale, scales, terms)
    output = graph.tensor((batch,
        output_size(padding, height, kernel[0], stride[0], dilation[0]),
        output_size(padding, width, kernel[1], stride[1], dilation[1]),
        filters), dtype, quant)
    graph.op(BuiltinOperator.DEPTHWISE_CONV_2D, [input, weights, bias],
        [output], options('DepthwiseConv2DOptions', padding=padding,
            strideH=stride[0], strideW=stride[1],
            dilationHFactor=dilation[0], dilationWFactor=dilation[1],
            depthMultiplier=multiplier, fusedActivationFunction=activation))
    return graph, [output]


@model
def depthwise_models():
    for dtype in ('float32', 'uint8', 'int8'):
        tag = 'depthwise_' + dtype
        per_channel = dtype == 'int8'
        yield depthwise_model(tag + '_3x3', dtype, (1, 9, 11, 13), 1,
            (3, 3), per_channel=per_channel)
        yield depthwise_model(tag + '_3x3_s2', dtype, (2, 10, 13, 8), 1,
            (3, 3), stride=(2, 2), activation=RELU6,
            per_channel=per_channel)
        yield depthwise_model(tag + '_m3', dtype, (1, 7, 8, 5), 3, (3, 3),
            activation=RELU, per_channel=per_channel)
        yield depthwise_model(tag + '_5x3_s2x3_valid', dtype, (1, 15, 14, 4),
            2, (5, 3), stride=(2, 3), padding=VALID,
            per_channel=per_channel)
        yield depthwise_model(tag + '_d2x3', dtype, (1, 12, 13, 6), 1,
            (3, 3), dilation=(2, 3), per_channel=per_channel)
        yield depthwise_model(tag + '_d2_valid', dtype, (1, 12, 13, 3), 2,
            (3, 3), dilation=(2, 2), padding=VALID,
            per_channel=per_channel)
        yield depthwise_model(tag + '_tall', dtype, (1, 3, 9, 35), 1,
            (5, 5), per_channel=per_channel)
    yield depthwise_model('depthwise_int8_per_tensor', 'int8', (1, 8, 9, 6),
        1, (3, 3), stride=(1, 2))


def fully_connected_model(name, dtype, input_shape, units, depth=None,
        keep=False, activation=NONE, per_channel=False, bias=True,
        weight_zero_point=None):
    graph = Graph(name)
    elements = int(np.prod(input_shape))
    depth = depth or (input_shape[-1] if keep else elements // input_shape[0])
    input = data_input(graph, input_shape, dtype)
    if dtype == 'int8' and weight_zero_point is not None:
        # only this layer takes signed weights with a zero point
        values = graph.rng.integers(-128, 128, (units, depth), dtype=np.int8)
        scales = [0.011]
        weights = graph.const(values, (scales[0], weight_zero_point))
    else:
        weights, scales = random_weights(graph, (units, depth), dtype, 0,
            per_channel)
    quant = None
    input_scale = QUANT[dtype][0] if dtype in QUANT else None
    if dtype != 'float32':
        quant = accumulator_quant(dtype, input_scale, scales, depth)
    inputs = [input, weights]
    if bias:
        inputs.append(random_bias(graph, dtype, units, input_scale, scales,
            depth))
    if keep:
        shape = tuple(input_shape[:-1]) + (units,)
    else:
        shape = (elements // depth, units)
    output = graph.tensor(shape, dtype, quant)
    graph.op(BuiltinOperator.FULLY_CONNECTED, inputs, [output],
        options('FullyConnectedOptions', keepNumDims=keep,
            fusedActivationFunction=activation))
    return graph, [output]


@model
def fully_connected_models():
    for dtype in ('float32', 'uint8', 'int8'):
        tag = 'fully_connected_' + dtype
        yield fully_connected_model(tag, dtype, (3, 37), 11)
        yield fully_connected_model(tag + '_flatten', dtype, (2, 3, 4, 5),
            70, activation=RELU)
        yield fully_connected_model(tag + '_keep', dtype, (2, 3, 4, 9), 6,
            keep=True, activation=RELU6)
        yield fully_connected_model(tag + '_no_bias', dtype, (1, 130), 9,
            bias=False)
    yield fully_connected_model('fully_connected_int8_per_channel', 'int8',
        (4, 33), 12, per_channel=True)
    yield fully_connected_model('fully_connected_int8_zero_point', 'int8',
        (4, 33), 12, weight_zero_point=11)


def transpose_conv_model(name, dtype, input_shape, filters, kernel, stride,
        padding=SAME, per_channel=False, bias=True, output_hw=None):
    graph = Graph(name)
    batch, height, width, channels = input_shape
    if output_hw is None:
        if padding == SAME:
            output_hw = (height * stride[0], width * stride[1])
        else:
            output_hw = ((height - 1) * stride[0] + kernel[0],
                (width - 1) * stride[1] + kernel[1])
    shape = (batch, output_hw[0], output_hw[1], filters)
    input = data_input(graph, input_shape, dtype)
    weights, scales = random_weights(graph,
        (filters, kernel[0], kernel[1], channels), dtype, 0, per_channel)
    terms = max(1, kernel[0] * kernel[1] * channels
        // (stride[0] * stride[1]))
    quant = None
    input_scale = QUANT[dtype][0] if dtype in QUANT else None
    if dtype != 'float32':
        quant = accumulator_quant(dtype, input_scale, scales, terms)
    inputs = [graph.const(np.array(shape, dtype=np.int32)), weights, input]
    if bias:
        inputs.append(random_bias(graph, dtype, filters, input_scale, scales,
            terms))
    output = graph.tensor(shape, dtype, quant)
    graph.op(BuiltinOperator.TRANSPOSE_CONV, inputs, [output],
        options('TransposeConvOptions', padding=padding, strideH=stride[0],
            strideW=stride[1]))
    return graph, [output]


@model
def transpose_conv_models():
    for dtype in ('float32', 'uint8', 'int8'):
        tag = 'transpose_conv_' + dtype
        per_channel = dtype == 'int8'
        yield transpose_conv_model(tag + '_3x3_s2', dtype, (1, 5, 6, 7), 4,
            (3, 3), (2, 2), per_channel=per_channel)
        yield transpose_conv_model(tag + '_4x4_s2_valid', dtype,
            (2, 4, 5, 3), 5, (4, 4), (2, 2), padding=VALID,
            per_channel=per_channel)
        yield transpose_conv_model(tag + '_2x2_s3', dtype, (1, 4, 3, 6), 3,
            (2, 2), (3, 3), padding=VALID, per_channel=per_channel)
        yield transpose_conv_model(tag + '_3x5_s1', dtype, (1, 6, 7, 9), 5,
            (3, 5), (1, 1), per_channel=per_channel, bias=False)
        # SAME padding where the output is not a multiple of the stride
        yield transpose_conv_model(tag + '_3x3_s2x3_odd', dtype,
            (1, 4, 5, 2), 6, (3, 3), (2, 3), per_channel=per_channel,
            output_hw=(7, 14))
    yield transpose_conv_model('transpose_conv_int8_per_tensor', 'int8',
        (1, 5, 5, 4), 3, (3, 3), (2, 2))


# -- pooling ----------------------------------------------------------------

def pool_model(name, code, dtype, input_shape, filter, stride, padding=SAME,
        activation=NONE):
    graph = Graph(name)
    batch, height, width, channels = input_shape
    input = data_input(graph, input_shape, dtype)
    output = graph.tensor((batch,
        output_size(padding, height, filter[0], stride[0]),
        output_size(padding, width, filter[1], stride[1]), channels), dtype,
        QUANT.get(dtype))
    graph.op(code, [input], [output], options('Pool2DOptions',
        padding=padding, strideH=stride[0], strideW=stride[1],
        filterHeight=filter[0], filterWidth=filter[1],
        fusedActivationFunction=activation))
    return graph, [output]


@model
def pool_models():
    for kind, code in (('average_pool', BuiltinOperator.AVERAGE_POOL_2D),
            ('max_pool', BuiltinOperator.MAX_POOL_2D)):
        for dtype in ('float32', 'uint8', 'int8'):
            tag = '%s_%s' % (kind, dtype)
            yield pool_model(tag + '_2x2_s2', code, dtype, (2, 8, 10, 5),
                (2, 2), (2, 2), padding=VALID)
            yield pool_model(tag + '_3x3_s2_same', code, dtype,
                (1, 9, 12, 7), (3, 3), (2, 2))
            yield pool_model(tag + '_2x3_s3x1_same', code, dtype,
                (1, 10, 7, 4), (2, 3), (3, 1), activation=RELU6)
            yield pool_model(tag + '_3x3_s1_same_relu1', code, dtype,
                (1, 5, 5, 3), (3, 3), (1, 1), activation=RELU1)
            yield pool_model(tag + '_global', code, dtype, (1, 7, 7, 33),
                (7, 7), (1, 1), padding=VALID)


# -- two tensors, element by element -----------------------------------------

BROADCASTS = (
    ('same', (2, 5, 6, 7), (2, 5, 6, 7)),
    ('channels', (1, 4, 5, 6), (6,)),
    ('mixed', (2, 1, 5, 1), (1, 4, 1, 3)),
    ('scalar', (3, 4, 5), ()),
    ('first', (1, 1, 7), (3, 4, 7)),
    ('rows', (4, 1, 6), (4, 5, 1)),
)


def broadcast_shape(first, second):
    rank = max(len(first), len(second))
    first = (1,) * (rank - len(first)) + tuple(first)
    second = (1,) * (rank - len(second)) + tuple(second)
    return tuple(max(a, b) for a, b in zip(first, second))


def binary_model(name, code, kind, dtype, first, second, activation=NONE,
        constant_second=False):
    graph = Graph(name)
    quant1 = quant2 = quant_out = None
    if dtype != 'float32':
        zero = {'uint8': (118, 131, 125), 'int8': (-10, 3, -4)}[dtype]
        quant1 = (0.021, zero[0])
        quant2 = (0.034, zero[1])
        if kind in ('add', 'sub'):
            quant_out = (0.043, zero[2])
        elif kind == 'mul':
            quant_out = (0.021 * 0.034 * 60, zero[2])
        else:
            quant1 = quant2 = quant_out = (0.021, zero[0])
    low, high = (-2.0, 2.0)
    a = graph.input(first, dtype, quant1, low, high)
    if constant_second:
        if dtype == 'float32':
            values = graph.rng.uniform(0.5, 2, second).astype(np.float32)
        else:
            values = graph.rng.integers(*integer_range(dtype), second,
                dtype=dtype, endpoint=True)
        b = graph.const(values, quant2)
    elif kind == 'div':
        b = graph.input(second, dtype, quant2, 0.5, 2.0)
    else:
        b = graph.input(second, dtype, quant2, low, high)
    output = graph.tensor(broadcast_shape(first, second), dtype, quant_out)
    kinds = {
        'add': 'AddOptions', 'sub': 'SubOptions', 'mul': 'MulOptions',
        'div': 'DivOptions',
    }
    if kind in kinds:
        option = options(kinds[kind], fusedActivationFunction=activation)
    else:
        option = options('MaximumMinimumOptions')
    graph.op(code, [a, b], [output], option)
    return graph, [output]


@model
def binary_models():
    operators = (
        ('add', BuiltinOperator.ADD), ('sub', BuiltinOperator.SUB),
        ('mul', BuiltinOperator.MUL), ('div', BuiltinOperator.DIV),
        ('maximum', BuiltinOperator.MAXIMUM),
        ('minimum', BuiltinOperator.MINIMUM),
    )
    for kind, code in operators:
        for dtype in ('float32', 'uint8', 'int8'):
            if kind == 'div' and dtype != 'float32':
                continue
            tag = '%s_%s' % (kind, dtype)
            for label, first, second in BROADCASTS:
                yield binary_model('%s_%s' % (tag, label), code, kind, dtype,
                    first, second)
            if kind in ('maximum', 'minimum'):
                continue
            yield binary_model(tag + '_relu', code, kind, dtype,
                (1, 5, 5, 4), (1, 5, 5, 4), RELU)
            yield binary_model(tag + '_relu6', code, kind, dtype,
                (1, 5, 5, 4), (5, 4), RELU6)
            yield binary_model(tag + '_relu1', code, kind, dtype, (4,),
                (2, 3, 4), RELU1)
            yield binary_model(tag + '_constant', code, kind, dtype,
                (2, 3, 8), (8,), constant_second=True)


# -- one tensor, element by element ------------------------------------------

def unary_model(name, code, dtype, shape, input_quant=None,
        output_quant=None, option=None, low=-6.0, high=6.0):
    graph = Graph(name)
    input = graph.input(shape, dtype, input_quant, low, high)
    output = graph.tensor(shape, dtype, output_quant)
    graph.op(code, [input], [output], option)
    return graph, [output]


@model
def activation_models():
    # large enough for every stored value to come up
    shape = (3, 9, 10, 6)
    zero = {'uint8': 128, 'int8': 0}
    lowest = {'uint8': 0, 'int8': -128}
    for dtype in ('float32', 'uint8', 'int8'):
        quantized = dtype != 'float32'
        in_quant = (0.05, zero[dtype] - 9) if quantized else None

        # the rectifiers may change the scale on the way
        out_quant = (0.031, lowest[dtype] + 3) if quantized else None
        yield unary_model('relu_' + dtype, BuiltinOperator.RELU, dtype,
            shape, in_quant, out_quant)
        yield unary_model('relu6_' + dtype, BuiltinOperator.RELU6, dtype,
            shape, in_quant, out_quant, low=-8, high=8)
        if quantized:
            yield unary_model('relu_%s_same_scale' % dtype,
                BuiltinOperator.RELU, dtype, shape, in_quant, in_quant)
            yield unary_model('relu6_%s_fine' % dtype,
                BuiltinOperator.RELU6, dtype, shape, in_quant,
                (6.0 / 255, lowest[dtype]))

        out_quant = (1.0 / 256, lowest[dtype]) if quantized else None
        yield unary_model('logistic_' + dtype, BuiltinOperator.LOGISTIC,
            dtype, shape, in_quant, out_quant, low=-12, high=20)
        if quantized:
            yield unary_model('logistic_%s_coarse' % dtype,
                BuiltinOperator.LOGISTIC, dtype, shape,
                (0.13, zero[dtype] + 20), out_quant)

        out_quant = (1.0 / 128, zero[dtype]) if quantized else None
        yield unary_model('tanh_' + dtype, BuiltinOperator.TANH, dtype,
            shape, in_quant, out_quant)
        if quantized:
            yield unary_model('tanh_%s_fine' % dtype, BuiltinOperator.TANH,
                dtype, shape, (0.011, zero[dtype] - 30), out_quant)

        out_quant = (0.027, zero[dtype] - 110) if quantized else None
        yield unary_model('hard_swish_' + dtype, BuiltinOperator.HARD_SWISH,
            dtype, shape, in_quant, out_quant, option=options(
                'HardSwishOptions'))
        if quantized:
            # scales that take the other branches of the fixed point code
            yield unary_model('hard_swish_%s_small' % dtype,
                BuiltinOperator.HARD_SWISH, dtype, shape,
                (0.0009, zero[dtype] + 5), (0.0011, zero[dtype] - 3),
                option=options('HardSwishOptions'))
            yield unary_model('hard_swish_%s_large' % dtype,
                BuiltinOperator.HARD_SWISH, dtype, shape,
                (0.4, zero[dtype] - 20), (0.5, zero[dtype] - 90),
                option=options('HardSwishOptions'))

        out_quant = (0.03, zero[dtype] - 60) if quantized else None
        for label, alpha in (('', 0.2), ('_steep', 1.7), ('_negative', -0.4)):
            yield unary_model('leaky_relu_%s%s' % (dtype, label),
                BuiltinOperator.LEAKY_RELU, dtype, shape, in_quant,
                out_quant, option=options('LeakyReluOptions', alpha=alpha))

        # PRELU: slopes per channel, per element, one for all
        for label, slope_shape in (('channels', (1, 1, 6)),
                ('elements', shape), ('single', (1,))):
            graph = Graph('prelu_%s_%s' % (dtype, label))
            input = graph.input(shape, dtype, in_quant, -6.0, 6.0)
            if quantized:
                values = graph.rng.integers(*integer_range(dtype),
                    slope_shape, dtype=dtype, endpoint=True)
                slope = graph.const(values, (0.004, zero[dtype] - 40))
            else:
                slope = graph.const(graph.rng.uniform(-0.5, 1.5,
                    slope_shape).astype(np.float32))
            output = graph.tensor(shape, dtype, out_quant)
            graph.op(BuiltinOperator.PRELU, [input, slope], [output])
            yield graph, [output]

        # SOFTMAX
        out_quant = (1.0 / 256, lowest[dtype]) if quantized else None
        for label, softmax_shape, scale, beta in (
                ('', (3, 10), 0.02, 1.0),
                ('_4d', (2, 3, 4, 7), 0.04, 1.0),
                ('_beta', (4, 25), 0.1, 0.3),
                ('_sharp', (2, 300), 0.11, 2.5),
                ('_flat', (2, 40), 0.0004, 1.0),
                ('_1d', (17,), 0.03, 1.0)):
            yield unary_model('softmax_%s%s' % (dtype, label),
                BuiltinOperator.SOFTMAX, dtype, softmax_shape,
                (scale, zero[dtype] + 3) if quantized else None, out_quant,
                option=options('SoftmaxOptions', beta=beta), low=-4, high=4)


# -- shapes -----------------------------------------------------------------

@model
def shape_models():
    for dtype in ('float32', 'uint8', 'int8', 'int16', 'int32', 'int64'):
        quant = QUANT.get(dtype)

        graph = Graph('reshape_' + dtype)
        input = graph.input((2, 3, 4, 5), dtype, quant)
        output = graph.tensor((6, 20), dtype, quant)
        graph.op(BuiltinOperator.RESHAPE, [input,
            graph.const(np.array([6, -1], dtype=np.int32))], [output],
            options('ReshapeOptions', newShape=[6, -1]))
        yield graph, [output]

        graph = Graph('squeeze_' + dtype)
        input = graph.input((1, 3, 1, 5), dtype, quant)
        output = graph.tensor((3, 5), dtype, quant)
        graph.op(BuiltinOperator.SQUEEZE, [input], [output],
            options('SqueezeOptions', squeezeDims=[0, 2]))
        yield graph, [output]

        # CONCATENATION along each dimension
        for label, shapes, axis in (
                ('channels', ((1, 4, 5, 3), (1, 4, 5, 1), (1, 4, 5, 6)), 3),
                ('rows', ((2, 3, 5), (2, 2, 5)), 1),
                ('last', ((7,), (2,), (4,)), -1),
                ('batch', ((1, 3, 2), (2, 3, 2)), 0)):
            graph = Graph('concat_%s_%s' % (dtype, label))
            inputs = [graph.input(shape, dtype, quant) for shape in shapes]
            shape = list(shapes[0])
            shape[axis] = sum(shape[axis] for shape in shapes)
            output = graph.tensor(shape, dtype, quant)
            graph.op(BuiltinOperator.CONCATENATION, inputs, [output],
                options('ConcatenationOptions', axis=axis))
            yield graph, [output]

        # PAD
        for label, shape, paddings in (
                ('picture', (1, 5, 6, 3), ((0, 0), (1, 2), (3, 0), (0, 0))),
                ('all', (2, 3, 4), ((1, 0), (0, 2), (2, 1))),
                ('row', (9,), ((3, 4),))):
            graph = Graph('pad_%s_%s' % (dtype, label))
            input = graph.input(shape, dtype, quant)
            padded = [size + before + after for size, (before, after)
                in zip(shape, paddings)]
            output = graph.tensor(padded, dtype, quant)
            graph.op(BuiltinOperator.PAD, [input,
                graph.const(np.array(paddings, dtype=np.int32))], [output],
                options('PadOptions'))
            yield graph, [output]

        graph = Graph('pad_%s_value' % dtype)
        input = graph.input((2, 4, 3), dtype, quant)
        value = {'float32': 1.5, 'uint8': 200, 'int8': -77, 'int16': -1234,
            'int32': -123456, 'int64': -123456789012}[dtype]
        output = graph.tensor((3, 7, 6), dtype, quant)
        graph.op(BuiltinOperator.PADV2, [input,
            graph.const(np.array(((0, 1), (2, 1), (0, 3)), dtype=np.int64)),
            graph.const(np.array([value], dtype=dtype).reshape(()), quant)],
            [output], options('PadV2Options'))
        yield graph, [output]

        # TRANSPOSE
        for label, shape, order in (
                ('picture', (2, 3, 4, 5), (0, 3, 1, 2)),
                ('reverse', (3, 4, 5), (2, 1, 0)),
                ('matrix', (6, 7), (1, 0)),
                ('same', (2, 5, 3), (0, 1, 2)),
                ('five', (2, 3, 2, 4, 3), (4, 0, 2, 1, 3))):
            graph = Graph('transpose_%s_%s' % (dtype, label))
            input = graph.input(shape, dtype, quant)
            output = graph.tensor([shape[axis] for axis in order], dtype,
                quant)
            graph.op(BuiltinOperator.TRANSPOSE, [input,
                graph.const(np.array(order, dtype=np.int32))], [output],
                options('TransposeOptions'))
            yield graph, [output]

        # STRIDED_SLICE: (shape, begin, end, strides, begin mask, end mask,
        # shrink mask); the result shape is what NumPy gives
        for label, shape, begin, end, strides, masks in (
                ('plain', (4, 6, 8), (1, 0, 2), (3, 6, 7), (1, 1, 1),
                    (0, 0, 0)),
                ('steps', (5, 9, 7), (0, 1, 0), (5, 9, 7), (2, 3, 2),
                    (0, 0, 0)),
                ('negative', (6, 7), (-2, -1), (-6, -8), (-1, -2),
                    (0, 0, 0)),
                ('masks', (5, 6, 7), (2, 9, 9), (9, 3, 9), (1, 1, -1),
                    (0b110, 0b101, 0)),
                ('shrink', (4, 5, 6), (2, 0, -1), (3, 5, 0), (1, 2, 1),
                    (0, 0, 0b101)),
                ('partial', (3, 4, 5, 2), (1, 1), (3, 3), (1, 1),
                    (0, 0, 0)),
                ('clipped', (5, 8), (-20, 3), (20, 100), (1, 2), (0, 0, 0)),
                ('backwards', (9,), (0,), (0,), (-1,), (1, 1, 0))):
            graph = Graph('strided_slice_%s_%s' % (dtype, label))
            input = graph.input(shape, dtype, quant)
            begin_mask, end_mask, shrink_mask = masks
            slices = []
            for axis in range(len(begin)):
                if shrink_mask & (1 << axis):
                    slices.append(begin[axis])
                    continue
                first = None if begin_mask & (1 << axis) else begin[axis]
                last = None if end_mask & (1 << axis) else end[axis]
                slices.append(slice(first, last, strides[axis]))
            result = np.zeros(shape)[tuple(slices)].shape
            output = graph.tensor(result, dtype, quant)
            graph.op(BuiltinOperator.STRIDED_SLICE, [input,
                graph.const(np.array(begin, dtype=np.int32)),
                graph.const(np.array(end, dtype=np.int32)),
                graph.const(np.array(strides, dtype=np.int32))], [output],
                options('StridedSliceOptions', beginMask=begin_mask,
                    endMask=end_mask, shrinkAxisMask=shrink_mask))
            yield graph, [output]

    # unsigned tensors that are quantized differently are rescaled
    graph = Graph('concat_uint8_rescale')
    inputs = [graph.input((1, 3, 4, 2), 'uint8', (0.02, 121)),
        graph.input((1, 3, 4, 5), 'uint8', (0.031, 90)),
        graph.input((1, 3, 4, 3), 'uint8', (0.011, 200)),
        graph.input((1, 3, 4, 1), 'uint8', (0.025, 110))]
    output = graph.tensor((1, 3, 4, 11), 'uint8', (0.025, 110))
    graph.op(BuiltinOperator.CONCATENATION, inputs, [output],
        options('ConcatenationOptions', axis=3))
    yield graph, [output]


@model
def mean_models():
    cases = (
        ('picture', (2, 5, 6, 7), (1, 2), True),
        ('picture_flat', (2, 5, 6, 7), (1, 2), False),
        ('channels', (1, 4, 5, 9), (3,), True),
        ('last', (3, 4, 11), (-1,), False),
        ('outer', (4, 3, 5), (0, 2), False),
        ('all', (2, 3, 4), (0, 1, 2), True),
        ('twice', (3, 6, 2), (1, -2), False),
        ('middle', (2, 7, 3, 4), (1,), False),
        ('vector', (12,), (0,), True),
    )
    for dtype in ('float32', 'uint8', 'int8'):
        for label, shape, axes, keep in cases:
            for rescale in (False, True):
                if rescale and dtype == 'float32':
                    continue
                graph = Graph('mean_%s_%s%s' % (dtype, label,
                    '_rescale' if rescale else ''))
                quant = QUANT.get(dtype)
                out_quant = quant
                if rescale:
                    out_quant = (0.0137, quant[1] + 9)
                input = graph.input(shape, dtype, quant)
                reduced = {axis % len(shape) for axis in axes}
                result = [1 if axis in reduced else size
                    for axis, size in enumerate(shape)
                    if keep or axis not in reduced]
                output = graph.tensor(result, dtype, out_quant)
                graph.op(BuiltinOperator.MEAN, [input,
                    graph.const(np.array(axes, dtype=np.int32))], [output],
                    options('ReducerOptions', keepDims=keep))
                yield graph, [output]


@model
def resize_models():
    sizes = (
        ('up', (1, 5, 7, 3), (11, 17)),
        ('up2', (2, 4, 4, 5), (8, 8)),
        ('down', (1, 13, 12, 2), (5, 7)),
        ('mixed', (1, 6, 9, 4), (15, 4)),
        ('one', (1, 1, 1, 3), (4, 5)),
        ('to_one', (1, 6, 5, 2), (1, 1)),
    )
    modes = (('', False, False), ('_align', True, False),
        ('_half', False, True))
    for dtype in ('float32', 'uint8', 'int8'):
        quant = QUANT.get(dtype)
        for label, shape, size in sizes:
            for mode, align, half in modes:
                result = (shape[0], size[0], size[1], shape[3])

                graph = Graph('resize_bilinear_%s_%s%s' % (dtype, label,
                    mode))
                input = graph.input(shape, dtype, quant)
                output = graph.tensor(result, dtype, quant)
                graph.op(BuiltinOperator.RESIZE_BILINEAR, [input,
                    graph.const(np.array(size, dtype=np.int32))], [output],
                    options('ResizeBilinearOptions', alignCorners=align,
                        halfPixelCenters=half))
                yield graph, [output]

                graph = Graph('resize_nearest_%s_%s%s' % (dtype, label,
                    mode))
                input = graph.input(shape, dtype, quant)
                output = graph.tensor(result, dtype, quant)
                graph.op(BuiltinOperator.RESIZE_NEAREST_NEIGHBOR, [input,
                    graph.const(np.array(size, dtype=np.int32))], [output],
                    options('ResizeNearestNeighborOptions',
                        alignCorners=align, halfPixelCenters=half))
                yield graph, [output]

        graph = Graph('resize_nearest_%s_align_half' % dtype)
        input = graph.input((1, 5, 7, 3), dtype, quant)
        output = graph.tensor((1, 9, 4, 3), dtype, quant)
        graph.op(BuiltinOperator.RESIZE_NEAREST_NEIGHBOR, [input,
            graph.const(np.array((9, 4), dtype=np.int32))], [output],
            options('ResizeNearestNeighborOptions', alignCorners=True,
                halfPixelCenters=True))
        yield graph, [output]


@model
def quantize_models():
    shape = (2, 5, 6, 3)
    quants = {'uint8': (0.031, 117), 'int8': (0.031, -11),
        'int16': (0.0007, 0)}

    for dtype in ('uint8', 'int8', 'int16'):
        graph = Graph('quantize_float32_' + dtype)
        input = graph.input(shape, 'float32', None, -5.0, 5.0)
        output = graph.tensor(shape, dtype, quants[dtype])
        graph.op(BuiltinOperator.QUANTIZE, [input], [output],
            options('QuantizeOptions'))
        yield graph, [output]

        graph = Graph('dequantize_' + dtype)
        input = graph.input(shape, dtype, quants[dtype])
        output = graph.tensor(shape, 'float32')
        graph.op(BuiltinOperator.DEQUANTIZE, [input], [output],
            options('DequantizeOptions'))
        yield graph, [output]

    # from one quantization to another
    for source, target, quant in (
            ('uint8', 'int8', (0.031, -11)),
            ('int8', 'uint8', (0.031, 117)),
            ('uint8', 'uint8', (0.05, 100)),
            ('int8', 'int8', (0.013, 20)),
            ('uint8', 'int8', (0.0471, 3)),
            ('int8', 'uint8', (0.0222, 140))):
        graph = Graph('quantize_%s_%s_%d' % (source, target,
            int(quant[0] * 10000)))
        input = graph.input(shape, source, quants[source])
        output = graph.tensor(shape, target, quant)
        graph.op(BuiltinOperator.QUANTIZE, [input], [output],
            options('QuantizeOptions'))
        yield graph, [output]

    # half precision constants, as models with 16 bit weights have them
    graph = Graph('dequantize_float16')
    values = graph.rng.uniform(-3, 3, (4, 6)).astype(np.float16)
    values[0, :5] = [0.0, np.float16(6e-5), np.float16(-1e-7), 65504.0,
        np.float16(6.1e-5)]
    input = graph.input((4, 6), 'float32')
    weights = graph.tensor((4, 6), 'float32')
    output = graph.tensor((4, 6), 'float32')
    graph.op(BuiltinOperator.DEQUANTIZE, [graph.const(values)], [weights],
        options('DequantizeOptions'))
    graph.op(BuiltinOperator.ADD, [input, weights], [output],
        options('AddOptions'))
    yield graph, [output]


# -- detection ----------------------------------------------------------------

def detection_model(name, dtype, boxes, classes, background, regular,
        max_detections=10, classes_per_detection=1, detections_per_class=4,
        score_threshold=0.3, iou_threshold=0.5, encoding_size=4):
    graph = Graph(name)
    # anchors on a grid, in two sizes, so that boxes overlap
    anchors = np.zeros((boxes, 4), dtype=np.float32)
    side = int(math.ceil(math.sqrt(boxes / 2.0)))
    for index in range(boxes):
        cell = index // 2
        anchors[index] = ((cell // side + 0.5) / side,
            (cell % side + 0.5) / side, 0.25 + 0.2 * (index % 2),
            0.3 + 0.15 * (index % 2))

    scores = classes + (1 if background else 0)
    if dtype == 'float32':
        encodings = graph.input((1, boxes, encoding_size), 'float32', None,
            -2.0, 2.0)
        predictions = graph.input((1, boxes, scores), 'float32', None, 0.0,
            1.0)
        anchor_tensor = graph.const(anchors)
    else:
        encodings = graph.input((1, boxes, encoding_size), 'uint8',
            (0.017, 119))
        predictions = graph.input((1, boxes, scores), 'uint8',
            (1.0 / 256, 0))
        anchor_tensor = graph.const(np.round(anchors / 0.004)
            .astype(np.uint8), (0.004, 0))

    count = max_detections * classes_per_detection
    outputs = [graph.tensor((1, count, 4)), graph.tensor((1, count)),
        graph.tensor((1, count)), graph.tensor((1,))]

    builder = flexbuffers.Builder()
    with builder.Map():
        builder.Int('max_detections', max_detections)
        builder.Int('max_classes_per_detection', classes_per_detection)
        builder.Int('detections_per_class', detections_per_class)
        builder.Bool('use_regular_nms', regular)
        builder.Float('nms_score_threshold', score_threshold)
        builder.Float('nms_iou_threshold', iou_threshold)
        builder.Int('num_classes', classes)
        builder.Float('y_scale', 10.0)
        builder.Float('x_scale', 10.0)
        builder.Float('h_scale', 5.0)
        builder.Float('w_scale', 5.0)
    graph.op(BuiltinOperator.CUSTOM, [encodings, predictions, anchor_tensor],
        outputs, custom_name='TFLite_Detection_PostProcess',
        custom_options=builder.Finish())
    return graph, outputs


@model
def detection_models():
    for dtype in ('float32', 'uint8'):
        tag = 'detection_' + dtype
        yield detection_model(tag + '_fast', dtype, 60, 5, True, False)
        yield detection_model(tag + '_fast_no_background', dtype, 48, 3,
            False, False, max_detections=7)
        yield detection_model(tag + '_fast_many', dtype, 30, 4, True, False,
            max_detections=40, score_threshold=0.05, iou_threshold=0.8)
        yield detection_model(tag + '_fast_wide', dtype, 40, 6, True, False,
            encoding_size=6, score_threshold=0.6)
        yield detection_model(tag + '_regular', dtype, 60, 5, True, True)
        yield detection_model(tag + '_regular_no_background', dtype, 48, 3,
            False, True, max_detections=25, detections_per_class=3,
            score_threshold=0.5)
        yield detection_model(tag + '_regular_many', dtype, 36, 7, True,
            True, max_detections=12, detections_per_class=30,
            score_threshold=0.1, iou_threshold=0.3)
        yield detection_model(tag + '_none', dtype, 20, 3, True, False,
            score_threshold=1.5)
    # several classes per box: with real scores only, since the reference
    # leaves the order of equal scores open
    yield detection_model('detection_float32_fast_3_classes', 'float32', 50,
        6, True, False, classes_per_detection=3)
    yield detection_model('detection_float32_fast_more_than_classes',
        'float32', 30, 2, True, False, classes_per_detection=3,
        max_detections=5)


# -- what transformers are made of ------------------------------------------

@model
def squared_difference_models():
    for label, first, second in BROADCASTS:
        graph = Graph('squared_difference_float32_' + label)
        a = graph.input(first, 'float32', None, -2.0, 2.0)
        b = graph.input(second, 'float32', None, -2.0, 2.0)
        output = graph.tensor(broadcast_shape(first, second))
        graph.op(BuiltinOperator.SQUARED_DIFFERENCE, [a, b], [output],
            options('SquaredDifferenceOptions'))
        yield graph, [output]

    # against a constant, as layer normalisation has it
    graph = Graph('squared_difference_float32_constant')
    a = graph.input((3, 5, 8), 'float32', None, -2.0, 2.0)
    b = graph.const(graph.rng.uniform(-1, 1, (5, 1)).astype(np.float32))
    output = graph.tensor((3, 5, 8))
    graph.op(BuiltinOperator.SQUARED_DIFFERENCE, [a, b], [output],
        options('SquaredDifferenceOptions'))
    yield graph, [output]


@model
def transformer_unary_models():
    yield unary_model('rsqrt_float32', BuiltinOperator.RSQRT, 'float32',
        (3, 9, 10, 6), low=0.001, high=9.0)
    yield unary_model('rsqrt_float32_1d', BuiltinOperator.RSQRT, 'float32',
        (37,), low=1e-6, high=1e-3)
    for label, approximate in (('', False), ('_approximate', True)):
        option = options('GeluOptions', approximate=approximate)
        # the usual range, where the precision counts
        yield unary_model('gelu_float32' + label, BuiltinOperator.GELU,
            'float32', (3, 9, 10, 6), option=option, low=-3.0, high=3.0)
        # past both ends of the kernel's table
        yield unary_model('gelu_float32_wide' + label, BuiltinOperator.GELU,
            'float32', (4, 500), option=option, low=-12.0, high=12.0)
        yield unary_model('gelu_float32_small' + label, BuiltinOperator.GELU,
            'float32', (129,), option=option, low=-0.01, high=0.01)


def matmul_model(name, first, second, adjoint_a=False, adjoint_b=False,
        constant_second=False):
    graph = Graph(name)
    a = graph.input(first, 'float32', None, -1.0, 1.0)
    if constant_second:
        b = graph.const(graph.rng.uniform(-1, 1, second).astype(np.float32))
    else:
        b = graph.input(second, 'float32', None, -1.0, 1.0)
    rows = first[-1] if adjoint_a else first[-2]
    columns = second[-2] if adjoint_b else second[-1]
    shape = broadcast_shape(first[:-2], second[:-2]) + (rows, columns)
    output = graph.tensor(shape)
    graph.op(BuiltinOperator.BATCH_MATMUL, [a, b], [output],
        options('BatchMatMulOptions', adjX=adjoint_a, adjY=adjoint_b))
    return graph, [output]


@model
def matmul_models():
    tag = 'batch_matmul_float32_'
    yield matmul_model(tag + '2d', (3, 4), (4, 5))
    yield matmul_model(tag + '3d', (6, 7, 9), (6, 9, 11))
    yield matmul_model(tag + '4d', (2, 3, 7, 5), (2, 3, 5, 6))
    # every stretch of the inner loops: sums of 1 to 9 terms, one row and
    # an odd number of them, one column
    for inner in range(1, 10):
        yield matmul_model('%sinner%d' % (tag, inner), (2, 5, inner),
            (2, inner, 19))
    yield matmul_model(tag + 'one_row', (4, 1, 13), (4, 13, 33))
    yield matmul_model(tag + 'one_column', (4, 6, 13), (4, 13, 1))
    yield matmul_model(tag + 'scalar', (1, 1), (1, 1))
    # as attention has them: few long rows, many short ones
    yield matmul_model(tag + 'wide', (3, 8, 16), (3, 16, 150))
    yield matmul_model(tag + 'tall', (3, 8, 150), (3, 150, 16))
    # repeated matrices
    yield matmul_model(tag + 'broadcast_a', (5, 6), (4, 6, 3))
    yield matmul_model(tag + 'broadcast_b', (2, 3, 4, 6), (6, 5))
    yield matmul_model(tag + 'broadcast_both', (2, 1, 4, 3), (3, 3, 5))
    yield matmul_model(tag + 'broadcast_inner', (1, 3, 4, 3), (2, 1, 3, 5))
    # transposed inputs
    yield matmul_model(tag + 'adjoint_a', (3, 6, 4), (3, 6, 5),
        adjoint_a=True)
    yield matmul_model(tag + 'adjoint_b', (3, 4, 6), (3, 5, 6),
        adjoint_b=True)
    yield matmul_model(tag + 'adjoint_both', (2, 2, 6, 4), (2, 5, 6),
        adjoint_a=True, adjoint_b=True)
    yield matmul_model(tag + 'constant', (3, 4, 6), (6, 5),
        constant_second=True)
    yield matmul_model(tag + 'constant_adjoint', (3, 4, 6), (5, 6),
        adjoint_b=True, constant_second=True)


def gather_model(name, dtype, shape, index_shape, axis, index_type='int32',
        constant_indices=False):
    graph = Graph(name)
    quant = QUANT.get(dtype)
    input = graph.input(shape, dtype, quant)
    rank = len(shape)
    real_axis = axis + rank if axis < 0 else axis
    size = shape[real_axis]
    if constant_indices:
        indices = graph.const(graph.rng.integers(0, size, index_shape,
            dtype=index_type))
    else:
        indices = graph.input(index_shape, index_type,
            int_range=(0, size - 1))
    result = tuple(shape[:real_axis]) + tuple(index_shape) \
        + tuple(shape[real_axis + 1:])
    output = graph.tensor(result, dtype, quant)
    graph.op(BuiltinOperator.GATHER, [input, indices], [output],
        options('GatherOptions', axis=axis, batchDims=0))
    return graph, [output]


@model
def gather_models():
    for dtype in ('float32', 'uint8', 'int8', 'int16', 'int32', 'int64'):
        tag = 'gather_' + dtype
        # a table of vectors, as words are looked up
        yield gather_model(tag + '_rows', dtype, (23, 7), (5,), 0)
        yield gather_model(tag + '_rows_2d', dtype, (23, 7), (2, 3), 0)
        yield gather_model(tag + '_middle', dtype, (3, 11, 4), (6,), 1)
        yield gather_model(tag + '_last', dtype, (3, 4, 9), (2, 2), -1)
        yield gather_model(tag + '_1d', dtype, (31,), (12,), 0)
        yield gather_model(tag + '_one', dtype, (9, 5), (1,), 0)
        yield gather_model(tag + '_wide_index', dtype, (9, 5), (4,), 0,
            index_type='int64')
        yield gather_model(tag + '_constant', dtype, (9, 5, 2), (3, 2), 1,
            constant_indices=True)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    directory = sys.argv[1]
    os.makedirs(directory, exist_ok=True)

    names = set()
    for function in MODELS:
        result = function()
        if isinstance(result, tuple):
            result = [result]
        for graph, outputs in result:
            if graph.name in names:
                raise SystemExit('two models named ' + graph.name)
            names.add(graph.name)
            graph.save(directory, outputs)
    print('%d models written to %s' % (len(names), directory))
    return 0


if __name__ == '__main__':
    sys.exit(main())
