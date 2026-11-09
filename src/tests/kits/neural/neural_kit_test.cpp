/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests of the Neural Kit that need no hardware: tensors, models made
	with the builder, sessions on the CPU, and the way a session shares a
	network between a device and the CPU. The device is a back end that is
	part of this program: it runs two kinds of operators itself and can be
	told to refuse programs and to go away.

	Prints what it checked and exits with 0 when everything passed.
*/


#include <math.h>
#include <new>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Bitmap.h>
#include <Looper.h>
#include <Message.h>
#include <NeuralKit.h>
#include <add-ons/neural/NeuralBackend.h>

#include <NeuralRosterPrivate.h>


static int sFailures;


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		sFailures++;
}


// #pragma mark - the test back end


static bool sRefusePrograms;
static int32 sRunsUntilLost = -1;
	// < 0: the device stays
static int32 sRunsToFail;
	// runs of a program that fail though the device stays
static bigtime_t sRunDelay;
	// how long a run of a program takes at least
static int32 sRunsInFlight;
static bool sDeletedInFlight;
	// a program was deleted while it ran
static int32 sProgramsMade;
static int32 sProgramsDeleted;


/*!	Runs RELU6 and same-shape ADD on float tensors, the slow and obvious
	way. Tensors inside its stretch live in memory of its own, as they
	would in a real device.
*/
class TestProgram : public BNeuralProgram {
public:
	TestProgram(const BNeuralGraph& graph, const neural_partition& partition)
		:
		fGraph(graph),
		fOperators(NULL),
		fOperatorCount(partition.operator_count),
		fScratch(NULL),
		fStatus(B_OK)
	{
		atomic_add(&sProgramsMade, 1);

		int32 tensorCount = graph.CountTensors();
		fOperators = (int32*)malloc(fOperatorCount * sizeof(int32));
		fScratch = (float**)calloc(tensorCount, sizeof(float*));
		if (fOperators == NULL || fScratch == NULL) {
			fStatus = B_NO_MEMORY;
			return;
		}
		memcpy(fOperators, partition.operators,
			fOperatorCount * sizeof(int32));

		// everything made here that the kit gives no memory for
		bool* outside = (bool*)calloc(tensorCount, sizeof(bool));
		for (int32 i = 0; i < partition.output_count; i++)
			outside[partition.outputs[i]] = true;
		for (int32 i = 0; i < fOperatorCount; i++) {
			const neural_graph_operator* op
				= graph.OperatorAt(fOperators[i]);
			int32 output = op->outputs[0];
			if (!outside[output]) {
				fScratch[output] = (float*)malloc(
					graph.TensorAt(output)->size);
			}
		}
		free(outside);
	}

	virtual ~TestProgram()
	{
		atomic_add(&sProgramsDeleted, 1);
		if (atomic_get(&sRunsInFlight) > 0)
			sDeletedInFlight = true;
		if (fScratch != NULL) {
			for (int32 i = 0; i < fGraph.CountTensors(); i++)
				free(fScratch[i]);
		}
		free(fScratch);
		free(fOperators);
	}

	status_t InitCheck() const { return fStatus; }

