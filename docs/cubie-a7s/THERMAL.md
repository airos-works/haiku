# Thermal management on the Radxa Cubie A7S (Allwinner A733)

The Cubie A7S has no fan (the BSP's pwm-fan PWM changes nothing on this
board). Before this work air/OS did no thermal management at all: the cores
ran at the boot firmware's 1014 MHz / 800 mV all the time, nothing read the
thermal sensor, and two looping 1080p Summit videos took the SoC from 71 to
100 C in 4.5 minutes, after which the board lost power (wifi-hang evidence,
2026-10-11).

## Design

One board driver, `sunxi_thermal` (src/add-ons/kernel/drivers/power), with the
PMIC access shared through the `sunxi_regulator` driver, and a monitor in the
power daemon.

### Sensors: `/dev/power/sunxi_thermal/<n>`

The A733's thermal sensor controller (THS, 0x02522000) has five sensors. The
driver sets it up as Radxa's Linux does (24 MHz sampling clock, bus gate and
reset at CCU 0xfc0/0xfc4/0xfe4; control 0x01df002f, filter 5, period 0x1c000)
and writes the factory calibration from the eFuse (SID 0x03006244, three
words: the test temperature and each sensor's code at it) into the
controller's calibration registers, which then report calibrated codes. A code
becomes millidegrees on two lines that meet at code 1769:
`(code - 2822) * -62` above it, `(code - 2835) * -59` at or below.

The zones follow the BSP's numbering: 0 `cpub` (Cortex-A76s), 1 `ddr`,
2 `npu`, 3 `cpul` (Cortex-A55s), 4 `gpu`. Each is a device in the text form of
the other thermal drivers (acpi_thermal, rpi_thermal):

    $ cat /dev/power/sunxi_thermal/0
      Zone: cpub
      Critical Temperature: 96.0 C
      Hot Temperature: 90.0 C
      Passive Temperature: 80.0 C
      Current Temperature: 64.8 C

AirTop scans `/dev/power` for "thermal" devices and shows the hottest.
`/dev/power/sunxi_thermal/status` has the whole state: zones, trips, throttle
level, both clusters' frequency (with a measured value: a timed loop of
dependent adds on one of the cluster's cores), voltage, load, caps and
operating points.

### CPU frequency and voltage scaling

Haiku's cpufreq modules (intel_pstates, amd_pstates) are driven from the
scheduler's hooks, which run on every context switch with interrupts off. On
the A733 a change is a PLL reprogramming that ramps for up to about a
millisecond and, for the voltage, an I2C transfer to the PMIC and its ramp:
neither can happen there. A cpufreq module could only record the wish for a
thread to act on, and that thread would need the per-cluster aggregation and
the thermal limits anyway, so the driver does it all: a kernel thread every
20 ms reads each core's active time, takes each cluster's busiest core and
picks an operating point (an ondemand governor: 80 % load or more goes
straight to the top, below that the frequency that would make the load 60 %;
a raise holds for 100 ms before it can come down).

The operating points are the BSP's (Linux 6.6 device tree, the 26 MHz crystal
columns), in `data/dts/cubie-a7s/sun60i-a733-opp-26m.dtsi`, generated from
the BSP's DTB by `tools/cubie-a7s/gen-opp-dts.py`. The chip's speed grade
(eFuse 0x0300624c, bits 15:8, or 23:16 when set; this board: code 5) picks the
voltage column (`opp-microvolt-vf0500`), as the BSP's cpufreq driver does
(unknown codes take its default, vf0100). For this chip:

| little (A55 x6, DCDC5) | big (A76 x2, DCDC3) |
|---|---|
| 416-1404 MHz 800 mV | 416-1508 MHz 800 mV |
| 1508 MHz 820 mV | 1612 MHz 820 mV |
| 1612 MHz 860 mV | 1716 MHz 850 mV |
| 1716 MHz 900 mV | 1794 MHz 900 mV |
| 1794 MHz 950 mV | 1898 MHz 940 mV, 1950 MHz 970 mV, 2002 MHz 1000 mV |

