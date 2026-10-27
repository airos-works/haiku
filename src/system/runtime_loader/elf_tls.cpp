/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include "elf_tls.h"

#include <stdlib.h>
#include <string.h>

#include <locks.h>

#include <support/TLS.h>

#include <tls.h>

#include <util/kernel_cpp.h>


class TLSBlock {
public:
	inline				TLSBlock();
	inline				TLSBlock(void* pointer);

	inline	status_t	Initialize(unsigned dso);

			void		Destroy();

			bool		IsInvalid() const	{ return fPointer == NULL; }

			void*		operator+(addr_t offset) const
							{ return (void*)((addr_t)fPointer + TLS_DTV_OFFSET + offset); }

private:
			void*		fPointer;
};

class Generation {
public:
	inline				Generation();

			unsigned	Counter() const	{ return fCounter; }
			unsigned	Size() const	{ return fSize; }

			void		SetCounter(unsigned counter)	{ fCounter = counter; }
			void		SetSize(unsigned size)	{ fSize = size; }

private:
			unsigned	fCounter;
			unsigned	fSize;
};

class DynamicThreadVector {
public:
	inline				DynamicThreadVector();

			void		DestroyAll();

	inline	TLSBlock&	operator[](unsigned dso);

private:
			bool		_DoesExist() const	{ return *fVector != NULL; }
			unsigned	_Size() const
							{ return _DoesExist()
									? fGeneration->Size() : 0; }

			unsigned	_Generation() const;

			status_t	_ResizeVector(unsigned minimumSize);

			TLSBlock**	fVector;
			Generation*	fGeneration;
			TLSBlock	fNullBlock;
};


TLSBlockTemplates*	TLSBlockTemplates::fInstance;

static const char* const kTemplateLockName = "runtime loader TLS templates";
static recursive_lock sTemplateLock
	= RECURSIVE_LOCK_INITIALIZER(kTemplateLockName);


void
TLSBlockTemplate::SetBaseAddress(addr_t baseAddress)
{
	fAddress = (void*)((addr_t)fAddress + baseAddress);
}


TLSBlock
TLSBlockTemplate::CreateBlock()
{
	void* pointer = malloc(fMemorySize + TLS_DTV_OFFSET);
	if (pointer == NULL)
		return TLSBlock();
	memset(pointer, 0, TLS_DTV_OFFSET);
	memcpy((char*)pointer + TLS_DTV_OFFSET, fAddress, fFileSize);
	if (fMemorySize > fFileSize)
		memset((char*)pointer + TLS_DTV_OFFSET + fFileSize, 0, fMemorySize - fFileSize);
	return TLSBlock(pointer);
}


TLSBlockTemplates&
TLSBlockTemplates::Get()
{
	if (fInstance == NULL)
		fInstance = new TLSBlockTemplates;
	return *fInstance;
}


unsigned
TLSBlockTemplates::Register(const TLSBlockTemplate& block)
{
	RecursiveLocker _(sTemplateLock);
	unsigned dso;

	if (!fFreeDSOs.empty()) {
		dso = fFreeDSOs.back();
		fFreeDSOs.pop_back();
		fTemplates[dso] = block;
	} else {
		dso = fTemplates.size();
		fTemplates.push_back(block);
	}

	fTemplates[dso].SetGeneration(fGeneration);
	return dso;
}


void
TLSBlockTemplates::Unregister(unsigned dso)
{
	if (dso == unsigned(-1))
		return;

	RecursiveLocker _(sTemplateLock);
	atomic_add(&fGeneration, 1);
	fFreeDSOs.push_back(dso);
}


void
TLSBlockTemplates::SetBaseAddress(unsigned dso, addr_t baseAddress)
{
	RecursiveLocker _(sTemplateLock);
	if (dso != unsigned(-1))
		fTemplates[dso].SetBaseAddress(baseAddress);
}


unsigned
TLSBlockTemplates::GetGeneration(unsigned dso) const
{
	if (dso == unsigned(-1))
		return atomic_get(&fGeneration);
	RecursiveLocker _(sTemplateLock);
	return fTemplates[dso].Generation();
}


TLSBlock
TLSBlockTemplates::CreateBlock(unsigned dso)
{
	// Register() can grow and free the template vector while another thread
	// first accesses TLS. Keep the template alive through allocation and copy;
	// established per-thread blocks do not take this lock.
	RecursiveLocker _(sTemplateLock);
	return fTemplates[dso].CreateBlock();
}


