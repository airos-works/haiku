/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	neural: the Neural Kit from the command line. Lists the devices that
	run networks, shows what is in a model, runs one (on a picture, a file
	of raw numbers or noise) and compares what a device computes with what
	the CPU computes.
*/


#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Bitmap.h>
#include <File.h>
#include <Message.h>
#include <NeuralKit.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>
#include <TranslationUtils.h>


static const char* kUsage =
	"usage: neural devices\n"
	"       neural info <model>\n"
	"       neural run <model> [options]\n"
	"       neural compare <model> [options]\n"
	"       neural bench <model> [options]\n"
	"\n"
	"devices   lists the devices that can run networks\n"
	"info      shows the inputs, outputs and operators of a model and how\n"
	"          much of it each device runs itself\n"
	"run       runs the model and prints the result\n"
	"compare   runs the model on a device and on the CPU with the same\n"
	"          input and compares the outputs\n"
	"bench     times the model on every device\n"
	"\n"
	"options:\n"
	"  -d, --device <id>   the device to use (default: the system's choice)\n"
	"      --cpu           use the CPU only\n"
	"      --no-fallback   fail if the device cannot run all of the model\n"
	"      --low-power     prefer the device that takes the least power\n"
	"  -i, --input <file>  a picture (any format with a translator) or a\n"
	"                      file of raw numbers for the inputs; default:\n"
	"                      noise that is the same on every run\n"
	"      --fit <how>     stretch (default), crop or letterbox the picture\n"
	"  -l, --labels <file> names of the classes, one per line\n"
	"  -n, --runs <n>      run n times (default 1; bench: 20)\n"
	"  -t, --top <n>       show the n best classes (default 5)\n"
	"  -p, --profile       show where each operator ran and how long\n"
	"  -o, --output <file> write the raw outputs to a file\n"
	"      --tolerance <n> compare: largest difference accepted in\n"
	"                      quantized outputs, in steps (default 1)\n"
	"      --float-tolerance <x>\n"
	"                      compare: largest difference accepted in float\n"
	"                      outputs, relative to the largest value\n"
	"                      (default 0.001)\n";


struct Options {
	const char*			model;
	const char*			device;
	const char*			input;
	const char*			labels;
	const char*			output;
	neural_image_fit	fit;
	uint32				flags;
	int32				runs;
	int32				top;
	int32				tolerance;
	float				floatTolerance;
	bool				profile;
};


static const char*
error_name(status_t status)
{
	switch (status) {
		case B_NEURAL_BAD_MODEL:
			return "the file is not a model or is damaged";
		case B_NEURAL_UNKNOWN_FORMAT:
			return "unknown model format";
		case B_NEURAL_UNSUPPORTED_OPERATOR:
			return "the model has an operator that is not supported";
		case B_NEURAL_TYPE_MISMATCH:
			return "wrong tensor type";
		case B_NEURAL_SHAPE_MISMATCH:
			return "wrong tensor shape";
		case B_NEURAL_NO_DEVICE:
			return "no such device";
		case B_NEURAL_DEVICE_LOST:
			return "the device is gone";
		case B_NEURAL_NOT_ACCELERATED:
			return "the device cannot run all of the model";
		default:
			return strerror(status);
	}
}


static void
print_details(const BMessage& details)
{
	char* name;
	type_code type;
	int32 count;
	for (int32 i = 0; details.GetInfo(B_ANY_TYPE, i, &name, &type, &count)
			== B_OK; i++) {
		for (int32 j = 0; j < count; j++) {
			printf("    %-16s ", name);
			switch (type) {
				case B_STRING_TYPE:
					printf("%s\n", details.GetString(name, j, ""));
					break;
				case B_INT32_TYPE:
					printf("%" B_PRId32 "\n", details.GetInt32(name, j, 0));
					break;
				case B_INT64_TYPE:
				{
					int64 value = 0;
					details.FindInt64(name, j, &value);
					printf("%" B_PRId64 "\n", value);
					break;
				}
				case B_BOOL_TYPE:
					printf("%s\n", details.GetBool(name, j, false)
						? "yes" : "no");
					break;
				case B_FLOAT_TYPE:
					printf("%g\n", details.GetFloat(name, j, 0));
					break;
				default:
					printf("...\n");
					break;
			}
		}
	}
}


