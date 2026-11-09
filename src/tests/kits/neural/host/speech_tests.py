#!/usr/bin/env python3
# Copyright 2026, air/OS contributors.
# Distributed under the terms of the MIT License.
"""Checks the Neural Kit's speech recognizer on the build host.

    speech_tests.py --host <build directory> --model <model directory>
        [--work <directory>] audio.wav...

The build directory has speech_host and neural_host (see Makefile), the
model directory is one whisper_convert.py made. With the recordings (16 bit
WAVE files at 16 kHz) these are compared:

 1. the audio converter (other rates, formats, channels) with tones whose
    result is known, and fed in odd pieces with fed at once;
 2. the tokenizer with Python's decoding of the same bytes;
 3. the front end's spectrogram of each recording with the NumPy one of
    speech_reference.py;
 4. encoder and decoder with LiteRT's reference kernels, tensor by tensor
    (compare.py --all), on noise and on what a recording puts into them;
 5. the text and times the kit's recognizer gives for each recording with
    the reference's (NumPy front end, LiteRT networks, Python decoding),
    with and without time stamps, and fed in pieces with fed at once.

Exits with 0 when everything agrees.

Needs: pip install numpy ai-edge-litert
"""

import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import speech_reference  # noqa: E402

failures = []


def check(ok, text):
    print('  %-66s %s' % (text, 'ok' if ok else 'FAILED'))
    if not ok:
        failures.append(text)


def run(command):
    result = subprocess.run(command, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr.decode('utf-8', 'replace'))
    return result.returncode, result.stdout.decode('utf-8')


def test_converter(host, work):
    print('== audio converter')
    formats = {
        'uint8': (np.uint8, lambda x: np.round(x * 127 + 128)),
        'int16': (np.dtype('<i2'), lambda x: np.round(x * 32767)),
        'int32': (np.dtype('<i4'), lambda x: np.round(x * 2147483000.0)),
        'float32': (np.dtype('<f4'), lambda x: x),
        'float64': (np.dtype('<f8'), lambda x: x),
    }
    # tones a 16 kHz recording can hold, and (for the higher rates) one it
    # cannot, which must be gone afterwards
    tones = ((300.0, 0.25), (1000.0, 0.2), (3100.0, 0.15))
    cases = (
        ('float32', 1, 16000), ('int16', 1, 16000), ('int16', 2, 44100),
        ('float32', 2, 48000), ('int16', 1, 8000), ('uint8', 1, 22050),
        ('int32', 6, 48000), ('float64', 1, 96000), ('int16', 2, 11025),
    )
    seconds = 1.5
    for name, channels, rate in cases:
        dtype, scale = formats[name]
        time = np.arange(int(rate * seconds)) / rate
        signal = sum(level * np.sin(2 * np.pi * frequency * time)
            for frequency, level in tones)
        if rate > 24000:
            signal = signal + 0.1 * np.sin(2 * np.pi * 11000.0 * time)
        # the channels differ, and their mean is the signal
        frames = np.stack([signal * (1 + 0.3 * (-1) ** channel)
            for channel in range(channels)], axis=1)
        if channels % 2 == 1:
            frames[:, -1] = signal
        raw = os.path.join(work, 'tone.raw')
        scale(frames).astype(dtype).tofile(raw)

        outputs = []
        for chunk in (65536, 7):
            out = os.path.join(work, 'tone%d.f32' % chunk)
            code, _ = run([host, 'convert', name, str(channels), str(rate),
                '16000', str(chunk), raw, out])
            outputs.append(np.fromfile(out, dtype='<f4') if code == 0
                else np.zeros(0, dtype='<f4'))
        ours = outputs[0]

        expected_count = len(time) * 16000 / rate
        time = np.arange(len(ours)) / 16000.0
        expected = sum(level * np.sin(2 * np.pi * frequency * time)
            for frequency, level in tones)
        # the ends, where the filter runs into silence, are left out
        inner = slice(1600, len(ours) - 1600)
        error = float(np.max(np.abs(ours[inner] - expected[inner]))) \
            if len(ours) > 4000 else 1.0
        # 8 bit samples are that coarse
        limit = 0.012 if name == 'uint8' else 0.002
        check(abs(len(ours) - expected_count) <= 1 and error < limit,
            '%s, %d channel(s), %d Hz: %d samples, off by %.1e'
            % (name, channels, rate, len(ours), error))
        check(len(outputs[1]) == len(ours)
            and np.array_equal(outputs[1], ours),
            '  the same fed 7 bytes at a time')


def test_tokenizer(host, model, model_directory):
    print('== tokenizer')
    vocabulary = model.vocabulary

    def find(text):
        return vocabulary.index(text)

    cases = [
        [find(b' Hello'), find(b','), find(b' world'), find(b'!')],
        # special tokens and time stamps are no text
        [model.sot, model.timestamp_begin, find(b' And'), find(b' so'),
            model.timestamp_begin + 400, model.eot],
        # characters of several bytes, whole and cut at either end
        [find(b'\xc3'), find(b'\xa9')],
        [find(b' caf'), find(b'\xc3')],
        [find(b'\xa9'), find(b' ok')],
        [find(b'\xe2\x80'), find(b'\x9c'), find(b'x'), find(b'\xe2\x80')],
        [find(b'\xf0\x9f'), find(b'\x98'), find(b'\x80'), find(b'\xf0\x9f'),
            find(b'!')],
        [find(b'\xe2\x80'), find(b'\xe2\x80'), find(b'\x9d')],
        [],
    ]
    for tokens in cases:
        expected = model.decode_text(tokens)
        code, output = run([host, 'text', model_directory]
            + [str(token) for token in tokens])
        check(code == 0 and output == expected + '\n',
            '%d tokens -> %r' % (len(tokens), expected))