These are exactly the pairs Linux programs (verified on the board's Debian,
evidence/thermal/debian/opp-sweep.txt: each frequency's PLL N and P and the
PMIC's DCDC3/DCDC5 codes). A change raises the voltage before a higher
frequency and lowers it after a lower one; the little cluster's rail also feeds
the DSU, which stays at the boot firmware's 780 MHz and needs 800 mV, so that
rail never goes below the DSU's operating point (or, without one, below the
boot voltage).

The CPU PLLs (0x08870000: little +0x1000, big +0x2000, DSU +0x3000) run
from the 26 MHz crystal: output = 26 MHz x N / P, N in bits 15:8, P in bits
17:16 of the cores' clock register (+0x1c), bit 26 latches a new N. As the
BSP's clock driver does at probe, the driver first switches on the PLLs'
linear frequency modulation (pattern 0 bits 29-30, pattern 1 bit 31, LFM
register 0xc0000100) so that a new N ramps at 1.125 MHz/us while the cores
keep running from the PLL; the boot firmware leaves it off (0x40000001). A
larger post divider is set before N, a smaller one after.

The voltages go through the PMIC module of the `sunxi_regulator` driver
(`headers/private/drivers/sunxi_pmic.h`): the AXP318W (AXP8191 in the BSP) at
0x36 on R_I2C0, DCDC2-5 codes in bits 6:0 of 0x13-0x16 (500 mV + 10 mV steps
to 1200 mV, then 20 mV steps), bit 7 kept; a write is read back, the module
refuses anything outside the device tree regulator's range
(800-1050 mV for both CPU rails, the envelope of the BSP's tables) and waits
for the 250 uV/us slew. The powervr driver's GPU supply check goes through the
same module, so that the two never share the I2C controller at once.

### Throttling, hot and critical

Every 240 ms the driver reads the zones; the hottest one decides.

- Passive, 80 C (release 76 C): the throttle level goes up a step at most
  every 2 s while at or above it and the temperature rises, every 8 s while
  it holds and not while it falls (two every 480 ms from 84 C, three on
  every reading from 88 C: with all eight cores at their top speed the SoC
  warms by 6 C a second). The temperature shows a new level only after
  more than 2 s: stepping every 2 s while it held took a YouTube start from
  level 3 to 6 in 6 s at 81 C (summit-15, 2026-10-15). The level comes down a
  step every 4 s below the release temperature, and every 6 s below 79 C
  while the temperature is not rising ("steady", as Linux's step_wise
  governor does). The trend is the hottest zone averaged over about a
  second (a quarter of each reading) against the same 2.16 s before; a
  quarter of a degree either way is rising or falling. Without the steady
  steps a level stayed for as long as the load did: between 76 and 80 C
  nothing took it back. The levels cap the clusters' top speed,
  the big cores first, and step the GPU's and the video engine's clocks
  down:

  | level | big | little | GPU | video engine |
  |---|---|---|---|---|
  | 0 | 2002 | 1794 | 600 | 600 |
  | 1 | 1794 | 1794 | 600 | 300 |
  | 2 | 1612 | 1716 | 400 | 300 |
  | 3 | 1404 | 1612 | 400 | 200 |
  | 4 | 1196 | 1404 | 300 | 200 |
  | 5 | 1014 | 1196 | 300 | 200 |
  | 6 | 780 | 1014 | 200 | 200 |
  | 7 | 416 | 780 | 200 | 150 |
  | 8 | 416 | 416 | 200 | 150 |

  (MHz.) The cores alone are not enough: with two looping 1080p Summit
  videos and both clusters held at 416 MHz the SoC still went from 79 to
  95.5 C in six minutes (evidence/thermal/m2b); most of that heat is the
  GPU's, the video engine's and the memory's. The GPU's clock (CCU 0xb20)
  moves from pll-peri0-600m to its 400, 300 and 200 MHz taps, as the BSP's
  GPU frequency scaling does (bit 27 latches the source); the video
  engine's (CCU 0xa88, pll-peri0-600m) gets a divider. The driver only
  touches them when their drivers run them as expected (gate on, the
  600 MHz source or one of its own steps), and sets the video engine's
  again on every reading because sunxi_ve rewrites it each time the engine
  powers up. The GPU's firmware is still told 600 MHz; its timing only
  gets longer in real time.
- Hot, 90 C (the BSP's 90 C passive trips of the GPU and the cores' idle
  injection): straight to the last level and an event for the power daemon,
  which shows a notification.
- Critical, 96 C (the BSP says 110 C, but the board lost power at about 100 C
  in the lab): a syslog line, the last level, and an event on which the power
  daemon shows a notification and shuts the system down through the registrar
  (`BRoster::Private::ShutDown()`, as for the power button). If nobody listens
  to the event, or the system is still running 30 s later, or the temperature
  rises 4 C further, the driver calls the kernel's own shutdown (sync, PSCI
  SYSTEM_OFF).