	virtual status_t Run(void* const* tensors)
	{
		if (sRunsToFail > 0) {
			sRunsToFail--;
			return B_IO_ERROR;
		}
		if (sRunDelay > 0) {
			atomic_add(&sRunsInFlight, 1);
			snooze(sRunDelay);
			atomic_add(&sRunsInFlight, -1);
		}
		if (sRunsUntilLost == 0)
			return B_NEURAL_DEVICE_LOST;
		if (sRunsUntilLost > 0)
			sRunsUntilLost--;

		for (int32 i = 0; i < fOperatorCount; i++) {
			const neural_graph_operator* op
				= fGraph.OperatorAt(fOperators[i]);
			const neural_graph_tensor* info = fGraph.TensorAt(op->outputs[0]);
			int64 count = info->size / sizeof(float);
			float* output = _Tensor(tensors, op->outputs[0]);
			const float* a = _Tensor(tensors, op->inputs[0]);

			if (op->code == B_NEURAL_OP_RELU6) {
				for (int64 j = 0; j < count; j++)
					output[j] = fminf(fmaxf(a[j], 0), 6);
			} else {
				const float* b = _Tensor(tensors, op->inputs[1]);
				for (int64 j = 0; j < count; j++)
					output[j] = a[j] + b[j];
			}
		}
		return B_OK;
	}

private:
	float* _Tensor(void* const* tensors, int32 index) const
	{
		const neural_graph_tensor* info = fGraph.TensorAt(index);
		if (info->data != NULL)
			return (float*)info->data;
		if (fScratch[index] != NULL)
			return fScratch[index];
		return (float*)tensors[index];
	}

	const BNeuralGraph&	fGraph;
	int32*				fOperators;
	int32				fOperatorCount;
	float**				fScratch;
	status_t			fStatus;
};


class TestBackend : public BNeuralBackend {
public:
	TestBackend()
		:
		BNeuralBackend(-1)
	{
	}

	virtual const char* Name() const { return "test"; }

	virtual int32 CountDevices() { return 1; }

	virtual status_t GetDeviceInfo(int32 device, neural_device_info* info,
		BMessage* details)
	{
		if (device != 0)
			return B_BAD_INDEX;
		strlcpy(info->id, "0", sizeof(info->id));
		strlcpy(info->name, "Test device", sizeof(info->name));
		strlcpy(info->vendor, "air/OS", sizeof(info->vendor));
		info->kind = B_NEURAL_DEVICE_NPU;
		info->flags = B_NEURAL_DEVICE_FLOAT;
		info->priority = 1;
		if (details != NULL)
			details->AddString("driver", "neural_kit_test");
		return B_OK;
	}

	virtual status_t GetSupportedOperators(int32 device,
		const BNeuralGraph& graph, bool* supported)
	{
		for (int32 i = 0; i < graph.CountOperators(); i++) {
			const neural_graph_operator* op = graph.OperatorAt(i);
			const neural_graph_tensor* output
				= graph.TensorAt(op->outputs[0]);
			bool ok = output->type == B_NEURAL_FLOAT32;
			if (op->code == B_NEURAL_OP_ADD) {
				ok &= graph.TensorAt(op->inputs[0])->size == output->size
					&& graph.TensorAt(op->inputs[1])->size == output->size
					&& ((const neural_binary_params*)op->params)->activation
						== B_NEURAL_ACTIVATION_NONE;
			} else
				ok &= op->code == B_NEURAL_OP_RELU6;
			supported[i] = ok;
		}
		return B_OK;
	}

	virtual status_t CreateProgram(int32 device, const BNeuralGraph& graph,
		const neural_partition& partition, uint32 flags,
		BNeuralProgram** _program)
	{
		if (sRefusePrograms)
			return B_NOT_SUPPORTED;

		TestProgram* program = new(std::nothrow) TestProgram(graph,
			partition);
		if (program == NULL || program->InitCheck() != B_OK) {
			delete program;
			return B_NO_MEMORY;
		}
		*_program = program;
		return B_OK;
	}
};


// #pragma mark - the tests


