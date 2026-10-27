# Boot time on x86

What a boot spends its time on, how to measure it, and what was changed to
make it shorter. Measured on the X399 workstation (Threadripper 1950X, NVMe,
UEFI, three AHCI controllers without a single SATA drive, a 16550 UART on
the board's COM header with nothing attached), October 2026.

## Where the time went

From the power button to a usable desktop, before the changes:

| phase | time | cause |
|---|---|---|
| firmware (POST) | 25 s | the board's own; "Fast Boot" and the memory training settings in the BIOS are the only lever |
| boot loader | 2.2 s | loading kernel, boot modules and the package table from NVMe through UEFI |
| kernel until the boot volume | 28 s | 10 s of serial console output at 115200 baud, 15 s of AHCI port resets, the rest real work |
| launch_daemon to desktop | 2-3 s | registrar, app_server, GPU driver bring-up |

Two kernel-side costs stood out, and both apply to every x86 machine with
the corresponding hardware:

**Serial debug output.** The kernel writes every `dprintf` line to COM1 as
it logs it, waiting for the UART to take each character (87 us at
115200 baud), under the debug output lock with interrupts off. A boot writes
a few hundred KB; on a board with a UART that is 10 to 25 s, spent whether or
not a cable is attached. Boards without a UART are not affected (the status
register reads 0xff and the wait ends at once), which is why laptops never
showed it.

**AHCI port probing.** `AHCIPort::PortReset()` waited one second for the task
file's BSY/DRQ bits to clear before a COMRESET, and half a second for a
device afterwards, on every port, one after the other. An empty port never
clears those bits (the task file shows whatever a floating bus leaves), so
each empty port cost 1.5 s: 15 s for the ten ports of the X399's three
controllers.

## What changed

- **Asynchronous serial output** (`src/system/kernel/debug/debug.cpp`): once
  the kernel has threads, debug lines are queued in a 1 MB ring buffer and a
  kernel thread ("serial debug writer") feeds them to the port, 16 characters
  per FIFO fill. Early boot, the kernel debugger, and panics still write
  synchronously, and entering the debugger first writes out what is queued.
  The kernel setting `serial_debug_sync true` restores the old behaviour for
  serial debugging of hangs that never reach the debugger. The x86 console
  also enables the 16550 FIFO and fills it in one go.
- **AHCI empty ports** (`src/add-ons/kernel/busses/scsi/ahci/ahci_port.cpp`):
  a port without an established link goes straight to the COMRESET, and the
  wait afterwards ends after 100 ms of "no device" (Linux's settling time);
  a device that is seen but not yet talking still gets the full 500 ms.
  Ten empty ports now cost 1 s instead of 15 s.
- **Boot time report**: the loader notes the time stamp counter on entry and
  the kernel reads it before resetting the TSC, so the kernel log carries
  `boot: firmware N ms, boot loader N ms before the kernel`. This is the only
  way to see how much of a slow boot is the firmware's.
- **Syslog buffer**: the `syslog_buffer_size` kernel setting may now be as
  large as 4 MB (was capped at 256 KB), so the start of a boot is no longer
  lost before syslog_daemon runs.

Results on the X399 (power button to ssh): 54-70 s before, 42-44 s after,
of which 26.6 s is firmware and loader. The kernel reaches launch_daemon in
8-9 s instead of 35 s.

## Measuring

1. In `~/config/settings/kernel/drivers/kernel`: `syslog_time_stamps true`
   and `syslog_buffer_size 1048576`. Every kernel line then starts with
   seconds since the kernel started, and nothing is lost before the daemon.
   Kernel lines are written in batches, so only the stamps order them, never
   their position in the file.
2. `launch_roster log` dates every service start and event to the second.
3. The `boot: firmware ...` syslog line splits the time before the kernel.
4. On the X399, `docs/x399-workstation/tools/boot-time.sh <name>` does a
   locked power cycle, times ssh from the outside, places the kernel's clock
   against the power button, and keeps the logs under `evidence/boot-time`.

Beware: the syslog file is appended across boots; the previous boot's tail
sits at the top. The timestamps restarting from zero mark the boot of
interest. A boot cut short by a power cycle leaves
`~/config/settings/kernel/drivers/amdgpu_drm.starting` behind and the next
boot runs on the framebuffer driver with a different timeline.

## Still on the table

- The firmware's 25 s: BIOS settings only.
- The loader's 2.2 s: untouched; a serial-less way to time its phases would be
  the same TSC trick at a few points inside the loader.
- Legacy drivers are loaded when a server first scans their `/dev` directory
  (net_server for `/dev/net`, media_server for `/dev/audio`), which puts the
  Ethernet link negotiation (about 3 s) after launch_daemon rather than in
  parallel with the kernel's device probing.
- A device that keeps failing enumeration (the X399's KVM USB link does)
  costs USB time at boot and noise forever after.
