#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""A reference for the Neural Kit's speech recognizer: the same model
directory, run with NumPy and LiteRT.

    speech_reference.py --model <directory> [--no-timestamps]
        [--language <code>] [--dump <directory>] [--tokens] audio.wav...
    speech_reference.py --model <directory> --test-vectors

Prints what speech_host prints for the same files: a line per segment,
"[start --> end] text" with the times in seconds. Everything here is written
from Whisper's published algorithm (audio.py, decoding.py, transcribe.py of
github.com/openai/whisper, MIT) with NumPy, independently of the kit's C++:
the log-mel spectrogram, the decoding loop with its rules for what a token
may be, the segments made from the time stamps. Only the two networks are
the kit's files, run by LiteRT.

With --dump the spectrogram of every window is written to
<directory>/<audio name>.mel<n>.bin (float32 [80, 3000]) and the inputs of
the decoder at its tenth step to <audio name>.decoder<n>.bin, for
compare.py --inputs. With --tokens the tokens of each window are printed.
--test-vectors prints the reference values of the kit's own test
(src/tests/kits/neural/speech_test.cpp).

What is not Whisper's: the largest value of the spectrogram, which limits
how quiet the rest may be, is taken per window (Whisper takes it over the
whole recording, which a recognizer fed bit by bit cannot); nothing of
the text before a window is given to the decoder; a window is decoded
once, without the second tries at higher temperatures.

