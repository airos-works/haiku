/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static const uint64 kOutput = 0x300000000ULL, kRaw = 0x200000000ULL;
static const uint32 kSentinel = 0x6b6b6b6b;
using Words = std::vector<uint32>;
static void Require(bool okay, const char* why)
{
	if (!okay) { fprintf(stderr,"FAIL: %s: %s\n",why,strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r={}; r.version=AMDGPU_HAIKU_ABI_VERSION; r.size=sizeof(r); return r;
}
static amdgpu_irq_info IRQ(int fd)
{
	auto r=Request<amdgpu_irq_info>();
	Require(ioctl(fd,AMDGPU_IRQ_INFO,&r,sizeof(r))==0 && r.enabled && r.msi
		&& r.status==B_OK && !r.vm_faults && !r.privileged_faults && !r.unknown
		&& !r.overflows && r.rptr==r.wptr,"healthy independent IRQ counters"); return r;
}
static amdgpu_buffer Buffer(int fd,uint64 address,uint32 bytes,uint32 permissions)
{
	auto b=Request<amdgpu_buffer>(); b.bytes=bytes;
	Require(ioctl(fd,AMDGPU_CREATE_SYSTEM_BUFFER,&b,sizeof(b))==0
		&& ioctl(fd,AMDGPU_MAP_BUFFER,&b,sizeof(b))==0,"allocate CPU-visible owned buffer");
	auto m=Request<amdgpu_vm_mapping>();m.handle=b.handle;m.address=address;
	m.bytes=bytes;m.permissions=permissions;
	Require(ioctl(fd,AMDGPU_VM_MAP,&m,sizeof(m))==0,"map client GPU buffer");return b;
}
static Words Packet(uint32 op,std::initializer_list<uint32> payload)
{
	Words w; w.reserve(payload.size()+1);
	w.push_back(0xc0000000 | uint32(payload.size()-1)<<16 | op<<8);
	for (uint32 word : payload) w.push_back(word);
	return w;
}
static void Submit(int fd,Words w,bool accepted)
{
	w.resize(256,0x80000000);
	auto r=Request<amdgpu_gfx_submit>();r.address=(addr_t)w.data();r.dwords=w.size();
	r.flags=AMDGPU_GFX_VALIDATE_COMMANDS;
	int status=ioctl(fd,AMDGPU_GFX_SUBMIT_COPY,&r,sizeof(r));
	if (!accepted) Require(status==-1 && errno==B_NOT_ALLOWED,"reject before GPU execution");
	else Require(status==0 && r.status==B_OK && r.completion && !r.vm_fault_status[0]
		&& !r.vm_fault_status[1] && r.rptr==r.wptr,"checked command execution retires");
}
int main(int argc,char** argv)
{
	setvbuf(stdout,NULL,_IOLBF,0);disable_debugger(1);
	if (argc==2 && strcmp(argv[1],"--expect-no-device")==0) {
		int fd=open("/dev/" AMDGPU_DEVICE_NAME,O_RDWR);
		Require(fd<0 && errno==ENOENT,"no AMD device in QEMU");
		puts("PASS: checked submission no-device handling");return 0;
	}
	if (argc==2 && strcmp(argv[1],"--unprivileged")==0) {
		Require(setgid(65534)==0 && setuid(65534)==0,"drop credentials");
		int fd=open("/dev/" AMDGPU_DEVICE_NAME,O_RDWR);Require(fd>=0,"open unprivileged observer");
		Submit(fd,Packet(0x12,{0}),false);close(fd);
		puts("PASS: checked general submission retains root gate");return 0;
	}
	Require(argc==1 && geteuid()==0,"checked qualification requires root");
	int monitor=open("/dev/" AMDGPU_DEVICE_NAME,O_RDWR);Require(monitor>=0,"open monitor");
	auto render=Request<amdgpu_render_info>();
	Require(ioctl(monitor,AMDGPU_RENDER_INFO,&render,sizeof(render))==0
		&& (render.capabilities & AMDGPU_RENDER_CHECKED_COPY),"checked copy capability");
	auto initial=IRQ(monitor);
	std::vector<Words> bad={
		{0}, {0x40000000}, {0x80000001}, {0xfffe1200},
		Packet(0x3f,{0x20000,0,0x2000100}), // Nested mutable IB/VMID.
		Packet(0x61,{0x20000,0,0,1}), // Memory-loaded register state.
		Packet(0x69,{0xda,0}), Packet(0x69,{0xd7,0,0,0,0}), // CP_VMID/span.
		Packet(0x76,{0x214,0}), Packet(0x76,{0x20e,0}), // COMPUTE_VMID/TBA.
		Packet(0x76,{0x212,0x100000}), Packet(0x76,{0x213,0x40}), // PRIV/TRAP.
		Packet(0x76,{0xa,0x100000}), Packet(0x76,{0xb,0x40}), // Pixel shader.
		Packet(0x76,{0x4a,0x400000}), // Vertex shader DEBUG_MODE.
		Packet(0x69,{0x100002aa,0,0}), // Indexed register span.
		Packet(0x38,{0,0x213,0x53,0x52,1,0,0,20,0}), // Indirect RSRC2 write.
		Packet(0x28,{0x80000001,0x80000000}), // Global state restore.
		Packet(0x37,{0x100000,0x20000,0,0}), // MMIO write.
		Packet(0x50,{0xe0300000,0x20000,0,0x30000,0,0x48000004}), // MMIO DMA.
		Packet(0x40,{0x100500,0,0,0x20000,0}), // MMIO read.
		Packet(0x47,{0x528,0x20000,0x22000000,1,0}), // IRQ injection.
		Packet(0x37,{0x100500,0x10000,0,0}), // Private command write.
		Packet(0x37,{0x100500,0xfffffffc,15,0,0}) // Address overflow.
	};
	unsigned jobs=0;
	for (unsigned round=0;round<4;round++) {
		int fd=open("/dev/" AMDGPU_DEVICE_NAME,O_RDWR);Require(fd>=0,"open VM client");
		auto vm=Request<amdgpu_vm_info>();Require(ioctl(fd,AMDGPU_VM_INFO,&vm,sizeof(vm))==0,"create VM");
		auto output=Buffer(fd,kOutput,131072,AMDGPU_VM_READ|AMDGPU_VM_WRITE);
		auto raw=Buffer(fd,kRaw,4096,AMDGPU_VM_READ|AMDGPU_VM_EXECUTE);
		auto pixels=(volatile uint32*)(addr_t)output.address;
		// This root-only raw IB reads the rejected private copy without replacing
		// it. Read all 64 KiB, not just packet headers or a few page samples.
		Words read=Packet(0x50,{0x80000000,AMDGPU_COPY_IB_ADDRESS,0,
			uint32(kOutput+4096),uint32(kOutput>>32),0x40010000});
		read.resize(256,0x80000000);memcpy((void*)(addr_t)raw.address,read.data(),1024);
		for (unsigned i=0;i<bad.size();i++) {
			for (unsigned j=0;j<32768;j++) pixels[j]=kSentinel;
			__sync_synchronize(); auto before=IRQ(monitor);
			Submit(fd,bad[i],false);auto after=IRQ(monitor);
			Require(before.completed_fences==after.completed_fences
				&& before.eop_events==after.eop_events,"rejection submits no GPU work");
			for (unsigned j=0;j<32768;j++) Require(pixels[j]==kSentinel,"rejected stream leaves all output unchanged");
			auto r=Request<amdgpu_gfx_submit>();r.address=kRaw;r.dwords=256;
			Require(ioctl(fd,AMDGPU_GFX_SUBMIT,&r,sizeof(r))==0 && r.status==B_OK
				&& r.completion && !r.vm_fault_status[0] && !r.vm_fault_status[1],"read rejected private copy");jobs++;
			for (unsigned j=0;j<32768;j++) Require(pixels[j]==(j>=1024 && j<17408?0:kSentinel),
				"entire rejected private copy is zero and neighboring guards unchanged");
			uint32 value=0x51000000 | round<<8 | i;
			Submit(fd,Packet(0x37,{0x100500,uint32(kOutput),uint32(kOutput>>32),value}),true);jobs++;
			for (unsigned j=0;j<32768;j++) Require(pixels[j]==(j==0?value:j>=1024 && j<17408?0:kSentinel),
				"checked memory write after rejection, exact output and all guards");
		}
		close(fd);Require(delete_area(output.area)==B_OK && delete_area(raw.area)==B_OK,"revoke buffer clones");
		printf("PASS: VM %u, %zu rejected streams, zeroed private copies and successful checked writes\n",round,bad.size());
	}
	auto final=IRQ(monitor);Require(final.completed_fences==initial.completed_fences+jobs
		&& final.eop_events==initial.eop_events+2*jobs,"exact checked/recovery job IRQ counts");
	printf("PASS: %zu rejected requests, %u GPU jobs, completions %llu -> %llu, no faults\n",
		bad.size()*4,jobs,(unsigned long long)initial.completed_fences,(unsigned long long)final.completed_fences);
	close(monitor);
}