The trips come from the device tree's thermal zones (air/OS additions in the
board's .dts). Two readings in a row must be over hot or critical.

The events: `/dev/power/trip/sunxi_ths`, a byte (1 hot, 2 critical) that a
read blocks for (select() works); the power daemon's `ThermalTripMonitor`
listens to every device in `/dev/power/trip`.

### Settings

The driver settings file `sunxi_thermal` (and, at run time, the same lines
written to `/dev/power/sunxi_thermal/status`):

    dvfs false                  # no frequency changes (settings file only)
    governor performance        # or powersave, ondemand
    throttle false
    steady_release false        # levels come off only below the release
                                # temperature (before 2026-10-15)
    throttle_devices false      # the GPU and the video engine stay at full speed
    throttle_level 3            # at run time only
    passive_temperature 80      # release_, hot_, critical_temperature too
    critical_grace 30           # critical_margin 4: kernel shutdown after
                                # 30 s or 4 C above critical
    max_big_mhz 1794            # max_little_mhz too; 0 is no cap
    log_interval 5              # a "watch" line on the serial port

Lab images set `log_interval 5` (tools/cubie-a7s/lab/sunxi_thermal).
For tests: `dvfs_stress <seconds>` (run time) moves both clusters between
their lowest and top point every 20 ms; `voltage_settle_percent` (default
200: a raised voltage gets twice the PMIC's nominal slew time) and
`wait_pll_ramp` (default true: a PLL change waits for its ramp to end, so
the voltage only comes down once the frequency has) set the margins. The
kernel debugger's `sunxi_thermal` command prints the last 64 frequency and
voltage steps and the PLLs.

### Other boards

The arm64 haiku package is shared with the Raspberry Pi 4 and the ROCK 5.
`sunxi_thermal` only binds to an `allwinner,sun60i-a733-ths` node; the PMIC
module only loads where the device tree has an `x-powers,axp318w` on the
A733's R_I2C0; the power daemon's monitor only exists when `/dev/power/trip`
has devices. The QEMU smoke test's boot (virt machine, the same arm64 haiku
package) shows none of them.

## Measurements (2026-10-11, evidence/thermal)

The lab board in its usual place, no fan, plug readings in whole watts.

### Sensor readings against Debian's

Radxa's Debian (BSP Linux 5.15) leaves the THS at control 0x01df002f,
period 0x0001c000, filter 5 and calibration 0x080e080d 0x08100815 0x00000807
(evidence/thermal/debian/regs-idle.txt); air/OS's driver programs exactly
these (the calibration words computed from the same eFuse, its boot log), so
a sensor's code means the same temperature in both. On the board:

- idle at 416 MHz: Debian 63.7-65.0 C (its console only), air/OS 66.0 C
  (the desktop running, idle-ab/);
