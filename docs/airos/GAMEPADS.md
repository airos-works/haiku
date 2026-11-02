# Game controllers

Game pads reach applications through `BJoystick` (`<device/Joystick.h>`,
libdevice), one device per pad under `/dev/joystick/` (`usb/0`,
`bluetooth/0`, ...). This page lists which pads appear there, how their
buttons and axes are laid out, and the rumble API that the web Gamepad API's
`vibrationActuator` (Summit, airos-works/summit#142) maps onto. Tracking issue:
airos-works/haiku#53.

## Which pads work

| Pad | Connection | Driver path | Rumble | Tested on hardware |
| --- | --- | --- | --- | --- |
| Any HID game pad or joystick (usage page 1, usage 4/5/8) | USB | `usb_hid`, generic | no | (existing support) |
| Xbox 360 wired | USB | `usb_hid` quirk (vendor class 0xff/0x5d/0x01) | yes | no |
| Xbox One, One S, Elite, Elite 2, Series X\|S, and third-party GIP pads (PowerA, PDP, Hori, Razer, ...) | USB | `usb_hid` GIP quirk (vendor class 0xff/0x47/0xd0, interface 0) | yes, with trigger motors | no |
| DualShock 4 (054c:05c4, 09cc, adapter 0ba0), DualSense (0ce6), DualSense Edge (0df2) | USB | `usb_hid`, generic HID + rumble quirk | yes | no |
| PlayStation 3 SIXAXIS | USB | `usb_hid` quirk (existing) | no | no |
| DualShock 4, DualSense, Switch Pro, Xbox One S (model 1708) | Bluetooth Classic | `bluetooth_server` HID host, `bluetooth_hid` | no | path tested with a fake device (see below) |
| Xbox Series X\|S, Xbox One (firmware 5.x), other HID-over-GATT pads | Bluetooth LE | `bluetooth_le_mouse` input server add-on, `bluetooth_hid` | no | no |

The GIP quirk matches the interface class, not a vendor list, so every pad
built to the Xbox One wired protocol is covered, including the IDs Linux's
`xpad` lists for PowerA (24c6:...), PDP (0e6f:...), Hori (0f0d:...), Razer,
Turtle Beach and others. The Xbox Wireless Adapter (045e:02e6/02fe) is a
different device (a Wi-Fi radio with its own protocol) and is not supported.

### Xbox One / Series over USB (GIP)

These pads are not HID devices. `usb_hid` treats them like the Xbox 360 pad:
it synthesizes a report descriptor and translates what the pad sends into
reports of that descriptor (`src/add-ons/kernel/drivers/input/usb_hid/
GamepadProtocols.cpp`, wired up in `QuirkyDevices.cpp`).

* On attach and whenever the joystick device is opened, the driver sends the
  initialization packets Linux's `xpad` sends (`xboxone_init_packets`): power
  on (`05 20 00 01 00`) for every pad, the One S / Elite 2 Bluetooth-mode reset
  (`05 20 00 0f 06`), the Elite 2 paddle request, the Hori/PDP identify ack,
  LED on and "authentication done" (PDP pads only report after these) and the
  short PowerA rumble pulse. The sequence number byte is counted per pad.
  When a pad announces itself again (`0x02`, after a reset) it is initialized
  again, at most every two seconds.
* Input packets `0x20` give the buttons, the 10 bit triggers and the sticks;
  the Guide button comes alone in `0x07` packets, which the One S wants
  acknowledged (the driver sends the ack). The Share button of the Series pad
  (045e:0b12) is read at 18 bytes from the end of the input packet, like
  `xpad` does. Status, firmware and other packets are dropped.
* Not done: the Elite paddles (requested, not reported), the Share button of
  third-party Series-style pads `xpad` lists with `MAP_SHARE_OFFSET`,
  battery level, the audio/headset interfaces.

### Button and axis layout of the Xbox pads

Both Xbox paths use the same layout, so an application maps them once:

| Buttons | |
| --- | --- |
| 1-4 | D-pad up, down, left, right |
| 5 | Start / Menu |
| 6 | Back / View |
| 7, 8 | left, right stick press |
| 9, 10 | left, right shoulder (LB, RB) |
| 11 | Guide (Xbox button) |
| 12-15 | A, B, X, Y |
| 16 | Share (GIP pads only; pressed only on Series pads) |

| Axes | |
| --- | --- |
| 0, 1 | left, right trigger: -32768 released to 32767 fully pressed (Z, Rz) |
| 2, 3 | left stick X, Y |
| 4, 5 | right stick X, Y (Rx, Ry) |

Stick Y axes grow downwards (up is negative), like on every HID game pad.
**Behaviour change:** before haiku#53 the Xbox 360 quirk passed the pad's own
Y axes through, which grow upwards, and also handed the pad's LED and rumble
status messages to the parser.

Generic HID pads (the Sony ones, most others) use the order of their own
report descriptor: for the DualShock 4 and DualSense that is X, Y (left
stick), Z, Rz (right stick), Rx, Ry (triggers), the D-pad as hat 0, and
buttons 1-14 as Square, Cross, Circle, Triangle, L1, R1, L2, R2, Share/Create,
Options, L3, R3, PS, touch pad click.

For the web Gamepad API's "standard" mapping Summit therefore needs a table
per pad family (Xbox layout above; Sony layout; Switch Pro), chosen from the
device's vendor and product IDs: `BJoystick::GetDeviceIDs(&vendor, &product)`
(Haiku extension, ioctl `B_JOYSTICK_GET_DEVICE_INFO`) returns them for USB
pads and Bluetooth Classic pads with PnP information; LE pads return 0:0 for
now (the add-on doesn't read the Device Information service). They also make
the usual Gamepad API `id` string (`"045e-0b12-..."`).

## Rumble

### BJoystick

Haiku extensions (non-virtual, added without touching the class layout or the
vtable, so existing binaries keep working):

```cpp
bool     BJoystick::HasRumble();
bool     BJoystick::HasTriggerRumble();
status_t BJoystick::SetRumble(uint16 strongMagnitude, uint16 weakMagnitude,
             bigtime_t duration, uint16 leftTriggerMagnitude = 0,
             uint16 rightTriggerMagnitude = 0);
```

* Magnitudes go from 0 (off) to 65535 (full). `strong` is the low frequency
  (left, heavy) motor, `weak` the high frequency (right, light) one; the
  trigger motors exist on Xbox One / Series pads (`HasTriggerRumble()`).
* A call replaces the running effect. `duration` is in microseconds; the
  driver stops the motors when it has passed. 0, or all magnitudes 0, stops
  them at once; `B_INFINITE_TIMEOUT` keeps them running until the next call.
* Closing the device (or the team ending) stops the motors.
* Returns `B_NOT_SUPPORTED` when the pad has no rumble (or its driver has no
  output path for it), `B_NO_INIT` when no device is open.

Mapping the Gamepad API onto it: `playEffect("dual-rumble", {duration,
startDelay, strongMagnitude, weakMagnitude})` →
`SetRumble(strong * 65535, weak * 65535, duration * 1000)` after `startDelay`
(Summit's own timer); `"trigger-rumble"` additionally passes
`leftTrigger`/`rightTrigger`; `reset()` → `SetRumble(0, 0, 0)`.
`vibrationActuator` should only be exposed when `HasRumble()` is true, and
`"trigger-rumble"` only listed in `effects` when `HasTriggerRumble()` is.

### Driver interface

`headers/private/device/joystick_driver.h`:

* ioctl `B_JOYSTICK_SET_RUMBLE` (`B_JOYSTICK_DRIVER_BASE + 7`) with a
  `joystick_rumble { uint16 strong_magnitude, weak_magnitude,
  left_trigger_magnitude, right_trigger_magnitude; bigtime_t duration; }`.
* A driver that supports it sets `js_flag_force_feedback` (and
  `js_flag_trigger_rumble`, new, `0x8`) in the `joystick_module_info` it
  returns for `B_JOYSTICK_GET_DEVICE_MODULE`.

In the HID drivers this lives in the shared `JoystickProtocolHandler`
(`hid_shared`), which handles the duration with a per-device thread and asks
the transport's `HIDDevice::SetRumble()`. `usb_hid` implements it per quirk:

| Pad | What is sent |
| --- | --- |
| Xbox 360 | `00 08 00 <strong> <weak> 00 00 00` on the interrupt OUT endpoint |
| GIP | `09 00 <seq> 09 00 0f <LT> <RT> <strong> <weak> ff 00 ff` (magnitudes 0-100) on the interrupt OUT endpoint |
| DualShock 4 | output report `05`, flags `01` (motors only, the light bar is left alone), `[4]` weak, `[5]` strong, 32 bytes |
| DualSense | output report `02`, flags `03` (compatible vibration + haptics select), `[3]` weak, `[4]` strong, 48 bytes |

`bluetooth_hid`, `i2c_hid` and the Hyper-V HID driver report no rumble.

Not done: generic HID force feedback (the PID usage page 0x0f) — no pad in
common use needs it; rumble over Bluetooth (DualShock 4/DualSense want vendor
output reports with a CRC, and the DualShock 4 then switches to an input
report its descriptor doesn't describe, which needs a translation like the
Xbox one; Xbox pads over LE want their HOGP output report written, which the
LE add-on doesn't do yet).

## Bluetooth

Classic HID pads (DualShock 4, DualSense, Switch Pro, Xbox One S model 1708)
are connected by the Bluetooth server's HID host, which hands their report
descriptor and input reports to `bluetooth_hid`. Its protocol handlers are
the same `hid_shared` ones `usb_hid` uses, so a descriptor with a Joystick,
Game Pad or Multi-axis collection gets a `/dev/joystick/bluetooth/N` device;
that worked before this change. The pads stay in their basic report mode
(report 1 for the Sony pads, 0x3f for the Switch Pro), which their
descriptors describe; motion sensors, touch pad and battery are not read.

HID-over-GATT (Bluetooth LE) devices used to be mice only: pairing refused a
device without a mouse input report. Now pairing also accepts a report map
with a joystick/game pad collection (`LEHIDReportMapHasGamepad()` in the
Bluetooth kit), and the `bluetooth_le_mouse` input server add-on, which
connects bonded LE HID devices, creates a `bluetooth_hid` device for such a
pad and writes its input reports there (report ID first, from the Report
Reference). That is the path the Xbox Series pads take.

## Testing

* `gamepad_protocols_test` (`src/tests/add-ons/kernel/drivers/gamepad`): the
  synthesized Xbox 360 and GIP descriptors parsed by the shared HID parser,
  packets in the layout the pads send (from `xpad`) translated and read back
  the way `JoystickProtocolHandler` reads them; initialization packets per
  model; GIP, 360, DualShock 4 and DualSense rumble bytes. `-v` prints the
  decoded states. Runs anywhere (no device needed).
* `bt_gamepad_test`: creates a fake Bluetooth pad (a descriptor laid out like
  the DualShock 4's Bluetooth report 1) on `bluetooth_hid`'s control device,
  writes reports and reads them through `BJoystick`.
* `le_gamepad_map_test` (`src/tests/kits/bluetooth`): which LE report maps
  count as game pads.
* `gamepad_test` (no arguments) lists the joystick devices; `gamepad_test
  [device [seconds]]` and `gamepad_test <device> rumble
  <strong> <weak> <ms> [<left> <right>]`: a real pad through `BJoystick`.

QEMU emulates no game pad (only keyboard, mouse and tablet; real pads need
`usb-host` passthrough).

### What to check with real controllers

1. Plug in an Xbox One / Series pad over USB: the Xbox button should stop
   blinking and stay lit within a second. `gamepad_test` lists `usb/0`;
   `gamepad_test usb/0 30` shows 16 buttons, 6 axes; check every button and
   axis against the table above (triggers -32768..32767, stick up negative),
   the Guide button press and release (One S: it must not get stuck), Share
   on a Series pad. syslog: `usb_hid: found quirky device`.
2. `gamepad_test usb/0 rumble 65535 0 1000`, then `0 65535 1000`, then
   `0 0 1000 65535 65535` (trigger motors): the motors run for one second and
   stop.
3. Unplug while `gamepad_test` runs, replug: comes back as a joystick.
4. A third-party pad (PowerA/PDP): it should report input at all (these need
   the extra init packets).
5. Xbox 360 pad: buttons and axes as before, but Y now grows downwards;
   rumble as in 2.
6. DualShock 4 / DualSense over USB: input as before, rumble as in 2, light
   bar unchanged.
7. Over Bluetooth: pair a DualShock 4 / DualSense / Switch Pro (Classic) and an
   Xbox Series pad (LE) in the Bluetooth preferences; `gamepad_test` should
   list `bluetooth/0` and show input. LE log: `game pad ...: published as a
   joystick device` in `~/config/var/log/bluetooth_le.log`.
8. On an arm64 board (Pi 4, ROCK 5, Cubie) repeat 1 and 2.

## Open points

* Vendor/product IDs of LE pads (Device Information service PnP ID).
* Rumble over Bluetooth; Elite paddles; battery levels.
