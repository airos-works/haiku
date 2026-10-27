# GPIO on the Raspberry Pi 4: the rpi_gpio driver and AirPins

The header's GPIO pins are available to programs through
`/dev/misc/rpi_gpio`, and the image carries **AirPins**, a GPIO tool after
[pigg](https://github.com/andrewdavidmackenzie/pigg) (Andrew Mackenzie and
contributors, Apache-2.0): configure pins as inputs or outputs, switch
outputs, watch levels as LEDs and waveforms, open and save pigg's `.pigg`
files. AirPins lives in `/mnt/HaikuWork/apps/airpins` (its README has the
attribution and the user guide).

## The driver

`src/add-ons/kernel/drivers/misc/rpi_gpio.cpp`, interface
`headers/private/drivers/rpi_gpio.h`, in the `haiku` package for arm64.

- Finds the controller by `brcm,bcm2711-gpio` in the device tree (registers
  at 0xfe200000, bank 0's interrupt GIC SPI 113). On another board it
  publishes nothing.
- `RPI_GPIO_GET_INFO` says `board` `RPI_GPIO_BOARD_RPI4` (0, also what
  drivers before the field left there). The Radxa Cubie A7S's `sunxi_gpio`
  offers the same interface for its headers (`docs/cubie-a7s/GPIO.md`).
- Everybody may read all 58 pins' function, pull and level
  (`RPI_GPIO_GET_STATE`). The firmware's board revision and serial number come
  with `RPI_GPIO_GET_INFO`.
- A file descriptor **claims** a pin of the 40-pin header (GPIO 0 to 27, all
  in bank 0) as an input with a pull resistor (up, down, none) or as an
  output. The pin's function, pull and level are saved at the first claim
  and restored when it is released or the descriptor is closed (also when
  the program crashes). Another descriptor gets `B_BUSY`. Pins 28 and up
  (Wi-Fi SDIO, Bluetooth UART, ...) cannot be claimed. `RPI_GPIO_CLAIM_DETACH`
  configures a pin and keeps nothing, for command line tools.
- Level changes are **events** in one ring of 8192 for all descriptors, each
  reading only its own pins: inputs' edges (rising and falling detection)
  timestamped with `system_time()` in the interrupt handler, outputs' writes
  when written, and the level at claim time. When the level an edge reports
  equals the last one recorded, two edges came before the handler looked: a
  pulse, recorded as two events with the same time. More than 400 edges in
  10 ms switch edge detection off for 50 ms, then the inputs are sampled once
  and detection resumes (`RPI_GPIO_EVENT_SAMPLED`): a fast signal cannot
  monopolise a core. Without the interrupt the inputs would be sampled every
  millisecond (not seen on the board).
- An input that only changes its pull keeps edge detection on, so the change
  of level it causes is an event of its own.
- Shared registers: h4bcm writes the pull register of pins 16 to 31 once,
  when Bluetooth is opened; the driver's read-modify-write accesses of that
  register could only collide with that moment.

### Lab tool

`rpi4_gpio` (`tools/rpi4/gpio.cpp`, in the lab overlay's
`~/config/non-packaged/bin`): `info`, `state`, `set <pin> in [up|down|none]`,
`set <pin> out <0|1>`, `watch <pin> [pull] [seconds]`, `selftest [pin]`.
The self test needs nothing wired: it drives the pin through its pull
resistors and as an output. On the board (2026-10-04, `evidence/airpins/
driver-selftest-1.txt`) all 20 checks pass:

| Check | Result |
| --- | --- |
| claim, state, claim event | input pull-up reads high |
| pull-up to pull-down | falling edge reported, timestamped 5 us after the change |
| 200 pull changes 1 ms apart | 200 alternating edges, none sampled, none lost |
| 20000 pull changes as fast as the ioctl goes (40 ms) | 402 events, throttled, nothing lost, nothing hung |
| a second descriptor | cannot claim (busy) nor write; sees the pin claimed |
| output | high, written low (written event, reads low), high again |
| release / close | function and pull restored |

## AirPins

Built by `tools/airos/build-arm64-app-packages.sh
/mnt/HaikuWork/rpi4/packages-arm64 airpins` (only when named: it belongs to
the Raspberry Pi image), staged by `tools/rpi4/stage-packages.sh` into the
`rpi4-airos` image. Tried on the board (screenshots in
`evidence/airpins/`), against the driver's state from `rpi4_gpio state`:

- the board, BCM and compact layouts, pigg's pin colours; GPIO 14 and 15
  shown as TXD0 and RXD0 (the serial console);
- a `.pigg` file from the command line and by double-click (`open`): its five
  pins claimed with the right functions, pulls and levels;
- an output switched high and low (the driver reads it back), the LED held
  to invert it (a pulse in the waveform), an input's pull changed from the
  menu (rising edge, LED green), a pin set to output from its disc's menu;
- taking over GPIO 14 asks first (UART0 TXD, the serial console); set back
  to Unused it is ALT0 without pull again;
- Save (pigg's JSON, typed `application/x-vnd.pigg-config`), quitting with
  unsaved changes asks; after quitting no pin is claimed and every pin is as
  it was at start;
- device details (model, revision c03115, BCM2711, 4 GB, serial, edge
  interrupts), simulated pins (Device menu), About with the pigg credit;
- cost with five pins configured: about 0.4 % of a core for AirPins and
  1.2 % for app_server drawing the waveforms 10 times a second.

The image built with both (sha256 7f900be7…) was flashed on 2026-10-04
(readback matching): AirPins is in the Deskbar's Applications with its
icon, starts with the board's GPIO (`evidence/airpins/fresh-card-*.jpg`),
and set GPIO 17 to an output and switched it (the driver read low, then
high).

Captures through the NanoKVM lose the colour of one-pixel horizontal lines on
every other row (the capture's chroma subsampling): the waveforms are drawn
two pixels thick, which also reads better.

## Open

- Nothing is wired to the lab board's header: inputs were driven by their
  own pull resistors only, never by an outside signal; the throttling was
  exercised by toggling the pulls from the self test.
- Only input and output: no ALT functions (I2C, SPI, PWM, UART) to choose,
  as in pigg.
- pigg's remote side (pigglet over TCP/iroh, porky on a Pi Pico) is not
  ported.
- The BCM2711 only; the Pi 5's RP1 is a different controller.
