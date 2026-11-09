/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef XDNA_BUNDLE_H
#define XDNA_BUNDLE_H


/*!	Design bundles: what the NPU runs.

	The NPU does not run operators; it runs designs, each compiled on a
	Linux host (mlir-aie, Peano) for one stretch of one network. A bundle
	is a directory with such a design and a manifest that says which
	stretch of which network it computes and how the network's tensors lie
	in the design's buffers. README in this directory has the format.

	Bundles are found by the graph key: a number computed from what the
	network is made of (not from its weights, so that asking is quick). A
	bundle then applies if the weights its stretch uses are the ones it was
	made for (the weights digest).
*/


#include <String.h>
#include <SupportDefs.h>

#include <add-ons/neural/NeuralBackend.h>


#define XDNA_BUNDLE_MANIFEST	"bundle.txt"
#define XDNA_BUNDLE_SUBPATH		"neural/amdxdna"
#define XDNA_BUNDLE_ENVIRONMENT	"NEURAL_AMDXDNA_BUNDLES"

//! most buffers a design has (the kernel's bo0 to bo4)
#define XDNA_MAX_BUFFERS		5


enum bundle_buffer_kind {
	BUNDLE_BUFFER_INPUT,
	BUNDLE_BUFFER_OUTPUT,
	BUNDLE_BUFFER_CONSTANT
};

//! How a tensor of the network lies in a buffer of the design.
enum bundle_layout {
	//! as in the network: byte for byte
	BUNDLE_LAYOUT_NHWC,
	/*!	[1, H, W, C], C a multiple of 8, as mlir-aie's kernels take
		pictures: line by line, each line in groups of 8 channels:
		[H][C / 8][W][8] */
	BUNDLE_LAYOUT_HCWC8,
	/*!	[O, 1, 1, I] weights of a 1x1 convolution, O and I multiples of 8,
		as mlir-aie's conv2dk1 takes them: [O / 8][I / 8][8 of I][8 of O] */
	BUNDLE_LAYOUT_OIIO8
};

struct bundle_buffer {
	bundle_buffer_kind	kind;
	size_t				size;
	//! the tensor: input or output \c index of operator \c op of the stretch
	int32				op;
	bool				output;
	int32				index;
	bundle_layout		layout;
};


/*!	The graph key of \a graph as 32 hexadecimal digits. Fails for a graph
	with operators whose parameters this code does not know. */
status_t xdna_graph_key(const BNeuralGraph& graph, char key[33]);

/*!	The digest (64 digits) of the constants the operators
	first .. first + count - 1 read. */
status_t xdna_weights_digest(const BNeuralGraph& graph, int32 first,
	int32 count, char digest[65]);

//! Whether any directory bundles could be in exists at all.
bool xdna_have_bundle_directories();


class Bundle {
public:
								Bundle();
								~Bundle();

	/*!	Looks for the bundle of the graph with \a key in the bundle
		directories and reads its manifest. B_ENTRY_NOT_FOUND: there is
		none. */
			status_t			Find(const char* key);
	//! Reads the manifest in \a directory.
			status_t			Load(const char* directory);

	/*!	Whether the bundle fits the graph: its stretch exists, its buffers
		are the tensors that cross the stretch's border and its constants,
		in layouts that suit their shapes. */
			status_t			Check(const BNeuralGraph& graph) const;

	//! The graph's tensor a buffer stands for.
			int32				TensorOf(const BNeuralGraph& graph,
									const bundle_buffer& buffer) const;

			status_t			ReadFile(const char* name, void** _data,
									size_t* _size) const;

			const char*			Path() const { return fPath.String(); }
			const char*			Name() const { return fName.String(); }
			const char*			GraphKey() const { return fKey.String(); }
			const char*			WeightsDigest() const
									{ return fWeights.String(); }
			int32				FirstOperator() const { return fFirst; }
			int32				CountOperators() const { return fCount; }
			uint32				Columns() const { return fColumns; }
			uint32				OpsPerCycle() const { return fOpsPerCycle; }
			uint32				Opcode() const { return fOpcode; }
			bool				SizeInBytes() const { return fSizeInBytes; }
			uint32				Timeout() const { return fTimeout; }
			const char*			PDI() const { return fPDI.String(); }
			const char*			Instructions() const
									{ return fInstructions.String(); }
			int32				CountBuffers() const { return fBufferCount; }
			const bundle_buffer& BufferAt(int32 index) const
									{ return fBuffers[index]; }

private:
			status_t			_ParseBuffer(char* value);

			BString				fPath;
			BString				fName;
			BString				fKey;
			BString				fWeights;
			BString				fPDI;
			BString				fInstructions;
			int32				fFirst;
			int32				fCount;
			uint32				fColumns;
			uint32				fOpsPerCycle;
			uint32				fOpcode;
			bool				fSizeInBytes;
			uint32				fTimeout;
			int32				fBufferCount;
			bundle_buffer		fBuffers[XDNA_MAX_BUFFERS];
};


/*!	Copies a tensor of the network into a design's buffer (\a toDevice)
	or back. Both have the tensor's size. */
void xdna_convert_layout(bundle_layout layout,
	const neural_graph_tensor& tensor, void* tensorData, void* bufferData,
	bool toDevice);


#endif	// XDNA_BUNDLE_H
