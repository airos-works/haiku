/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "qcom_rpmh.h"

#include <string.h>

#include <KernelExport.h>
#include <arch/atomic.h>

#include <libfdt.h>


extern void* gFDT;


struct cmd_db_rsc_header {
	uint16	slave_id;
	uint16	header_offset;
	uint16	data_offset;
	uint16	count;
	uint16	version;
	uint16	reserved[3];
};

struct cmd_db_entry {
	char	id[8];
	uint32	priority[2];
	uint32	address;
	uint16	length;
	uint16	offset;
};

struct cmd_db_header {
	uint32	version;
	uint8	magic[4];
	cmd_db_rsc_header resources[8];
	uint32	checksum;
	uint32	reserved;
	uint8	data[];
};


static bool
fdt_reg(int node, int index, phys_addr_t* _base, size_t* _size)
{
	int length;
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(gFDT, node, "reg",
		&length);
	if (reg == NULL || length < (index + 1) * 16)
		return false;
	reg += index * 4;
	*_base = (uint64)fdt32_to_cpu(reg[0]) << 32 | fdt32_to_cpu(reg[1]);
	*_size = (uint64)fdt32_to_cpu(reg[2]) << 32 | fdt32_to_cpu(reg[3]);
	return true;
}


status_t
qcom_rpmh_init(qcom_rpmh& rpmh)
{
	rpmh.cmdDbArea = rpmh.rscArea = -1;
	if (gFDT == NULL)
		return B_DEVICE_NOT_FOUND;
	int cmdDb = fdt_node_offset_by_compatible(gFDT, -1, "qcom,cmd-db");
	int rsc = fdt_node_offset_by_compatible(gFDT, -1, "qcom,rpmh-rsc");
	const fdt32_t* drvId = rsc >= 0 ? (const fdt32_t*)fdt_getprop(gFDT, rsc,
		"qcom,drv-id", NULL) : NULL;
	const fdt32_t* tcsOffset = rsc >= 0 ? (const fdt32_t*)fdt_getprop(gFDT,
		rsc, "qcom,tcs-offset", NULL) : NULL;
	phys_addr_t cmdDbBase, rscBase;
	size_t cmdDbSize, rscSize;
	if (cmdDb < 0 || drvId == NULL || tcsOffset == NULL
		|| !fdt_reg(cmdDb, 0, &cmdDbBase, &cmdDbSize)
		|| !fdt_reg(rsc, fdt32_to_cpu(*drvId), &rscBase, &rscSize))
		return B_DEVICE_NOT_FOUND;
	rpmh.tcsOffset = fdt32_to_cpu(*tcsOffset);

	rpmh.cmdDbArea = map_physical_memory("qcom cmd-db", cmdDbBase, cmdDbSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&rpmh.cmdDb);
	rpmh.rscArea = map_physical_memory("qcom apps rsc", rscBase, rscSize,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&rpmh.rsc);
	if (rpmh.cmdDbArea < 0 || rpmh.rscArea < 0) {
		qcom_rpmh_uninit(rpmh);
		return B_NO_MEMORY;
	}
	return B_OK;
}


void
qcom_rpmh_uninit(qcom_rpmh& rpmh)
{
	if (rpmh.cmdDbArea >= 0)
		delete_area(rpmh.cmdDbArea);
	if (rpmh.rscArea >= 0)
		delete_area(rpmh.rscArea);
	rpmh.cmdDbArea = rpmh.rscArea = -1;
}


uint32
qcom_cmd_db_address(const qcom_rpmh& rpmh, const char* id, uint32* _levels)
{
	const cmd_db_header* header = (const cmd_db_header*)rpmh.cmdDb;
	static const uint8 kMagic[] = {0xdb, 0x30, 0x03, 0x0c};
	if (header == NULL || memcmp(header->magic, kMagic, 4) != 0)
		return 0;

	char query[8] = {};
	memcpy(query, id, strnlen(id, 8));
	for (int i = 0; i < 8 && header->resources[i].slave_id != 0; i++) {
		const cmd_db_rsc_header& rsc = header->resources[i];
		const cmd_db_entry* entry
			= (const cmd_db_entry*)(header->data + rsc.header_offset);
		for (int j = 0; j < rsc.count; j++, entry++) {
			if (memcmp(entry->id, query, 8) == 0) {
				if (_levels != NULL)
					*_levels = entry->length / 2;
				return entry->address;
			}
		}
	}
	return 0;
}


