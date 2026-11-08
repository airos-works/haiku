// cputype: CPUID 0x80000026 (AMD core type) and 0x1A (Intel hybrid) of each
// CPU, pinned to it (get_cpuid() answers for the CPU it runs on)
#include <OS.h>
#include <stdio.h>
#include <string.h>
extern status_t _kern_set_thread_affinity(thread_id, const void*, size_t);
extern int _kern_get_cpu(void);
int main(void)
{
	system_info si; get_system_info(&si);
	for (uint32 c = 0; c < si.cpu_count; c++) {
		uint32 mask[8]; memset(mask, 0, sizeof(mask)); mask[c / 32] = 1u << (c % 32);
		_kern_set_thread_affinity(find_thread(NULL), mask, sizeof(mask)); snooze(3000);
		cpuid_info a, b, h;
		get_cpuid(&a, 0x80000000, c);
		uint32 maxExt = a.regs.eax;
		printf("cpu %2u (on %d):", c, _kern_get_cpu());
		if (maxExt >= 0x80000026) {
			get_cpuid(&b, 0x80000026, c);
			printf(" 80000026 eax=%08x ebx=%08x ecx=%08x edx=%08x hetero=%u type=%u",
				b.regs.eax, b.regs.ebx, b.regs.ecx, b.regs.edx,
				(b.regs.eax >> 30) & 1, b.regs.ebx >> 28);
		}
		get_cpuid(&h, 0, c);
		if (h.regs.eax >= 0x1a) { get_cpuid(&h, 0x1a, c); printf(" 1a eax=%08x", h.regs.eax); }
		printf("\n");
	}
	return 0;
}