- across warm restarts with both idle at 416 MHz (handoff/): Debian
  61.3 C, air/OS's first reading 34 s later 58.3-59.3 C; air/OS 67.4 C,
  Debian's first reading 29 s after the reset 63.5-63.7 C. The SoC cools by
  the same amount either way through a restart unless one side reads
  differently: the two drops (2.6 and 3.8 C) put air/OS about 0.6 C above
  Debian;
- the same load (all eight cores busy at 1014 MHz, 800 mV, thermal
  management off in both, compare/): Debian warmed by 9.3, 6.0 and
  5.6 C/min through 75-80, 80-85 and 85-90 C, air/OS by 8.3, 6.9 and
  7.1 C/min (air/OS also runs its desktop and the GPU's firmware).

AirTop shows the hottest zone (m2c/airtop.jpg: "68.2 C, hottest thermal
sensor /dev/power/sunxi_thermal/3").

### Two looping 1080p videos in Summit (the load that powered the board off)

| run | throttling | result |
|---|---|---|
| wifi-hang run2 (before) | none, cores at 1014 MHz | 71 -> 100 C in 4.5 min, safety stop; earlier the board lost power |
| m2b | cores only | level 8 (416/416 MHz) from minute 2, still 79 -> 95.5 C in 6 min, safety stop |
| m2c | cores, GPU, video engine | 30 min: max 82.1 C, mean 79.2 C, 6 W; level 6-8 (GPU 200, video engine 150 MHz); 68.5 C and level 0 five minutes after |
| m4 (final policy) | cores, GPU, video engine | 30 min: max 79.7 C, mean 78.1 C, 6 W; level 5 for most of it (big 1014, little 1196, GPU 300, video engine 200 MHz); 67.5 C and level 0 five minutes after |

The curves: m2b/load.svg, m2c/load.svg, m4/load.svg (hottest zone,
level, both clusters', the GPU's and the video engine's clocks, plug watts).

### Idle

Five minutes each on one boot (idle-ab/): the driver's way (ondemand, both
clusters at 416 MHz, 800 mV) 66.0 C and 5.0 W; the clocks air/OS ran at
before (both clusters fixed at 1014 MHz, 800 mV) 65.9 C and 4.9 W. The
boot firmware already runs the cores at 800 mV, so idle temperature does not
change; nothing throttles at idle (level 0, GPU 600 MHz).

### Cost of throttling

Each level held with the stepping off (perf2/, critical/; the GPU and video
engine follow the level):

| | level 0 (cool) | level 4 | level 8 (hottest) |
|---|---|---|---|
| cores (big/little MHz) | 2002/1794 | 1196/1404 | 416/416 |
| GPU / video engine MHz | 600/600 | 300/200 | 200/150 |
| cubie_cpubench, 8 threads (Mwords/s) | 1680 (a 2 s burst) | 1196-1203 | 373 |
| one A76 / one A55 thread | 315 / 178 | 189 / 139 | 66 / 41 |
| GLTeapot (FPS) | 175-176 | 135-137 | 58-59 |
| airTime, 1080p30 H.264 on the video engine | video ahead of the audio (below) | 30.0 fps, 0 dropped, decode 16.4 ms | 30.0 fps, 0 dropped, decode 21.2 ms |
| airTime, the same in software (FFmpeg) | 30.0 fps, 26 of 572 dropped | 30.0 fps, 30-34 dropped | 5-7 fps |

airTime on the video engine with the engine and the GPU at 600 MHz shows
all of the 60 s film's frames in about 16 s (fps 100-115, the video 43 s
ahead of the audio, airtime/): at every level whose engine and GPU clocks
are lower it plays at 30 fps. The same happens with the clocks the system
had before this driver (cores fixed at 1014 MHz, GPU and engine at 600,
`throttle_devices false`, airtime/caps.txt), so it is a pacing problem of
airTime or the engine's add-on on this board, not the throttling's.

For comparison, before this work the cores were fixed at 1014 MHz: about
half of level 0's single-thread rate (scaled by clock: A76 160, A55 100,
about 920 for eight threads). With all eight cores busy
at level 0 the SoC goes from 63 to 95 C in about 5 s: unthrottled all-core
speed is a burst; the policy takes it down within a second or two.

