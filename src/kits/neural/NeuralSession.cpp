/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <neural/NeuralSession.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <Message.h>

#include <NeuralGraphData.h>
#include <NeuralInterpreter.h>
#include <NeuralRosterPrivate.h>


namespace BPrivate {


static const size_t kTensorAlignment = 64;
static const size_t kTensorPadding = 64;

//! Failed runs in a row after which a device loses its part of a session.
static const int32 kFailuresUntilLost = 3;


/*!	A model split into stages that run one after the other: stretches of
	operators the device runs as one program each, and the stretches in
	between, which the CPU interpreter runs.
*/
class NeuralSessionData {
public:
	struct Stage {
		bool				on_device;
		int32				first;
		int32				count;
		int32				partition;
		NeuralInterpreter*	interpreter;
			// for a device stage: made when the device first fails
		BNeuralProgram*		program;
		int32				failures;
			// runs in a row the device did not manage
	};

								NeuralSessionData();
								~NeuralSessionData();

			status_t			Init(const BNeuralModel& model,
									const BNeuralDevice& device,
									uint32 flags);
			status_t			Run();

			status_t			RunAsync(const BMessenger& target,
									const BMessage* message, int32* _token);
			status_t			WaitForRun(bigtime_t timeout);
			bool				IsRunning() const
									{ return atomic_get(
										(int32*)&fRunning) != 0; }

			BNeuralModel		fModel;
			BNeuralDevice		fDevice;
			uint32				fFlags;
			const NeuralGraphData* fGraph;

			BTensor*			fInputs;
			BTensor*			fOutputs;

			Stage*				fStages;
			int32				fStageCount;

			// per operator
			bool*				fOnDevice;
			int32*				fPartition;
			bigtime_t*			fDurations;

			neural_run_info		fInfo;
			bigtime_t			fTotalDuration;

private:
			status_t			_CreateProgram(int32 first, int32 end,
									BNeuralProgram** _program);
			status_t			_AllocateTensor(int32 index);
			status_t			_AllocateOperands(int32 first, int32 end);
			status_t			_PrepareInterpreter(Stage& stage);
			status_t			_FallBack(Stage& stage);
			status_t			_Run();
	static	status_t			_AsyncEntry(void* data);

			BNeuralBackend*		fBackend;
			int32				fBackendIndex;

			void**				fTensors;
			bool*				fOwned;

			int32				fRunning;
			thread_id			fThread;
			status_t			fAsyncStatus;
			int32				fToken;
			BMessenger			fTarget;
			BMessage			fMessage;
};


NeuralSessionData::NeuralSessionData()
	:
	fFlags(0),
	fGraph(NULL),
	fInputs(NULL),
	fOutputs(NULL),
	fStages(NULL),
	fStageCount(0),
	fOnDevice(NULL),
	fPartition(NULL),
	fDurations(NULL),
	fTotalDuration(0),
	fBackend(NULL),
	fBackendIndex(0),
	fTensors(NULL),
	fOwned(NULL),
	fRunning(0),
	fThread(-1),
	fAsyncStatus(B_OK),
	fToken(0)
{
	memset(&fInfo, 0, sizeof(fInfo));
}


NeuralSessionData::~NeuralSessionData()
{
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}

	for (int32 i = 0; i < fStageCount; i++) {
		delete fStages[i].interpreter;
		delete fStages[i].program;
	}
	free(fStages);

	delete[] fInputs;
	delete[] fOutputs;

	if (fTensors != NULL && fOwned != NULL) {
		for (int32 i = 0; i < fGraph->CountTensors(); i++) {
			if (fOwned[i])
				free(fTensors[i]);
		}
	}
	free(fTensors);
	free(fOwned);
	free(fOnDevice);
	free(fPartition);
	free(fDurations);
}


status_t
NeuralSessionData::_AllocateTensor(int32 index)
{
	if (index < 0 || fTensors[index] != NULL)
		return B_OK;

	const neural_graph_tensor& tensor = fGraph->Tensor(index);
	void* memory;
	if (posix_memalign(&memory, kTensorAlignment,
			tensor.size + kTensorPadding) != 0) {
		return B_NO_MEMORY;
	}
	memset(memory, 0, tensor.size + kTensorPadding);

	fTensors[index] = memory;
	fOwned[index] = true;
	return B_OK;
}


