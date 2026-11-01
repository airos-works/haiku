/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
// Uses the kernel VM/user-copy fixtures in test_mali_client.cpp.
#include "test_mali_page_table.h"

static VmCreate NewVm(void* client, uint64 limit = 0)
{
	VmCreate value{1, 0, limit, 0, 0, 0};
	assert(Call(client, kCreateVm, value) == B_OK);
	assert(value.handle != 0 && value.userLimit == (limit ? limit : kVmUserLimit));
	return value;
}
static VmInfo VmDetails(void* client, uint32 handle = 0)
{
	VmInfo value{}; value.version = 1; value.handle = handle;
	assert(Call(client, kGetVmInfo, value) == B_OK);
	return value;
}
static void DestroyVm(void* client, uint32 handle)
{
	VmHandle value{1, handle, 0};
	assert(Call(client, kDestroyVm, value) == B_OK);
	assert(Call(client, kDestroyVm, value) == B_ENTRY_NOT_FOUND);
}
static status_t Bind(void* client, uint32 handle, const std::vector<VmOperation>& operations,
	uint64 expected = 0, uint64* returned = NULL)
{
	VmBind header{1, handle, uint32(operations.size()), 0, expected, 0};
	std::vector<uint8> request(sizeof(header) + operations.size() * sizeof(VmOperation));
	memcpy(request.data(), &header, sizeof(header));
	if (!operations.empty())
		memcpy(request.data() + sizeof(header), operations.data(), operations.size() * sizeof(VmOperation));
	sCopies = 0;
	status_t status = ControlClient(client, kBindVm, request.data(), request.size());
	memcpy(&header, request.data(), sizeof(header));
	if (returned != NULL) *returned = header.newGeneration;
	return status;
}
static std::map<uint64, uint64> Contents(const ClientVmLease& lease)
{
	const auto* generation = (const ClientGeneration*)lease.state;
	assert(generation != NULL && generation->number == lease.generation);
	assert(generation->tables.pages[0] == lease.rootPhysical);
	return DecodeGpuTables(generation->tables.address, generation->tables.pages,
		generation->tables.bytes / B_PAGE_SIZE);
}
static void CheckVms()
{
	Empty();
	void *a, *b;
	for (unsigned failure = 1; failure <= 2; failure++) {
		sHeapCalls = 0; sFailHeap = failure;
		assert(OpenClient(true, &a) == B_NO_MEMORY);
		sFailHeap = 0; Empty();
	}
	assert(OpenClient(true, &a) == B_OK && OpenClient(true, &b) == B_OK);
	VmCreate create{1, 0, 0, 0, 0, 0};
	for (unsigned error = 0; error < 7; error++) {
		auto bad = create;
		if (error == 0) bad.version++;
		if (error == 1) bad.flags++;
		if (error == 2) bad.handle++;
		if (error == 3) bad.reserved++;
		if (error == 4) bad.reserved2++;
		if (error == 5) bad.userLimit = 1;
		if (error == 6) bad.userLimit = kVmUserLimit + 4096;
		assert(Call(a, kCreateVm, bad) == B_BAD_VALUE);
	}
	for (unsigned failure = 1; failure <= 3; failure++) {
		sHeapCalls = 0; sFailHeap = failure;
		assert(Call(a, kCreateVm, create) == B_NO_MEMORY);
		sFailHeap = 0;
		assert(sAreas.empty() && sVms == 0 && sGenerations == 0 && sTablePages == 0);
	}
	for (unsigned failure = 1; failure <= 6; failure++) {
		sFailAllocation = failure;
		assert(Call(a, kCreateVm, create) != B_OK);
		sFailAllocation = 0;
		assert(sAreas.empty() && sVms == 0 && sGenerations == 0 && sTablePages == 0);
	}
	for (unsigned failure = 1; failure <= 2; failure++) {
		sFailCopy = failure;
		assert(Call(a, kCreateVm, create) == B_BAD_ADDRESS);
		sFailCopy = 0;
		assert(sAreas.empty() && sVms == 0 && sGenerations == 0 && sTablePages == 0);
	}
	auto vm = NewVm(a), otherVm = NewVm(b, 4096 * 8);
	auto info = VmDetails(a, vm.handle);
	assert(info.generation == 1 && info.mappings == 0 && info.tablePages == 1);
	assert(info.clientVms == 1 && info.globalVms == 2 && info.globalGenerations == 2);
	VmHandle handle{1, vm.handle, 0};
	assert(Call(b, kDestroyVm, handle) == B_ENTRY_NOT_FOUND);
	VmInfo foreign{}; foreign.version = 1; foreign.handle = vm.handle;
	assert(Call(b, kGetVmInfo, foreign) == B_ENTRY_NOT_FOUND);
	auto buffer = Create(a, 65536), replacement = Create(a, 65536);
	auto view = Map(a, buffer.handle);
	memset((void*)(uintptr_t)view.address, 0x71, view.bytes);
	VmOperation map{kVmMapOperation, kVmNoExecute, (UINT64_C(1) << 39) - 8192,
		16384, buffer.handle, 0, 4096};
	assert(Bind(b, otherVm.handle, {map}) == B_BAD_VALUE); // outside the smaller VM
	map.address = 0x1fe000;
	assert(Bind(b, otherVm.handle, {{kVmMapOperation, kVmNoExecute, 0, 4096,
		buffer.handle, 0, 0}}) == B_ENTRY_NOT_FOUND);
	assert(Bind(a, vm.handle, {map}, 123) == B_BUSY);
	uint64 generation;
	assert(Bind(a, vm.handle, {map}, 1, &generation) == B_OK && generation == 2);
	ClientVmLease original{};
	assert(AcquireClientVm(a, vm.handle, 2, original) == B_OK);
	assert(AcquireClientVm(a, vm.handle, 2, original) == B_BAD_VALUE);
	auto before = Contents(original);
	assert(before.size() == 4);
	assert(ClientVmRoot(original) != NULL);
	assert(ClientVmRange(original, map.address, map.bytes));
	assert(ClientVmRange(original, map.address + 3, map.bytes - 3));
	assert(!ClientVmRange(original, map.address - 1, 2));
	assert(!ClientVmRange(original, map.address, map.bytes + 1));
	assert(!ClientVmRange(original, map.address, 0));
	assert(!ClientVmRange(original, UINT64_MAX - 7, 16));
	assert((before.at(map.address) & UINT64_C(0xfffffff000))
		== sAreas.at(view.area).ram->physical + 8192);
	ClientVmLease stale{};
	assert(AcquireClientVm(a, vm.handle, 1, stale) == B_BUSY && stale.state == NULL);
	sTeam++;
	assert(AcquireClientVm(a, vm.handle, 0, stale) == B_NOT_ALLOWED);
	assert(Bind(a, vm.handle, {map}) == B_NOT_ALLOWED);
	sTeam--;
	VmOperation change{kVmMapOperation, kVmReadOnly, map.address + 4096,
		8192, replacement.handle, 0, 8192};
	// All transient allocations, backing allocation phases and copy directions
	// fail individually while a previous root is retained and remains readable.
	for (unsigned failure = 1; failure <= 6; failure++) {
		sHeapCalls = 0; sFailHeap = failure;
		assert(Bind(a, vm.handle, {change}) == B_NO_MEMORY);
		sFailHeap = 0;
		assert(VmDetails(a, vm.handle).generation == 2 && Contents(original) == before);
		assert(sGenerations == 2 && sTablePages == 6 && sBuffers == 2 && sAreas.size() == 5);
	}
	for (unsigned failure = 1; failure <= 6; failure++) {
		sFailAllocation = failure;
		assert(Bind(a, vm.handle, {change}) != B_OK);
		sFailAllocation = 0;
		assert(VmDetails(a, vm.handle).generation == 2 && Contents(original) == before);
		assert(sGenerations == 2 && sTablePages == 6 && sBuffers == 2 && sAreas.size() == 5);
	}
	for (unsigned failure = 1; failure <= 3; failure++) {
		sFailCopy = failure;
		assert(Bind(a, vm.handle, {change}) == B_BAD_ADDRESS);
		sFailCopy = 0;
		assert(VmDetails(a, vm.handle).generation == 2 && Contents(original) == before);
		assert(sGenerations == 2 && sTablePages == 6 && sBuffers == 2 && sAreas.size() == 5);
	}
	for (unsigned error = 0; error < 12; error++) {
		auto bad = change;
		if (error == 0) bad.type = 2;
		if (error == 1) bad.flags = 0;
		if (error == 2) bad.flags = 4;
		if (error == 3) bad.flags = 9;
		if (error == 4) bad.address++;
		if (error == 5) bad.bytes = 0;
		if (error == 6) bad.bytes++;
		if (error == 7) bad.offset++;
		if (error == 8) bad.offset = replacement.bytes;
		if (error == 9) bad.reserved++;
		if (error == 10) bad.address = UINT64_MAX - 4095;
		if (error == 11) bad.type = kVmUnmapOperation;
		assert(Bind(a, vm.handle, {change, bad}) == B_BAD_VALUE);
		assert(VmDetails(a, vm.handle).generation == 2 && Contents(original) == before);
	}
	assert(Bind(a, vm.handle, {}) == B_BAD_VALUE);
	assert(Bind(a, vm.handle, std::vector<VmOperation>(65, change)) == B_BAD_VALUE);
	assert(Bind(a, vm.handle, {change}, 2, &generation) == B_OK && generation == 3);
	assert(VmDetails(a, vm.handle).mappings == 3 && Contents(original) == before);
	assert(sGenerations == 3 && sTablePages == 11);
	Destroy(a, buffer.handle); Destroy(a, replacement.handle);
	assert(sBuffers == 2 && ((Client*)a)->account->buffers == 2);
	assert(Bind(a, vm.handle, {map}) == B_ENTRY_NOT_FOUND);
	VmOperation unmap{kVmUnmapOperation, 0, map.address + 8192, 8192, 0, 0, 0};
	assert(Bind(a, vm.handle, {unmap}) == B_OK);
	ClientVmLease last{};
	assert(AcquireClientVm(a, vm.handle, 0, last) == B_OK);
	assert(Contents(last).size() == 2 && Contents(original) == before);
	assert(ClientVmRange(last, map.address + 1, 8191)); // two buffers/permissions
	assert(!ClientVmRange(last, map.address + 4096, 8192)); // the second page is a hole
	DestroyVm(a, vm.handle); DestroyVm(b, otherVm.handle);
	CloseClient(a); CloseClient(a); FreeClient(a); FreeClient(b);
	assert(sClients == 0 && sVms == 0 && sAccounts == 1 && sBuffers == 2 && sGenerations == 2);
	assert(Contents(original) == before && Contents(last).size() == 2);
	ReleaseClientVm(last);
	assert(last.state == NULL && last.rootPhysical == 0 && sBuffers == 1 && sGenerations == 1);
	ReleaseClientVm(original); ReleaseClientVm(original);
	assert(ClientVmRoot(original) == NULL && !ClientVmRange(original, 0, 1));
	assert(sAccounts == 0 && sBuffers == 0 && sGenerations == 0 && sAreas.size() == 1);
	for (size_t i = 0; i < view.bytes; i++) assert(((uint8*)(uintptr_t)view.address)[i] == 0x71);
	delete_area(view.area); Empty();
	// A page-level reference model checks 400 ordered batches, with old roots
	// held across replacements. It does not use the driver's interval algorithm.
	assert(OpenClient(true, &a) == B_OK);
	vm = NewVm(a);
	std::vector<BufferCreate> buffers;
	std::map<uint32, std::vector<uint64>> physical;
	for (unsigned i = 0; i < 8; i++) {
		buffers.push_back(Create(a, 65536));
		ClientBuffer* item = *FindBuffer((Client*)a, buffers.back().handle);
		physical[item->handle] = std::vector<uint64>(item->pages, item->pages + 16);
	}
	std::map<uint64, uint64> oracle;
	uint64 random = 0x1f7a87ef;
	for (unsigned round = 0; round < 400; round++) {
		ClientVmLease previous{};
		assert(AcquireClientVm(a, vm.handle, 0, previous) == B_OK);
		auto saved = Contents(previous);
		std::vector<VmOperation> operations;
		for (unsigned i = 0; i < 4; i++) {
			random = random * UINT64_C(6364136223846793005) + 1;
			uint64 start = ((random >> 12) % 120) * 4096 + (UINT64_C(1) << 39) - 64 * 4096;
			uint64 bytes = ((random >> 20) % 8 + 1) * 4096;
			VmOperation op{kVmUnmapOperation, 0, start, bytes, 0, 0, 0};
			if ((random >> 30) % 3 != 0) {
				op.type = kVmMapOperation;
				op.flags = (random & 1) ? kVmReadOnly : kVmNoExecute;
				op.flags |= random & kVmUncached;
				op.buffer = buffers[(random >> 24) % 8].handle;
				op.offset = ((random >> 32) % 8) * 4096;
			}
			operations.push_back(op);
			for (uint64 offset = 0; offset < bytes; offset += 4096) {
				if (op.type == kVmUnmapOperation) oracle.erase(start + offset);
				else {
					uint64 attributes = 0x443;
					if (op.flags & kVmReadOnly) attributes |= 0x80;
					if (op.flags & kVmNoExecute) attributes |= UINT64_C(3) << 53;
					attributes |= (op.flags & kVmUncached) ? 0x200 : 0x304;
					oracle[start + offset] = physical.at(op.buffer).at((op.offset + offset) / 4096) | attributes;
				}
			}
		}
		assert(Bind(a, vm.handle, operations) == B_OK);
		ClientVmLease current{};
		assert(AcquireClientVm(a, vm.handle, 0, current) == B_OK);
		assert(Contents(current) == oracle && Contents(previous) == saved);
		assert(VmDetails(a, vm.handle).mappedBytes == oracle.size() * 4096);
		ReleaseClientVm(current); ReleaseClientVm(previous);
		assert(sGenerations == 1 && sVms == 1);
	}
	FreeClient(a); Empty();
	// Resident BO quota still applies after every handle has been closed.
	assert(OpenClient(true, &a) == B_OK);
	vm = NewVm(a);
	for (unsigned i = 0; i < kMaxClientBuffers; i++) {
		auto item = Create(a, 4096);
		assert(Bind(a, vm.handle, {{kVmMapOperation, kVmNoExecute, i * 8192,
			4096, item.handle, 0, 0}}) == B_OK);
		Destroy(a, item.handle);
	}
	BufferCreate tooMany{1, 0, 4096, 0, 0, 0};
	assert(Call(a, kCreateBuffer, tooMany) == B_NO_MEMORY);
	assert(Bind(a, vm.handle, {{kVmUnmapOperation, 0, 0, kVmUserLimit, 0, 0, 0}}) == B_OK);
	assert(VmDetails(a, vm.handle).mappings == 0 && sBuffers == 0);
	Create(a, 4096); FreeClient(a); Empty();
	std::vector<void*> owners;
	for (unsigned i = 0; i < 4; i++) {
		assert(OpenClient(true, &a) == B_OK); owners.push_back(a);
		for (unsigned n = 0; n < kMaxClientVms; n++) NewVm(a);
		assert(Call(a, kCreateVm, create) == B_NO_MEMORY);
	}
	assert(OpenClient(true, &a) == B_OK);
	assert(Call(a, kCreateVm, create) == B_NO_MEMORY);
	FreeClient(a);
	for (void* owner : owners) FreeClient(owner);
	Empty();
	// Retired empty roots count against the generation budget, even after their
	// VM is updated. Releasing the leases restores capacity without a reboot.
	assert(OpenClient(true, &a) == B_OK);
	vm = NewVm(a);
	std::vector<ClientVmLease> retired;
	unmap = {kVmUnmapOperation, 0, 0, 4096, 0, 0, 0};
	for (unsigned i = 1; i < kMaxGlobalGenerations; i++) {
		ClientVmLease lease{}; assert(AcquireClientVm(a, vm.handle, 0, lease) == B_OK);
		retired.push_back(lease);
		assert(Bind(a, vm.handle, {unmap}) == B_OK);
	}
	assert(sGenerations == kMaxGlobalGenerations && sTablePages == kMaxGlobalGenerations);
	assert(Bind(a, vm.handle, {unmap}) == B_NO_MEMORY);
	for (auto& lease : retired) ReleaseClientVm(lease);
	assert(Bind(a, vm.handle, {unmap}) == B_OK);
	FreeClient(a); Empty();
	// Mapping and table-page budgets include retired roots. Rejected updates
	// preserve the current generation and releasing leases restores capacity.
	assert(OpenClient(true, &a) == B_OK);
	vm = NewVm(a); buffer = Create(a, 4096);
	std::vector<VmOperation> sparse;
	for (unsigned i = 0; i < kMaxVmMappings; i++) {
		sparse.push_back({kVmMapOperation, kVmNoExecute, uint64(i) * 8192,
			4096, buffer.handle, 0, 0});
		if (sparse.size() == 64) { assert(Bind(a, vm.handle, sparse) == B_OK); sparse.clear(); }
	}
	assert(VmDetails(a, vm.handle).mappings == kMaxVmMappings);
	assert(Bind(a, vm.handle, {{kVmMapOperation, kVmNoExecute, uint64(kMaxVmMappings) * 8192,
		4096, buffer.handle, 0, 0}}) == B_NO_MEMORY);
	assert(VmDetails(a, vm.handle).mappings == kMaxVmMappings);
	assert(Bind(a, vm.handle, {{kVmUnmapOperation, 0, 0, kVmUserLimit, 0, 0, 0}}) == B_OK);
	for (unsigned i = 0; i < 64; i++)
		sparse.push_back({kVmMapOperation, kVmNoExecute, uint64(i) << 30,
			4096, buffer.handle, 0, 0});
	assert(Bind(a, vm.handle, sparse) == B_OK && sTablePages == 130);
	std::vector<ClientVmLease> tableLeases;
	// Generations of 130 table pages that fit the global budget.
	const unsigned fit = kMaxGlobalTablePages / 130;
	for (unsigned i = 0; i < fit; i++) {
		ClientVmLease lease{}; assert(AcquireClientVm(a, vm.handle, 0, lease) == B_OK);
		tableLeases.push_back(lease);
		status_t result = Bind(a, vm.handle, sparse);
		assert(result == (i < fit - 1 ? B_OK : B_NO_MEMORY));
	}
	assert(sTablePages == fit * 130 && sGenerations == fit);
	for (auto& lease : tableLeases) ReleaseClientVm(lease);
	assert(Bind(a, vm.handle, sparse) == B_OK && sTablePages == 130);
	FreeClient(a); Empty();
	assert(OpenClient(false, &a) == B_OK);
	assert(Call(a, kCreateVm, create) == B_NOT_ALLOWED);
	assert(VmDetails(a).globalVms == 0);
	FreeClient(a); Empty();
	puts("MALI_CSF_VM_TEST_PASS batches=400 retained_generations=511 failure_paths=23");
}