static void
test_tensors()
{
	int32 dims[] = { 2, 3 };
	BTensorInfo info(B_NEURAL_UINT8, 2, dims, "values");
	info.SetQuantization(0.5f, 10);
	check(info.IsValid() && info.CountElements() == 6 && info.ByteSize() == 6
		&& info.IsQuantized(), "tensor info: shape and size");
	check(strcmp(info.ToString().String(),
			"values uint8 [2, 3] scale 0.5 zero 10") == 0,
		"tensor info: description");

	int32 bad[] = { 2, 0 };
	check(BTensorInfo(B_NEURAL_UINT8, 2, bad).IsValid() == false,
		"tensor info: a dimension of 0 is refused");

	BTensor tensor(info);
	check(tensor.InitCheck() == B_OK && tensor.ByteSize() == 6,
		"tensor: allocated");
	check(tensor.ValueAt(0) == -5.0f, "tensor: zeroed storage reads as "
		"scale * (0 - zero point)");

	tensor.SetValueAt(1, 2.0f);
	tensor.SetValueAt(2, 1000.0f);
	tensor.SetValueAt(3, -1000.0f);
	const uint8* stored = (const uint8*)tensor.Data();
	check(stored[1] == 14 && stored[2] == 255 && stored[3] == 0,
		"tensor: values are quantized and clamped");
	check(tensor.IndexOfMaximum() == 2, "tensor: index of maximum");

	float values[6];
	check(tensor.GetValues(values, 6) == B_OK && values[1] == 2.0f
		&& values[2] == 122.5f, "tensor: values come back as real numbers");
	check(tensor.GetValues(values, 7) == B_BAD_VALUE,
		"tensor: reading past the end is refused");

	int64 top[3];
	float topValues[3];
	check(BNeuralUtils::GetTopValues(tensor, 3, top, topValues) == 3
		&& top[0] == 2 && top[1] == 1 && topValues[1] == 2.0f,
		"utils: top values");

	// through a message and back
	BMessage archive;
	check(tensor.Archive(&archive) == B_OK, "tensor: archive");
	BArchivable* instantiated = instantiate_object(&archive);
	BTensor* copy = dynamic_cast<BTensor*>(instantiated);
	check(copy != NULL && copy->InitCheck() == B_OK
		&& copy->Info() == tensor.Info()
		&& strcmp(copy->Info().Name(), "values") == 0
		&& memcmp(copy->Data(), tensor.Data(), 6) == 0,
		"tensor: comes back from an archive");
	delete instantiated;

	// somebody else's memory
	float memory[4] = { 1, 2, 3, 4 };
	int32 four = 4;
	BTensor wrapped(BTensorInfo(B_NEURAL_FLOAT32, 1, &four), memory,
		sizeof(memory));
	wrapped.SetValueAt(3, 9);
	check(wrapped.InitCheck() == B_OK && memory[3] == 9
		&& wrapped.Data() == memory, "tensor: uses the caller's memory");
	check(BTensor(BTensorInfo(B_NEURAL_FLOAT32, 1, &four), memory, 8)
			.InitCheck() == B_BAD_VALUE,
		"tensor: memory that is too small is refused");
}