static int
list_devices()
{
	BNeuralRoster* roster = BNeuralRoster::Default();
	BNeuralDevice preferred;
	roster->GetDefaultDevice(&preferred);

	int32 count = roster->CountDevices();
	for (int32 i = 0; i < count; i++) {
		BNeuralDevice device;
		if (roster->GetDeviceAt(i, &device) != B_OK)
			continue;

		BString what;
		if ((device.Flags() & B_NEURAL_DEVICE_QUANTIZED) != 0)
			what << "quantized";
		if ((device.Flags() & B_NEURAL_DEVICE_FLOAT) != 0)
			what << (what.Length() > 0 ? ", " : "") << "float";
		if ((device.Flags() & B_NEURAL_DEVICE_REMOVABLE) != 0)
			what << ", removable";

		printf("%-20s %s %s%s%s (%s)%s\n", device.ID(),
			neural_device_kind_name(device.Kind()), device.Vendor(),
			device.Vendor()[0] != '\0' ? " " : "", device.Name(),
			what.String(), device == preferred ? " [default]" : "");

		BMessage details;
		if (device.GetInfo(&details) == B_OK)
			print_details(details);
	}
	return 0;
}


static status_t
load_model(const Options& options, BNeuralModel& model)
{
	status_t status = model.SetTo(options.model);
	if (status != B_OK) {
		fprintf(stderr, "neural: %s: %s", options.model, error_name(status));
		if (model.ErrorDetails()[0] != '\0')
			fprintf(stderr, " (%s)", model.ErrorDetails());
		fprintf(stderr, "\n");
	}
	return status;
}


static int
show_info(const Options& options)
{
	BNeuralModel model;
	if (load_model(options, model) != B_OK)
		return 1;

	BMessage info;
	model.GetInfo(&info);
	int64 weights = 0;
	info.FindInt64("weights:bytes", &weights);
	printf("%s: %s model, %" B_PRId32 " operators, %.1f MiB of weights\n",
		model.Name(), model.Format(), model.CountOperators(),
		weights / (1024.0 * 1024.0));
	if (model.Description()[0] != '\0')
		printf("%s\n", model.Description());

	printf("inputs:\n");
	for (int32 i = 0; i < model.CountInputs(); i++) {
		BTensorInfo tensor;
		model.GetInputInfo(i, &tensor);
		printf("  %s\n", tensor.ToString().String());
	}
	printf("outputs:\n");
	for (int32 i = 0; i < model.CountOutputs(); i++) {
		BTensorInfo tensor;
		model.GetOutputInfo(i, &tensor);
		printf("  %s\n", tensor.ToString().String());
	}

	printf("operators:\n");
	int32 counts[B_NEURAL_OP_COUNT];
	memset(counts, 0, sizeof(counts));
	for (int32 i = 0; i < model.CountOperators(); i++)
		counts[model.OperatorAt(i)]++;
	for (int32 i = 0; i < B_NEURAL_OP_COUNT; i++) {
		if (counts[i] > 0) {
			printf("  %3" B_PRId32 " %s\n", counts[i],
				neural_operator_name((neural_operator_code)i));
		}
	}

	printf("devices:\n");
	BNeuralRoster* roster = BNeuralRoster::Default();
	for (int32 i = 0; i < roster->CountDevices(); i++) {
		BNeuralDevice device;
		if (roster->GetDeviceAt(i, &device) != B_OK)
			continue;
		int32 supported = device.CountSupportedOperators(model);
		if (supported < 0) {
			printf("  %-20s %s\n", device.ID(), error_name(supported));
		} else {
			printf("  %-20s runs %" B_PRId32 " of %" B_PRId32
				" operators\n", device.ID(), supported,
				model.CountOperators());
		}
	}
	return 0;
}


static bool
is_image_tensor(const BTensor& tensor)
{
	int32 rank = tensor.Rank();
	if (rank != 3 && rank != 4)
		return false;
	if (rank == 4 && tensor.DimensionAt(0) != 1)
		return false;
	int32 channels = tensor.DimensionAt(rank - 1);
	return (channels == 3 || channels == 1) && tensor.DimensionAt(rank - 2) > 1
		&& (tensor.Type() == B_NEURAL_UINT8 || tensor.Type() == B_NEURAL_INT8
			|| tensor.Type() == B_NEURAL_FLOAT32);
}


/*!	Fills the inputs of a session: from a picture, from a file of raw
	numbers, or with noise. */