static inline uint32
rsc_read(qcom_rpmh& rpmh, uint32 offset)
{
	return *(volatile uint32*)(rpmh.rsc + offset);
}


static inline void
rsc_write(qcom_rpmh& rpmh, uint32 offset, uint32 value)
{
	*(volatile uint32*)(rpmh.rsc + offset) = value;
	memory_full_barrier();
}


status_t
qcom_rpmh_write(qcom_rpmh& rpmh, uint32 tcs, const uint32* addresses,
	const uint32* data, int count)
{
	if (rpmh.rsc == NULL || count < 1 || count > 16)
		return B_BAD_VALUE;
	bool v3 = ((rsc_read(rpmh, 0) >> 16) & 0xff) >= 3;
	// rpmh_rsc_reg_offset_ver_3_0 / _2_7, from the TCS base
	const uint32 tcsSize = 672, cmdSize = v3 ? 24 : 20;
	const uint32 irqEnable = 0x00, irqStatus = 0x04, irqClear = 0x08;
	const uint32 waitForCompletion = v3 ? 0x20 : 0x10;
	const uint32 control = v3 ? 0x24 : 0x14, enable = v3 ? 0x2c : 0x1c;
	const uint32 msgId = v3 ? 0x34 : 0x30, addr = v3 ? 0x38 : 0x34;
	const uint32 dataRegister = v3 ? 0x3c : 0x38;
	const uint32 base = rpmh.tcsOffset;
	const uint32 t = base + tcsSize * tcs;

	// the completion only latches in the status with its interrupt enabled
	// (rpmh_rsc_probe() enables the active TCSes'; the firmware only the
	// first's)
	uint32 enabled = rsc_read(rpmh, base + irqEnable);
	if ((enabled & (1u << tcs)) == 0)
		rsc_write(rpmh, base + irqEnable, enabled | 1u << tcs);

	uint32 mask = 0;
	for (int i = 0; i < count; i++) {
		uint32 c = t + cmdSize * i;
		rsc_write(rpmh, c + msgId, 8 | 1u << 16 | 1u << 8);	// write, response
		rsc_write(rpmh, c + addr, addresses[i]);
		rsc_write(rpmh, c + dataRegister, data[i]);
		mask |= 1u << i;
	}
	rsc_write(rpmh, t + waitForCompletion, mask);
	rsc_write(rpmh, t + enable, mask);
	// __tcs_set_trigger(): AMC mode, then the trigger
	uint32 value = rsc_read(rpmh, t + control);
	value &= ~(1u << 24);
	rsc_write(rpmh, t + control, value);
	value &= ~(1u << 16);
	rsc_write(rpmh, t + control, value);
	value = 1u << 16;
	rsc_write(rpmh, t + control, value);
	value |= 1u << 24;
	rsc_write(rpmh, t + control, value);

	bool done = false;
	for (bigtime_t deadline = system_time() + 100000;
			system_time() < deadline;) {
		if ((rsc_read(rpmh, base + irqStatus) & (1u << tcs)) != 0) {
			done = true;
			break;
		}
		spin(10);
	}
	rsc_write(rpmh, base + irqClear, 1u << tcs);
	rsc_write(rpmh, t + control, 0);
	rsc_write(rpmh, t + enable, 0);
	return done ? B_OK : B_TIMED_OUT;
}


status_t
qcom_rpmh_ldo(qcom_rpmh& rpmh, uint32 tcs, const char* name, bool on,
	uint32 millivolts)
{
	uint32 base = qcom_cmd_db_address(rpmh, name);
	if (base == 0)
		return B_DEVICE_NOT_FOUND;
	// PMIC5 LDO modes: 7 high power, 5 low power
	uint32 addresses[3] = {base + 0, base + 8, base + 4};
	uint32 data[3] = {millivolts, 7, on ? 1u : 0u};
	return on ? qcom_rpmh_write(rpmh, tcs, addresses, data, 3)
		: qcom_rpmh_write(rpmh, tcs, &addresses[2], &data[2], 1);
}