/*!	Memory for everything the operators of a stretch read and write. */
status_t
NeuralSessionData::_AllocateOperands(int32 first, int32 end)
{
	for (int32 i = first; i < end; i++) {
		const neural_graph_operator& op = fGraph->Operator(i);
		for (int32 j = 0; j < op.input_count; j++) {
			status_t status = _AllocateTensor(op.inputs[j]);
			if (status != B_OK)
				return status;
		}
		for (int32 j = 0; j < op.output_count; j++) {
			status_t status = _AllocateTensor(op.outputs[j]);
			if (status != B_OK)
				return status;
		}
	}
	return B_OK;
}


/*!	Has the back end make a program of the operators first .. end - 1. */
status_t
NeuralSessionData::_CreateProgram(int32 first, int32 end,
	BNeuralProgram** _program)
{
	int32 tensorCount = fGraph->CountTensors();
	int32 opCount = fGraph->CountOperators();

	// 1: made inside, 2: listed as input, 4: listed as output
	uint8* marks = (uint8*)calloc(tensorCount > 0 ? tensorCount : 1, 1);
	int32* operators = (int32*)malloc((end - first) * sizeof(int32));
	int32* inputs = (int32*)malloc((tensorCount + 1) * sizeof(int32));
	int32* outputs = (int32*)malloc((tensorCount + 1) * sizeof(int32));
	if (marks == NULL || operators == NULL || inputs == NULL
		|| outputs == NULL) {
		free(marks);
		free(operators);
		free(inputs);
		free(outputs);
		return B_NO_MEMORY;
	}

	int32 inputCount = 0;
	int32 outputCount = 0;

	for (int32 i = first; i < end; i++) {
		const neural_graph_operator& op = fGraph->Operator(i);
		operators[i - first] = i;

		for (int32 j = 0; j < op.input_count; j++) {
			int32 index = op.inputs[j];
			if (index < 0 || fGraph->Tensor(index).data != NULL
				|| marks[index] != 0) {
				continue;
			}
			marks[index] |= 2;
			inputs[inputCount++] = index;
		}
		for (int32 j = 0; j < op.output_count; j++)
			marks[op.outputs[j]] |= 1;
	}

	// what is made inside and needed outside: by a later operator or as a
	// result of the model
	for (int32 i = end; i < opCount; i++) {
		const neural_graph_operator& op = fGraph->Operator(i);
		for (int32 j = 0; j < op.input_count; j++) {
			int32 index = op.inputs[j];
			if (index >= 0 && (marks[index] & 5) == 1) {
				marks[index] |= 4;
				outputs[outputCount++] = index;
			}
		}
	}
	for (int32 i = 0; i < fGraph->CountOutputs(); i++) {
		int32 index = fGraph->OutputAt(i);
		if ((marks[index] & 5) == 1) {
			marks[index] |= 4;
			outputs[outputCount++] = index;
		}
	}

	neural_partition partition;
	memset(&partition, 0, sizeof(partition));
	partition.operators = operators;
	partition.operator_count = end - first;
	partition.inputs = inputs;
	partition.input_count = inputCount;
	partition.outputs = outputs;
	partition.output_count = outputCount;

	BNeuralProgram* program = NULL;
	status_t status = fBackend->CreateProgram(fBackendIndex,
		fGraph->Public(), partition, fFlags, &program);
	if (status == B_OK && program == NULL)
		status = B_ERROR;

	// The boundary tensors live in the session's memory.
	for (int32 i = 0; status == B_OK && i < inputCount; i++)
		status = _AllocateTensor(inputs[i]);
	for (int32 i = 0; status == B_OK && i < outputCount; i++)
		status = _AllocateTensor(outputs[i]);

	free(marks);
	free(operators);
	free(inputs);
	free(outputs);

	if (status != B_OK) {
		delete program;
		return status;
	}

	*_program = program;
	return B_OK;
}