static void
test_bitmap()
{
	// a 4 x 2 bitmap: left half red, right half blue
	BBitmap bitmap(BRect(0, 0, 3, 1), B_BITMAP_NO_SERVER_LINK, B_RGB32);
	check(bitmap.InitCheck() == B_OK, "bitmap: made without the app_server");
	if (bitmap.InitCheck() != B_OK)
		return;

	for (int32 y = 0; y < 2; y++) {
		uint8* row = (uint8*)bitmap.Bits() + y * bitmap.BytesPerRow();
		for (int32 x = 0; x < 4; x++) {
			row[x * 4] = x < 2 ? 0 : 255;		// blue
			row[x * 4 + 1] = 0;
			row[x * 4 + 2] = x < 2 ? 255 : 0;	// red
			row[x * 4 + 3] = 255;
		}
	}

	int32 dims[] = { 1, 2, 4, 3 };
	BTensor tensor(BTensorInfo(B_NEURAL_UINT8, 4, dims));
	check(BNeuralUtils::SetTensorFromBitmap(&tensor, &bitmap) == B_OK,
		"bitmap: copied into a tensor of its size");
	const uint8* data = (const uint8*)tensor.Data();
	check(data[0] == 255 && data[1] == 0 && data[2] == 0
		&& data[3 * 3] == 0 && data[3 * 3 + 2] == 255,
		"bitmap: red, green, blue in that order");

	// into a square, keeping the proportions
	int32 square[] = { 1, 4, 4, 3 };
	BTensor boxed(BTensorInfo(B_NEURAL_FLOAT32, 4, square));
	neural_image_options options;
	options.fit = B_NEURAL_FIT_LETTERBOX;
	neural_image_mapping mapping;
	check(BNeuralUtils::SetTensorFromBitmap(&boxed, &bitmap, NULL, &options,
			&mapping) == B_OK, "bitmap: letterboxed");
	const float* pixels = (const float*)boxed.Data();
	check(mapping.destination == BRect(0, 1, 3, 2)
		&& fabsf(pixels[0] - (128 - 127.5f) / 127.5f) < 1e-5f
		&& pixels[(1 * 4 + 0) * 3] == 1.0f
		&& pixels[(1 * 4 + 3) * 3 + 2] == 1.0f,
		"bitmap: the picture is in the middle, gray around it");

	BRect back = mapping.ToBitmap(0, 0.25f, 0.5f, 0.75f);
	check(back == BRect(0, 0, 2, 2), "bitmap: boxes map back to the picture");

	int32 wrong[] = { 1, 4, 4, 2 };
	BTensor twoChannels(BTensorInfo(B_NEURAL_UINT8, 4, wrong));
	check(BNeuralUtils::SetTensorFromBitmap(&twoChannels, &bitmap)
			== B_NEURAL_SHAPE_MISMATCH,
		"bitmap: a tensor that is no picture is refused");
}


/*!	x -> RELU6 -> a;  a + x -> b;  b -> RELU -> c;  c -> RELU6 -> d.
	The outputs are d and a. The test device runs everything but RELU. */
static status_t
build_model(BNeuralModel* model)
{
	BNeuralModelBuilder builder("test");
	int32 dims[] = { 1, 4 };
	BTensorInfo info(B_NEURAL_FLOAT32, 2, dims);

	info.SetName("x");
	int32 x = builder.AddInput(info);
	info.SetName("a");
	int32 a = builder.AddOperator(B_NEURAL_OP_RELU6, &x, 1, info);

	neural_binary_params add;
	add.activation = B_NEURAL_ACTIVATION_NONE;
	int32 addInputs[] = { a, x };
	info.SetName("b");
	int32 b = builder.AddOperator(B_NEURAL_OP_ADD, addInputs, 2, info, &add,
		sizeof(add));

	info.SetName("c");
	int32 c = builder.AddOperator(B_NEURAL_OP_RELU, &b, 1, info);
	info.SetName("d");
	int32 d = builder.AddOperator(B_NEURAL_OP_RELU6, &c, 1, info);

	builder.MarkOutput(d);
	builder.MarkOutput(a);
	return builder.Build(model);
}


static const float kInput[4] = { -3, 1, 2.5f, 8 };
static const float kExpectedD[4] = { 0, 2, 5, 6 };
static const float kExpectedA[4] = { 0, 1, 2.5f, 6 };


static bool
run_and_check(BNeuralSession& session)
{
	if (session.InputAt(0) == NULL
		|| session.InputAt(0)->CopyFrom(kInput, sizeof(kInput)) != B_OK
		|| session.Run() != B_OK) {
		return false;
	}
	return memcmp(session.OutputAt(0)->Data(), kExpectedD,
			sizeof(kExpectedD)) == 0
		&& memcmp(session.OutputAt(1)->Data(), kExpectedA,
			sizeof(kExpectedA)) == 0;
}