### Hot and critical trips (lowered for the test)

- hot at 76 C with the benchmark running (m3/): "HOT: 87.0 C at or above
  76.0 C", level 8, and the power daemon's notification "The system is very
  hot - It runs at its lowest speed until it cools down"
  (m3/hot-notification.jpg);
- critical at 75 C (critical/): "CRITICAL: 76.2 C at or above the critical
  75.0 C: shutting down" at 14:38:11.197, the notification "Shutting down:
  overheating" (critical/critical-2.jpg), the applications closed, "PSCI:
  requesting system off" at 14:38:17.445, the plug at 0 W; the same once
  more at 13:57 (perf/). With the temperature already 4 C past a lowered
  critical trip the driver used the kernel's shutdown at once (m3/: 13:38).
- After a PSCI system off the board stays off; it only starts again after
  about 60 s without power (a 5 s plug cycle is not enough), as after the
  thermal power-off.

### Frequency changes

The first two-video run (m2/) ended in a panic 40 s into the load: both
Cortex-A76s stopped answering (the kernel debugger's `running` showed the
powervr interrupt thread and a Mesa thread on them), the other cores waited
on their locks. The voltage settling then was the PMIC's nominal slew and a
PLL change did not wait for its ramp; both margins were added (twice the
slew, the ramp's end) along with the kernel debugger command for the last
steps. Since then: two 30-minute video runs (m2c, m4), stress runs of
DVFS (dvfs_stress: every 20 ms each cluster between its lowest and top
allowed point) with 1000 full-range and 30000 more changes per cluster
(stress1/, stress2/) and a full-range run of ten minutes at level 0 with
one thread busy (handoff/: 416/800 <-> 2002/1000 big, 416/800 <->
1794/950 little, about 29000 changes per cluster), no hang and no error. The cause of the first one is not
known.

### Steady release (2026-10-15, evidence/thermal/steady)

Before the steady steps, a level reached under a short burst stayed until
the load ended. Each run below starts at 66 C; "off" is `steady_release
false`, Summit plays the video:

| load | order | mean level | fps / result |
|---|---|---|---|
| YouTube 720p60, 110 s | off, on, on, off | 2.17, 1.93, 1.90, 2.13 | 26.3, 28.7, 34.0, 24.2 fps |
| one looping 1080p video, 6 min | off, on, on, off | 3.23, 4.04, 5.86, 6.26 | level 8 in runs 2-4 |
| two looping 1080p videos, 6 min | off, on, on, off | 5.84, 6.14, 6.56, 6.34 | level 8 from minute 3-4 |

With the steady steps the short run comes down 10-11 times and keeps
more of its frames, at 0.3-1 C more. The six-minute loads keep the SoC
at 79-80 C, where the steady steps do not act, and the level follows the
run's place in the sequence rather than the setting: after the first run
the board no longer cooled to 66 C within 400 s (66.7, 67.9, 69.5 C
before runs 2-4). The hottest reading in all twelve runs was 81.5 C, in
a run without the steady steps.

## Open ends

- The first run's A76 hang (above): not reproduced since the margins.
- The policy is a step controller: the steady steps (2026-10-15) take
  levels back between 76 and 79 C, but loads that hold the SoC at 80 C
  still end at level 6-8 after a few minutes once the board has warmed
  (below); a power budget per level would do better.
- Not scaled: the DRAM (the BSP's dmcfreq, 400-2400 MHz, needs the
  firmware's retraining), the DSU (stays at 780 MHz), the NPU (off).
- The cores only wait for interrupts when idle (no PSCI CPU_SUSPEND power
  down as Linux's cpuidle does): air/OS idles about 2 C above Debian.
- The GPU's firmware is not told when its clock changes.
- The power daemon gives the critical notification 2 s before it asks the
  registrar to shut down; the board was off 6 s after the trip.