status_t
NeuralSessionData::Init(const BNeuralModel& model,
	const BNeuralDevice& device, uint32 flags)
{
	status_t status = model.InitCheck();
	if (status != B_OK)
		return status;
	if (!device.IsValid())
		return B_NEURAL_NO_DEVICE;

	fModel = model;
		// this copy keeps the graph alive
	fGraph = fModel.fGraph;
	fDevice = device;
	fFlags = flags;

	status = NeuralRosterData::Default()->GetBackend(device.ID(), &fBackend,
		&fBackendIndex);
	if (status != B_OK)
		return status;

	int32 opCount = fGraph->CountOperators();
	int32 tensorCount = fGraph->CountTensors();

	fTensors = (void**)calloc(tensorCount > 0 ? tensorCount : 1,
		sizeof(void*));
	fOwned = (bool*)calloc(tensorCount > 0 ? tensorCount : 1, sizeof(bool));
	fOnDevice = (bool*)calloc(opCount > 0 ? opCount : 1, sizeof(bool));
	fPartition = (int32*)malloc((opCount > 0 ? opCount : 1) * sizeof(int32));
	fDurations = (bigtime_t*)calloc(opCount > 0 ? opCount : 1,
		sizeof(bigtime_t));
	BNeuralProgram** programs = (BNeuralProgram**)calloc(
		opCount > 0 ? opCount : 1, sizeof(BNeuralProgram*));
	fStages = (Stage*)calloc(opCount > 0 ? opCount : 1, sizeof(Stage));
	if (fTensors == NULL || fOwned == NULL || fOnDevice == NULL
		|| fPartition == NULL || fDurations == NULL || programs == NULL
		|| fStages == NULL) {
		free(programs);
		return B_NO_MEMORY;
	}

	for (int32 i = 0; i < tensorCount; i++)
		fTensors[i] = (void*)fGraph->Tensor(i).data;
	for (int32 i = 0; i < opCount; i++)
		fPartition[i] = -1;

	// What the device takes: every stretch of operators it supports
	// becomes one program, unless the back end thinks better of it.
	if (fBackend != NULL) {
		status = fBackend->GetSupportedOperators(fBackendIndex,
			fGraph->Public(), fOnDevice);
		if (status != B_OK)
			memset(fOnDevice, 0, opCount * sizeof(bool));

		for (int32 first = 0; first < opCount;) {
			int32 end = first + 1;
			if (!fOnDevice[first]) {
				first = end;
				continue;
			}
			while (end < opCount && fOnDevice[end])
				end++;

			// Whatever keeps the back end from making the program, also
			// a device that is out of memory, the CPU runs the stretch.
			status = _CreateProgram(first, end, &programs[first]);
			if (status != B_OK) {
				for (int32 i = first; i < end; i++)
					fOnDevice[i] = false;
			}
			first = end;
		}
	}

	if ((fFlags & B_NEURAL_NO_FALLBACK) != 0 && fBackend != NULL) {
		for (int32 i = 0; i < opCount; i++) {
			if (!fOnDevice[i]) {
				for (int32 j = 0; j < opCount; j++)
					delete programs[j];
				free(programs);
				return B_NEURAL_NOT_ACCELERATED;
			}
		}
	}

	// the stages: programs and what is between them
	status = B_OK;
	int32 partitions = 0;
	for (int32 first = 0; first < opCount && status == B_OK;) {
		int32 end = first + 1;
		while (end < opCount && fOnDevice[end] == fOnDevice[first]
			&& programs[end] == NULL) {
			end++;
		}

		Stage& stage = fStages[fStageCount++];
		stage.first = first;
		stage.count = end - first;
		stage.on_device = fOnDevice[first];
		stage.partition = -1;

		if (stage.on_device) {
			stage.program = programs[first];
			programs[first] = NULL;
			stage.partition = partitions++;
			for (int32 i = first; i < end; i++)
				fPartition[i] = stage.partition;
		} else {
			stage.interpreter = new(std::nothrow) NeuralInterpreter(fGraph);
			int32* operators = (int32*)malloc(stage.count * sizeof(int32));
			if (stage.interpreter == NULL || operators == NULL) {
				status = B_NO_MEMORY;
			} else {
				for (int32 i = 0; i < stage.count; i++)
					operators[i] = first + i;
				status = stage.interpreter->Prepare(operators, stage.count);
				if (status == B_OK)
					status = _AllocateOperands(first, end);
			}
			free(operators);
		}

		first = end;
	}

	for (int32 i = 0; i < opCount; i++)
		delete programs[i];
	free(programs);
	if (status != B_OK)
		return status;

	// what the caller sees
	int32 inputCount = fGraph->CountInputs();
	int32 outputCount = fGraph->CountOutputs();
	fInputs = new(std::nothrow) BTensor[inputCount > 0 ? inputCount : 1];
	fOutputs = new(std::nothrow) BTensor[outputCount > 0 ? outputCount : 1];
	if (fInputs == NULL || fOutputs == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < inputCount + outputCount; i++) {
		bool isInput = i < inputCount;
		int32 index = isInput ? fGraph->InputAt(i)
			: fGraph->OutputAt(i - inputCount);

		// An input nothing reads, or a constant as output, still has to
		// be there.
		const neural_graph_tensor& tensor = fGraph->Tensor(index);
		if (tensor.data == NULL) {
			status = _AllocateTensor(index);
			if (status != B_OK)
				return status;
		}

		BTensorInfo info(tensor.type, tensor.rank, tensor.dimensions,
			tensor.name);
		if (tensor.quantized)
			info.SetQuantization(tensor.scale, tensor.zero_point);

		BTensor& target = isInput ? fInputs[i] : fOutputs[i - inputCount];
		status = target.SetTo(info, fTensors[index], tensor.size);
		if (status != B_OK)
			return status;
	}

	fInfo.device_partitions = partitions;
	for (int32 i = 0; i < opCount; i++) {
		if (fOnDevice[i])
			fInfo.device_operators++;
		else
			fInfo.cpu_operators++;
	}

	return B_OK;
}


