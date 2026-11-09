# Neural Kit

The Neural Kit runs neural networks on the processors a machine has for
them: neural processing units (NPUs), graphics processors and, when there is
nothing better or for the parts nothing better can do, the CPU. It is a kit
like the others: a library (`libneural.so`), public headers
(`<NeuralKit.h>`), add-ons for the hardware (`add-ons/neural`) and kernel
drivers that publish the processors under `/dev/npu`. Branch `neural-kit`
(2026-11).

A program hands the kit a model file and gets answers; which processor
computed them is the kit's business, and the answers are the same on all of
them.

```cpp
#include <NeuralKit.h>

BNeuralModel model("/boot/home/models/mobilenet_v1_1.0_224_quant.tflite");
BNeuralSession session(model);                  // loads it onto the best device
if (session.InitCheck() != B_OK)
	return session.InitCheck();

BNeuralUtils::SetTensorFromBitmap(session.InputAt(0), bitmap);
if (session.Run() == B_OK)
	int64 best = session.OutputAt(0)->IndexOfMaximum();
```

Link with `-lneural -lbe`.

## What runs where

| Machine | Device | Back end | Driver | Runs |
| --- | --- | --- | --- | --- |
| every machine | `cpu` | built into the kit | none | every operator the kit knows, float and quantized |
| ROCK 5 ITX and other RK3588 boards | `rk3588_npu/0`, the SoC's NPU | `add-ons/neural/rk3588_npu.neural` | `drivers/npu/rk3588_npu` → `/dev/npu/rk3588/0` | quantized convolutions (see [RK3588](#rockchip-rk3588)) |
| machines with a Vulkan driver: the X399's GTX 1070 (NVK), AMD graphics (RADV), … | `vulkan/0`, … | `add-ons/neural/vulkan.neural` | the graphics driver | float networks in compute shaders (see [Vulkan](#vulkan)) |
| Framework 13 with a Ryzen AI 300 (and other AMD XDNA NPUs) | `amdxdna/0` | `add-ons/neural/amdxdna.neural` | `amdxdna` (Linux's driver on the linux-drm compatibility layer) → `/dev/npu/amdxdna/0` | compiled NPU designs (see [AMD XDNA](#amd-xdna)) |

What has been measured on which machine is under [Status](#status).

## How it is built

```
headers/os/NeuralKit.h                       everything below at once
headers/os/neural/NeuralDefs.h               types, flags, messages, errors
headers/os/neural/Tensor.h                   BTensorInfo, BTensor
headers/os/neural/NeuralModel.h              BNeuralModel, BNeuralModelBuilder
headers/os/neural/NeuralOperators.h          the operators and their parameters
headers/os/neural/NeuralRoster.h             BNeuralDevice, BNeuralRoster
headers/os/neural/NeuralSession.h            BNeuralSession
headers/os/neural/NeuralUtils.h              BNeuralUtils: bitmaps, labels
headers/os/neural/SpeechRecognizer.h         BSpeechRecognizer: speech to text
headers/os/add-ons/neural/NeuralBackend.h    the back-end API
headers/private/neural/                      the graph, the interpreter, the roster's inside,
                                             the speech recognizer's core
src/kits/neural/                             libneural.so
src/kits/neural/ops/                         the CPU kernels, one file per family
src/add-ons/neural/                          the back ends
src/add-ons/kernel/drivers/npu/              kernel drivers
src/bin/neural/                              the neural command
src/tests/kits/neural/                       tests; host/ runs the interpreter on the build host
```

```
 application ── BNeuralModel ── BNeuralSession ── BTensor
                     │                │
                     │        ┌───────┴────────────────────┐
              model loader    │ partitions of the network   │
              (TensorFlow     ▼                             ▼
               Lite)     CPU interpreter            BNeuralProgram   (one per stretch
                         (the "cpu" device)               │           the device runs)
                                                          │
 libneural.so ────────────────────────────────────────────┼─────────────
 add-ons/neural/*.neural            BNeuralBackend ───────┘
                                          │ ioctl, Vulkan, …
 kernel ───────────────────────── /dev/npu/<driver>/<n>, /dev/dri/…
```

- **A model** (`BNeuralModel`) is a network read into the kit's own
  representation: tensors, operators, weights. The file is mapped, not
  read, so the weights are paged in on demand and shared between teams.
  Models cannot be changed and copies share the network.
- **A session** (`BNeuralSession`) is a model made ready on one device. When
  it is set up, the kit asks the device's back end which operators it can
  run, cuts the network into stretches of such operators and stretches of
  the others, has the back end make a *program* for each of its stretches
  and prepares the CPU interpreter for the rest. `Run()` then runs the
  stretches in order. A device that runs nothing of a model is not picked
  for it; a back end that fails to make a program is replaced by the CPU
  for that stretch; a run the device fails is done by the CPU (the caller
  gets its answer, `neural_run_info::device_failures` counts), and a
  device that goes away, or fails three runs in a row, loses its stretch
  to the CPU for good. So a
  session always works, and gives the same answers (bit for bit for
  quantized networks on the CPU; within the tolerances under
  [Status](#status) on devices).
- **The roster** (`BNeuralRoster`) knows the devices. It loads the back-end
  add-ons the first time it is asked something; a back end whose hardware
  is not there reports no devices and costs nothing more.
- **Back ends** are add-ons, found like translators and accelerants:
  `add-ons/neural` in the user's non-packaged directory, the user's
  packages, the system's non-packaged directory and the system package, in
  that order; the first file of a name wins, so a back end under development
  shadows the packaged one.
- **Kernel drivers** only give access to a processor: memory it can reach,
  power, a queue of jobs. They know nothing of networks. Each lives in
  `add-ons/kernel/drivers/npu` (or is linked from `drivers/dev/npu`) and
  publishes `/dev/npu/<driver>/<n>`; the directory `drivers/dev/npu` in the
  package is what makes `/dev/npu` exist, and looking into it is what loads
  the drivers, so a machine that never runs a network never loads them.

## The API

### Tensors

A `BTensorInfo` says what a tensor looks like: element type
(`B_NEURAL_FLOAT32`, `B_NEURAL_UINT8`, `B_NEURAL_INT8`, …), shape (up to
`B_NEURAL_MAX_RANK` dimensions; pictures are batch, height, width, channels)
and, for quantized tensors, the scale and zero point that turn stored
integers into real values: `real = scale * (stored - zero point)`.

A `BTensor` is a `BTensorInfo` and the numbers. It owns its memory
(`SetTo(info)`) or uses the caller's (`SetTo(info, data, size)`). `Data()`
is the raw storage; `ValueAt()`, `GetValues()`, `SetValueAt()` and
`SetValues()` read and write real values whatever the storage, so the same
code handles a float model and its quantized sibling. A `BTensor` is a
`BArchivable`: `Archive()` puts shape and numbers in a `BMessage`, which
can be sent to another team, stored in a file or an attribute, and
`BTensor::Instantiate()` makes a tensor of it again.

### Models

```cpp
BNeuralModel model;
status_t status = model.SetTo(&ref);            // or a path, a BDataIO, a buffer
if (status != B_OK)
	fprintf(stderr, "%s: %s\n", strerror(status), model.ErrorDetails());

for (int32 i = 0; i < model.CountInputs(); i++) {
	BTensorInfo info;
	model.GetInputInfo(i, &info);
	puts(info.ToString());                      // "input uint8 [1, 224, 224, 3] scale 0.0078125 zero 128"
}
```

The kit reads TensorFlow Lite files (`.tflite`, schema version 3) itself,
with a reader that checks every offset: a model file is input from
anywhere. `BNeuralModel::IsModel()` tells whether a file is one without
reading it. `GetInfo()` fills a `BMessage` with what else is known (name,
description, format, operator kinds, bytes of weights).

A model whose operators the kit does not know does not load
(`B_NEURAL_UNSUPPORTED_OPERATOR`; `ErrorDetails()` names the operator). The
operators the kit knows are in `NeuralOperators.h` and under
[Operators](#operators); shapes are fixed when the model is made (no
dynamic shapes, no control flow). A model keeps nothing between runs:
one that works in steps, like the decoder of a speech model, gets what
its earlier steps computed as inputs (see
[Speech recognition](#speech-recognition)).

`BNeuralModelBuilder` makes a model in memory, operator by operator, for
programs that compute their network and for tests:

```cpp
BNeuralModelBuilder builder("scale");
int32 dims[] = { 1, 8 };
int32 x = builder.AddInput(BTensorInfo(B_NEURAL_FLOAT32, 2, dims, "x"));
int32 y = builder.AddOperator(B_NEURAL_OP_RELU6, &x, 1,
	BTensorInfo(B_NEURAL_FLOAT32, 2, dims, "y"));
builder.MarkOutput(y);
BNeuralModel model;
status_t status = builder.Build(&model);
```

### Devices

```cpp
BNeuralRoster* roster = BNeuralRoster::Default();
for (int32 i = 0; i < roster->CountDevices(); i++) {
	BNeuralDevice device;
	roster->GetDeviceAt(i, &device);
	printf("%s: %s %s, runs %d of %d operators\n", device.ID(),
		neural_device_kind_name(device.Kind()), device.Name(),
		(int)device.CountSupportedOperators(model),
		(int)model.CountOperators());
}
```

A `BNeuralDevice` is a value that names a device: `ID()` (`"cpu"`,
`"rk3588_npu/0"`, `"vulkan/0"`: stable across restarts, the thing to keep in
a settings file), `Name()`, `Vendor()`, `Kind()` (CPU, GPU, NPU), `Flags()`
(quantized and/or float networks, removable) and `GetInfo()` for whatever
else the back end knows (driver, version, cores, clock, memory).

`GetDefaultDevice(&device, flags, &model)` is the choice the kit makes when
a program has none: an NPU, else a graphics processor, else the CPU, among
the devices that run at least part of the model. The flags change it:

| Flag | Meaning |
| --- | --- |
| `B_NEURAL_PREFER_SPEED` (0) | the fastest kind of device (the default) |
| `B_NEURAL_PREFER_LOW_POWER` | NPU, else CPU, else graphics processor |
| `B_NEURAL_NO_FALLBACK` | only devices that run all of the model; a session fails with `B_NEURAL_NOT_ACCELERATED` instead of using the CPU for part of it |
| `B_NEURAL_CPU_ONLY` | the CPU, whatever else there is |
| `B_NEURAL_PROFILE` | (sessions) keep per-operator times |

The user's choice comes first when it can do the job: the settings file
`~/config/settings/neural` (driver settings syntax) and the environment,

```
preferred_device rk3588_npu/0      # or NEURAL_DEVICE=rk3588_npu/0
disable_backend vulkan             # or NEURAL_DISABLE_BACKENDS="vulkan" ("all": CPU only)
```

`NEURAL_THREADS=n` sets how many threads the CPU device uses (default: one
per CPU, at most 16).

`StartWatching(messenger)` sends `B_NEURAL_DEVICE_ADDED` and
`B_NEURAL_DEVICE_REMOVED` (fields `"device"`: the identifier, `"name"`,
`"kind"`) when devices come and go: the roster watches `/dev/npu` and
`Rescan()` asks the back ends anew.

### Sessions

```cpp
BNeuralSession session;
status_t status = session.SetTo(model);                 // the default device
// or: session.SetTo(model, device, B_NEURAL_NO_FALLBACK);

BTensor* input = session.InputAt(0);                    // owned by the session
// fill input->Data(), or use BNeuralUtils
status = session.Run();                                 // blocks until done
const BTensor* output = session.OutputAt(0);
```

Setting a session up is the slow part (the back end translates the stretch
of the network it takes and uploads the weights); running is fast and
repeated as often as wanted. The input and output tensors belong to the
session and keep their memory between runs. A session runs one thing at a
time (`B_BUSY` otherwise); several sessions, also of one model, run at once
and share a device.

A window or any other looper does not block on a network:

```cpp
session.RunAsync(BMessenger(this));             // returns at once

void MyWindow::MessageReceived(BMessage* message)
{
	if (message->what == B_NEURAL_RUN_COMPLETED
		&& message->GetInt32("neural:status", B_ERROR) == B_OK) {
		// session.OutputAt() is ready; "neural:duration" says how long it took
	}
}
```

`RunAsync()` takes a message of the caller's instead of
`B_NEURAL_RUN_COMPLETED` if wanted (the `neural:` fields are added to it)
and returns a token that comes back in `"neural:token"`;
`"neural:session"` is a pointer to the session, for a handler that has
several. `WaitForRun(timeout)` waits for such a run. Deleting a session
waits for its run to end: a run cannot be cancelled yet.

`GetRunInfo()` says how the model was split (operators on the device, on the
CPU, number of device partitions) and how long runs take;
`GetOperatorInfo(index)` says where one operator runs and, with
`B_NEURAL_PROFILE`, how long it took.

### Pictures, labels

`BNeuralUtils` ties tensors to the Interface Kit:

```cpp
neural_image_options options;                   // fit, channel order, mean/deviation
options.fit = B_NEURAL_FIT_LETTERBOX;
neural_image_mapping mapping;
BNeuralUtils::SetTensorFromBitmap(session.InputAt(0), bitmap, NULL,
	&options, &mapping);
...
// a detection model gives boxes as fractions of its input:
BRect inBitmap = mapping.ToBitmap(left, top, right, bottom);
```

`SetTensorFromBitmap()` scales a bitmap of any size and of the usual color
spaces into an image tensor (uint8, int8 or float; one or three channels),
stretched, cropped or letterboxed; `GetBitmapFromTensor()` goes the other
way; `GetLabels()` reads the label files models come with;
`GetTopValues()` picks the best classes of a classifier, and
`IsDetector()` / `GetDetections()` read what a detector found:

```cpp
neural_detection found[32];
int32 count = BNeuralUtils::GetDetections(session, found, 32);
for (int32 i = 0; i < count; i++) {
	BRect box = mapping.ToBitmap(found[i].left, found[i].top,
		found[i].right, found[i].bottom);
	// labels.StringAt(found[i].class_index + 1), found[i].score
}
```

### Errors

Besides the usual ones (`B_NO_INIT`, `B_BAD_VALUE`, `B_NO_MEMORY`,
`B_BUSY`, file errors; `neural_error_string()` puts any of them in
words):

| Error | When |
| --- | --- |
| `B_NEURAL_BAD_MODEL` | the file is not a model or is damaged |
| `B_NEURAL_UNKNOWN_FORMAT` | no loader knows the format or version |
| `B_NEURAL_UNSUPPORTED_OPERATOR` | the model has an operator the kit has no kernel for |
| `B_NEURAL_TYPE_MISMATCH`, `B_NEURAL_SHAPE_MISMATCH` | a tensor does not fit |
| `B_NEURAL_NO_DEVICE` | no such device |
| `B_NEURAL_DEVICE_LOST` | the device went away and the CPU could not take over (`B_NEURAL_NO_FALLBACK`) |
| `B_NEURAL_NOT_ACCELERATED` | `B_NEURAL_NO_FALLBACK` and the device cannot run all of the model |

## Operators

The kit's operators follow TensorFlow Lite's, whose quantized arithmetic is
specified to the bit; the CPU kernels reproduce it, which makes the CPU
device the reference every back end is checked against (`neural compare`).

| Group | Operators |
| --- | --- |
| Convolution | `CONV_2D`, `DEPTHWISE_CONV_2D`, `TRANSPOSE_CONV`, `FULLY_CONNECTED` |
| Pooling | `AVERAGE_POOL_2D`, `MAX_POOL_2D`, `MEAN` |
| Arithmetic | `ADD`, `SUB`, `MUL`, `DIV`, `MAXIMUM`, `MINIMUM` (broadcasting) |
| Activation | `RELU`, `RELU6`, `LEAKY_RELU`, `PRELU`, `LOGISTIC`, `TANH`, `HARD_SWISH`, `SOFTMAX` |
| Shape | `RESHAPE`, `SQUEEZE`, `CONCATENATION`, `PAD`, `TRANSPOSE`, `STRIDED_SLICE`, `RESIZE_BILINEAR`, `RESIZE_NEAREST` |
| Types | `QUANTIZE`, `DEQUANTIZE` |
| Transformers | `BATCH_MATMUL` (products of matrices; the dimensions before the last two broadcast, either input may be taken transposed), `GELU` (exact and the tanh approximation), `GATHER` (slices picked by index along a dimension: embeddings; any element type), `SQUARED_DIFFERENCE`, `RSQRT`; float32 |
| Detection | `DETECTION_POSTPROCESS` (TensorFlow Lite's `TFLite_Detection_PostProcess`: boxes, classes, scores and count from a single-shot detector's raw output) |

That covers the MobileNet family (V1, V2; classification and SSD
detection), which is what the first uses need: recognising what a camera
sees, and the encoder and decoder of a Whisper speech model (layer
normalisation is `MEAN`, `SQUARED_DIFFERENCE`, `RSQRT` and arithmetic, as
converters write it). `GATHER` checks indices that come with the input on
every run: one that is out of range fails the run. Weights stored as
8 bit integers with floating point activations ("dynamic range"
quantization) are not supported.

## Speech recognition

`BSpeechRecognizer` turns speech into text on the machine itself, with a
Whisper model the kit runs. Audio goes in as it is recorded, text comes
out as messages; nothing the caller does waits for the model.

```cpp
#include <NeuralKit.h>

BSpeechRecognizer recognizer;
status_t status = recognizer.SetTo();           // the model that is installed
recognizer.SetTarget(BMessenger(this));
recognizer.SetInterimInterval(2000000);         // read unfinished audio too

recognizer.Start(&format);                      // a media_raw_audio_format
// for every buffer the recording gives, from whatever thread has it:
recognizer.Feed(buffer, size);
// when the recording is over:
recognizer.Finish();

void MyWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_SPEECH_RESULT:
		{
			const char* text = message->GetString("speech:text", "");
			if (message->GetBool("speech:final", false))
				fTranscript << text << " ";     // a phrase that stands
			else
				fPending = text;                // replaces the one before
			break;
		}
		case B_SPEECH_FINISHED:
			// "speech:status"; the recording is done with
			break;
	}
}
```

or, for a recording that is complete (16000 samples per second, one
channel, float; or any raw format):

```cpp
BString text;
recognizer.Transcribe(samples, count, &text);
for (int32 i = 0; i < recognizer.CountSegments(); i++) {
	speech_segment segment;
	recognizer.GetSegment(i, &segment);         // start, end (µs), text
}
```

| Method | Does |
| --- | --- |
| `SetTo(model, flags)`, `SetTo(model, device, flags)` | loads a model: a name (`"whisper-tiny.en"`), the path of a model directory, or `NULL` for the first one found; device and flags as for `BNeuralSession` |
| `FindModel()`, `GetModels()` | the installed models: directories with a `speech.conf` under `neural/models` of the data directories |
| `GetLanguages()`, `SetLanguage("de")` | for models of many languages; `"en"` to begin with |
| `SetTimestamps(bool)` | whether the model is asked for the times of its phrases (default: yes) |
| `SetTarget(messenger, message)` | where results go; with a message of the caller's the fields are added to it, and `"speech:finished"` tells the two kinds apart |
| `SetInterimInterval(µs)` | how often, at most, audio that is not final yet is read for a result that is not final; 0: never |
| `Start(format, &token)` | begins a recording in a `media_raw_audio_format` (`NULL`: 16 kHz, mono, float); the token comes back in the messages |
| `Feed(buffer, size)` | more audio; returns at once (`B_BUSY` when ten minutes are waiting) |
| `Finish()` | the recording is over: the rest is recognized, then `B_SPEECH_FINISHED` is sent |
| `Cancel()` | drops the recording; no more results |
| `WaitUntilFinished(timeout)`, `IsRecognizing()` | for callers that do want to wait |
| `Transcribe(...)`, `CountSegments()`, `GetSegment()` | a whole recording at once; the phrases of the recording so far |
| `GetInfo()` | audio recognized, time spent on it (front end, encoder, decoder), windows, tokens |

`B_SPEECH_RESULT` has `"speech:text"`, `"speech:final"` (bool),
`"speech:start"` and `"speech:end"` (int64: microseconds from the
beginning of the recording), `"speech:token"` and `"speech:recognizer"`;
`B_SPEECH_FINISHED` has `"speech:status"`, `"speech:audio"` and
`"speech:duration"` (microseconds of audio, and of work).

**How it works.** Whisper takes 30 seconds of audio at a time. The
recognizer's thread makes the log-mel spectrogram of such a window (a
400-sample Hann window every 160 samples, 80 mel filters), runs the
encoder on it once and the decoder once per token, picking the likeliest
token each time under Whisper's rules for what may follow what. The model
writes time stamps between its phrases: a phrase is **final** when the
model has seen its end, and the next window begins where the last whole
phrase ended, so that a phrase cut by the end of a window is heard again
in full. A recording shorter than a window therefore gives its final
phrases only at `Finish()`; what is heard meanwhile comes as results that
are not final, if `SetInterimInterval()` asks for them (each such reading
costs what a window costs, see below; one result carries the reading of
everything after the last final phrase and replaces the one before).
Audio of any raw format of the Media Kit (8, 16, 32 bit, float; up to
eight channels, any rate, either byte order) is brought to one channel at
16 kHz with a windowed sinc filter as it is fed.

Deleting a recognizer is safe at any time; it waits for the step the
model is in (at most an encoder run).

**What is not Whisper's reference implementation:** the loudest value of
the spectrogram, below which nothing may be quieter by more than 80 dB,
is taken per window, not over the whole recording; the decoder is not
given the text before the window; a window is decoded once (greedy, no
beam search, no second tries at higher temperatures when the text looks
wrong), so a hard stretch of audio can give repeated or wrong text where
the reference would try again. A window the model takes for silence
gives no text.

### Speech models

A speech model is a directory:

| File | Is |
| --- | --- |
| `speech.conf` | lines of a key and values: the family (`whisper`), the file names, the sizes of the front end, the numbers of the special tokens, the tokens that are never produced, the languages |
| `encoder.tflite` | spectrogram `"mel"` [1, 80, 3000] to, for each layer of the decoder, the keys and values its attention to the audio needs (`"cross_k_<n>"`, `"cross_v_<n>"`) |
| `decoder.tflite` | one token per run: `"token"`, `"position"`, the encoder's outputs, and the keys and values of the steps before (`"self_k_<n>"`, `"self_v_<n>"`) to `"logits"` and this step's keys and values (`"new_k_<n>"`, `"new_v_<n>"`), which the recognizer puts into the inputs at the step's position |
| `vocabulary.bin` | the byte strings of the text tokens |
| `mel_filters.bin` | the mel filter bank |
| `LICENSE` | the licence of the weights |

Both networks have fixed shapes and no control flow; the loop over the
tokens is the recognizer's (`src/kits/neural/SpeechWhisper.cpp`, which is
all the kit knows about Whisper; the numbers of a model are in its
`speech.conf`, and another family of models would be another decoder
class beside it).

OpenAI publishes Whisper as PyTorch checkpoints (MIT). The pre-converted
TensorFlow Lite files that could be found run the whole decoding loop
inside the model (a `WHILE` operator and subgraphs) or put encoder and
decoder into one file with call operators, and run the decoder over all
tokens so far for every new one; so the kit's files are made from the
checkpoint by `tools/airos/neural/whisper_convert.py`, which needs
neither PyTorch nor TensorFlow, and checks the result against a NumPy
version of Whisper's equations. `fetch-models.sh <data directory> speech`
downloads the checkpoint and the vocabulary by their checksums and
converts them (on a Linux host with Python, numpy, flatbuffers and
ai-edge-litert; the directory is then copied to the machine):

| Set | Model | Files |
| --- | --- | --- |
| `speech` | `neural/models/whisper-tiny.en`: Whisper tiny.en, English, 39 million weights | encoder 37.6 MB, decoder 114.3 MB (float32), vocabulary 0.4 MB |
| `speech-multilingual` | `neural/models/whisper-tiny`: Whisper tiny, 99 languages | the same sizes |

No model is in the images yet.

### In a browser

Summit's `SpeechRecognition` (`webkitSpeechRecognition`,
airos-works/summit#159) needs a back end that does this:

1. `start()`: open the microphone with the Media Kit (a consumer node
   connected to the system's audio input; the kit's private
   `BMediaRecorder` is such a node) and call `Start(&format)` with the
   format the input gives. `lang` maps to
   `SetLanguage()` with a multilingual model (`"de-DE"` to `"de"`);
   `interimResults` to `SetInterimInterval()` (two to three seconds is a
   good start on machines like the ROCK 5, where a reading takes one to
   two and a half).
2. Every buffer of the recording goes to `Feed()` from the thread that
   has it; nothing blocks.
3. `B_SPEECH_RESULT` with `"speech:final"` false becomes a `result` event
   whose one result has `isFinal` false; with true, a result with
   `isFinal` true that is added to the list. Whisper gives no confidence;
   report 1 or leave it at 0.
4. `stop()`: `Finish()`; the remaining results come, then
   `B_SPEECH_FINISHED`, which is the `end` event (an error status becomes
   `error`). `abort()`: `Cancel()`, then `end`.
5. The recognizer is set up once and kept (a few tenths of a second; the
   machine's used memory grows by about 425 MB while it works, the 150 MB
   of mapped model files included); `"speech:token"` tells the results of
   an aborted recording from those of the next.

What the kit does not do yet and a browser will want: noticing by itself
that the speaker has stopped (the Web Speech API ends a recognition that
is not `continuous` at the first pause; see
[Open points](#open-points)), and permission handling for the microphone,
which is the browser's.

## Writing a back end

A back end is one class in an add-on named `<name>.neural`:

```cpp
#include <add-ons/neural/NeuralBackend.h>

class MyBackend : public BNeuralBackend {
public:
	MyBackend(image_id image) : BNeuralBackend(image) {}

	virtual const char* Name() const { return "mine"; }
	virtual int32 CountDevices();                       // look for hardware
	virtual status_t GetDeviceInfo(int32 device, neural_device_info* info,
		BMessage* details);
	virtual status_t GetSupportedOperators(int32 device,
		const BNeuralGraph& graph, bool* supported);
	virtual status_t CreateProgram(int32 device, const BNeuralGraph& graph,
		const neural_partition& partition, uint32 flags,
		BNeuralProgram** _program);
};

BNeuralBackend*
instantiate_neural_backend(image_id image)
{
	return new(std::nothrow) MyBackend(image);
}
```

- `BNeuralGraph` shows the network: `TensorAt(i)` (type, shape,
  quantization, per-channel scales, and for constants the numbers),
  `OperatorAt(i)` (code, input and output tensor indices, the parameter
  structure of `NeuralOperators.h`). Operators are in an order in which
  they can run.
- `GetSupportedOperators()` must be exact and quick: it looks at codes,
  types, shapes and parameters and says yes only to what the device will
  compute correctly. The kit asks before anything is loaded, also just to
  choose a device.
- `CreateProgram()` gets a `neural_partition`: operators that follow one
  another, the tensors they need from outside and the tensors they make
  that are needed outside. It converts and uploads weights and returns a
  `BNeuralProgram`. An error means "run this stretch on the CPU".
- `BNeuralProgram::Run(tensors)` gets a table with the memory of the
  partition's inputs and outputs (indexed like the graph's tensors, in the
  graph's types and NHWC layout), computes, and returns when the outputs
  are written. On an error the kit computes the stretch on the CPU for
  this run and tries the device again next time (three failures in a row
  end that); `B_NEURAL_DEVICE_LOST` hands the stretch to the CPU at once
  and for good.
- Programs of different sessions run at the same time: a back end is
  thread safe.

A back end is checked with `neural compare <model> -d <device>`, which
runs device and CPU on the same input and shows the differences, and with
single-operator models made by `BNeuralModelBuilder` (see
`src/tests/kits/neural`).

## The neural command

```
neural devices                          the devices and what their back ends say about them
neural info model.tflite                inputs, outputs, operators, and what each device runs of it
neural run model.tflite -i cat.jpg -l labels.txt
                                        runs it on the picture and prints the best classes or the
                                        detections; -d <device>, --cpu, --no-fallback, -n <runs>,
                                        -p (where each operator ran and how long), -o <file>
neural compare model.tflite -d rk3588_npu/0
                                        the device against the CPU on the same input;
                                        --tolerance <steps>, --float-tolerance <fraction>
neural bench model.tflite               times it on every device
neural transcribe speech.wav            writes down what is said in a recording (any file the
                                        Media Kit decodes; a WAVE file also without it), the
                                        phrases with their times, and how long it took;
                                        --model <name or directory>, -d <device>, --cpu,
                                        --language <code>, --no-timestamps, --interim <seconds>
```

Without `-i` the input is noise that is the same on every run.

## Drivers and back ends

### Rockchip RK3588

`rk3588_npu` (`src/add-ons/kernel/drivers/npu/rk3588_npu`, interface in
`rk3588_npu.h`) drives the first of the SoC's three NPU cores (an NVDLA
descendant). The firmware's device tree does not describe the NPU on
today's boards, so the driver attaches to the RK3588's clock and reset
unit and uses the SoC's addresses. It touches nothing until the device is
opened; then it switches the core's power domain on (PMU, clocks from the
CRU at GPLL/6 = 198 MHz, the frequency Linux uses), and off again when the
last client closes.

| ioctl | Does |
| --- | --- |
| `RK3588_NPU_GET_INFO` | interface and hardware version, cores, clock |
| `RK3588_NPU_CREATE_BUFFER` | memory the NPU can reach: an area in the caller's team and its device address |
| `RK3588_NPU_DELETE_BUFFER` | frees one |
| `RK3588_NPU_SYNC_BUFFER` | cache maintenance before the device reads what the CPU wrote, and the other way round |
| `RK3588_NPU_SUBMIT` | runs "register command" lists (one per layer or slice of one) in sequence and waits for them |

Every open of the device has its own address space in the core's IOMMU
(the driver has a small driver for Rockchip's IOMMU in it): a client's
jobs can only reach that client's buffers, whatever its command lists say,
and a job that reaches outside fails with `B_BAD_ADDRESS` and a line in the
syslog, after which the core is reset. The buffers belong to the kernel;
the client's areas are views of them, so deleting an area cannot pull
memory from under a running job.

The back end (`rk3588_npu.neural`) is a port of the code generator of
Mesa's `rocket` driver (MIT): it turns quantized convolutions into register
command lists, splits layers that do not fit the core's convolution buffer
into tasks, and converts tensors between the kit's layout and the NPU's.
Several layer shapes that Mesa's code gets wrong on this hardware (17 to
31 and 33 to 48 input channels, some depthwise channel counts, large
kernels on 1024 channels) are corrected, and the activation ranges and the
scale of fused additions are computed instead of looked up.

Settings: `clock_divider n` in `~/config/settings/kernel/drivers/rk3588_npu`
(the core's clock is 1188 MHz / n; the default 6 is safe at every supply
voltage of the NPU's operating table).

### Vulkan

`vulkan.neural` runs float networks in compute shaders on any Vulkan
device that is a real graphics processor. It has no link-time dependency:
it loads `libvulkan.so.1` when the roster asks for devices and reports none
when there is no loader or no GPU. On the X399 that is NVK on the GTX 1070:
inference on that card with open user-space code (the kernel side there is
still NVIDIA's resource manager, as for everything else the card does).

### AMD XDNA

The NPU of Ryzen AI processors ("XDNA": an array of AI Engine tiles) is
driven by Linux's `amdxdna` driver (the 7.4-rc2 sources), built for air/OS
on the same compatibility layer as `amdgpu` (repository `linux-drm`,
branch `airos-xdna`) as an add-on of its own. It publishes
`/dev/npu/amdxdna/0` and keeps Linux's ioctls (`amdxdna_accel.h`), plus
the mapping ioctls of the amdgpu port. It touches no hardware until the
device is opened; the NPU has power from the first open to the last close.

Haiku has no AMD IOMMU driver, so the port uses the mode upstream added
for that case ("carveout"): buffers are ranges of one physically
contiguous block (128 MiB by default, `carveout_mb`), and the NPU is
given physical addresses. That means there is **no isolation**: the
addresses sit in instruction streams the kernel cannot check, so a client
could make the NPU read or write any memory. The device can therefore
only be opened by root; for everyone else the back end's programs fail
and the CPU runs the model.

The NPU does not run networks as such: it runs *designs* compiled for its
array, with the open `mlir-aie` and Peano tool chain on a Linux host. The
back end (`amdxdna.neural`) works from **design bundles**: a directory
with a design for one stretch of one network, found by a key computed from
that stretch (`data/neural/amdxdna/<key>/bundle.txt`; see
`src/add-ons/neural/amdxdna/README` for the format and
`haiku/tests/xdna/make-bundle.py` in linux-drm for making one). It claims
exactly the operators a bundle covers, and nothing without one.

## Testing

- **On the build host:** `src/tests/kits/neural/host` builds the graph, the
  TensorFlow Lite loader and the CPU interpreter for Linux and compares
  them with LiteRT's reference kernels, tensor by tensor
  (`run_tests.sh`; needs `pip install numpy ai-edge-litert`).
- **`neural compare`** on a machine: a device against the CPU.
- **`rk3588_npu_test`** (`src/tests/add-ons/kernel/drivers/npu`): the
  driver alone: power, buffers, refused command lists.
- **`src/tests/kits/neural`:** `neural_kit_test`, the kit's own classes
  with a back end inside the test (also run by CI in every image's smoke
  test), and per-back-end tests that run single operators on a device and
  the CPU (`rk3588_npu_backend_test`, `vulkan_backend_test`,
  `amdxdna_backend_test`).
- **Speech:** `host/speech_tests.sh` builds the recognizer's core on the
  build host and compares it with a reference written with NumPy from
  Whisper's published algorithm (`speech_reference.py`; the networks run
  by LiteRT): the audio converter, the tokenizer, spectrograms, encoder
  and decoder tensor by tensor, the text of recordings, and damaged
  model directories.
  `speech_test` (`src/tests/kits/neural`) runs on the machine: the front
  end against stored reference values, model directories and decoding
  with a model and networks made up in the test, and, with a model
  installed, `BSpeechRecognizer` itself (messages, interim results,
  `Cancel()`, deletion in the middle of its work).
- **Lookout** (`src/apps/lookout`, in the Demos menu): a picture or the
  camera, with what a detection or classification model finds in it; the
  kit as an application uses it (`RunAsync()` to a window, the roster's
  device messages, bitmaps). `tools/airos/neural/fetch-models.sh` fetches
  the public test models it and the tests use.

## Status

What was measured, on which machine, on 2026-11-09. "Agrees" means
`neural compare` against the CPU device on the same input.

### The CPU device

- **Against LiteRT 2.3** (TensorFlow Lite's reference kernels, on the
  build host, `src/tests/kits/neural/host/run_tests.sh`): 726 generated
  single-operator models (every operator and type, three random inputs
  each) and 9 real models (MobileNet V1/V2 quantized and float, SSD
  MobileNet V1, EfficientNet-Lite0 int8 and float, EfficientDet-Lite0
  int8, DeepLab V3 float, BlazeFace float16) agree on every tensor: all
  integer tensors equal, float within 1e-4. 3737 deliberately damaged
  model files are refused or run without a crash, also under the address
  and undefined-behaviour sanitizers.
- **On arm64** (ROCK 5 ITX, Haiku): the outputs of the three quantized
  MobileNet/SSD models are byte for byte those of the host run that LiteRT
  confirmed; MobileNet V2 float differs by 5e-7. `neural_kit_test` passes.
- **Speed** (best of 10 runs, ms):

  | Model | Xeon E5-2650 v4, 1 thread | same, 8 threads | ROCK 5 ITX, 8 cores | X399 (16 threads) |
  | --- | --- | --- | --- | --- |
  | mobilenet_v1_1.0_224_quant | 41.5 | 8.7 | 21.9 | |
  | mobilenet_v2_1.0_224_quant | 34.8 | 8.5 | 19.4 | |
  | ssd_mobilenet_v1_quant | 57.8 | 14.1 | 35.0 | |
  | mobilenet_v2_1.0_224 (float) | 49.1 | 12.6 | 28.3 | 29.5 |

  x86_64 uses SSE2 only (no run-time dispatch to AVX2 yet).

### RK3588 NPU (ROCK 5 ITX)

| Model | On the NPU | Against the CPU | CPU ms | NPU ms |
| --- | --- | --- | --- | --- |
| mobilenet_v1_1.0_224_quant | 28 of 31 operators, 2 partitions | equal (0 of 1001 outputs differ) | 21.8 | 8.5 |
| mobilenet_v2_1.0_224_quant | 63 of 65, 2 partitions | 524 of 1001 differ, by at most 3 steps; same top five on a picture | 19.0 | 10.0 |
| ssd_mobilenet_v1_quant | 47 of 64, 12 partitions | same detections and classes; scores within 0.008, boxes within 0.007 of the picture | 34.3 | 14.7 |

- Each operator alone, fed the CPU's own intermediate tensors, is within
  one quantized step of the CPU (`rk3588_npu_backend_test --model`). The
  hardware rounds differently in two places (negative results of layers
  without an activation; an addition fused into a convolution adds before
  it rounds), and single steps add up through MobileNet V2's bottlenecks.
  `neural compare --tolerance 3` (V2) and `--float-tolerance 0.03` (SSD)
  are what these models need.
- What the back end takes: `CONV_2D` and `DEPTHWISE_CONV_2D`, uint8 or
  int8 with one scale per tensor, batch 1, square kernels up to 7, strides
  1 and 2, no dilation, at least two input channels, depth multiplier 1,
  activations none/ReLU/ReLU6/ReLU-1..1; `ADD` when it can be fused into
  the convolution that makes one of its operands. The rest (pooling,
  softmax, reshapes, the detection post-processing) runs on the CPU.
- Of MobileNet V1's 8.5 ms, 8.4 are the NPU's; converting tensors between
  the kit's layout and the NPU's takes 75 µs.
- Jobs that reach for memory they do not have (unmapped addresses,
  another client's buffer) fail with `B_BAD_ADDRESS` within 50 ms, the
  other client's buffer is untouched and the next job runs; clients killed
  in the middle of a job, two clients at once and 1000 runs in a row
  behave. The core's IOMMU does not deliver its fault interrupt (cause
  unknown), so the driver looks at it every 50 ms while a job runs.
- The clock is left at 198 MHz: the board's NPU supply cannot be read
  from Haiku (no I2C bus driver there), so higher frequencies were not
  tried. One core of three is used.

### Vulkan (X399, GTX 1070, NVK; Framework 13, Radeon 840M, RADV)

| Model | On the GPU | Against the CPU | CPU ms | GPU ms |
| --- | --- | --- | --- | --- |
| mobilenet_v2_1.0_224 (float) | 66 of 66 operators, 1 partition | agrees (relative difference 2e-6); same top five on a picture | 29.5 | 3.9 |
| mobilenet_v1_0.25_128 (float) | 31 of 31 | agrees | 5.8 | 0.8 |

- Float32 only: convolutions (also dilated, depthwise with any
  multiplier), fully connected, pooling, `ADD`/`SUB`/`MUL`, the
  activations, softmax, reshapes, channel concatenation, `MEAN` over
  height and width, `PAD`, `RESIZE_BILINEAR`. Quantized models are left to
  the CPU (or an NPU). `vulkan_backend_test`: 149 single-operator cases
  pass.
- 1000 runs with identical results; two sessions and two processes at
  once; 200 sessions made and deleted without device memory growing; a
  machine without a Vulkan loader lists the CPU only.
- Setting a session up takes about 120 ms (pipelines, weights, and timing
  two shader variants per convolution to keep the faster).
- **RADV** (Framework 13, Radeon 840M of the Ryzen AI 5 340, Mesa 25.3.6):
  `vulkan_backend_test` passes all 149 cases; MobileNet V2 float runs 66
  of 66 operators on the GPU, agrees with the CPU (relative difference
  2e-6, same top classes on a picture) and takes 8.9 ms against 15.1 ms
  on the twelve CPU threads.
- Not run on Turnip, V3DV or PowerVR yet: run `vulkan_backend_test` there
  first.

### AMD XDNA (Framework 13, Ryzen AI 5 340)

**Runs designs on the hardware; not stable yet.** With the driver
(`amdxdna` add-on, linux-drm branch `airos-xdna`) loaded on the laptop:

- The NPU starts: firmware 1.1.2.64 loads, the firmware answers (8
  columns by 6 rows of tiles, "NPU Krackan 1"), interrupts work, and the
  NPU powers down when the device is closed and up again when it is
  opened.
- Two designs built with mlir-aie and Peano compute the right bytes
  through the driver's test program `xdna_test`: a pass-through (4096
  bytes) and a 1x1 int8 convolution, 32x32x64 to 64 channels (65536 bytes
  equal to the host reference on five runs in a row, and the same for the
  kit's design bundle). The NPU rounds exact halves away from zero, as
  the kit's CPU kernels do.
- A command takes 112.5 ms whatever the design, which is a fixed delay
  somewhere and not computing time; not understood yet.
- **The laptop then hung** (no answer on the network) in about the
  seventh open/run/close cycle after boot, in the same paths that had
  passed before; the cause is not known. Until it is, the driver stays on
  its branch and out of the images.
- The contiguous block the NPU's buffers need (128 MiB, aligned to
  64 MiB) could not be had on a machine that had been up for days and was
  found in 12 ms after a boot. The driver now takes it when it is loaded
  and a launch job loads it at boot (tried in a virtual machine only).
- Not done on hardware: the back end `amdxdna.neural` (in a virtual
  machine with the driver's stand-in device it lists `amdxdna/0` without
  starting the NPU, claims the convolution exactly when its design bundle
  is installed, and hands a failed run to the CPU with the right result),
  `neural compare`, timing against the CPU, a soak test.

### Speech recognition (Whisper tiny.en)

- **The conversion** (`whisper_convert.py --check`): the encoder's
  outputs are within 1e-5 (of the largest value) of a NumPy version of
  Whisper's equations on the checkpoint's weights, the decoder's logits
  over six steps, token by token against all at once, within 5e-6 with
  the same best token each time; the mel filter bank computed there
  differs from the one Whisper ships by 2e-9.
- **On the build host** (`host/speech_tests.sh`, 54 checks, also under
  the address and undefined-behaviour sanitizers): the front end's
  spectrograms are within 1.2e-7 of the NumPy reference; encoder and
  decoder agree with LiteRT's reference kernels on every tensor within
  1e-4 on what three recordings put into them (on noise the decoder
  too; the encoder within 3e-4: noise drives its last layer to values
  speech never does, where the differences reach 1.2e-4); the audio
  converter gives tones back within 3e-5 from 8 to 96 kHz, 16 bit to
  float, one to six channels, and removes what 16 kHz cannot carry; the
  text and times of three recordings (President Kennedy's "ask not"
  from whisper.cpp's samples, 11 s; a LibriSpeech utterance, 10 s; both
  twice in a row, 44 s, two windows) are those of the reference, with
  and without time stamps, fed at once and in pieces; 600 model
  directories with a damaged description, vocabulary or filter bank are
  refused or read without a crash.
- **On arm64** (ROCK 5 ITX, `neural transcribe`): the same text and times
  for the three recordings, line for line; also from a 44.1 kHz stereo
  file through the Media Kit's decoder and through the kit's own WAVE
  reader. `speech_test` passes (59 checks with the model and a
  recording). The Media Kit's FLAC decoder gave only 8.7 of an
  utterance's 10.4 seconds ("audio decode error"), and the text ended
  there.
- **Speed** (ROCK 5 ITX; a window of 30 s full of speech, 85 tokens;
  seconds):

  | `NEURAL_THREADS` | Window | Encoder | Decoder | Of real time |
  | --- | --- | --- | --- | --- |
  | 1 | 5.83 | 4.27 | 1.50 | 0.19 |
  | 4 | 2.15 | 1.06 | 1.07 | 0.07 |
  | 8 (the default there) | 2.30 | 1.05 | 1.22 | 0.08 |

  The front end takes 0.02 s. A token costs 12.5 to 17.6 ms, most of it
  the last layer's 80 MB of weights; the 11 s recording takes 1.6 s. The
  four slow cores add nothing (see [Open points](#open-points)).
- **Memory:** the machine's used memory grows by about 425 MB while a
  recording is transcribed, the 150 MB of mapped model files included
  (1 GB before tensors shared memory).
- **The multilingual model** (tiny) converts and checks the same way;
  the kit's text for the two English recordings is the reference's.
  No recording in another language was tried.
- **Not verified:** audio from a microphone (only files were fed, at
  once and in pieces); a model found by name in the data directories
  (the models were given by their path, nothing was installed on the
  board); any device but the CPU; x86_64 beyond building.

### Lookout

On the X399: detections on pictures (SSD MobileNet V1 on the CPU) and the
top classes (MobileNet V1 quantized on the CPU, MobileNet V2 float on
`vulkan/0` in 4.3 ms) are the ones `neural run` prints for the same
files; a quantized model on the GPU is reported as running on the CPU; the
window is laid out properly at 200%. **Not verified:** the camera (no
camera was attached to any lab machine that day), switching source and
device from the menus, drag and drop and the settings round trip.

## Open points

- **More model formats.** ONNX is the other format models are published
  in; the loader would be a second file next to `TFLiteLoader.cpp`.
- **Speech.** What `BSpeechRecognizer` does not do yet:
  - *Smaller, faster decoding.* Half of the time of a window full of
    speech goes into the decoder's last layer, 80 MB of weights read per
    token. Stored as 8 bit integers ("dynamic range" quantization, which
    the fully connected kernel would have to learn) they would be 20 MB,
    and the model 45 MB instead of 153.
  - *The end of speech.* Nothing notices that the speaker has stopped
    (voice activity detection): the caller says when a recording is over.
    The model's own "no speech" estimate is only used to drop silent
    windows.
  - *Devices.* The CPU runs all of it. A graphics processor would take
    the encoder's matrix products and fully connected layers; the Vulkan
    back end has not been tried on these networks (`B_NEURAL_CPU_ONLY`
    keeps a recognizer off it). NPUs want quantized networks.
  - *Processors with fast and slow cores.* The thread pool hands the
    same share of work to every core: on the ROCK 5 eight threads are no
    faster than four, and slower in the decoder.
  - *Other languages* work with the multilingual model, which was only
    tried on English recordings; the language is not detected. Larger
    models (base, small) convert the same way and were not tried.
  - *Whisper's second tries* and the text before a window as a prompt
    (see [Speech recognition](#speech-recognition)).
  - *A media node* that takes the system's audio input and gives text,
    so that every program does not open the microphone itself.
- **Sharing device memory.** Tensors cross the boundary between a device
  program and the CPU by copy. A back end could hand out its own buffers
  for the session's inputs and outputs (`BNeuralProgram` has reserved
  slots for it).
- **Memory of intermediate tensors** is reused between operators when
  the CPU runs all of a model (a speech recognizer then takes 0.6 GB less).
  With a device in the session every tensor still has memory of its own:
  a session needs the sum of them, not the maximum.
- **A media node.** A `BBufferConsumer` that runs a model on every video
  frame (a camera's) and sends the results as messages would save every
  camera program the same glue; `BNeuralUtils::SetTensorFromBitmap()` is
  the piece it would be made of.
- **The other two cores of the RK3588**, for two jobs at once.
- **Cancelling a run.** Deleting a session waits for its run to end.
- **Models in the image.** None is shipped; `fetch-models.sh` fetches the
  public test models. System tasks will want a package of models.
- **The Vulkan back end on other drivers**, and int8 networks in shaders
  for machines whose only accelerator is a GPU.
