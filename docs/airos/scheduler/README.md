# Scheduler and CPU clock measurements

How air/OS places CPU-bound work and how fast the clocks follow it, measured on
three machines (November 2026):

| Machine | CPU | Cores | Clock control |
| --- | --- | --- | --- |
| X399 workstation | Threadripper 1950X (Zen 1) | 16 cores, 32 threads | none (no CPPC; P0 always), ACPI C1 + CC6 |
| Framework 13 | Ryzen AI 5 340 (Krackan) | 3 Zen 5 + 3 Zen 5c, 12 threads | amd_pstates (CPPC, hardware chooses) |
| ROCK 5 ITX | RK3588 | 4 Cortex-A76 + 4 Cortex-A55 | none (firmware clocks: about 2.2 and 1.8 GHz) |

## Tools

- `schedperf` (`schedperf.c`): the work is either a chain of dependent
  additions (one per clock cycle on all these cores, so additions per
  microsecond read as the clock the thread really got) or, with
  `SCHEDPERF_WORK=mixed`, typical integer code (independent arithmetic, loads
  from a 256 KiB table, a data-dependent branch), where big and little cores
  differ by much more than their clocks.
  - `cpus`: each CPU's speed, pinned to it
  - `ramp [cpu] [runs]`: speed in 1 ms steps after 1.5 s idle (t90 = ms to 90 %)
  - `place N [s]`: N busy threads: total and per-thread speed, the CPUs they
    used, moves between CPUs and between cores
  - `burst P W F [bg] [prio]`: a job of W us (at top speed) every P ms, alone
    or next to `bg` busy threads: duration percentiles, share of the job run on
    slow cores, moves during jobs
  - `wake [n] [bg]`: latency from `release_sem()` to the blocked thread running
  - `topo`, `mode lowlat|powersave`
- `suite.sh [label]`: all of it in both scheduler modes, `R` result lines.
- `energy.sh`: socket power (amdgpu sensor) for idle, one busy thread, a light
  periodic load and six busy threads.
- `kswap.sh <kernel> <rev> [path=file ...]`: on the machine, repacks the
  active haiku package with a test kernel (and add-ons) under a new revision
  and installs it with pkgman; the previous state stays in the boot menu.
- `cputype.c`: CPUID core type of each CPU (pinned; `get_cpuid()` answers for
  the CPU it runs on).
- `build.sh`: cross-builds them against a configured build dir.

## What was wrong, what changed

| Problem | Machine | Fix |
| --- | --- | --- |
| Every SMT thread was its own core (Zen topology read from the APIC ID width) | X399, laptop | CPUID 0x8000001E threads per core |
| Zen 5c cores looked like Zen 5 | laptop | CPUID 0x80000026 core type -> capacity 768/1024 |
| Two busy threads on the halves of one core, other cores idle | X399, laptop | quantum end on a shared core moves to an idle core |
| Interactive thread got an idle little core while busy threads held the big ones | ROCK 5 | preempt a lower priority thread on a core at least 1.5x faster, or use the idle half of a faster core |
| Busy threads bounced big -> little at every wake-up of another thread | ROCK 5 | leave a faster core only if the rest of its load keeps it busy |
| Power saving: N busy threads stayed packed on one processor | all | requested load; overloaded cores spread |
| Power saving froze arm64 (`pack_irqs()` endless loop, interrupts cannot move) | ROCK 5 (stock kernel too) | stop at an interrupt that stays put |
| Power saving capped AMD at the guaranteed clock (2.0 of 4.8 GHz) | laptop | boost allowed, EPP balance_power |
| Low latency kept idle Zen 1 cores in C1 (CC6 declared 400 us): no boost headroom | X399 | deep states after long idle periods (10x exit latency) |

## Results (mixed work, units per second; higher is better)

X399, low latency:

| | stock | now |
| --- | --- | --- |
| 1 thread | 8 030 | 8 940 |
| 16 threads | 107 000 - 124 000 | 138 900 |
| 32 threads | 183 900 | 183 300 |
| 3 ms job every 16.7 ms, idle machine | 2.89 ms (add chain) | 3.25 ms mixed, was 3.67 |
| wake-up latency p50 / p99 | 8 / 10 us | 8 / 10 us |

Framework 13 (Ryzen AI 5 340):

| | stock | now |
| --- | --- | --- |
| low latency, 3 threads | 40 200 | 44 900 (all on Zen 5) |
| low latency, 6 threads | 55 800 | 76 600 |
| low latency, 12 threads | 100 700 | 100 700 - 101 500 |
| low latency, 3 ms job next to 6 busy threads | 7.65 ms | 5.8 ms |
| power saving, 1 thread | 7 570 at 3.75 W | 11 070 at 4.72 W |
| power saving, 6 threads | 7 420 (one CPU) | 66 400 |
| clock ramp after idle (t90) | 2-6 ms | 2-3 ms (CPPC, hardware) |

ROCK 5 ITX, low latency:

| | stock | now |
| --- | --- | --- |
| 4 threads | 22 640 | 22 730 |
| 5 threads | 24 560 | 24 600 |
| 3 ms job next to 4 busy threads, p50 | 14.45 ms (on an A55) | 2.81 ms |
| same at B_DISPLAY_PRIORITY | 14.46 ms | 2.80 ms |
| power saving mode | froze the machine | works: 4 threads 11 400 - 15 100 (packed on A55s), 8 threads 30 200 |

## Open

- The X399 never lowers its clock: Zen 1 has no CPPC, and there is no driver
  for its P-states (MSR C001_0062, three states on the 1950X). Idle cores do
  reach CC6.
- The ROCK 5 has no DVFS at all: the A76 and A55 clusters stay at the
  firmware's clocks. Lowering them needs SCMI clock calls to the trusted
  firmware and the RK8602/RK806 regulators.
- Power saving on big.LITTLE packs onto the slow cores and never moves a busy
  thread up: a 3 ms job takes 14.5 ms there.
- arm64 cannot route interrupts to other CPUs (`arch_int_assign_to_cpu()`).
- 2026-11-08 ~09:00 UTC the Framework 13 dropped off the network during
  `suite.sh` on the test kernel (haiku package -5): a hang or a closed lid,
  not known. Look at the syslog of that boot before trusting the branch there.