/*!	Gets the CPU ready to run a device stage. */
status_t
NeuralSessionData::_PrepareInterpreter(Stage& stage)
{
	if (stage.interpreter != NULL)
		return B_OK;

	NeuralInterpreter* interpreter = new(std::nothrow) NeuralInterpreter(
		fGraph);
	int32* operators = (int32*)malloc(stage.count * sizeof(int32));
	status_t status = interpreter != NULL && operators != NULL
		? B_OK : B_NO_MEMORY;
	if (status == B_OK) {
		for (int32 i = 0; i < stage.count; i++)
			operators[i] = stage.first + i;
		status = interpreter->Prepare(operators, stage.count);
	}
	if (status == B_OK)
		status = _AllocateOperands(stage.first, stage.first + stage.count);
	free(operators);

	if (status != B_OK) {
		delete interpreter;
		return status;
	}

	stage.interpreter = interpreter;
	return B_OK;
}


/*!	Moves a stage from the device, which is gone, to the CPU. */
status_t
NeuralSessionData::_FallBack(Stage& stage)
{
	status_t status = _PrepareInterpreter(stage);
	if (status != B_OK)
		return status == B_NO_MEMORY ? status : B_NEURAL_DEVICE_LOST;

	delete stage.program;
	stage.program = NULL;
	stage.on_device = false;
	stage.partition = -1;
	for (int32 i = stage.first; i < stage.first + stage.count; i++) {
		fOnDevice[i] = false;
		fPartition[i] = -1;
	}
	fInfo.device_operators -= stage.count;
	fInfo.cpu_operators += stage.count;
	fInfo.device_partitions--;
	return B_OK;
}


status_t
NeuralSessionData::_Run()
{
	bool profile = (fFlags & B_NEURAL_PROFILE) != 0;
	bigtime_t start = system_time();
	bigtime_t deviceTime = 0;

	for (int32 i = 0; i < fStageCount; i++) {
		Stage& stage = fStages[i];

		if (stage.on_device) {
			bigtime_t stageStart = system_time();
			status_t status = stage.program->Run(fTensors);
			bigtime_t duration = system_time() - stageStart;

			if (status == B_OK) {
				stage.failures = 0;
				deviceTime += duration;
				if (profile) {
					for (int32 j = 0; j < stage.count; j++)
						fDurations[stage.first + j] = duration;
				}
				continue;
			}

			// The device did not manage. Its part of this run is done
			// by the CPU, so that the caller gets its answer; a device
			// that is gone, or fails again and again, loses the stage
			// for good.
			if ((fFlags & B_NEURAL_NO_FALLBACK) != 0) {
				return status == B_NEURAL_DEVICE_LOST
					? status : (status_t)B_NEURAL_DEVICE_LOST;
			}

			fInfo.device_failures++;
			if (status == B_NEURAL_DEVICE_LOST
				|| ++stage.failures >= kFailuresUntilLost) {
				status = _FallBack(stage);
			} else
				status = _PrepareInterpreter(stage);
			if (status != B_OK)
				return status;
		}

		status_t status = stage.interpreter->Run(fTensors,
			profile ? fDurations + stage.first : NULL);
		if (status != B_OK)
			return status;
	}

	bigtime_t duration = system_time() - start;
	fInfo.runs++;
	fInfo.last_duration = duration;
	fInfo.last_device_duration = deviceTime;
	fTotalDuration += duration;
	fInfo.average_duration = fTotalDuration / fInfo.runs;
	return B_OK;
}


