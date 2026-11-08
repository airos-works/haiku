# Scheduler and CPU clock measurements

How air/OS places CPU-bound work and how fast the clocks follow it, measured on
three machines (November 2026):

| Machine | CPU | Cores | Clock control |
| --- | --- | --- | --- |
| X399 workstation | Threadripper 1950X (Zen 1) | 16 cores, 32 threads | amd_pstates Zen 1 P-states (measured: kept at P0), ACPI C1 + CC6 |
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
- `schedperf power <s> [threads]`, `energy`, `pstates on|off`, `cpuflip a b s`:
  AMD package energy (amd_pstates' `amd_energy` syscall), Zen 1 P-state
  scaling switch, and disabling/enabling CPUs as s2idle does.
- `pstate-ab.sh`: Zen 1 P-state scaling off vs on in one boot.
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
| Power saving on big.LITTLE packed busy threads onto little cores for good | ROCK 5 | slowest cores for small tasks, misfit threads move up, hysteresis down |
| A thread pinned to a CPU being disabled found no core (assert) | laptop s2idle | clear the CPU's enabled bit before removing it |
| Zen 1 had no P-state driver | X399 | amd_pstates drives P0-P2; measured not worth it, stays at P0 |

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
| power saving mode | froze the machine | works |
| power saving, 4 threads | (froze) | 22 750 |
| power saving, 3 ms job alone / next to 4 / next to 8 busy threads, p50 | (froze) | 2.8 / 2.8 / 2.8 ms |

X399, Zen 1 P-state scaling (package energy counter), low latency:

| | P0 always | scaling P0-P2 |
| --- | --- | --- |
| idle | 32.3 W | 31.9 W |
| 1 ms job every 16.7 ms | 1.14 ms, 33.1 W | 1.81 ms, 31.8 W |
| 4 ms job every 16.7 ms | 4.42 ms, 35.3 W | 6.70 ms, 34.2 W |
| 90 % speed after idle | 1 ms | 10-13 ms |
| power saving mode, 4 ms job | 4.22 ms, 31.1 W | 6.25 ms, 31.7 W |

So Zen 1 stays at P0 (idle cores sleep in CC6; Precision Boost above P0).

## Open

- The ROCK 5 has no DVFS at all: the A76 and A55 clusters stay at the
  firmware's clocks. Lowering them needs SCMI clock calls to the trusted
  firmware and the RK8602/RK806 regulators.
- arm64 cannot route interrupts to other CPUs (`arch_int_assign_to_cpu()`).
- 2026-11-08 ~09:00 UTC the Framework 13 panicked ("core != NULL" in
  power_saving.cpp choose_core) going to sleep while schedperf had a thread
  pinned to a CPU: fixed (enabled bit cleared before the CPU's core goes),
  verified with `cpuflip` on the X399 and on the laptop (sleep-like cycles
  in both modes, then the whole suite).
- power_daemon crashed at boots with the lid closed on an external display
  (stock kernel too): BApplication reconnected a never-connected BServer
  while its event loop connected it; fixed, two clean boots.
- Power saving on the laptop: EPP 0x80 (balance_performance) drew the same
  power as 0xbf and ramps faster; a thread starts on a Zen 5c core and moves
  to a Zen 5 once its load shows it does not fit (90 % speed after 0.3-0.4 s,
  50 % after 1-2 ms).
- Disabling and enabling CPUs hundreds of times a second in low latency mode
  hangs the stock kernel too (X399); a sleep-like cycle is fine.