Needs: pip install numpy ai-edge-litert
"""

import argparse
import os
import sys
import wave

import numpy as np
from ai_edge_litert.interpreter import Interpreter


class Model:
    def __init__(self, directory, reference_kernels=False):
        self.config = {}
        self.languages = {}
        self.suppress = []
        with open(os.path.join(directory, 'speech.conf')) as file:
            for line in file:
                words = line.split('#')[0].split()
                if not words:
                    continue
                if words[0] == 'language':
                    self.languages[words[1]] = int(words[2])
                elif words[0] == 'suppress':
                    self.suppress = [int(word) for word in words[1:]]
                else:
                    self.config[words[0]] = words[1]

        def number(key):
            return int(self.config[key])

        self.sample_rate = number('sample_rate')
        self.fft_size = number('fft_size')
        self.hop = number('hop')
        self.mel_bins = number('mel_bins')
        self.frames = number('window_frames')
        self.max_tokens = number('max_tokens')
        self.text_context = number('text_context')
        self.timestamp_seconds = number('timestamp_ms') / 1000.0
        self.eot = number('token_eot')
        self.sot = number('token_sot')
        self.transcribe = number('token_transcribe')
        self.no_speech = number('token_no_speech')
        self.no_timestamps = number('token_no_timestamps')
        self.timestamp_begin = number('token_timestamp_begin')
        self.blank = number('token_blank')
        self.multilingual = number('multilingual') != 0

        self.filters = np.fromfile(os.path.join(directory,
            self.config['mel_filters']), dtype='<f4').reshape(
                self.mel_bins, self.fft_size // 2 + 1).astype(np.float64)

        self.vocabulary = []
        with open(os.path.join(directory, self.config['vocabulary']),
                'rb') as file:
            data = file.read()
        at = 0
        while at < len(data):
            self.vocabulary.append(data[at + 1:at + 1 + data[at]])
            at += 1 + data[at]

        options = {}
        if reference_kernels:
            from ai_edge_litert.interpreter import OpResolverType
            options['experimental_op_resolver_type'] \
                = OpResolverType.BUILTIN_REF
        self.encoder = Interpreter(model_path=os.path.join(directory,
            self.config['encoder']), **options)
        self.encoder.allocate_tensors()
        self.decoder = Interpreter(model_path=os.path.join(directory,
            self.config['decoder']), **options)
        self.decoder.allocate_tensors()
        self.decoder_inputs = {detail['name']: detail
            for detail in self.decoder.get_input_details()}
        self.decoder_outputs = {detail['name']: detail
            for detail in self.decoder.get_output_details()}

    def decode_text(self, tokens):
        text = b''.join(self.vocabulary[token] for token in tokens
            if token < self.eot)
        return text.decode('utf-8', errors='replace')


def read_wave(path, sample_rate):
    with wave.open(path, 'rb') as file:
        if file.getframerate() != sample_rate or file.getsampwidth() != 2:
            raise SystemExit('%s: 16 bit samples at %d Hz are needed here'
                % (path, sample_rate))
        samples = np.frombuffer(file.readframes(file.getnframes()),
            dtype='<i2').astype(np.float32) / 32768.0
        channels = file.getnchannels()
    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1)
    return samples.astype(np.float32)


def log_mel(model, audio, first):
    """The spectrogram of the window that starts at sample `first`:
    float32 [mel_bins, frames]."""
    half = model.fft_size // 2
    count = (model.frames - 1) * model.hop + model.fft_size
    index = np.arange(first - half, first - half + count)
    # before the recording its beginning mirrored, after it silence
    mirrored = np.where(index < 0, -index, index)
    inside = mirrored < len(audio)
    samples = np.zeros(count, dtype=np.float64)
    samples[inside] = audio[mirrored[inside]]

    window = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(model.fft_size)
        / model.fft_size)
    starts = np.arange(model.frames) * model.hop
    frames = samples[starts[:, None] + np.arange(model.fft_size)[None, :]]
    power = np.abs(np.fft.rfft(frames * window, axis=1)) ** 2
    mel = np.log10(np.maximum(model.filters @ power.T, 1e-10))
    mel = np.maximum(mel, mel.max() - 8.0)
    return ((mel + 4.0) / 4.0).astype(np.float32)


def log_softmax(logits):
    shifted = logits - logits.max()
    return shifted - np.log(np.sum(np.exp(shifted)))


def decode_window(model, mel, prompt, timestamps, dump=None):
    """Greedy decoding of one window. Returns (tokens, no speech
    probability, mean log probability)."""
    encoder = model.encoder
    encoder.set_tensor(encoder.get_input_details()[0]['index'], mel[None])
    encoder.invoke()

    decoder = model.decoder
    inputs = model.decoder_inputs
    outputs = model.decoder_outputs
    for detail in encoder.get_output_details():
        decoder.set_tensor(inputs[detail['name']]['index'],
            encoder.get_tensor(detail['index']))
    state = {name: np.zeros(detail['shape'], dtype=np.float32)
        for name, detail in inputs.items() if name.startswith('self_')}

    def step(position, token):
        decoder.set_tensor(inputs['token']['index'], np.int32([token]))
        decoder.set_tensor(inputs['position']['index'],
            np.int32([position]))
        for name, value in state.items():
            decoder.set_tensor(inputs[name]['index'], value)
        if dump is not None and position == 10:
            with open(dump, 'wb') as file:
                for detail in decoder.get_input_details():
                    file.write(decoder.get_tensor(detail['index']).tobytes())
        decoder.invoke()
        for name in state:
            fresh = decoder.get_tensor(
                outputs['new_' + name[len('self_'):]]['index'])
            # the one dimension in which they differ counts the positions
            axis = [i for i in range(fresh.ndim)
                if fresh.shape[i] != state[name].shape[i]][0]
            index = [slice(None)] * fresh.ndim
            index[axis] = slice(position, position + 1)
            state[name][tuple(index)] = fresh
        return decoder.get_tensor(outputs['logits']['index'])[0] \
            .astype(np.float64)

    tokens = list(prompt)
    begin = len(prompt)
    no_speech = 0.0
    for position in range(begin - 1):
        logits = step(position, tokens[position])
        if tokens[position] == model.sot:
            no_speech = float(np.exp(log_softmax(logits))[model.no_speech])

    first_timestamp = model.timestamp_begin
    total = 0.0
    count = 0
    for _ in range(model.max_tokens):
        logits = step(len(tokens) - 1, tokens[-1])
        if tokens[-1] == model.sot:
            no_speech = float(np.exp(log_softmax(logits))[model.no_speech])
        text = tokens[begin:]

        # what the next token must not be
        if not text:
            logits[model.blank] = -np.inf
            logits[model.eot] = -np.inf
        logits[model.suppress] = -np.inf
        if not timestamps:
            logits[first_timestamp:] = -np.inf
        else:
            logits[model.no_timestamps] = -np.inf
            # time stamps come in pairs, except right before the end
            last = len(text) >= 1 and text[-1] >= first_timestamp
            before = len(text) < 2 or text[-2] >= first_timestamp
            if last:
                if before:
                    logits[first_timestamp:] = -np.inf
                else:
                    logits[:model.eot] = -np.inf
            # and do not go back in time
            stamps = [token for token in text if token >= first_timestamp]
            if stamps:
                limit = stamps[-1] if last and not before \
                    else stamps[-1] + 1
                logits[first_timestamp:limit] = -np.inf
            if not text:
                # a window starts with a time stamp, of its first second
                logits[:first_timestamp] = -np.inf
                logits[first_timestamp + 50 + 1:] = -np.inf
            # all time stamps together likelier than any text: a time
            # stamp it is
            chances = log_softmax(logits)
            stamp = chances[first_timestamp:]
            finite = stamp[np.isfinite(stamp)]
            together = -np.inf if finite.size == 0 else finite.max() \
                + np.log(np.sum(np.exp(finite - finite.max())))
            if together > chances[:first_timestamp].max():
                logits[:first_timestamp] = -np.inf

        chances = log_softmax(logits)
        best = int(np.argmax(logits))
        total += float(chances[best])
        count += 1
        if best == model.eot:
            break
        tokens.append(best)
        if len(tokens) >= model.text_context:
            break

    return tokens[begin:], no_speech, total / (count + 1)


def transcribe(model, audio, timestamps=True, language=None,
        dump_prefix=None, show_tokens=False):
    """Returns the segments: (start, end, text), times in seconds."""
    prompt = [model.sot]
    if model.multilingual:
        prompt += [model.languages[language or 'en'], model.transcribe]
    if not timestamps:
        prompt.append(model.no_timestamps)

    first_timestamp = model.timestamp_begin
    frame_seconds = model.hop / model.sample_rate
    frames_per_stamp = round(model.timestamp_seconds / frame_seconds)
    content = len(audio) // model.hop
    segments = []
    seek = 0
    window = 0
    while seek < content:
        mel = log_mel(model, audio, seek * model.hop)
        dump = None
        if dump_prefix is not None:
            mel.tofile('%s.mel%d.bin' % (dump_prefix, window))
            dump = '%s.decoder%d.bin' % (dump_prefix, window)
        tokens, no_speech, mean = decode_window(model, mel, prompt,
            timestamps, dump)
        if show_tokens:
            print('  window %d at %.2f: no speech %.3f, mean log '
                'probability %.3f, tokens %s' % (window,
                    seek * frame_seconds, no_speech, mean, tokens))
        window += 1

        size = min(model.frames, content - seek)
        offset = seek * frame_seconds
        if no_speech > 0.6 and mean < -1.0:
            seek += size
            continue

        def add(start, end, part):
            text = model.decode_text(part)
            if end > start and text.strip():
                segments.append((start, end, text))

        is_stamp = [token >= first_timestamp for token in tokens]
        single_end = is_stamp[-2:] == [False, True]
        cuts = [i + 1 for i in range(len(tokens) - 1)
            if is_stamp[i] and is_stamp[i + 1]]
        if cuts:
            if single_end:
                cuts.append(len(tokens))
            last = 0
            for cut in cuts:
                part = tokens[last:cut]
                add(offset + (part[0] - first_timestamp)
                        * model.timestamp_seconds,
                    offset + (part[-1] - first_timestamp)
                        * model.timestamp_seconds, part)
                last = cut
            if single_end:
                seek += size
            else:
                seek += (tokens[last - 1] - first_timestamp) \
                    * frames_per_stamp
        else:
            duration = size * frame_seconds
            stamps = [token for token in tokens if token >= first_timestamp]
            if stamps and stamps[-1] != first_timestamp:
                duration = (stamps[-1] - first_timestamp) \
                    * model.timestamp_seconds
            add(offset, offset + duration, tokens)
            seek += size
    return segments


def test_vectors(model):
    """Prints what src/tests/kits/neural/speech_test.cpp has as its
    reference: the spectrogram of a signal the test makes the same way,
    at some places, and some weights of the filter bank."""
    count = 3 * model.sample_rate
    state = 12345
    audio = np.zeros(count, dtype=np.float32)
    for n in range(count):
        state = (state * 1664525 + 1013904223) & 0xffffffff
        noise = ((state >> 16) / 32768.0 - 1.0) * 0.02
        audio[n] = 0.4 * np.sin(2 * np.pi * 440.0 * n / model.sample_rate) \
            + 0.2 * np.sin(2 * np.pi * 1875.0 * n / model.sample_rate
                + 0.5) + noise

    print('// %d Hz, transforms of %d every %d, %d filters, %d frames'
        % (model.sample_rate, model.fft_size, model.hop, model.mel_bins,
            model.frames))
    for first in (0, 8000):
        mel = log_mel(model, audio, first)
        print('// the window from sample %d: filter, frame, value' % first)
        for filter in (0, 9, 24, 33, 50, 79):
            for frame in (0, 1, 57, 150, 299, 301, 2999):
                print('\t{ %d, %d, %d, %.7ff },' % (first, filter, frame,
                    mel[filter, frame]))
    print('// the filter bank: filter, bin, weight')
    for filter in (0, 5, 20, 40, 60, 79):
        # the largest weight and its neighbours
        peak = int(np.argmax(model.filters[filter]))
        for bin in (peak - 1, peak, peak + 1):
            print('\t{ %d, %d, %.9ff },' % (filter, bin,
                model.filters[filter, bin]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--test-vectors', action='store_true')
    parser.add_argument('--model', required=True)
    parser.add_argument('--no-timestamps', action='store_true')
    parser.add_argument('--language')
    parser.add_argument('--dump')
    parser.add_argument('--tokens', action='store_true')
    parser.add_argument('--reference-kernels', action='store_true')
    parser.add_argument('audio', nargs='*')
    args = parser.parse_args()

    model = Model(args.model, args.reference_kernels)
    if args.test_vectors:
        test_vectors(model)
        return 0
    for path in args.audio:
        audio = read_wave(path, model.sample_rate)
        prefix = None
        if args.dump:
            os.makedirs(args.dump, exist_ok=True)
            prefix = os.path.join(args.dump, os.path.basename(path))
        print('%s: %.2f s' % (path, len(audio) / model.sample_rate))
        for start, end, text in transcribe(model, audio,
                not args.no_timestamps, args.language, prefix, args.tokens):
            print('[%7.2f --> %7.2f] %s' % (start, end, text))
    return 0


if __name__ == '__main__':
    sys.exit(main())