status_t
NeuralSessionData::Run()
{
	if (atomic_test_and_set(&fRunning, 1, 0) != 0)
		return B_BUSY;

	status_t status = _Run();
	atomic_set(&fRunning, 0);
	return status;
}


/*static*/ status_t
NeuralSessionData::_AsyncEntry(void* data)
{
	NeuralSessionData* self = (NeuralSessionData*)data;

	status_t status = self->_Run();
	self->fAsyncStatus = status;

	BMessage message(self->fMessage);
	BMessenger target(self->fTarget);
	message.AddInt32("neural:status", status);
	message.AddInt32("neural:token", self->fToken);
	message.AddInt64("neural:duration",
		status == B_OK ? self->fInfo.last_duration : 0);

	// The run is over before the target hears of it, so that it may
	// start the next one right away.
	atomic_set(&self->fRunning, 0);
	target.SendMessage(&message, (BHandler*)NULL, 0);
	return status;
}


status_t
NeuralSessionData::RunAsync(const BMessenger& target,
	const BMessage* message, int32* _token)
{
	if (atomic_test_and_set(&fRunning, 1, 0) != 0)
		return B_BUSY;

	// the thread of the run before, which is over
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

	fTarget = target;
	if (message != NULL)
		fMessage = *message;
	else {
		fMessage.MakeEmpty();
		fMessage.what = B_NEURAL_RUN_COMPLETED;
	}
	fToken++;
	if (_token != NULL)
		*_token = fToken;

	fThread = spawn_thread(&_AsyncEntry, "neural run", B_NORMAL_PRIORITY,
		this);
	if (fThread < 0) {
		status_t status = fThread;
		fThread = -1;
		atomic_set(&fRunning, 0);
		return status;
	}

	resume_thread(fThread);
	return B_OK;
}


status_t
NeuralSessionData::WaitForRun(bigtime_t timeout)
{
	if (fThread < 0)
		return fAsyncStatus;

	status_t result;
	status_t status = wait_for_thread_etc(fThread,
		timeout == B_INFINITE_TIMEOUT ? 0 : B_RELATIVE_TIMEOUT, timeout,
		&result);
	if (status == B_TIMED_OUT || status == B_WOULD_BLOCK
		|| status == B_INTERRUPTED) {
		return status == B_WOULD_BLOCK ? B_TIMED_OUT : status;
	}

	fThread = -1;
	return fAsyncStatus;
}


}	// namespace BPrivate


using BPrivate::NeuralRosterData;
using BPrivate::NeuralSessionData;


// #pragma mark - BNeuralSession