static status_t
fill_inputs(const Options& options, BNeuralSession& session,
	neural_image_mapping* _mapping, bool* _isImage)
{
	*_isImage = false;

	if (options.input != NULL) {
		// The Translation Kit looks for relative names next to the
		// program, not where the user is.
		BPath path(options.input, NULL, true);
		BBitmap* bitmap = BTranslationUtils::GetBitmap(
			path.InitCheck() == B_OK ? path.Path() : options.input);
		BTensor* first = session.InputAt(0);
		if (bitmap != NULL && first != NULL && is_image_tensor(*first)) {
			neural_image_options imageOptions;
			imageOptions.fit = options.fit;
			status_t status = BNeuralUtils::SetTensorFromBitmap(first, bitmap,
				NULL, &imageOptions, _mapping);
			delete bitmap;
			if (status != B_OK) {
				fprintf(stderr, "neural: %s: %s\n", options.input,
					error_name(status));
			}
			*_isImage = status == B_OK;
			return status;
		}
		delete bitmap;

		BFile file(options.input, B_READ_ONLY);
		status_t status = file.InitCheck();
		for (int32 i = 0; status == B_OK && i < session.CountInputs(); i++) {
			BTensor* tensor = session.InputAt(i);
			ssize_t bytesRead = file.Read(tensor->Data(),
				tensor->ByteSize());
			if (bytesRead != (ssize_t)tensor->ByteSize())
				status = bytesRead < 0 ? (status_t)bytesRead : B_BAD_DATA;
		}
		if (status != B_OK) {
			fprintf(stderr, "neural: %s: not a picture, and not the raw "
				"inputs of this model either (%s)\n", options.input,
				strerror(status));
		}
		return status;
	}

	// noise, the same every time
	uint32 state = 0x2545f491;
	for (int32 i = 0; i < session.CountInputs(); i++) {
		BTensor* tensor = session.InputAt(i);
		int64 count = tensor->CountElements();
		for (int64 j = 0; j < count; j++) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			switch (tensor->Type()) {
				case B_NEURAL_FLOAT32:
					((float*)tensor->Data())[j]
						= (int32)(state & 0xffff) / 32768.0f - 1.0f;
					break;
				case B_NEURAL_UINT8:
				case B_NEURAL_INT8:
					((uint8*)tensor->Data())[j] = state >> 24;
					break;
				default:
					tensor->SetValueAt(j, (float)(state >> 28));
					break;
			}
		}
	}
	return B_OK;
}


static bool
is_detection(const BNeuralSession& session)
{
	if (session.CountOutputs() != 4)
		return false;
	const BTensor* boxes = session.OutputAt(0);
	return boxes->Rank() == 3 && boxes->DimensionAt(2) == 4
		&& boxes->Type() == B_NEURAL_FLOAT32
		&& session.OutputAt(3)->CountElements() == 1;
}


static void
print_results(const Options& options, const BNeuralSession& session,
	const neural_image_mapping& mapping, bool isImage)
{
	BStringList labels;
	if (options.labels != NULL
		&& BNeuralUtils::GetLabels(options.labels, &labels) != B_OK) {
		fprintf(stderr, "neural: cannot read the labels in %s\n",
			options.labels);
	}

	if (is_detection(session)) {
		const float* boxes = (const float*)session.OutputAt(0)->Data();
		const BTensor* classes = session.OutputAt(1);
		const BTensor* scores = session.OutputAt(2);
		int32 count = (int32)session.OutputAt(3)->ValueAt(0);
		if (count > classes->CountElements())
			count = classes->CountElements();

		printf("%" B_PRId32 " detections:\n", count);
		for (int32 i = 0; i < count; i++) {
			int32 index = (int32)classes->ValueAt(i);
			// label files of detection models start with a "???" line for
			// the background class
			BString label = labels.StringAt(index + 1);
			if (label.IsEmpty())
				label.SetToFormat("class %" B_PRId32, index);

			const float* box = boxes + 4 * i;
			printf("  %5.1f%% %-20s", scores->ValueAt(i) * 100,
				label.String());
			if (isImage) {
				BRect rect = mapping.ToBitmap(box[1], box[0], box[3], box[2]);
				printf(" at (%.0f, %.0f) - (%.0f, %.0f)\n", rect.left,
					rect.top, rect.right, rect.bottom);
			} else {
				printf(" at (%.3f, %.3f) - (%.3f, %.3f)\n", box[1], box[0],
					box[3], box[2]);
			}
		}
		return;
	}

	for (int32 i = 0; i < session.CountOutputs(); i++) {
		const BTensor* output = session.OutputAt(i);
		printf("%s\n", output->Info().ToString().String());

		int64 indices[32];
		float values[32];
		int32 top = options.top > 32 ? 32 : options.top;
		int32 found = BNeuralUtils::GetTopValues(*output, top, indices,
			values);
		for (int32 j = 0; j < found; j++) {
			BString label = labels.StringAt(indices[j]);
			printf("  %8.4f  %6" B_PRId64 "  %s\n", values[j], indices[j],
				label.String());
		}
	}
}


