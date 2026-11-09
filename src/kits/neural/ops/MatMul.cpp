/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	BATCH_MATMUL: products of matrices, which is what the attention of a
	transformer consists of (every position's query against every
	position's key, then the weights against the values).

	A row of the product is a sum of rows of the second matrix, each
	times one number of the first: written that way the inner loop walks
	whole rows of both, which the compiler turns into vector instructions
	whatever the shapes are (many short rows as well as few long ones).
	Four rows of the second matrix are added per walk, and two rows of
	the product are made at a time, which share what is read of the
	second matrix.
*/


// Multiply-add instructions are welcome here; the results then differ in
// the last digit between processors that have them and those that do
// not, as they do for the convolutions.
#pragma GCC optimize("O3", "align-loops=32")


#include "Kernels.h"


namespace BPrivate {


namespace {


struct MatMulData {
	//! matrices of the product
	int32	batches;
	//! of a matrix of the product: rows, columns; and what is summed over
	int32	rows;
	int32	columns;
	int32	inner;
	bool	adjoint_a;
	bool	adjoint_b;
	//! the dimensions before the last two, as the product has them
	int32	batch_rank;
	int32	batch_size[B_NEURAL_MAX_RANK];
	//! matrices to step in each input when the index of one grows
	int32	batch_step_a[B_NEURAL_MAX_RANK];
	int32	batch_step_b[B_NEURAL_MAX_RANK];
	int32	matrices_a;
	int32	matrices_b;
};


struct MatMulRun {
	const MatMulData*	data;
	//! as [rows, inner] and [inner, columns] matrices
	const float*		a;
	const float*		b;
	float*				output;
};


/*!	Two rows of a product: \a a0 and \a a1 are rows of the first matrix,
	\a b is the second one. */
static inline void
two_rows(const float* __restrict a0, const float* __restrict a1,
	const float* __restrict b, int32 inner, int32 columns,
	float* __restrict c0, float* __restrict c1)
{
	for (int32 j = 0; j < columns; j++) {
		c0[j] = 0;
		c1[j] = 0;
	}

	int32 p = 0;
	for (; p + 4 <= inner; p += 4) {
		const float* __restrict b0 = b + (size_t)p * columns;
		const float* __restrict b1 = b0 + columns;
		const float* __restrict b2 = b1 + columns;
		const float* __restrict b3 = b2 + columns;
		float x00 = a0[p], x01 = a0[p + 1], x02 = a0[p + 2], x03 = a0[p + 3];
		float x10 = a1[p], x11 = a1[p + 1], x12 = a1[p + 2], x13 = a1[p + 3];
		for (int32 j = 0; j < columns; j++) {
			float v0 = b0[j];
			float v1 = b1[j];
			float v2 = b2[j];
			float v3 = b3[j];
			c0[j] += x00 * v0 + x01 * v1 + x02 * v2 + x03 * v3;
			c1[j] += x10 * v0 + x11 * v1 + x12 * v2 + x13 * v3;
		}
	}
	for (; p < inner; p++) {
		const float* __restrict b0 = b + (size_t)p * columns;
		float x0 = a0[p];
		float x1 = a1[p];
		for (int32 j = 0; j < columns; j++) {
			c0[j] += x0 * b0[j];
			c1[j] += x1 * b0[j];
		}
	}
}


static inline void
one_row(const float* __restrict a0, const float* __restrict b, int32 inner,
	int32 columns, float* __restrict c0)
{
	for (int32 j = 0; j < columns; j++)
		c0[j] = 0;

	int32 p = 0;
	for (; p + 4 <= inner; p += 4) {
		const float* __restrict b0 = b + (size_t)p * columns;
		const float* __restrict b1 = b0 + columns;
		const float* __restrict b2 = b1 + columns;
		const float* __restrict b3 = b2 + columns;
		float x0 = a0[p], x1 = a0[p + 1], x2 = a0[p + 2], x3 = a0[p + 3];
		for (int32 j = 0; j < columns; j++)
			c0[j] += x0 * b0[j] + x1 * b1[j] + x2 * b2[j] + x3 * b3[j];
	}
	for (; p < inner; p++) {
		const float* __restrict b0 = b + (size_t)p * columns;
		float x0 = a0[p];
		for (int32 j = 0; j < columns; j++)
			c0[j] += x0 * b0[j];
	}
}


/*!	Which matrix of each input goes into matrix \a batch of the product. */
static inline void
batch_matrices(const MatMulData& data, int32 batch, int32* _a, int32* _b)
{
	int32 a = 0;
	int32 b = 0;
	for (int32 i = data.batch_rank - 1; i >= 0; i--) {
		int32 index = batch % data.batch_size[i];
		batch /= data.batch_size[i];
		a += index * data.batch_step_a[i];
		b += index * data.batch_step_b[i];
	}
	*_a = a;
	*_b = b;
}


//! Rows \a first to \a end, counted through all matrices of the product.
static void
matmul_rows(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	const MatMulRun& run = *(const MatMulRun*)cookie;
	const MatMulData& data = *run.data;
	int32 rows = data.rows;
	int32 columns = data.columns;
	int32 inner = data.inner;

	int32 row = first;
	while (row < end) {
		int32 batch = row / rows;
		int32 batchEnd = (batch + 1) * rows;
		if (batchEnd > end)
			batchEnd = end;

		int32 matrixA, matrixB;
		batch_matrices(data, batch, &matrixA, &matrixB);
		const float* a = run.a + (size_t)matrixA * rows * inner
			+ (size_t)(row - batch * rows) * inner;
		const float* b = run.b + (size_t)matrixB * inner * columns;
		float* c = run.output + (size_t)row * columns;

		for (; row + 2 <= batchEnd; row += 2) {
			two_rows(a, a + inner, b, inner, columns, c, c + columns);
			a += 2 * (size_t)inner;
			c += 2 * (size_t)columns;
		}
		if (row < batchEnd) {
			one_row(a, b, inner, columns, c);
			row++;
		}
	}
}


/*!	The matrices of an input, each turned over: \a rows and \a columns
	are those of the result. */
static void
transpose_matrices(const float* __restrict input, int32 count, int32 rows,
	int32 columns, float* __restrict output)
{
	for (int32 matrix = 0; matrix < count; matrix++) {
		for (int32 i = 0; i < rows; i++) {
			for (int32 j = 0; j < columns; j++)
				output[(size_t)i * columns + j] = input[(size_t)j * rows + i];
		}
		input += (size_t)rows * columns;
		output += (size_t)rows * columns;
	}
}


static status_t
prepare_batch_matmul(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	if (op->input_count != 2 || !neural_has_inputs(op, 2)
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_batch_matmul_params& params
		= *(const neural_batch_matmul_params*)op->params;
	const neural_graph_tensor& a = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& b = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	// floating point only: weights stored as integers ("hybrid") and
	// quantized products are not implemented
	int32 rank = a.rank > b.rank ? a.rank : b.rank;
	if (a.type != B_NEURAL_FLOAT32 || b.type != B_NEURAL_FLOAT32
		|| output.type != B_NEURAL_FLOAT32 || a.rank < 2 || b.rank < 2
		|| output.rank != rank || neural_element_count(a) < 0
		|| neural_element_count(b) < 0
		|| neural_element_count(output) < 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	MatMulData header;
	memset(&header, 0, sizeof(header));
	header.adjoint_a = params.adjoint_a;
	header.adjoint_b = params.adjoint_b;

	int32 rowsA = a.dimensions[a.rank - 2];
	int32 columnsA = a.dimensions[a.rank - 1];
	int32 rowsB = b.dimensions[b.rank - 2];
	int32 columnsB = b.dimensions[b.rank - 1];
	header.rows = params.adjoint_a ? columnsA : rowsA;
	header.inner = params.adjoint_a ? rowsA : columnsA;
	header.columns = params.adjoint_b ? rowsB : columnsB;
	if (header.rows < 1 || header.inner < 1 || header.columns < 1
		|| (params.adjoint_b ? columnsB : rowsB) != header.inner
		|| output.dimensions[rank - 2] != header.rows
		|| output.dimensions[rank - 1] != header.columns) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// The dimensions before the matrices go together NumPy style: a
	// size of 1 is repeated.
	header.batch_rank = rank - 2;
	header.batches = 1;
	int32 stepA = 1;
	int32 stepB = 1;
	for (int32 i = rank - 3; i >= 0; i--) {
		int32 indexA = i - (rank - a.rank);
		int32 indexB = i - (rank - b.rank);
		int32 sizeA = indexA >= 0 ? a.dimensions[indexA] : 1;
		int32 sizeB = indexB >= 0 ? b.dimensions[indexB] : 1;
		int32 size = sizeA > sizeB ? sizeA : sizeB;
		if ((sizeA != size && sizeA != 1) || (sizeB != size && sizeB != 1)
			|| output.dimensions[i] != size) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}

		header.batch_size[i] = size;
		header.batch_step_a[i] = sizeA == size ? stepA : 0;
		header.batch_step_b[i] = sizeB == size ? stepB : 0;
		header.batches *= size;
		stepA *= sizeA;
		stepB *= sizeB;
	}
	header.matrices_a = stepA;
	header.matrices_b = stepB;

	// rows are what the threads share out, counted in 32 bits
	if ((int64)header.batches * header.rows > kNeuralMaxElements)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	MatMulData* data = (MatMulData*)malloc(sizeof(MatMulData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static status_t
run_batch_matmul(const NeuralOpContext& context)
{
	const MatMulData& data = *(const MatMulData*)context.prepared;

	MatMulRun run;
	run.data = &data;
	run.a = context.Input<float>(0);
	run.b = context.Input<float>(1);
	run.output = context.Output<float>(0);

	// An input that is to be used transposed is turned over first; that
	// is little work next to the product.
	NeuralScratch scratch;
	size_t sizeA = (size_t)data.matrices_a * data.rows * data.inner;
	size_t sizeB = (size_t)data.matrices_b * data.inner * data.columns;
	size_t offsetA = 0;
	size_t offsetB = 0;
	if (data.adjoint_a)
		offsetA = scratch.Reserve(sizeA, sizeof(float));
	if (data.adjoint_b)
		offsetB = scratch.Reserve(sizeB, sizeof(float));
	if (data.adjoint_a || data.adjoint_b) {
		if (!scratch.Allocate())
			return B_NO_MEMORY;
		if (data.adjoint_a) {
			transpose_matrices(run.a, data.matrices_a, data.rows, data.inner,
				scratch.At<float>(offsetA));
			run.a = scratch.At<float>(offsetA);
		}
		if (data.adjoint_b) {
			transpose_matrices(run.b, data.matrices_b, data.inner,
				data.columns, scratch.At<float>(offsetB));
			run.b = scratch.At<float>(offsetB);
		}
	}

	// a few dozen microseconds of work are worth a thread
	int64 rowWork = (int64)data.inner * data.columns + 16;
	int64 grain = 65536 / rowWork;
	context.pool->Run(data.batches * data.rows, grain < 1 ? 1 : (int32)grain,
		&matmul_rows, &run);
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kBatchMatMulKernel = {
	B_NEURAL_OP_BATCH_MATMUL, &prepare_batch_matmul, &run_batch_matmul
};


}	// namespace BPrivate
