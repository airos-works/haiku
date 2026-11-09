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
headers/os/add-ons/neural/NeuralBackend.h    the back-end API
headers/private/neural/                      the graph, the interpreter, the roster's inside
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
  for it; a back end that fails to make a program, or a device that goes
  away while the session lives, is replaced by the CPU on the spot. So a
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
dynamic shapes, no control flow, no state kept between runs yet).

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
`B_NEURAL_RUN_COMPLETED` if wanted (the three `neural:` fields are added to
it) and returns a token that comes back in `"neural:token"`.
`WaitForRun(timeout)` waits for such a run.

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
`GetTopValues()` picks the best classes.

### Errors

Besides the usual ones (`B_NO_INIT`, `B_BAD_VALUE`, `B_NO_MEMORY`,
`B_BUSY`, file errors):

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
| Detection | `DETECTION_POSTPROCESS` (TensorFlow Lite's `TFLite_Detection_PostProcess`: boxes, classes, scores and count from a single-shot detector's raw output) |

That covers the MobileNet family (V1, V2; classification and SSD
detection), which is what the first uses need: recognising what a camera
sees. Recurrent and transformer networks (speech, language) need more
operators and state between runs; see [Open points](#open-points).

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
  are written. `B_NEURAL_DEVICE_LOST` hands the stretch to the CPU for
  good.
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
                                        the device against the CPU on the same input
neural bench model.tflite               times it on every device
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
driven by Linux's `amdxdna` driver, built for air/OS on the same
compatibility layer as `amdgpu` (repository `linux-drm`, branch
`airos-xdna`). Haiku has no AMD IOMMU driver, so the port uses the mode
upstream added for that case: physically contiguous buffers and physical
addresses. The NPU does not run networks as such: it runs *designs*
compiled for its array (with the open `mlir-aie` and Peano tool chain on a
Linux host); the back end maps operators to the designs it has.

## Testing

- **On the build host:** `src/tests/kits/neural/host` builds the graph, the
  TensorFlow Lite loader and the CPU interpreter for Linux and compares
  them with LiteRT's reference kernels, tensor by tensor
  (`run_tests.sh`; needs `pip install numpy ai-edge-litert`).
- **`neural compare`** on a machine: a device against the CPU.
- **`rk3588_npu_test`** (`src/tests/add-ons/kernel/drivers/npu`): the
  driver alone: power, buffers, refused command lists.
- **`src/tests/kits/neural`:** the kit's own classes, and per-back-end
  tests that run single operators on a device and the CPU.

## Status

To be filled in with what was measured (see the end of this file's
history): which models agree on which device, with what tolerance, and how
fast.

## Open points

- **More model formats.** ONNX is the other format models are published
  in; the loader would be a second file next to `TFLiteLoader.cpp`.
- **Speech and language.** Summit's speech recognition
  (airos-works/summit#159) needs a Whisper-class model: transformer
  operators (matrix multiplication, layer normalisation, GELU, gather), a
  mel spectrogram front end for Media Kit audio, and a decoding loop that
  keeps state between runs. The session API is ready for it (the decoder is
  one more model, run repeatedly); the operators are not there yet.
- **Sharing device memory.** Tensors cross the boundary between a device
  program and the CPU by copy. A back end could hand out its own buffers
  for the session's inputs and outputs (`BNeuralProgram` has reserved
  slots for it).
- **Memory of intermediate tensors** is not reused between operators on
  the CPU: a session needs the sum of them, not the maximum.
- **A media node.** A `BBufferConsumer` that runs a model on every video
  frame (a camera's) and sends the results as messages would save every
  camera program the same glue; `BNeuralUtils::SetTensorFromBitmap()` is
  the piece it would be made of.
- **The other two cores of the RK3588**, for two jobs at once.