static void
print_profile(const BNeuralSession& session)
{
	neural_run_info info;
	session.GetRunInfo(&info);
	printf("device %s: %" B_PRId32 " operators in %" B_PRId32
		" partitions; CPU: %" B_PRId32 " operators\n",
		session.Device().ID(), info.device_operators,
		info.device_partitions, info.cpu_operators);

	int32 count = session.Model().CountOperators();
	int32 lastPartition = -1;
	for (int32 i = 0; i < count; i++) {
		neural_operator_info op;
		if (session.GetOperatorInfo(i, &op) != B_OK)
			continue;
		if (op.on_device && op.partition == lastPartition) {
			printf("  %4" B_PRId32 " %-22s   \"\n", i,
				neural_operator_name(op.code));
			continue;
		}
		lastPartition = op.on_device ? op.partition : -1;
		printf("  %4" B_PRId32 " %-22s %-6s %8" B_PRIdBIGTIME " us\n", i,
			neural_operator_name(op.code), op.on_device ? "device" : "CPU",
			op.last_duration);
	}
}


static status_t
make_session(const Options& options, const BNeuralModel& model,
	BNeuralSession& session, const char* deviceID, uint32 extraFlags)
{
	uint32 flags = options.flags | extraFlags
		| (options.profile ? B_NEURAL_PROFILE : 0);

	status_t status;
	if (deviceID != NULL) {
		BNeuralDevice device;
		status = BNeuralRoster::Default()->FindDevice(deviceID, &device);
		if (status != B_OK) {
			fprintf(stderr, "neural: no device \"%s\" (see neural devices)\n",
				deviceID);
			return status;
		}
		status = session.SetTo(model, device, flags);
	} else
		status = session.SetTo(model, flags);

	if (status != B_OK) {
		fprintf(stderr, "neural: cannot set up the model%s%s: %s\n",
			deviceID != NULL ? " on " : "", deviceID != NULL ? deviceID : "",
			error_name(status));
	}
	return status;
}


static status_t
run_session(BNeuralSession& session, int32 runs, bigtime_t* _best,
	bigtime_t* _average)
{
	bigtime_t best = -1;
	bigtime_t total = 0;
	for (int32 i = 0; i < runs; i++) {
		status_t status = session.Run();
		if (status != B_OK) {
			fprintf(stderr, "neural: run failed: %s\n", error_name(status));
			return status;
		}
		neural_run_info info;
		session.GetRunInfo(&info);
		total += info.last_duration;
		if (best < 0 || info.last_duration < best)
			best = info.last_duration;
	}
	*_best = best;
	*_average = runs > 0 ? total / runs : 0;
	return B_OK;
}


