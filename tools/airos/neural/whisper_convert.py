#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""Makes the model directory of the Neural Kit's BSpeechRecognizer from a
Whisper checkpoint as OpenAI publishes it.

    whisper_convert.py --checkpoint tiny.en.pt --vocabulary gpt2.tiktoken
        --output DIRECTORY [--name tiny.en] [--mel-filters mel_filters.npz]
        [--check]

Writes into DIRECTORY:

    encoder.tflite      log-mel spectrogram [1, 80, 3000] -> for each layer
                        of the decoder the keys and values its attention to
                        the audio needs ("cross_k_<n>", "cross_v_<n>")
    decoder.tflite      one step: "token" and "position" (int32 [1]), the
                        encoder's outputs and the keys and values of the
                        steps before ("self_k_<n>", "self_v_<n>": one
                        dimension counts the positions) -> "logits"
                        [1, tokens] and this step's keys and values for
                        each layer ("new_k_<n>", "new_v_<n>": the same
                        shapes with one position), which the caller puts
                        into the inputs at this step's position
    vocabulary.bin      the byte strings of the text tokens: per token one
                        byte of length and the bytes, in the order of the
                        token numbers
    mel_filters.bin     the mel filter bank, float32 [80, 201]
    speech.conf         what the recognizer needs to know besides: sizes of
                        the front end, numbers of the special tokens, the
                        tokens that are never to be produced
    LICENSE             Whisper's licence (MIT), which covers the weights

Both networks have fixed shapes and no control flow: the loop over the
tokens is the recognizer's. Whisper as published runs its decoder over all
tokens so far for every new one; here the decoder gets what earlier steps
computed (their keys and values) as inputs instead and works on one token,
which is the same arithmetic done once.

The checkpoint is a PyTorch file; it is read here without PyTorch (a zip
archive with a pickle that names the tensors and one file of numbers per
tensor; the pickle is read with a reader that knows those few names and
refuses everything else). The networks are written as TensorFlow Lite flat
buffers directly, without TensorFlow.

--mel-filters compares the filter bank computed here with the one Whisper
ships (whisper/assets/mel_filters.npz). --check runs both networks with
LiteRT and compares them with a plain NumPy implementation of Whisper's
equations on the checkpoint's weights (needs ai-edge-litert's interpreter).

Where the files come from (as of 2026-11; all MIT, (c) 2022 OpenAI):
    https://openaipublic.azureedge.net/main/whisper/models/<sha256>/<name>
        tiny.en.pt
            d3dd57d32accea0b295c96e26691aa14d8822fac7d9d27d5dc00b4ca2826dd03
        tiny.pt
            65147644a518d12f04e32d6f3b26facc3f8dd46e5390956a9424a650c0ce22b9
    https://github.com/openai/whisper  whisper/assets/gpt2.tiktoken (the
        English models), multilingual.tiktoken, mel_filters.npz; LICENSE

The numbers of the special tokens, the list of symbols whose tokens are
suppressed and the filter bank's definition follow Whisper's tokenizer.py,
decoding.py and audio.py (MIT) and librosa's mel filters (ISC).

