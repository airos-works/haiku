/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <NeuralInterpreter.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


namespace BPrivate {


// #pragma mark - NeuralThreadPool


struct NeuralThreadPool::Impl {
	pthread_mutex_t	lock;
	pthread_cond_t	workAvailable;
	pthread_cond_t	workDone;
	pthread_mutex_t	runLock;
		// one Run() at a time
	pthread_t*		threads;

	// the job being run
	Function		function;
	void*			cookie;
	int32			count;
	int32			sliceSize;
	int32			nextSlice;
	int32			sliceCount;
	int32			activeWorkers;
	uint32			generation;
};


static pthread_once_t sPoolOnce = PTHREAD_ONCE_INIT;
static NeuralThreadPool* sPool;


NeuralThreadPool::NeuralThreadPool()
	:
	fImpl(new Impl),
	fThreadCount(1)
{
	memset(fImpl, 0, sizeof(Impl));
	pthread_mutex_init(&fImpl->lock, NULL);
	pthread_mutex_init(&fImpl->runLock, NULL);
	pthread_cond_init(&fImpl->workAvailable, NULL);
	pthread_cond_init(&fImpl->workDone, NULL);

	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpus < 1)
		cpus = 1;
	if (cpus > 16)
		cpus = 16;

	const char* setting = getenv("NEURAL_THREADS");
	if (setting != NULL && atoi(setting) > 0)
		cpus = atoi(setting);

	// The caller of Run() works too, so one thread less than CPUs.
	fImpl->threads = (pthread_t*)calloc(cpus, sizeof(pthread_t));
	if (fImpl->threads == NULL)
		return;

	for (long i = 1; i < cpus; i++) {
		if (pthread_create(&fImpl->threads[fThreadCount - 1], NULL,
				&_WorkerEntry, this) != 0) {
			break;
		}
		fThreadCount++;
	}
}


NeuralThreadPool::~NeuralThreadPool()
{
	// never destroyed: the pool lives as long as the team
}


/*static*/ void
NeuralThreadPool::_Create()
{
	sPool = new NeuralThreadPool();
}


/*static*/ NeuralThreadPool*
NeuralThreadPool::Default()
{
	pthread_once(&sPoolOnce, &_Create);
	return sPool;
}


/*static*/ void*
NeuralThreadPool::_WorkerEntry(void* data)
{
	((NeuralThreadPool*)data)->_Worker();
	return NULL;
}


/*!	Takes slices of the current job until none is left; called with the
	lock held, returns with it held. */
bool
NeuralThreadPool::_RunSlices()
{
	bool ran = false;
	while (fImpl->nextSlice < fImpl->sliceCount) {
		int32 slice = fImpl->nextSlice++;
		Function function = fImpl->function;
		void* cookie = fImpl->cookie;
		int32 first = slice * fImpl->sliceSize;
		int32 end = first + fImpl->sliceSize;
		if (end > fImpl->count)
			end = fImpl->count;

		pthread_mutex_unlock(&fImpl->lock);
		function(cookie, first, end);
		pthread_mutex_lock(&fImpl->lock);
		ran = true;
	}
	return ran;
}


void
NeuralThreadPool::_Worker()
{
	uint32 seen = 0;

	pthread_mutex_lock(&fImpl->lock);
	while (true) {
		while (fImpl->generation == seen
			|| fImpl->nextSlice >= fImpl->sliceCount) {
			seen = fImpl->generation;
			pthread_cond_wait(&fImpl->workAvailable, &fImpl->lock);
		}

		seen = fImpl->generation;
		fImpl->activeWorkers++;
		_RunSlices();
		fImpl->activeWorkers--;
		if (fImpl->activeWorkers == 0)
			pthread_cond_broadcast(&fImpl->workDone);
	}
}


void
NeuralThreadPool::Run(int32 count, int32 grain, Function function,
	void* cookie)
{
	if (count <= 0)
		return;
	if (grain < 1)
		grain = 1;

	int32 slices = count / grain;
	if (slices > fThreadCount * 4)
		slices = fThreadCount * 4;
	if (fThreadCount == 1 || slices < 2) {
		function(cookie, 0, count);
		return;
	}

	pthread_mutex_lock(&fImpl->runLock);
	pthread_mutex_lock(&fImpl->lock);

	fImpl->function = function;
	fImpl->cookie = cookie;
	fImpl->count = count;
	fImpl->sliceSize = (count + slices - 1) / slices;
	fImpl->sliceCount = (count + fImpl->sliceSize - 1) / fImpl->sliceSize;
	fImpl->nextSlice = 0;
	fImpl->generation++;
	pthread_cond_broadcast(&fImpl->workAvailable);

	_RunSlices();
	while (fImpl->activeWorkers > 0)
		pthread_cond_wait(&fImpl->workDone, &fImpl->lock);

	pthread_mutex_unlock(&fImpl->lock);
	pthread_mutex_unlock(&fImpl->runLock);
}


// #pragma mark - NeuralInterpreter


struct NeuralInterpreter::Step {
	const neural_graph_operator*	op;
	const NeuralKernel*				kernel;
	void*							prepared;
};


NeuralInterpreter::NeuralInterpreter(const NeuralGraphData* graph)
	:
	fGraph(graph),
	fSteps(NULL),
	fStepCount(0)
{
}


NeuralInterpreter::~NeuralInterpreter()
{
	for (int32 i = 0; i < fStepCount; i++)
		free(fSteps[i].prepared);
	free(fSteps);
}


/*static*/ bool
NeuralInterpreter::Supports(const NeuralGraphData* graph, int32 index)
{
	if (index < 0 || index >= graph->CountOperators())
		return false;

	const neural_graph_operator& op = graph->Operator(index);
	const NeuralKernel* kernel = neural_find_kernel(op.code);
	if (kernel == NULL)
		return false;

	void* prepared = NULL;
	status_t status = kernel->prepare(graph, &op, &prepared);
	free(prepared);
	return status == B_OK;
}


status_t
NeuralInterpreter::Prepare(const int32* operators, int32 count)
{
	if (fSteps != NULL)
		return B_NOT_ALLOWED;
	if (count < 0)
		return B_BAD_VALUE;

	fSteps = (Step*)calloc(count > 0 ? count : 1, sizeof(Step));
	if (fSteps == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < count; i++) {
		if (operators[i] < 0 || operators[i] >= fGraph->CountOperators())
			return B_BAD_INDEX;

		Step& step = fSteps[i];
		step.op = &fGraph->Operator(operators[i]);
		step.kernel = neural_find_kernel(step.op->code);
		if (step.kernel == NULL)
			return B_NEURAL_UNSUPPORTED_OPERATOR;

		fStepCount = i + 1;
		status_t status = step.kernel->prepare(fGraph, step.op,
			&step.prepared);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


status_t
NeuralInterpreter::Run(void* const* tensors, bigtime_t* durations)
{
	NeuralOpContext context;
	context.graph = fGraph;
	context.tensors = tensors;
	context.pool = NeuralThreadPool::Default();

	for (int32 i = 0; i < fStepCount; i++) {
		const Step& step = fSteps[i];
		context.op = step.op;
		context.prepared = step.prepared;

		bigtime_t start = durations != NULL ? system_time() : 0;
		status_t status = step.kernel->run(context);
		if (status != B_OK)
			return status;
		if (durations != NULL)
			durations[i] = system_time() - start;
	}

	return B_OK;
}


}	// namespace BPrivate