static int
run_model(const Options& options)
{
	BNeuralModel model;
	if (load_model(options, model) != B_OK)
		return 1;

	bigtime_t setupStart = system_time();
	BNeuralSession session;
	if (make_session(options, model, session, options.device, 0) != B_OK)
		return 1;
	bigtime_t setupTime = system_time() - setupStart;

	neural_image_mapping mapping;
	bool isImage;
	if (fill_inputs(options, session, &mapping, &isImage) != B_OK)
		return 1;

	bigtime_t best, average;
	if (run_session(session, options.runs, &best, &average) != B_OK)
		return 1;

	neural_run_info info;
	session.GetRunInfo(&info);
	printf("%s on %s (%s): set up in %.1f ms, run in %.2f ms",
		model.Name(), session.Device().ID(), session.Device().Name(),
		setupTime / 1000.0, best / 1000.0);
	if (options.runs > 1) {
		printf(" (best of %" B_PRId32 ", average %.2f ms)", options.runs,
			average / 1000.0);
	}
	printf("; %" B_PRId32 " of %" B_PRId32 " operators on the device\n",
		info.device_operators, info.device_operators + info.cpu_operators);

	if (options.profile)
		print_profile(session);

	print_results(options, session, mapping, isImage);

	if (options.output != NULL) {
		BFile file(options.output,
			B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (file.InitCheck() != B_OK) {
			fprintf(stderr, "neural: cannot write %s\n", options.output);
			return 1;
		}
		for (int32 i = 0; i < session.CountOutputs(); i++) {
			const BTensor* output = session.OutputAt(i);
			file.Write(output->Data(), output->ByteSize());
		}
	}
	return 0;
}


static int
compare_model(const Options& options)
{
	BNeuralModel model;
	if (load_model(options, model) != B_OK)
		return 1;

	BNeuralSession device;
	BNeuralSession cpu;
	if (make_session(options, model, device, options.device, 0) != B_OK
		|| make_session(options, model, cpu, "cpu", 0) != B_OK) {
		return 1;
	}

	neural_run_info info;
	device.GetRunInfo(&info);
	printf("%s: %s runs %" B_PRId32 " of %" B_PRId32 " operators in %"
		B_PRId32 " partitions\n", model.Name(), device.Device().ID(),
		info.device_operators, info.device_operators + info.cpu_operators,
		info.device_partitions);
	if (info.device_operators == 0) {
		printf("nothing to compare: the device runs none of the model\n");
		return 2;
	}

	neural_image_mapping mapping;
	bool isImage;
	if (fill_inputs(options, device, &mapping, &isImage) != B_OK)
		return 1;
	for (int32 i = 0; i < device.CountInputs(); i++)
		cpu.InputAt(i)->CopyFrom(*device.InputAt(i));

	bigtime_t deviceBest, cpuBest, average;
	if (run_session(device, options.runs, &deviceBest, &average) != B_OK
		|| run_session(cpu, options.runs, &cpuBest, &average) != B_OK) {
		return 1;
	}

	// What was compared is only the device's work if the device did it.
	neural_run_info after;
	device.GetRunInfo(&after);
	if (after.device_failures > 0
		|| after.device_operators != info.device_operators) {
		printf("%s FAILED %" B_PRId32 " time(s) while running; the CPU did "
			"its part (see the syslog and the back end's messages)\n",
			device.Device().ID(), after.device_failures);
		return 4;
	}

	bool same = true;
	for (int32 i = 0; i < device.CountOutputs(); i++) {
		const BTensor* a = device.OutputAt(i);
		const BTensor* b = cpu.OutputAt(i);
		int64 count = a->CountElements();

		if (a->Type() == B_NEURAL_FLOAT32) {
			const float* x = (const float*)a->Data();
			const float* y = (const float*)b->Data();
			float largest = 0;
			float scale = 1e-6f;
			for (int64 j = 0; j < count; j++) {
				largest = fmaxf(largest, fabsf(x[j] - y[j]));
				scale = fmaxf(scale, fabsf(y[j]));
			}
			bool ok = largest / scale <= options.floatTolerance;
			printf("  %-40s largest difference %g (relative %g) %s\n",
				a->Info().Name(), largest, largest / scale,
				ok ? "ok" : "DIFFERENT");
			same &= ok;
		} else {
			int64 different = 0;
			float largest = 0;
			float scale = a->Info().IsQuantized() ? a->Info().Scale() : 1;
			for (int64 j = 0; j < count; j++) {
				float difference = fabsf(a->ValueAt(j) - b->ValueAt(j))
					/ scale;
				if (difference > 0.5f)
					different++;
				largest = fmaxf(largest, difference);
			}
			bool ok = largest <= options.tolerance + 0.5f;
			printf("  %-40s %" B_PRId64 " of %" B_PRId64 " differ, largest "
				"difference %.0f %s\n", a->Info().Name(), different, count,
				largest, ok ? "ok" : "DIFFERENT");
			same &= ok;
		}
	}

	printf("%s %.2f ms, cpu %.2f ms: %s\n", device.Device().ID(),
		deviceBest / 1000.0, cpuBest / 1000.0,
		same ? "the outputs agree" : "THE OUTPUTS DIFFER");
	return same ? 0 : 3;
}


static int
bench_model(const Options& options)
{
	BNeuralModel model;
	if (load_model(options, model) != B_OK)
		return 1;

	BNeuralRoster* roster = BNeuralRoster::Default();
	printf("%-20s %10s %10s %10s  %s\n", "device", "set up ms", "best ms",
		"average ms", "operators on device");

	for (int32 i = 0; i < roster->CountDevices(); i++) {
		BNeuralDevice device;
		if (roster->GetDeviceAt(i, &device) != B_OK)
			continue;
		if (options.device != NULL && strcmp(options.device, device.ID()) != 0)
			continue;

		bigtime_t setupStart = system_time();
		BNeuralSession session;
		if (session.SetTo(model, device, options.flags) != B_OK) {
			printf("%-20s %s\n", device.ID(),
				error_name(session.InitCheck()));
			continue;
		}
		bigtime_t setupTime = system_time() - setupStart;

		neural_image_mapping mapping;
		bool isImage;
		if (fill_inputs(options, session, &mapping, &isImage) != B_OK)
			return 1;

		bigtime_t best, average;
		if (run_session(session, 1, &best, &average) != B_OK
			|| run_session(session, options.runs, &best, &average) != B_OK) {
			continue;
		}

		neural_run_info info;
		session.GetRunInfo(&info);
		printf("%-20s %10.1f %10.2f %10.2f  %" B_PRId32 " of %" B_PRId32
			"\n", device.ID(), setupTime / 1000.0, best / 1000.0,
			average / 1000.0, info.device_operators,
			info.device_operators + info.cpu_operators);
	}
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fputs(kUsage, stderr);
		return 2;
	}

	const char* command = argv[1];
	if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
		fputs(kUsage, stdout);
		return 0;
	}

	Options options;
	memset(&options, 0, sizeof(options));
	options.fit = B_NEURAL_FIT_STRETCH;
	options.runs = strcmp(command, "bench") == 0 ? 20 : 1;
	options.top = 5;
	options.tolerance = 1;
	options.floatTolerance = 1e-3f;

	for (int i = 2; i < argc; i++) {
		const char* arg = argv[i];
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(arg, "--cpu") == 0)
			options.flags |= B_NEURAL_CPU_ONLY;
		else if (strcmp(arg, "--no-fallback") == 0)
			options.flags |= B_NEURAL_NO_FALLBACK;
		else if (strcmp(arg, "--low-power") == 0)
			options.flags |= B_NEURAL_PREFER_LOW_POWER;
		else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--profile") == 0)
			options.profile = true;
		else if (arg[0] == '-' && value == NULL) {
			fprintf(stderr, "neural: %s needs a value\n", arg);
			return 2;
		} else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--device") == 0) {
			options.device = value;
			i++;
		} else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--input") == 0) {
			options.input = value;
			i++;
		} else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--labels") == 0) {
			options.labels = value;
			i++;
		} else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
			options.output = value;
			i++;
		} else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--runs") == 0) {
			options.runs = atoi(value);
			if (options.runs < 1)
				options.runs = 1;
			i++;
		} else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--top") == 0) {
			options.top = atoi(value);
			i++;
		} else if (strcmp(arg, "--tolerance") == 0) {
			options.tolerance = atoi(value);
			i++;
		} else if (strcmp(arg, "--float-tolerance") == 0) {
			options.floatTolerance = atof(value);
			i++;
		} else if (strcmp(arg, "--fit") == 0) {
			if (strcmp(value, "crop") == 0)
				options.fit = B_NEURAL_FIT_CROP;
			else if (strcmp(value, "letterbox") == 0)
				options.fit = B_NEURAL_FIT_LETTERBOX;
			else if (strcmp(value, "stretch") != 0) {
				fprintf(stderr, "neural: unknown fit \"%s\"\n", value);
				return 2;
			}
			i++;
		} else if (arg[0] == '-') {
			fprintf(stderr, "neural: unknown option %s\n", arg);
			return 2;
		} else if (options.model == NULL)
			options.model = arg;
		else {
			fprintf(stderr, "neural: one model at a time\n");
			return 2;
		}
	}

	if (strcmp(command, "devices") == 0)
		return list_devices();

	if (options.model == NULL) {
		fputs(kUsage, stderr);
		return 2;
	}

	// Pictures are read with the Translation Kit, which makes bitmaps:
	// that takes an application.
	BApplication* app = NULL;
	if (options.input != NULL)
		app = new BApplication("application/x-vnd.airos-neural");

	int result;
	if (strcmp(command, "info") == 0)
		result = show_info(options);
	else if (strcmp(command, "run") == 0)
		result = run_model(options);
	else if (strcmp(command, "compare") == 0)
		result = compare_model(options);
	else if (strcmp(command, "bench") == 0)
		result = bench_model(options);
	else {
		fputs(kUsage, stderr);
		result = 2;
	}

	delete app;
	return result;
}