BNeuralSession::BNeuralSession()
	:
	fData(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BNeuralSession::BNeuralSession(const BNeuralModel& model, uint32 flags)
	:
	fData(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(model, flags);
}


BNeuralSession::BNeuralSession(const BNeuralModel& model,
	const BNeuralDevice& device, uint32 flags)
	:
	fData(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(model, device, flags);
}


BNeuralSession::~BNeuralSession()
{
	Unset();
}


status_t
BNeuralSession::SetTo(const BNeuralModel& model, uint32 flags)
{
	Unset();

	if (model.InitCheck() != B_OK)
		return fStatus = model.InitCheck();

	BNeuralDevice device;
	status_t status = BNeuralRoster::Default()->GetDefaultDevice(&device,
		flags, &model);
	if (status != B_OK)
		return fStatus = status;

	status = SetTo(model, device, flags);
	if (status == B_OK || (flags & B_NEURAL_NO_FALLBACK) != 0
		|| device.Kind() == B_NEURAL_DEVICE_CPU) {
		return status;
	}

	// The device of choice let us down; the CPU does not.
	if (BNeuralRoster::Default()->FindDevice(BPrivate::kNeuralCPUDeviceID,
			&device) != B_OK) {
		return fStatus = status;
	}
	return SetTo(model, device, flags);
}


status_t
BNeuralSession::SetTo(const BNeuralModel& model, const BNeuralDevice& device,
	uint32 flags)
{
	Unset();

	NeuralSessionData* data = new(std::nothrow) NeuralSessionData();
	if (data == NULL)
		return fStatus = B_NO_MEMORY;

	BNeuralDevice target(device);
	if ((flags & B_NEURAL_CPU_ONLY) != 0) {
		NeuralRosterData::Default()->FindDevice(
			BPrivate::kNeuralCPUDeviceID, &target);
	}

	status_t status = data->Init(model, target, flags);
	if (status != B_OK) {
		delete data;
		return fStatus = status;
	}

	fData = data;
	return fStatus = B_OK;
}


void
BNeuralSession::Unset()
{
	delete fData;
	fData = NULL;
	fStatus = B_NO_INIT;
}


status_t
BNeuralSession::InitCheck() const
{
	return fStatus;
}


const BNeuralModel&
BNeuralSession::Model() const
{
	static const BNeuralModel sNoModel;
	return fData != NULL ? fData->fModel : sNoModel;
}


const BNeuralDevice&
BNeuralSession::Device() const
{
	static const BNeuralDevice sNoDevice;
	return fData != NULL ? fData->fDevice : sNoDevice;
}


int32
BNeuralSession::CountInputs() const
{
	return fData != NULL ? fData->fGraph->CountInputs() : 0;
}


BTensor*
BNeuralSession::InputAt(int32 index)
{
	if (fData == NULL || index < 0 || index >= fData->fGraph->CountInputs())
		return NULL;
	return &fData->fInputs[index];
}


BTensor*
BNeuralSession::FindInput(const char* name)
{
	if (fData == NULL)
		return NULL;
	return InputAt(fData->fModel.FindInput(name));
}


int32
BNeuralSession::CountOutputs() const
{
	return fData != NULL ? fData->fGraph->CountOutputs() : 0;
}


const BTensor*
BNeuralSession::OutputAt(int32 index) const
{
	if (fData == NULL || index < 0
		|| index >= fData->fGraph->CountOutputs()) {
		return NULL;
	}
	return &fData->fOutputs[index];
}


const BTensor*
BNeuralSession::FindOutput(const char* name) const
{
	if (fData == NULL)
		return NULL;
	return OutputAt(fData->fModel.FindOutput(name));
}


status_t
BNeuralSession::Run()
{
	if (fData == NULL)
		return fStatus != B_OK ? fStatus : B_NO_INIT;
	return fData->Run();
}


status_t
BNeuralSession::RunAsync(const BMessenger& target, const BMessage* message,
	int32* _token)
{
	if (fData == NULL)
		return fStatus != B_OK ? fStatus : B_NO_INIT;
	return fData->RunAsync(target, message, _token);
}


status_t
BNeuralSession::WaitForRun(bigtime_t timeout)
{
	if (fData == NULL)
		return fStatus != B_OK ? fStatus : B_NO_INIT;
	return fData->WaitForRun(timeout);
}


bool
BNeuralSession::IsRunning() const
{
	return fData != NULL && fData->IsRunning();
}


status_t
BNeuralSession::GetRunInfo(neural_run_info* info) const
{
	if (fData == NULL)
		return B_NO_INIT;
	if (info == NULL)
		return B_BAD_VALUE;

	*info = fData->fInfo;
	return B_OK;
}


status_t
BNeuralSession::GetOperatorInfo(int32 index, neural_operator_info* info) const
{
	if (fData == NULL)
		return B_NO_INIT;
	if (info == NULL)
		return B_BAD_VALUE;
	if (index < 0 || index >= fData->fGraph->CountOperators())
		return B_BAD_INDEX;

	memset(info, 0, sizeof(*info));
	info->code = fData->fGraph->Operator(index).code;
	info->on_device = fData->fOnDevice[index];
	info->partition = fData->fPartition[index];
	info->last_duration = fData->fDurations[index];
	return B_OK;
}


void BNeuralSession::_ReservedNeuralSession1() {}
void BNeuralSession::_ReservedNeuralSession2() {}
void BNeuralSession::_ReservedNeuralSession3() {}
void BNeuralSession::_ReservedNeuralSession4() {}