class Receiver : public BLooper {
public:
	Receiver()
		:
		BLooper("receiver"),
		fDone(create_sem(0, "done")),
		fStatus(B_ERROR),
		fToken(-1),
		fAdded(0)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_NEURAL_RUN_COMPLETED:
			case 'mine':
				fWhat = message->what;
				fStatus = message->GetInt32("neural:status", B_ERROR);
				fToken = message->GetInt32("neural:token", -1);
				release_sem(fDone);
				break;
			case B_NEURAL_DEVICE_ADDED:
				fAddedDevice = message->GetString("device", "");
				atomic_add(&fAdded, 1);
				release_sem(fDone);
				break;
			default:
				BLooper::MessageReceived(message);
		}
	}

	sem_id		fDone;
	uint32		fWhat;
	status_t	fStatus;
	int32		fToken;
	int32		fAdded;
	BString		fAddedDevice;
};


static void
ignore_signal(int)
{
}


static status_t
delete_session(void* data)
{
	delete (BNeuralSession*)data;
	return B_OK;
}


static void
test_sessions(Receiver* receiver)
{
	BNeuralModel model;
	check(build_model(&model) == B_OK, "builder: the model is made");
	check(model.CountInputs() == 1 && model.CountOutputs() == 2
		&& model.CountOperators() == 4 && model.FindOutput("a") == 1
		&& model.FindInput("nothing") == B_NAME_NOT_FOUND
		&& strcmp(model.Format(), "built") == 0, "model: what is in it");

	BNeuralRoster* roster = BNeuralRoster::Default();
	BNeuralDevice cpu;
	check(roster->FindDevice("cpu", &cpu) == B_OK
		&& cpu.Kind() == B_NEURAL_DEVICE_CPU && cpu.IsAvailable()
		&& cpu.CountSupportedOperators(model) == 4,
		"roster: the CPU is there and runs all of the model");

	{
		BNeuralSession session(model, cpu);
		check(session.InitCheck() == B_OK && run_and_check(session),
			"session: the CPU computes the right numbers");

		neural_run_info info;
		session.GetRunInfo(&info);
		check(info.runs == 1 && info.cpu_operators == 4
			&& info.device_operators == 0, "session: run info on the CPU");
	}

	// the test device appears
	roster->StartWatching(BMessenger(receiver));
	BPrivate::NeuralRosterData::Default()->AddBackend(
		new(std::nothrow) TestBackend());
	check(acquire_sem_etc(receiver->fDone, 1, B_RELATIVE_TIMEOUT, 5000000)
			== B_OK && receiver->fAddedDevice == "test/0",
		"roster: watchers hear of a new device");
	roster->StopWatching(BMessenger(receiver));

	BNeuralDevice device;
	check(roster->FindDevice("test/0", &device) == B_OK
		&& device.Kind() == B_NEURAL_DEVICE_NPU
		&& strcmp(device.Backend(), "test") == 0
		&& device.CountSupportedOperators(model) == 3,
		"roster: the test device runs three of four operators");

	BNeuralDevice chosen;
	check(roster->GetDefaultDevice(&chosen, 0, &model) == B_OK
		&& chosen == device, "roster: an NPU that runs part of a model is "
		"the default for it");
	check(roster->GetDefaultDevice(&chosen, B_NEURAL_CPU_ONLY, &model) == B_OK
		&& chosen == cpu, "roster: B_NEURAL_CPU_ONLY");
	check(roster->GetDefaultDevice(&chosen, B_NEURAL_NO_FALLBACK, &model)
			== B_OK && chosen == cpu,
		"roster: with B_NEURAL_NO_FALLBACK only a device that runs all");

	{
		BNeuralSession session(model, device, B_NEURAL_PROFILE);
		check(session.InitCheck() == B_OK && run_and_check(session),
			"session: device and CPU together compute the right numbers");

		neural_run_info info;
		session.GetRunInfo(&info);
		check(info.device_operators == 3 && info.cpu_operators == 1
			&& info.device_partitions == 2,
			"session: two device partitions around the CPU's operator");

		neural_operator_info op;
		check(session.GetOperatorInfo(0, &op) == B_OK && op.on_device
			&& op.partition == 0 && session.GetOperatorInfo(2, &op) == B_OK
			&& !op.on_device && op.partition == -1
			&& op.code == B_NEURAL_OP_RELU
			&& session.GetOperatorInfo(3, &op) == B_OK && op.partition == 1
			&& session.GetOperatorInfo(4, &op) == B_BAD_INDEX,
			"session: operator info says what runs where");

		// asynchronously, to a looper
		session.InputAt(0)->CopyFrom(kInput, sizeof(kInput));
		int32 token = 0;
		check(session.RunAsync(BMessenger(receiver), NULL, &token) == B_OK,
			"session: asynchronous run starts");
		check(acquire_sem_etc(receiver->fDone, 1, B_RELATIVE_TIMEOUT,
				5000000) == B_OK && receiver->fStatus == B_OK
			&& receiver->fToken == token
			&& receiver->fWhat == (uint32)B_NEURAL_RUN_COMPLETED,
			"session: the completion message arrives");
		check(session.WaitForRun() == B_OK && !session.IsRunning()
			&& memcmp(session.OutputAt(0)->Data(), kExpectedD,
				sizeof(kExpectedD)) == 0,
			"session: the asynchronous run's results are right");

		BMessage mine('mine');
		check(session.RunAsync(BMessenger(receiver), &mine) == B_OK
			&& acquire_sem_etc(receiver->fDone, 1, B_RELATIVE_TIMEOUT,
				5000000) == B_OK && receiver->fWhat == (uint32)'mine'
			&& receiver->fStatus == B_OK,
			"session: the caller's own message is used");
		session.WaitForRun();
	}
	check(sProgramsMade == 2 && sProgramsDeleted == 2,
		"session: programs are deleted with the session");

	check(BNeuralSession(model, device, B_NEURAL_NO_FALLBACK).InitCheck()
			== B_NEURAL_NOT_ACCELERATED,
		"session: B_NEURAL_NO_FALLBACK fails when the device cannot run all");

	// a back end that does not make its programs
	sRefusePrograms = true;
	{
		BNeuralSession session(model, device);
		neural_run_info info;
		session.GetRunInfo(&info);
		check(session.InitCheck() == B_OK && info.device_operators == 0
			&& info.cpu_operators == 4 && run_and_check(session),
			"session: the CPU takes over when the back end refuses");
	}
	sRefusePrograms = false;

	// a device that goes away
	{
		BNeuralSession session(model, device);
		sRunsUntilLost = 2;
			// the first run's two partitions
		check(run_and_check(session), "session: runs while the device is "
			"there");
		check(run_and_check(session), "session: still the right numbers "
			"after the device is lost");

		neural_run_info info;
		session.GetRunInfo(&info);
		check(info.device_operators == 0 && info.cpu_operators == 4
			&& info.device_partitions == 0 && info.runs == 2,
			"session: the CPU has taken over for good");
		sRunsUntilLost = -1;
		check(run_and_check(session), "session: and stays there");
	}

	// A session deleted while it runs waits for the run, also when the
	// wait is interrupted (as it is on the thread of a looper that is
	// being quit): the network must not run on freed tensors.
	{
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_handler = &ignore_signal;
		sigaction(SIGUSR1, &action, NULL);

		BNeuralSession* session = new BNeuralSession(model, device);
		session->InputAt(0)->CopyFrom(kInput, sizeof(kInput));
		sRunDelay = 300000;
		bool started = session->RunAsync(BMessenger(receiver)) == B_OK;
		snooze(50000);

		thread_id deleter = spawn_thread(&delete_session, "deleter",
			B_NORMAL_PRIORITY, session);
		resume_thread(deleter);
		snooze(50000);
		send_signal(deleter, SIGUSR1);
		snooze(20000);
		send_signal(deleter, SIGUSR1);

		status_t result;
		while (wait_for_thread(deleter, &result) == B_INTERRUPTED)
			;
		sRunDelay = 0;
		check(started && !sDeletedInFlight, "session: deleting waits for "
			"the run even when the wait is interrupted");
		acquire_sem_etc(receiver->fDone, 1, B_RELATIVE_TIMEOUT, 5000000);
	}

	// a device that fails now and then, and one that keeps failing
	{
		BNeuralSession session(model, device);
		sRunsToFail = 2;
			// both partitions of one run
		check(run_and_check(session), "session: the right numbers though "
			"the device failed");

		neural_run_info info;
		session.GetRunInfo(&info);
		check(info.device_failures == 2 && info.device_operators == 3
			&& info.device_partitions == 2,
			"session: one failure does not cost the device its part");
		check(run_and_check(session) && session.GetRunInfo(&info) == B_OK
			&& info.device_failures == 2,
			"session: the device is used again");

		sRunsToFail = 1000;
		bool right = true;
		for (int32 i = 0; i < 4; i++)
			right &= run_and_check(session);
		session.GetRunInfo(&info);
		check(right && info.device_operators == 0
			&& info.device_partitions == 0,
			"session: a device that keeps failing loses its part");
		sRunsToFail = 0;
	}

	{
		BNeuralSession session(model, device, B_NEURAL_NO_FALLBACK
			| B_NEURAL_CPU_ONLY);
		check(session.InitCheck() == B_OK
			&& session.Device() == cpu, "session: B_NEURAL_CPU_ONLY");
	}
}


