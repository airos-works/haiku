/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TENSOR_H
#define _TENSOR_H


#include <Archivable.h>
#include <neural/NeuralDefs.h>
#include <OS.h>
#include <String.h>


/*!	What a tensor looks like, without its numbers: element type, shape and,
	for quantized tensors, how the stored integers map to real values:

		real = scale * (stored - zero point)

	Models describe their inputs and outputs with these.
*/
class BTensorInfo {
public:
								BTensorInfo();
								BTensorInfo(neural_data_type type,
									int32 rank, const int32* dimensions,
									const char* name = NULL);
								BTensorInfo(const BTensorInfo& other);
								~BTensorInfo();

			BTensorInfo&		operator=(const BTensorInfo& other);
	//! Same type, shape and quantization; the name is not compared.
			bool				operator==(const BTensorInfo& other) const;
			bool				operator!=(const BTensorInfo& other) const;

			status_t			SetTo(neural_data_type type, int32 rank,
									const int32* dimensions,
									const char* name = NULL);
			void				Unset();
			bool				IsValid() const;

			const char*			Name() const;
			void				SetName(const char* name);

			neural_data_type	Type() const;
			int32				Rank() const;
	//! 0 when the index is out of range
			int32				DimensionAt(int32 index) const;
			int64				CountElements() const;
			size_t				ByteSize() const;

	//! true when scale and zero point mean something
			bool				IsQuantized() const;
			float				Scale() const;
			int32				ZeroPoint() const;
			void				SetQuantization(float scale,
									int32 zeroPoint);

	//! "input uint8 [1, 224, 224, 3] scale 0.0078 zero 128"
			BString				ToString() const;

			status_t			Archive(BMessage* into) const;
			status_t			Unarchive(const BMessage* from);

private:
			BString				fName;
			neural_data_type	fType;
			int32				fRank;
			int32				fDimensions[B_NEURAL_MAX_RANK];
			float				fScale;
			int32				fZeroPoint;
			uint32				_reserved[4];
};


/*!	A block of numbers with a shape. A tensor owns its memory unless it was
	given memory to use; the inputs and outputs of a BNeuralSession are
	tensors owned by the session, whose memory may be the device's own.

	Tensors are archivable: one may be put in a BMessage with its numbers
	and sent to another team, or kept in a file.
*/
class BTensor : public BArchivable {
public:
								BTensor();
								BTensor(const BTensorInfo& info);
								BTensor(const BTensorInfo& info, void* data,
									size_t size);
								BTensor(const BTensor& other);
								BTensor(BMessage* archive);
	virtual						~BTensor();

			BTensor&			operator=(const BTensor& other);

	virtual	status_t			Archive(BMessage* into,
									bool deep = true) const;
	static	BArchivable*		Instantiate(BMessage* archive);

			status_t			InitCheck() const;

	/*!	Makes the tensor one of the given description with memory of its
		own, all zero. */
			status_t			SetTo(const BTensorInfo& info);
	/*!	Makes the tensor use the caller's memory, which must stay valid as
		long as the tensor uses it and be at least info.ByteSize() long. */
			status_t			SetTo(const BTensorInfo& info, void* data,
									size_t size);
			void				Unset();

			const BTensorInfo&	Info() const;
			neural_data_type	Type() const;
			int32				Rank() const;
			int32				DimensionAt(int32 index) const;
			int64				CountElements() const;

			void*				Data();
			const void*			Data() const;
			size_t				ByteSize() const;

			status_t			CopyFrom(const void* data, size_t size);
			status_t			CopyTo(void* data, size_t size) const;
	//! Copies the numbers of a tensor of the same type and shape.
			status_t			CopyFrom(const BTensor& other);

	/*!	The elements as real values, whatever the storage: quantized
		tensors are converted with their scale and zero point, and values
		that do not fit are clamped. The index counts elements in storage
		order (the last dimension varies fastest). */
			float				ValueAt(int64 index) const;
			status_t			SetValueAt(int64 index, float value);
			status_t			GetValues(float* values, int64 count,
									int64 first = 0) const;
			status_t			SetValues(const float* values, int64 count,
									int64 first = 0);

	//! Index of the largest element, -1 for an empty tensor.
			int64				IndexOfMaximum() const;

private:
	friend class BNeuralSession;

	virtual	void				_ReservedTensor1();
	virtual	void				_ReservedTensor2();
	virtual	void				_ReservedTensor3();
	virtual	void				_ReservedTensor4();

			BTensorInfo			fInfo;
			void*				fData;
			size_t				fSize;
			bool				fOwnsData;
			status_t			fStatus;
			uint32				_reserved[6];
};


#endif	// _TENSOR_H