Needs: pip install numpy flatbuffers ai-edge-litert
"""

import argparse
import base64
import collections
import math
import os
import pickle
import sys
import zipfile

import flatbuffers
import numpy as np
from ai_edge_litert import schema_py_generated as schema

Op = schema.BuiltinOperator

# -- the checkpoint ---------------------------------------------------------

STORAGE_TYPES = {
    'HalfStorage': np.float16,
    'FloatStorage': np.float32,
    'DoubleStorage': np.float64,
    'LongStorage': np.int64,
    'IntStorage': np.int32,
}


def _rebuild_tensor(storage, offset, size, stride, *unused):
    """A tensor is a view of a block of numbers: element [i, j, ...] is at
    offset + i * stride[0] + j * stride[1] + ... (the checkpoints store
    the weights of linear layers column by column that way)."""
    size = tuple(int(dimension) for dimension in size)
    stride = tuple(int(step) for step in stride)
    if len(size) != len(stride) or offset < 0 \
            or any(dimension < 1 for dimension in size) \
            or any(step < 0 for step in stride):
        raise pickle.UnpicklingError('a tensor has an odd shape')
    last = offset + sum((dimension - 1) * step
        for dimension, step in zip(size, stride))
    if last >= len(storage):
        raise pickle.UnpicklingError('a tensor reaches past its storage')
    view = np.lib.stride_tricks.as_strided(storage[offset:], shape=size,
        strides=tuple(step * storage.itemsize for step in stride),
        writeable=False)
    return np.array(view)


class _CheckpointReader(pickle.Unpickler):
    """Reads the pickle of a PyTorch checkpoint: dictionaries, numbers,
    strings and tensors, nothing else."""

    def __init__(self, file, archive, prefix):
        super().__init__(file)
        self.archive = archive
        self.prefix = prefix

    def find_class(self, module, name):
        if (module, name) == ('collections', 'OrderedDict'):
            return collections.OrderedDict
        if (module, name) == ('torch._utils', '_rebuild_tensor_v2'):
            return _rebuild_tensor
        if module == 'torch' and name in STORAGE_TYPES:
            return name
        raise pickle.UnpicklingError('the checkpoint refers to %s.%s'
            % (module, name))

    def persistent_load(self, identifier):
        kind, storage_type, key = identifier[0], identifier[1], identifier[2]
        if kind != 'storage' or storage_type not in STORAGE_TYPES:
            raise pickle.UnpicklingError('unknown storage')
        data = self.archive.read('%sdata/%s' % (self.prefix, key))
        return np.frombuffer(data, dtype=STORAGE_TYPES[storage_type])


def load_checkpoint(path):
    """Returns (dims, weights): the model's sizes and its tensors as
    float32 arrays by name."""
    with zipfile.ZipFile(path) as archive:
        pickles = [name for name in archive.namelist()
            if name.endswith('/data.pkl')]
        if len(pickles) != 1:
            raise SystemExit('%s is not a PyTorch checkpoint' % path)
        prefix = pickles[0][:-len('data.pkl')]
        with archive.open(pickles[0]) as file:
            checkpoint = _CheckpointReader(file, archive, prefix).load()
    weights = {name: np.asarray(tensor, dtype=np.float32)
        for name, tensor in checkpoint['model_state_dict'].items()}
    return dict(checkpoint['dims']), weights


# -- the front end's filter bank --------------------------------------------

SAMPLE_RATE = 16000
FFT_SIZE = 400
HOP = 160
WINDOW_SECONDS = 30


def mel_filters(mel_bins, sample_rate=SAMPLE_RATE, fft_size=FFT_SIZE):
    """Triangular filters evenly spaced on the mel scale (Slaney's:
    linear below 1 kHz, logarithmic above), each with unit area: what
    librosa.filters.mel(sr=16000, n_fft=400, n_mels=...) gives, which is
    what Whisper was trained with."""
    def to_mel(hz):
        hz = np.asarray(hz, dtype=np.float64)
        linear = hz / (200.0 / 3)
        log = 15.0 + np.log(np.maximum(hz, 1e-10) / 1000.0) \
            / (math.log(6.4) / 27.0)
        return np.where(hz >= 1000.0, log, linear)

    def to_hz(mel):
        mel = np.asarray(mel, dtype=np.float64)
        linear = mel * (200.0 / 3)
        log = 1000.0 * np.exp((math.log(6.4) / 27.0) * (mel - 15.0))
        return np.where(mel >= 15.0, log, linear)

    bins = fft_size // 2 + 1
    frequencies = np.linspace(0, sample_rate / 2.0, bins)
    edges = to_hz(np.linspace(to_mel(0.0), to_mel(sample_rate / 2.0),
        mel_bins + 2))
    widths = np.diff(edges)
    ramps = edges[:, None] - frequencies[None, :]

    filters = np.zeros((mel_bins, bins))
    for i in range(mel_bins):
        rising = -ramps[i] / widths[i]
        falling = ramps[i + 2] / widths[i + 1]
        filters[i] = np.maximum(0, np.minimum(rising, falling))
    filters *= (2.0 / (edges[2:mel_bins + 2] - edges[:mel_bins]))[:, None]
    return filters.astype(np.float32)


# -- the vocabulary ---------------------------------------------------------

LANGUAGES = (
    'en zh de es ru ko fr ja pt tr pl ca nl ar sv it id hi fi vi he uk el '
    'ms cs ro da hu ta no th ur hr bg lt la mi ml cy sk te fa lv bn sr az '
    'sl kn et mk br eu is hy ne mn bs kk sq sw gl mr pa si km sn yo so af '
    'oc ka be tg sd gu am yi lo uz fo ht ps tk nn mt sa lb my bo tl mg as '
    'tt haw ln ha ba jw su yue').split()


class Vocabulary:
    def __init__(self, path, vocabulary_size):
        self.tokens = []
        with open(path) as file:
            for line in file:
                if not line.strip():
                    continue
                token, rank = line.split()
                if int(rank) != len(self.tokens):
                    raise SystemExit('%s: ranks out of order' % path)
                self.tokens.append(base64.b64decode(token))
        self.ranks = {token: rank for rank, token in enumerate(self.tokens)}

        # The special tokens follow the text tokens, in the order of
        # Whisper's tokenizer.py; the time stamps (0.00 to 30.00 seconds
        # in steps of 0.02) are last.
        text = len(self.tokens)
        languages = vocabulary_size - text - 1501 - 8
        if languages < 99 or languages > len(LANGUAGES):
            raise SystemExit('a vocabulary of %d tokens does not go with %d '
                'text tokens' % (vocabulary_size, text))
        self.multilingual = text != 50256
        self.eot = text
        self.sot = text + 1
        self.languages = [(LANGUAGES[i], text + 2 + i)
            for i in range(languages)]
        self.translate = text + 2 + languages
        self.transcribe = self.translate + 1
        self.sot_lm = self.translate + 2
        self.sot_prev = self.translate + 3
        self.no_speech = self.translate + 4
        self.no_timestamps = self.translate + 5
        self.timestamp_begin = self.translate + 6
        self.size = vocabulary_size

    def encode(self, text):
        """Byte pair encoding of one word or run of symbols: the pair of
        neighbours that makes the token of the lowest number is joined
        until no pair makes a token."""
        parts = [bytes([byte]) for byte in text.encode('utf-8')]
        while len(parts) > 1:
            best = None
            for i in range(len(parts) - 1):
                rank = self.ranks.get(parts[i] + parts[i + 1])
                if rank is not None and (best is None or rank < best[0]):
                    best = (rank, i)
            if best is None:
                break
            i = best[1]
            parts[i:i + 2] = [parts[i] + parts[i + 1]]
        return [self.ranks[part] for part in parts]

    def suppressed(self):
        """The tokens a transcript must not have: symbols that are not
        speech (brackets, notes, ...: Whisper's "non speech tokens") and
        the special tokens that only go into the prompt."""
        symbols = list('"#()*+/:;<=>@[\\]^_`{|}~「」『』')
        symbols += ('<< >> <<< >>> -- --- -( -[ (\' (" (( )) ((( ))) [[ ]] '
            '{{ }} ♪♪ ♪♪♪').split()
        notes = list('♩♪♫♬♭♮♯')

        result = {self.encode(' -')[0], self.encode(" '")[0]}
        for symbol in symbols + notes:
            for tokens in (self.encode(symbol), self.encode(' ' + symbol)):
                if len(tokens) == 1 or symbol in notes:
                    result.add(tokens[0])
        result.update((self.sot, self.translate, self.transcribe,
            self.sot_lm, self.sot_prev, self.no_speech))
        return sorted(result)

    def write(self, path):
        with open(path, 'wb') as file:
            for token in self.tokens:
                if len(token) > 255:
                    raise SystemExit('a token is longer than 255 bytes')
                file.write(bytes([len(token)]) + token)


# -- TensorFlow Lite files --------------------------------------------------

TENSOR_TYPES = {
    'float32': schema.TensorType.FLOAT32,
    'int32': schema.TensorType.INT32,
}


class Graph:
    """A network being put together. Tensors are numbers; ops return the
    tensor they make."""

    def __init__(self, name, description):
        self.name = name
        self.description = description
        self.tensors = []
        self.shapes = []
        self.buffers = [None]
        self.codes = []
        self.operators = []
        self.inputs = []
        self.constants = {}

    def _tensor(self, shape, dtype, name, data=None):
        tensor = schema.TensorT()
        tensor.shape = [int(size) for size in shape]
        tensor.type = TENSOR_TYPES[dtype]
        tensor.name = name or 't%d' % len(self.tensors)
        tensor.buffer = 0
        if data is not None:
            tensor.buffer = len(self.buffers)
            self.buffers.append(np.ascontiguousarray(data))
        self.tensors.append(tensor)
        self.shapes.append(tuple(int(size) for size in shape))
        return len(self.tensors) - 1

    def input(self, shape, name, dtype='float32'):
        index = self._tensor(shape, dtype, name)
        self.inputs.append(index)
        return index

    def const(self, array, name=None):
        array = np.asarray(array)
        return self._tensor(array.shape, str(array.dtype), name, array)

    def ints(self, values):
        """A small constant of whole numbers, made once per value."""
        key = tuple(int(value) for value in np.ravel(values)), \
            np.shape(values)
        if key not in self.constants:
            self.constants[key] = self.const(np.asarray(values,
                dtype=np.int32))
        return self.constants[key]

    def op(self, code, inputs, shape, options=None, name=None,
            dtype='float32'):
        if code not in self.codes:
            self.codes.append(code)
        output = self._tensor(shape, dtype, name)
        operator = schema.OperatorT()
        operator.opcodeIndex = self.codes.index(code)
        operator.inputs = [int(index) for index in inputs]
        operator.outputs = [output]
        if options is not None:
            kind = type(options).__name__[:-1]
            operator.builtinOptionsType = getattr(schema.BuiltinOptions,
                kind)
            operator.builtinOptions = options
        self.operators.append(operator)
        return output

    # the operators the networks are made of

    def fully_connected(self, x, weights, bias=None, name=None):
        options = schema.FullyConnectedOptionsT()
        return self.op(Op.FULLY_CONNECTED,
            [x, weights, -1 if bias is None else bias],
            (self.shapes[x][0], self.shapes[weights][0]), options, name)

    def binary(self, code, a, b, name=None):
        options = {
            Op.ADD: schema.AddOptionsT, Op.SUB: schema.SubOptionsT,
            Op.MUL: schema.MulOptionsT,
            Op.SQUARED_DIFFERENCE: schema.SquaredDifferenceOptionsT,
        }[code]()
        first, second = self.shapes[a], self.shapes[b]
        rank = max(len(first), len(second))
        first = (1,) * (rank - len(first)) + first
        second = (1,) * (rank - len(second)) + second
        shape = tuple(max(x, y) for x, y in zip(first, second))
        return self.op(code, [a, b], shape, options, name)

    def add(self, a, b, name=None):
        return self.binary(Op.ADD, a, b, name)

    def mul(self, a, b, name=None):
        return self.binary(Op.MUL, a, b, name)

    def gelu(self, x):
        options = schema.GeluOptionsT()
        options.approximate = False
        return self.op(Op.GELU, [x], self.shapes[x], options)

    def reshape(self, x, shape, name=None):
        options = schema.ReshapeOptionsT()
        options.newShape = [int(size) for size in shape]
        return self.op(Op.RESHAPE, [x, self.ints(shape)], shape, options,
            name)

    def slice(self, x, begin, end):
        shape = tuple(last - first for first, last in zip(begin, end))
        return self.op(Op.STRIDED_SLICE, [x, self.ints(begin),
            self.ints(end), self.ints([1] * len(begin))], shape,
            schema.StridedSliceOptionsT())

    def transpose(self, x, order, name=None):
        shape = tuple(self.shapes[x][axis] for axis in order)
        return self.op(Op.TRANSPOSE, [x, self.ints(order)], shape,
            schema.TransposeOptionsT(), name)

    def mean_last(self, x):
        options = schema.ReducerOptionsT()
        options.keepDims = True
        shape = self.shapes[x][:-1] + (1,)
        return self.op(Op.MEAN, [x, self.ints([len(shape) - 1])], shape,
            options)

    def matmul(self, a, b):
        options = schema.BatchMatMulOptionsT()
        shape = self.shapes[a][:-1] + (self.shapes[b][-1],)
        return self.op(Op.BATCH_MATMUL, [a, b], shape, options)

    def softmax(self, x):
        options = schema.SoftmaxOptionsT()
        options.beta = 1.0
        return self.op(Op.SOFTMAX, [x], self.shapes[x], options)

    def gather(self, table, indices, name=None):
        options = schema.GatherOptionsT()
        options.axis = 0
        options.batchDims = 0
        shape = self.shapes[indices] + self.shapes[table][1:]
        return self.op(Op.GATHER, [table, indices], shape, options, name)

    def layer_norm(self, x, weight, bias):
        """(x - mean) / sqrt(variance + 1e-5) * weight + bias over the
        last dimension, from the operators a converter makes of it."""
        mean = self.mean_last(x)
        centered = self.binary(Op.SUB, x, mean)
        variance = self.mean_last(self.binary(Op.SQUARED_DIFFERENCE, x,
            mean))
        scale = self.op(Op.RSQRT, [self.add(variance,
            self.const(np.float32([1e-5])))], self.shapes[variance])
        return self.add(self.mul(self.mul(centered, scale),
            self.const(weight)), self.const(bias))

    def save(self, path, outputs):
        model = schema.ModelT()
        model.version = 3
        model.description = self.description
        model.operatorCodes = []
        for code in self.codes:
            operator_code = schema.OperatorCodeT()
            operator_code.deprecatedBuiltinCode = min(code, 127)
            operator_code.builtinCode = code
            operator_code.version = 1
            model.operatorCodes.append(operator_code)

        subgraph = schema.SubGraphT()
        subgraph.name = self.name
        subgraph.tensors = self.tensors
        subgraph.operators = self.operators
        subgraph.inputs = self.inputs
        subgraph.outputs = [int(index) for index in outputs]
        model.subgraphs = [subgraph]

        model.buffers = []
        size = 0
        for data in self.buffers:
            buffer = schema.BufferT()
            if data is not None:
                buffer.data = np.frombuffer(data.tobytes(), dtype=np.uint8)
                size += buffer.data.size
            model.buffers.append(buffer)

        builder = flatbuffers.Builder(size + (1 << 20))
        builder.Finish(model.Pack(builder), b'TFL3')
        with open(path, 'wb') as file:
            file.write(builder.Output())


def attention_scale(state, heads):
    # Whisper multiplies queries and keys by this each; it goes into the
    # weights here.
    return float(state // heads) ** -0.25


def build_encoder(dims, weights, name):
    frames = WINDOW_SECONDS * SAMPLE_RATE // HOP
    context = dims['n_audio_ctx']
    state = dims['n_audio_state']
    heads = dims['n_audio_head']
    width = state // heads
    scale = attention_scale(state, heads)
    if frames != 2 * context:
        raise SystemExit('the encoder is not one for 30 second windows')

    def w(key):
        return weights['encoder.' + key]

    graph = Graph('encoder', 'Whisper %s encoder (OpenAI, MIT), converted '
        'by air/OS whisper_convert.py' % name)
    mel = graph.input((1, dims['n_mels'], frames), 'mel')

    # The two convolutions over time, as convolutions over pictures one
    # column wide: time runs down the rows, which is what the kernels
    # share out among the processors.
    x = graph.reshape(graph.transpose(mel, (0, 2, 1)),
        (1, frames, 1, dims['n_mels']))
    options = schema.Conv2DOptionsT()
    options.padding = schema.Padding.SAME
    options.strideW = options.strideH = 1
    options.dilationWFactor = options.dilationHFactor = 1
    x = graph.op(Op.CONV_2D, [x, graph.const(
            w('conv1.weight').transpose(0, 2, 1)[:, :, None, :]),
        graph.const(w('conv1.bias'))], (1, frames, 1, state), options)
    x = graph.gelu(x)

    # a step of two with one frame of padding on both sides, which is
    # not what "same" padding means here: padded first
    x = graph.op(Op.PAD, [x, graph.const(np.int32(
        [[0, 0], [1, 1], [0, 0], [0, 0]]))], (1, frames + 2, 1, state),
        schema.PadOptionsT())
    options = schema.Conv2DOptionsT()
    options.padding = schema.Padding.VALID
    options.strideW = 1
    options.strideH = 2
    options.dilationWFactor = options.dilationHFactor = 1
    x = graph.op(Op.CONV_2D, [x, graph.const(
            w('conv2.weight').transpose(0, 2, 1)[:, :, None, :]),
        graph.const(w('conv2.bias'))], (1, context, 1, state), options)
    x = graph.gelu(x)
    x = graph.add(graph.reshape(x, (context, state)),
        graph.const(w('positional_embedding')))

    for layer in range(dims['n_audio_layer']):
        block = 'blocks.%d.' % layer

        # every frame looks at every frame
        y = graph.layer_norm(x, w(block + 'attn_ln.weight'),
            w(block + 'attn_ln.bias'))
        query = graph.fully_connected(y,
            graph.const(w(block + 'attn.query.weight') * scale),
            graph.const(w(block + 'attn.query.bias') * scale))
        key = graph.fully_connected(y,
            graph.const(w(block + 'attn.key.weight') * scale))
        value = graph.fully_connected(y,
            graph.const(w(block + 'attn.value.weight')),
            graph.const(w(block + 'attn.value.bias')))
        query = graph.transpose(graph.reshape(query,
            (context, heads, width)), (1, 0, 2))
        key = graph.transpose(graph.reshape(key, (context, heads, width)),
            (1, 2, 0))
        value = graph.transpose(graph.reshape(value,
            (context, heads, width)), (1, 0, 2))
        y = graph.matmul(graph.softmax(graph.matmul(query, key)), value)
        y = graph.reshape(graph.transpose(y, (1, 0, 2)), (context, state))
        x = graph.add(x, graph.fully_connected(y,
            graph.const(w(block + 'attn.out.weight')),
            graph.const(w(block + 'attn.out.bias'))))

        y = graph.layer_norm(x, w(block + 'mlp_ln.weight'),
            w(block + 'mlp_ln.bias'))
        y = graph.gelu(graph.fully_connected(y,
            graph.const(w(block + 'mlp.0.weight')),
            graph.const(w(block + 'mlp.0.bias'))))
        x = graph.add(x, graph.fully_connected(y,
            graph.const(w(block + 'mlp.2.weight')),
            graph.const(w(block + 'mlp.2.bias'))))

    x = graph.layer_norm(x, w('ln_post.weight'), w('ln_post.bias'))

    # What each layer of the decoder makes of this, which does not depend
    # on the tokens: done here, once per window, instead of per token.
    outputs = []
    text_heads = dims['n_text_head']
    text_state = dims['n_text_state']
    text_width = text_state // text_heads
    text_scale = attention_scale(text_state, text_heads)
    for layer in range(dims['n_text_layer']):
        block = 'decoder.blocks.%d.cross_attn.' % layer
        key = graph.fully_connected(x,
            graph.const(weights[block + 'key.weight'] * text_scale))
        value = graph.fully_connected(x,
            graph.const(weights[block + 'value.weight']),
            graph.const(weights[block + 'value.bias']))
        outputs.append(graph.transpose(graph.reshape(key,
            (context, text_heads, text_width)), (1, 2, 0),
            'cross_k_%d' % layer))
        outputs.append(graph.transpose(graph.reshape(value,
            (context, text_heads, text_width)), (1, 0, 2),
            'cross_v_%d' % layer))
    return graph, outputs


def build_decoder(dims, weights, name):
    audio = dims['n_audio_ctx']
    context = dims['n_text_ctx']
    state = dims['n_text_state']
    heads = dims['n_text_head']
    layers = dims['n_text_layer']
    width = state // heads
    scale = attention_scale(state, heads)

    def w(key):
        return weights['decoder.' + key]

    graph = Graph('decoder', 'Whisper %s decoder, one token per run (OpenAI, '
        'MIT), converted by air/OS whisper_convert.py' % name)
    token = graph.input((1,), 'token', 'int32')
    position = graph.input((1,), 'position', 'int32')
    # what the steps before left, laid out as the products below take
    # them: keys with the positions last, values with the positions in
    # the middle
    self_keys = [graph.input((heads, width, context), 'self_k_%d' % layer)
        for layer in range(layers)]
    self_values = [graph.input((heads, context, width), 'self_v_%d' % layer)
        for layer in range(layers)]
    cross_keys = [graph.input((heads, width, audio), 'cross_k_%d' % layer)
        for layer in range(layers)]
    cross_values = [graph.input((heads, audio, width), 'cross_v_%d' % layer)
        for layer in range(layers)]

    embedding = graph.const(w('token_embedding.weight'))
    x = graph.add(graph.gather(embedding, token),
        graph.gather(graph.const(w('positional_embedding')), position))

    # A table with a row per position: which positions of the steps
    # before a token at this position may look at (0; the others, which
    # hold nothing or something old, get a number so low that their
    # share comes out as nothing).
    visible = graph.gather(graph.const(np.triu(np.full((context, context),
        -1e9, dtype=np.float32), 0)), position)

    outputs = []
    for layer in range(layers):
        block = 'blocks.%d.' % layer

        # the tokens so far: those of the steps before, and this one
        y = graph.layer_norm(x, w(block + 'attn_ln.weight'),
            w(block + 'attn_ln.bias'))
        query = graph.fully_connected(y,
            graph.const(w(block + 'attn.query.weight') * scale),
            graph.const(w(block + 'attn.query.bias') * scale))
        key = graph.fully_connected(y,
            graph.const(w(block + 'attn.key.weight') * scale))
        value = graph.fully_connected(y,
            graph.const(w(block + 'attn.value.weight')),
            graph.const(w(block + 'attn.value.bias')))
        query = graph.reshape(query, (heads, 1, width))
        key = graph.reshape(key, (heads, width, 1), 'new_k_%d' % layer)
        value = graph.reshape(value, (heads, 1, width), 'new_v_%d' % layer)
        outputs += [key, value]

        options = schema.ConcatenationOptionsT()
        options.axis = 2
        shares = graph.softmax(graph.op(Op.CONCATENATION, [
            graph.add(graph.matmul(query, self_keys[layer]), visible),
            graph.matmul(query, key)], (heads, 1, context + 1), options))
        before = graph.slice(shares, (0, 0, 0), (heads, 1, context))
        own = graph.slice(shares, (0, 0, context), (heads, 1, context + 1))
        y = graph.add(graph.matmul(before, self_values[layer]),
            graph.mul(own, value))
        x = graph.add(x, graph.fully_connected(graph.reshape(y, (1, state)),
            graph.const(w(block + 'attn.out.weight')),
            graph.const(w(block + 'attn.out.bias'))))

        # the audio
        y = graph.layer_norm(x, w(block + 'cross_attn_ln.weight'),
            w(block + 'cross_attn_ln.bias'))
        query = graph.fully_connected(y,
            graph.const(w(block + 'cross_attn.query.weight') * scale),
            graph.const(w(block + 'cross_attn.query.bias') * scale))
        shares = graph.softmax(graph.matmul(
            graph.reshape(query, (heads, 1, width)), cross_keys[layer]))
        y = graph.reshape(graph.matmul(shares, cross_values[layer]),
            (1, state))
        x = graph.add(x, graph.fully_connected(y,
            graph.const(w(block + 'cross_attn.out.weight')),
            graph.const(w(block + 'cross_attn.out.bias'))))

        y = graph.layer_norm(x, w(block + 'mlp_ln.weight'),
            w(block + 'mlp_ln.bias'))
        y = graph.gelu(graph.fully_connected(y,
            graph.const(w(block + 'mlp.0.weight')),
            graph.const(w(block + 'mlp.0.bias'))))
        x = graph.add(x, graph.fully_connected(y,
            graph.const(w(block + 'mlp.2.weight')),
            graph.const(w(block + 'mlp.2.bias'))))

    x = graph.layer_norm(x, w('ln.weight'), w('ln.bias'))
    # how well each token of the vocabulary fits: against the same table
    # the tokens' vectors came from
    logits = graph.fully_connected(x, embedding, None, 'logits')
    return graph, [logits] + outputs


# -- the configuration ------------------------------------------------------

def write_configuration(path, name, dims, vocabulary):
    max_tokens = dims['n_text_ctx'] // 2
    lines = [
        '# Whisper %s for the Neural Kit\'s BSpeechRecognizer; written by'
            % name,
        '# tools/airos/neural/whisper_convert.py from OpenAI\'s checkpoint.',
        '# Weights and vocabulary: (c) 2022 OpenAI, MIT (see LICENSE).',
        'family whisper',
        'name %s' % name,
        'encoder encoder.tflite',
        'decoder decoder.tflite',
        'vocabulary vocabulary.bin',
        'mel_filters mel_filters.bin',
        '',
        '# the front end: samples per second, per transform and between',
        '# transforms, filters, frames of a window',
        'sample_rate %d' % SAMPLE_RATE,
        'fft_size %d' % FFT_SIZE,
        'hop %d' % HOP,
        'mel_bins %d' % dims['n_mels'],
        'window_frames %d' % (WINDOW_SECONDS * SAMPLE_RATE // HOP),
        '',
        '# the decoder: positions it has, tokens it may add per window,',
        '# milliseconds per time stamp token',
        'text_context %d' % dims['n_text_ctx'],
        'max_tokens %d' % max_tokens,
        'timestamp_ms 20',
        '',
        'vocabulary_size %d' % vocabulary.size,
        'token_eot %d' % vocabulary.eot,
        'token_sot %d' % vocabulary.sot,
        'token_translate %d' % vocabulary.translate,
        'token_transcribe %d' % vocabulary.transcribe,
        'token_no_speech %d' % vocabulary.no_speech,
        'token_no_timestamps %d' % vocabulary.no_timestamps,
        'token_timestamp_begin %d' % vocabulary.timestamp_begin,
        '# " ", which must not open a window',
        'token_blank %d' % vocabulary.encode(' ')[0],
        'multilingual %d' % (1 if vocabulary.multilingual else 0),
        '',
        '# tokens that are never produced',
        'suppress ' + ' '.join(str(token)
            for token in vocabulary.suppressed()),
    ]
    if vocabulary.multilingual:
        lines += ['', '# the languages and their tokens']
        lines += ['language %s %d' % language
            for language in vocabulary.languages]
    with open(path, 'w') as file:
        file.write('\n'.join(lines) + '\n')


# -- checks -----------------------------------------------------------------

def reference_layer_norm(x, weight, bias):
    mean = x.mean(-1, keepdims=True)
    variance = ((x - mean) ** 2).mean(-1, keepdims=True)
    return (x - mean) / np.sqrt(variance + 1e-5) * weight + bias


def reference_gelu(x):
    erf = np.vectorize(math.erf, otypes=[np.float64])
    return 0.5 * x * (1 + erf(x / math.sqrt(2.0)))


def reference_attention(weights, prefix, heads, x, source=None, mask=None):
    """Whisper's MultiHeadAttention in double precision."""
    source = x if source is None else source
    state = x.shape[-1]
    scale = (state // heads) ** -0.25
    q = x @ weights[prefix + 'query.weight'].T + weights[prefix + 'query.bias']
    k = source @ weights[prefix + 'key.weight'].T
    v = source @ weights[prefix + 'value.weight'].T \
        + weights[prefix + 'value.bias']
    q = q.reshape(q.shape[0], heads, -1).transpose(1, 0, 2) * scale
    k = k.reshape(k.shape[0], heads, -1).transpose(1, 2, 0) * scale
    v = v.reshape(v.shape[0], heads, -1).transpose(1, 0, 2)
    qk = q @ k
    if mask is not None:
        qk = qk + mask
    qk = qk - qk.max(-1, keepdims=True)
    shares = np.exp(qk)
    shares /= shares.sum(-1, keepdims=True)
    y = (shares @ v).transpose(1, 0, 2).reshape(x.shape[0], state)
    return y @ weights[prefix + 'out.weight'].T + weights[prefix + 'out.bias']


def reference_encoder(dims, weights, mel):
    """mel [mel_bins, frames] -> [n_audio_ctx, n_audio_state]"""
    def conv(x, weight, bias, stride):
        padded = np.pad(x, ((0, 0), (1, 1)))
        frames = (x.shape[1] + 2 - 3) // stride + 1
        result = np.zeros((weight.shape[0], frames))
        for tap in range(3):
            taps = padded[:, tap:tap + stride * frames:stride]
            result += weight[:, :, tap].astype(np.float64) @ taps
        return result + bias[:, None]

    def w(key):
        return weights['encoder.' + key].astype(np.float64)

    x = reference_gelu(conv(mel.astype(np.float64), w('conv1.weight'),
        w('conv1.bias'), 1))
    x = reference_gelu(conv(x, w('conv2.weight'), w('conv2.bias'), 2))
    x = x.T + w('positional_embedding')
    doubles = {key: value.astype(np.float64)
        for key, value in weights.items()}
    for layer in range(dims['n_audio_layer']):
        block = 'encoder.blocks.%d.' % layer
        x = x + reference_attention(doubles, block + 'attn.',
            dims['n_audio_head'], reference_layer_norm(x,
                doubles[block + 'attn_ln.weight'],
                doubles[block + 'attn_ln.bias']))
        y = reference_layer_norm(x, doubles[block + 'mlp_ln.weight'],
            doubles[block + 'mlp_ln.bias'])
        y = reference_gelu(y @ doubles[block + 'mlp.0.weight'].T
            + doubles[block + 'mlp.0.bias'])
        x = x + y @ doubles[block + 'mlp.2.weight'].T \
            + doubles[block + 'mlp.2.bias']
    return reference_layer_norm(x, w('ln_post.weight'), w('ln_post.bias'))


def reference_decoder(dims, weights, tokens, audio):
    """Logits [len(tokens), vocabulary] for a sequence of tokens, the way
    Whisper computes them: all positions at once."""
    doubles = {key: value.astype(np.float64)
        for key, value in weights.items() if key.startswith('decoder.')}
    count = len(tokens)
    x = doubles['decoder.token_embedding.weight'][tokens] \
        + doubles['decoder.positional_embedding'][:count]
    mask = np.triu(np.full((count, count), -np.inf), 1)
    for layer in range(dims['n_text_layer']):
        block = 'decoder.blocks.%d.' % layer
        x = x + reference_attention(doubles, block + 'attn.',
            dims['n_text_head'], reference_layer_norm(x,
                doubles[block + 'attn_ln.weight'],
                doubles[block + 'attn_ln.bias']), mask=mask)
        x = x + reference_attention(doubles, block + 'cross_attn.',
            dims['n_text_head'], reference_layer_norm(x,
                doubles[block + 'cross_attn_ln.weight'],
                doubles[block + 'cross_attn_ln.bias']), source=audio)
        y = reference_layer_norm(x, doubles[block + 'mlp_ln.weight'],
            doubles[block + 'mlp_ln.bias'])
        y = reference_gelu(y @ doubles[block + 'mlp.0.weight'].T
            + doubles[block + 'mlp.0.bias'])
        x = x + y @ doubles[block + 'mlp.2.weight'].T \
            + doubles[block + 'mlp.2.bias']
    x = reference_layer_norm(x, doubles['decoder.ln.weight'],
        doubles['decoder.ln.bias'])
    return x @ doubles['decoder.token_embedding.weight'].T


def check(directory, dims, weights, vocabulary):
    """Runs the files just written with LiteRT and compares them with the
    equations above. Returns True when they agree."""
    from ai_edge_litert.interpreter import Interpreter

    rng = np.random.default_rng(1)
    frames = WINDOW_SECONDS * SAMPLE_RATE // HOP
    # like a spectrogram: mostly the floor, some louder stretches
    mel = np.clip(rng.normal(-0.4, 0.5, (dims['n_mels'], frames)), -1, 1.5)
    mel[:, frames // 3:] = -1.0
    mel = mel.astype(np.float32)

    def relative(ours, reference):
        return float(np.max(np.abs(ours - reference))
            / max(np.max(np.abs(reference)), 1e-9))

    ok = True
    encoder = Interpreter(model_path=os.path.join(directory,
        'encoder.tflite'))
    encoder.allocate_tensors()
    encoder.set_tensor(encoder.get_input_details()[0]['index'], mel[None])
    encoder.invoke()
    audio = reference_encoder(dims, weights, mel)

    heads = dims['n_text_head']
    scale = attention_scale(dims['n_text_state'], heads)
    cross = {}
    for detail in encoder.get_output_details():
        ours = encoder.get_tensor(detail['index'])
        cross[detail['name']] = ours
        kind, layer = detail['name'].rsplit('_', 1)
        block = 'decoder.blocks.%s.cross_attn.' % layer
        if kind == 'cross_k':
            reference = (audio @ weights[block + 'key.weight'].T * scale) \
                .reshape(audio.shape[0], heads, -1).transpose(1, 2, 0)
        else:
            reference = (audio @ weights[block + 'value.weight'].T
                + weights[block + 'value.bias']) \
                .reshape(audio.shape[0], heads, -1).transpose(1, 0, 2)
        error = relative(ours, reference)
        print('  encoder %-10s relative error %.1e' % (detail['name'],
            error))
        ok = ok and error < 1e-3

    # a prompt and some text, token by token, against all at once
    tokens = [vocabulary.sot] + vocabulary.encode(' The')  \
        + vocabulary.encode(' quick') + [vocabulary.timestamp_begin + 100] \
        + vocabulary.encode(' fox') + [vocabulary.eot]
    reference = reference_decoder(dims, weights, tokens, audio)

    decoder = Interpreter(model_path=os.path.join(directory,
        'decoder.tflite'))
    decoder.allocate_tensors()
    inputs = {detail['name']: detail
        for detail in decoder.get_input_details()}
    outputs = {detail['name']: detail
        for detail in decoder.get_output_details()}
    state = {name: np.zeros(detail['shape'], dtype=np.float32)
        for name, detail in inputs.items() if name.startswith('self_')}
    worst = 0.0
    same = True
    for step, token in enumerate(tokens):
        decoder.set_tensor(inputs['token']['index'], np.int32([token]))
        decoder.set_tensor(inputs['position']['index'], np.int32([step]))
        for name, value in cross.items():
            decoder.set_tensor(inputs[name]['index'], value)
        for name, value in state.items():
            decoder.set_tensor(inputs[name]['index'], value)
        decoder.invoke()
        logits = decoder.get_tensor(outputs['logits']['index'])[0]
        worst = max(worst, relative(logits, reference[step]))
        same = same and int(np.argmax(logits)) \
            == int(np.argmax(reference[step]))
        for name in state:
            fresh = decoder.get_tensor(
                outputs['new_' + name[len('self_'):]]['index'])
            # the one dimension in which they differ counts the positions
            axis = [i for i in range(fresh.ndim)
                if fresh.shape[i] != state[name].shape[i]][0]
            index = [slice(None)] * fresh.ndim
            index[axis] = slice(step, step + 1)
            state[name][tuple(index)] = fresh
    print('  decoder logits over %d steps: relative error %.1e, best token '
        '%s' % (len(tokens), worst, 'the same' if same else 'DIFFERENT'))
    return ok and same and worst < 1e-3


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--checkpoint', required=True)
    parser.add_argument('--vocabulary', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--name')
    parser.add_argument('--license')
    parser.add_argument('--mel-filters')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()

    name = args.name or os.path.splitext(os.path.basename(
        args.checkpoint))[0]
    dims, weights = load_checkpoint(args.checkpoint)
    print('Whisper %s: %s' % (name, ', '.join('%s %d' % item
        for item in dims.items())))
    vocabulary = Vocabulary(args.vocabulary, dims['n_vocab'])
    os.makedirs(args.output, exist_ok=True)

    filters = mel_filters(dims['n_mels'])
    if args.mel_filters:
        published = np.load(args.mel_filters)['mel_%d' % dims['n_mels']]
        difference = float(np.max(np.abs(filters - published)))
        print('mel filters: differ from %s by at most %.1e'
            % (os.path.basename(args.mel_filters), difference))
        if difference > 1e-6:
            raise SystemExit('the filter bank is not Whisper\'s')
    filters.astype('<f4').tofile(os.path.join(args.output,
        'mel_filters.bin'))

    vocabulary.write(os.path.join(args.output, 'vocabulary.bin'))
    write_configuration(os.path.join(args.output, 'speech.conf'), name,
        dims, vocabulary)
    if args.license:
        with open(args.license, 'rb') as source, open(os.path.join(
                args.output, 'LICENSE'), 'wb') as target:
            target.write(source.read())

    graph, outputs = build_encoder(dims, weights, name)
    graph.save(os.path.join(args.output, 'encoder.tflite'), outputs)
    graph, outputs = build_decoder(dims, weights, name)
    graph.save(os.path.join(args.output, 'decoder.tflite'), outputs)
    for file in sorted(os.listdir(args.output)):
        print('  %-18s %10d bytes' % (file, os.path.getsize(os.path.join(
            args.output, file))))

    if args.check:
        print('checking against the checkpoint\'s equations:')
        if not check(args.output, dims, weights, vocabulary):
            raise SystemExit('FAILED: the converted networks do not agree '
                'with the checkpoint')
        print('the converted networks agree with the checkpoint')
    return 0


if __name__ == '__main__':
    sys.exit(main())