static void
test_bad_models()
{
	BNeuralModel model;
	check(model.InitCheck() == B_NO_INIT && model.CountInputs() == 0,
		"model: not initialized");
	check(model.SetTo("/nonexistent/model.tflite") == B_ENTRY_NOT_FOUND,
		"model: a missing file");

	const char text[] = "this is not a model at all";
	check(model.SetTo(text, sizeof(text)) == B_NEURAL_UNKNOWN_FORMAT,
		"model: something else than a model");

	// the right magic and nothing behind it
	uint8 header[64];
	memset(header, 0xff, sizeof(header));
	memcpy(header + 4, "TFL3", 4);
	check(model.SetTo(header, sizeof(header)) == B_NEURAL_BAD_MODEL,
		"model: a damaged TensorFlow Lite file");
	memset(header, 0, sizeof(header));
	memcpy(header + 4, "TFL3", 4);
	check(model.SetTo(header, sizeof(header)) != B_OK,
		"model: an empty TensorFlow Lite file");

	BNeuralSession session(model);
	check(session.InitCheck() != B_OK && session.Run() != B_OK
		&& session.InputAt(0) == NULL,
		"session: of a model that did not load");

	// an operator whose operands do not fit
	BNeuralModelBuilder builder;
	int32 small[] = { 1, 4 };
	int32 large[] = { 1, 5 };
	int32 x = builder.AddInput(BTensorInfo(B_NEURAL_FLOAT32, 2, small));
	int32 y = builder.AddOperator(B_NEURAL_OP_RELU, &x, 1,
		BTensorInfo(B_NEURAL_FLOAT32, 2, large));
	builder.MarkOutput(y);
	check(builder.Build(&model) == B_NEURAL_UNSUPPORTED_OPERATOR
		&& model.ErrorDetails()[0] != '\0',
		"builder: an operator with operands that do not fit is refused");

	BNeuralModelBuilder noOutput;
	noOutput.AddInput(BTensorInfo(B_NEURAL_FLOAT32, 2, small));
	check(noOutput.Build(&model) != B_OK, "builder: a model without outputs");
}


int
main()
{
	// no add-ons: what this machine has is not the subject
	setenv("NEURAL_DISABLE_BACKENDS", "all", 1);

	Receiver* receiver = new Receiver();
	receiver->Run();

	test_tensors();
	test_bitmap();
	test_bad_models();
	test_sessions(receiver);

	receiver->Lock();
	receiver->Quit();

	printf("%s\n", sFailures == 0 ? "all checks passed" : "CHECKS FAILED");
	return sFailures == 0 ? 0 : 1;
}