void
tls_reinit_after_fork()
{
	recursive_lock_init(&sTemplateLock, kTemplateLockName);
}


TLSBlockTemplates::TLSBlockTemplates()
	:
	fGeneration(0)
{
}


TLSBlock::TLSBlock()
	:
	fPointer(NULL)
{
}


TLSBlock::TLSBlock(void* pointer)
	:
	fPointer(pointer)
{
}


status_t
TLSBlock::Initialize(unsigned dso)
{
	fPointer = TLSBlockTemplates::Get().CreateBlock(dso).fPointer;
	return fPointer != NULL ? B_OK : B_NO_MEMORY;
}


void
TLSBlock::Destroy()
{
	free(fPointer);
	fPointer = NULL;
}


Generation::Generation()
	:
	fCounter(TLSBlockTemplates::Get().GetGeneration(-1)),
	fSize(0)
{
	// A new vector contains only blocks from the current generation. Starting
	// at zero would discard a freshly initialized block on the next access
	// if a TLS image had already been unloaded before this thread's first use.
}


DynamicThreadVector::DynamicThreadVector()
	:
	fVector((TLSBlock**)tls_address(TLS_DYNAMIC_THREAD_VECTOR)),
	fGeneration(NULL)
{
	if (*fVector != NULL)
		fGeneration = (Generation*)*(void**)*fVector;
}


void
DynamicThreadVector::DestroyAll()
{
	for (unsigned i = 0; i < _Size(); i++) {
		TLSBlock& block = (*fVector)[i + 1];
		if (!block.IsInvalid())
			block.Destroy();
	}

	free(*fVector);
	*fVector = NULL;

	delete fGeneration;
}


TLSBlock&
DynamicThreadVector::operator[](unsigned dso)
{
	unsigned generation = TLSBlockTemplates::Get().GetGeneration(-1);
	if (_Generation() < generation) {
		// We need to destroy any blocks whose DSO generation has changed
		// to be greater than our own generation.
		for (unsigned dsoIndex = 0; dsoIndex < _Size(); dsoIndex++) {
			TLSBlock& block = (*fVector)[dsoIndex + 1];
			unsigned dsoGeneration
				= TLSBlockTemplates::Get().GetGeneration(dsoIndex);
			if (dsoGeneration > _Generation() && dsoGeneration <= generation)
				block.Destroy();
		}

		fGeneration->SetCounter(generation);
	}

	if (_Size() <= dso) {
		status_t result = _ResizeVector(dso + 1);
		if (result != B_OK)
			return fNullBlock;
	}

	TLSBlock& block = (*fVector)[dso + 1];
	if (block.IsInvalid()) {
		status_t result = block.Initialize(dso);
		if (result != B_OK)
			return fNullBlock;
	}

	return block;
}


unsigned
DynamicThreadVector::_Generation() const
{
	if (fGeneration != NULL)
		return fGeneration->Counter();
	return unsigned(-1);
}


status_t
DynamicThreadVector::_ResizeVector(unsigned minimumSize)
{
	static const unsigned kInitialSize = 4;
	unsigned size = std::max(minimumSize, kInitialSize);
	unsigned oldSize = _Size();
	if (size <= oldSize)
		return B_OK;

	void* newVector = realloc(*fVector, (size + 1) * sizeof(TLSBlock));
	if (newVector == NULL)
		return B_NO_MEMORY;

	*fVector = (TLSBlock*)newVector;
	memset((void*)(*fVector + oldSize + 1), 0, (size - oldSize) * sizeof(TLSBlock));
	if (fGeneration == NULL) {
		fGeneration = new Generation;
		if (fGeneration == NULL)
			return B_NO_MEMORY;

		// A new vector holds no blocks, so none of them can be stale: it is
		// as current as the templates. Starting at 0 made the thread's next
		// access destroy the blocks of every DSO registered after the first
		// unload, including the one just created, and lose what was written
		// to it in between.
		fGeneration->SetCounter(TLSBlockTemplates::Get().GetGeneration(-1));
	}

	*(Generation**)*fVector = fGeneration;
	fGeneration->SetSize(size);

	return B_OK;
}


void*
get_tls_address(unsigned dso, addr_t offset)
{
	DynamicThreadVector dynamicThreadVector;
	TLSBlock& block = dynamicThreadVector[dso];
	if (block.IsInvalid())
		return NULL;
	return block + offset;
}


void
destroy_thread_tls()
{
	DynamicThreadVector().DestroyAll();
}