def test_front_end(host, model, model_directory, audio_paths, work):
    print('== front end')
    for path in audio_paths:
        audio = speech_reference.read_wave(path, model.sample_rate)
        # the first window, and one that begins inside the recording
        for first in (0, (len(audio) // 3) // model.hop * model.hop):
            reference = speech_reference.log_mel(model, audio, first)
            out = os.path.join(work, 'mel.bin')
            code, _ = run([host, 'mel', model_directory, path, str(first),
                out])
            ours = np.fromfile(out, dtype='<f4').reshape(reference.shape) \
                if code == 0 else np.zeros_like(reference)
            error = float(np.max(np.abs(ours - reference)))
            check(code == 0 and error < 2e-4,
                '%s from sample %d: largest difference %.1e'
                % (os.path.basename(path), first, error))


def test_networks(host_directory, model_directory, dumps, python):
    print('== networks against LiteRT\'s reference kernels')
    compare = [python, '-I', os.path.join(here, 'compare.py'), '--host',
        os.path.join(host_directory, 'neural_host'), '--all', '--quiet']
    encoder = os.path.join(model_directory, 'encoder.tflite')
    decoder = os.path.join(model_directory, 'decoder.tflite')

    # Noise: every tensor within the harness' 1e-4 of the largest value,
    # but for the last layers of the encoder, which noise drives to
    # values the model never sees (their differences are 1.2e-4).
    code = subprocess.run(compare + ['--seeds', '2', '--tolerance', '3e-4',
        encoder]).returncode
    check(code == 0, 'encoder on noise (two inputs), all tensors')
    code = subprocess.run(compare + ['--seeds', '2', decoder]).returncode
    check(code == 0, 'decoder on noise (two inputs), all tensors')

    for mel, state in dumps:
        code = subprocess.run(compare + ['--inputs', mel, encoder]) \
            .returncode
        check(code == 0, 'encoder on %s, all tensors'
            % os.path.basename(mel))
        code = subprocess.run(compare + ['--inputs', state, decoder]) \
            .returncode
        check(code == 0, 'decoder on %s, all tensors'
            % os.path.basename(state))


def test_text(host, model, model_directory, audio_paths, work):
    print('== text')
    dumps = []
    for path in audio_paths:
        audio = speech_reference.read_wave(path, model.sample_rate)
        prefix = os.path.join(work, os.path.basename(path))
        for timestamps in (True, False):
            segments = speech_reference.transcribe(model, audio, timestamps,
                dump_prefix=prefix if timestamps else None)
            expected = ''.join('[%7.2f --> %7.2f] %s\n' % segment
                for segment in segments)
            options = [] if timestamps else ['--no-timestamps']
            code, output = run([host, 'transcribe', model_directory]
                + options + [path])
            ours = ''.join(output.splitlines(True)[1:])
            label = '%s%s' % (os.path.basename(path),
                '' if timestamps else ', no time stamps')
            check(code == 0 and ours == expected and len(segments) > 0,
                label + ': the reference\'s text')
            if ours != expected:
                print('reference:\n%skit:\n%s' % (expected, ours))
            elif timestamps:
                print(''.join('      ' + line
                    for line in expected.splitlines(True)), end='')

            # a second and a bit at a time, as a recorder hands it over
            code, output = run([host, 'transcribe', model_directory]
                + options + ['--chunk', '17000', path])
            check(code == 0
                and ''.join(output.splitlines(True)[1:]) == expected,
                label + ': the same fed in pieces')
        dumps.append((prefix + '.mel0.bin', prefix + '.decoder0.bin'))
    return dumps


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--model', required=True)
    parser.add_argument('--work')
    parser.add_argument('audio', nargs='+')
    args = parser.parse_args()

    work = args.work or tempfile.mkdtemp(prefix='neural-speech-')
    os.makedirs(work, exist_ok=True)
    host = os.path.join(args.host, 'speech_host')
    model = speech_reference.Model(args.model)

    # what compare.py needs to know to make noise for the networks: the
    # range of a spectrogram, and of the decoder's token and position
    with open(os.path.join(args.model, 'encoder.tflite.json'), 'w') as file:
        file.write('{"inputs": [[-1.0, 1.5]]}\n')
    with open(os.path.join(args.model, 'decoder.tflite.json'), 'w') as file:
        file.write('{"int_ranges": {"0": [0, %d], "1": [0, %d]}}\n'
            % (model.timestamp_begin - 1, model.text_context - 1))

    test_converter(host, work)
    test_tokenizer(host, model, args.model)
    test_front_end(host, model, args.model, args.audio, work)
    dumps = test_text(host, model, args.model, args.audio, work)
    test_networks(args.host, args.model, dumps, sys.executable)

    if failures:
        print('FAILED (%d): %s' % (len(failures), '; '.join(failures)))
        return 1
    print('all speech tests passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
