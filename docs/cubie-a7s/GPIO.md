# GPIO on the Radxa Cubie A7S: the sunxi_gpio driver and AirPins

The board's pin headers are available to programs through
`/dev/misc/sunxi_gpio`, with the interface of the Raspberry Pi's
`rpi_gpio` (`headers/private/drivers/rpi_gpio.h`; see `docs/rpi4/GPIO.md`).
`rpi_gpio_info.board` is `RPI_GPIO_BOARD_CUBIE_A7S`, which tells AirPins
to show this board's headers.

## The headers

The A7S has no 40-pin header. It has a 30-pin header **J11**, whose power,
ground and console pins sit where the Raspberry Pi's first 30 do, and a
15-pin header **J6** (schematic RS504 v1.10, sheet 14 "40PIN Connector").
The driver numbers pins by position: J11 pin n is pin n, and J6 pin n is
pin 30 + n.

| J11 | Signal | | J11 | Signal |
| --- | --- | --- | --- | --- |
| 1 | 3.3 V | | 2 | 5 V |
| 3 | PJ23 (TWI7 SDA) | | 4 | 5 V |
| 5 | PJ22 (TWI7 SCK) | | 6 | Ground |
| 7 | PB0 | | 8 | PB9, UART0 TX (console, reserved) |
| 9 | Ground | | 10 | PB10, UART0 RX (console, reserved) |
| 11 | PB1 | | 12 | PB5 (I2S0 BCLK) |
| 13 | PL6 | | 14 | Ground |
| 15 | PL7 | | 16 | PJ24 |
| 17 | 3.3 V | | 18 | PJ25 |
| 19 | PD12 (SPI1 MOSI) | | 20 | Ground |
| 21 | PD13 (SPI1 MISO) | | 22 | PL5 |
| 23 | PD11 (SPI1 CLK) | | 24 | PD10 (SPI1 CS0) |
| 25 | Ground | | 26 | PD14 |
| 27 | PD17 (TWI2 SDA) | | 28 | PD16 (TWI2 SCK) |
| 29 | PB2 | | 30 | Ground |

| J6 | Signal | Driver pin |
| --- | --- | --- |
| 1 | PB3 | 31 |
| 2 | PM3 | 32 |
| 3 | PM4 | 33 |
| 4 | Ground | |
| 5 | PB6 (I2S0 LRCK) | 35 |
| 6 | PB4 (I2S0 MCLK) | 36 |
| 7 | PB8 (I2S0 DIN0) | 37 |
| 8 | PB7 (I2S0 DOUT0) | 38 |
| 9 | Ground | |
| 10 to 15 | PG0 to PG5 | 40 to 45 |

That makes 31 claimable pins. The console pins can be read but not claimed.

## The driver

`src/add-ons/kernel/drivers/misc/sunxi_gpio.cpp`, a device manager driver
in the `haiku` package for arm64. Like `sunxi_ve`, it lives in
`add-ons/kernel/drivers/misc`, so a non-packaged test copy takes effect at
the next boot and nothing loads it while the system runs.

- Attaches to the device tree's `allwinner,sun60i-a733-pinctrl` node when
  the root node is `radxa,cubie-a7s` (the header map belongs to this board
  only), and finds the `allwinner,sun60i-a733-r-pinctrl` node for ports L
  and M itself.
- Register layout from Allwinner's pinctrl driver for the A733 (BSP
  `pinctrl-sun60iw2.c`, hardware type 4; R_PIO `-r.c`, type 1). PIO bank n
  (A = 0) is at 0x80 + 0x80·n: multiplexer +0x00 (4 bits a pin), data +0x10,
  pull +0x30. Its interrupt configuration, enable, status and debounce
  registers are at +0x40, +0x50, +0x54 and +0x58. R_PIO banks are 0x30
  apart from 0 (pull at +0x24), and their interrupt registers are at
  0x200 + 0x20·n. The device tree gives one interrupt per bank: the PIO's
  from bank B, the R_PIO's from bank L.
- Claims, restore on release or close, `RPI_GPIO_CLAIM_DETACH`, the event
  ring and the throttling (more than 400 edges in 10 ms: 50 ms of sampling)
  are rpi_gpio's. State reports the A733's multiplexer values: 0 input,
  1 output, 2 to 13 peripherals, 14 interrupt, 15 off (the reset state).
- A claimed input is switched to its interrupt function (14), with the
  bank's interrupt on both edges. Its data bit still reads the pad there,
  and programs see it as an input. A bank whose interrupt cannot be
  installed, or one with interrupts that other code enabled, is sampled
  every millisecond instead.
- Levels are written read-modify-write, as Linux does. The other writers of
  these banks' data registers are sunxi_regulator (port M, once at boot)
  and sunxi_display (the USB-C VBUS switch on port L, when the port changes
  role).

Lab tool: `cubie_gpio` (`tools/rpi4/gpio.cpp`, built in
`tools/cubie-a7s/Jamfile`) is the Pi's `rpi4_gpio`. It opens whichever
device exists and runs `info`, `state`, `set`, `watch` and `selftest [pin]`
(default J11 pin 26, PD14). The self test needs nothing wired: it drives the
pin through its pull resistors and as an output, and checks that the
console pin and a power pin cannot be claimed.

## AirPins

AirPins opens `/dev/misc/rpi_gpio` and then `/dev/misc/sunxi_gpio`. It
shows the board's connectors from `rpi_gpio_info.board`: J11 in two
columns and J6 in one. Pins carry the A733's names (PB0, PL5...), their
functions are the multiplexer's (F2 to F13, EINT) with the schematic's
signal names, and the "Port" layout sorts them by port. Configuration files
written on the Cubie carry `"board":"cubie-a7s"`. A file only opens on the
board it was written for, so the Pi's GPIO numbers never land on the
Cubie's positions.

## Status

Build-checked only (2026-10-27). The lab board was off the network, with
neither air/OS nor the Debian recovery answering after a power cycle. Before
relying on it, run `cubie_gpio info`, `state` and `selftest`, and AirPins
against the board.
