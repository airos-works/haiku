# Tablet mode (2-in-1 laptops)

A laptop whose screen folds all the way back (a "Yoga") knows how it is
folded: as a laptop, or as a tablet, a tent or a stand with the keyboard out
of reach. The system follows the hinge, tells the firmware, and lets programs
ask and be told when it changes.

```
$ tabletmode
laptop, hinge at 91 degrees
$ tabletmode -w            # prints every change
16:01:43  laptop, hinge at 91 degrees
16:04:10  stand, hinge at 262 degrees
16:04:12  tablet, hinge at 357 degrees
```

## The modes

| Mode   | Hinge                              | Keyboard     |
|--------|------------------------------------|--------------|
| laptop | up to about 190 degrees (flat: 180)| in use       |
| stand  | further, the screen upright        | out of reach |
| tent   | further, the hinge at the top      | out of reach |
| tablet | folded back all the way (360)      | out of reach |

"Tablet mode" in the wide sense is every mode but the first:
`is_tablet_mode()` in `<accelerometer.h>`, and the exit status 0 of
`tabletmode -q`.

The laptop becomes something else above 200 degrees and a laptop again below
188; it is a tablet above 340 degrees until it is below 325. A mode has to be
measured for three quarters of a second before it counts.

## Where it comes from

Such a machine has no switch for this and (the Yoga 11e) no gyroscope either.
It has two accelerometers, one behind the screen and one under the keyboard;
both feel gravity, and the angle between what they feel, around the hinge, is
the angle of the hinge. Windows has a service that works that out and hands
the mode to the firmware; here the kernel driver does both.

- **`bosch_accel`** (`src/add-ons/kernel/drivers/sensor/bosch_accel`) drives
  Bosch's I2C accelerometers (BMA222, BMA250E, BMA253/254/255, BMA280, the
  accelerometer half of the BMC150 and BMI055) that the firmware describes
  as ACPI `BOSC0200` and its relatives. A `BOSC0200` with two I2C addresses
  is the pair of a 2-in-1: the first behind the screen, the second under the
  keyboard, each with a matrix that says how the chip is mounted (the
  methods `ROMS` and `ROMK`).
- It reads both four times a second (`HingeTracker`, plain integer
  arithmetic) and publishes the result as `/dev/sensor/hinge/0`.
- On every change it tells the firmware: Lenovo's `ANGL` method, which hands
  the mode to the embedded controller. The controller then knows that the
  keyboard is folded away, picks its fan curve for the mode, and reports the
  mode itself where the firmware asks for it (the ThinkPad hotkey device's
  `GMMS`). The values `ANGL` takes are not documented anywhere; they were
  found by trying on a Yoga 11e (5th generation): 0 laptop, 4 stand, 5 tent,
  6 tablet.

What the accelerometers can't tell apart is a closed machine from one folded
back all the way: both halves are parallel either way. While the hinge is
followed there is no doubt (it does not jump from 20 to 340 degrees). When it
was not followed (at boot, after sleeping, after a while with the hinge held
vertically) the lid switch decides: closed is closed.

The angle can only be measured while the hinge is not close to vertical, and
while the machine is not being shaken: a tablet held upright like a book says
nothing about its hinge. The mode stays what it was until it can be measured
again.

## What the desktop does

`tablet_server` (started at login, `src/servers/tablet`; it quits at once on
a machine without `/dev/sensor/hinge/0`) follows the hinge and does what the
Screen preferences say. Both settings only show there on a machine with the
hinge sensor.

- **Keep the screen upright in tent and tablet modes** (on by default).
  Folded into a tent, the panel is upside down; a tablet is upside down
  when it is held with the hinge at the top. In the stand, tent and tablet
  modes the accelerometer behind the screen says which way up the screen
  is, a few times a second (the screen's top must point at least 55% of the
  way up or down, and further than to the side, twice in a row). While it
  cannot tell, because the machine lies flat, a tent or a tablet is turned
  and a stand is not. As a laptop the picture is always the right way.
- **In tablet mode: Desktop** (the default) leaves the desktop as it is;
  **air/OS Mobile (experimental)** starts `MobileShell`, the touch shell
  ([MOBILE-SHELL.md](MOBILE-SHELL.md)), in its place while the machine is a
  stand, a tent or a tablet, and quits it when it is a laptop again, which
  puts the desktop and the windows back as they were.

The settings are kept in `~/config/settings/tablet_mode`:

```
upright_rotation true
tablet_view desktop     # or mobile
```

### How the picture is turned

The display engine turns it as it scans the frame buffer out: nothing is
drawn again, no window moves, and screenshots stay the right way round. The
accelerant hook `B_SET_DISPLAY_OUTPUT_ROTATION` (0 or 180 degrees; outputs
that can do it say so with `B_DISPLAY_OUTPUT_CAN_ROTATE`) sets the
`rotation` property of the primary plane, and of the cursor's plane where
it has one; `amdgpu_kms` has it, for `i915_drm` (the Yoga's Gemini Lake),
`amdgpu_drm` and `nouveau_drm`. Where the cursor's plane cannot be turned
(amdgpu's), app_server draws the cursor into the frame buffer instead.
Programs turn a display with a display layout request
(`BPrivate::set_display_layout()`) holding `"rotation"` (int32) and, for
another display than the built-in panel, its `"id"`; the layout
(`get_display_layout()`) reports `"can rotate"` and `"rotation"` per display.

The touch screen turns with the panel. A HID digitizer that is part of a
display (a touch screen, or the pen of one: the usages Touch Screen and
Pen) says so (`MS_IS_DISPLAY_DIGITIZER`), and input_server places its
touches on the built-in panel, the way up the panel is turned, instead of on
the whole desktop around every monitor.

## For programs

`headers/private/drivers/accelerometer.h` has the details.

- `/dev/sensor/hinge/0`: `HINGE_GET_STATE` returns the mode, the angle and
  when the mode last changed. The device is readable for `select()`,
  `poll()` and `wait_for_objects()` whenever the mode changed since the
  descriptor last asked, so a program waits for it like for anything else.
  Read as a file it is a line of text: `tablet 357`.
- `/dev/sensor/accelerometer/<n>`: `ACCELEROMETER_GET_SAMPLE` returns what
  one sensor measures, in thousandths of the standard gravity, in the axes of
  the part it sits in (x to the right, y to the top of the screen, z out of
  the screen: a screen lying face up reads 0 0 1000). `ACCELEROMETER_GET_INFO`
  says which chip it is and where (`ACCELEROMETER_LOCATION_DISPLAY`,
  `..._BASE`). Read as a file it is a line of text: `15 1004 -68`.
- `tabletmode` for scripts: `tabletmode -q && echo "no keyboard"`.

## Testing without folding anything

`tabletmode --set tablet` (or `stand`, `tent`, `laptop`) makes the driver
report that mode whatever the hinge does, firmware included; `tabletmode --set
auto` goes back to measuring. `tabletmode -a` also prints what the two
accelerometers measure.

What follows the mode can be tried on any machine: `tablet_server --hinge
<file> [--display-accelerometer <file>]` reads the mode from a text file
(`tablet 357`, as the device reads) and which way up the screen is from
another (`x y z` in mg), instead of the devices. Quit the running one first
(`hey application/x-vnd.airOS-tablet_server quit`).

The arithmetic has a host test with made-up poses (folding back and forth
with noisy sensors, a vertical hinge, shaking, the closed-or-folded-back
question): `src/add-ons/kernel/drivers/sensor/bosch_accel/test/run.sh`.

## What it does not do (yet)

- Only the two ways up of the panel: no portrait (90 and 270 degrees),
  which would need the desktop laid out anew rather than the picture
  turned. The on-screen keyboard comes with air/OS Mobile, not with the
  desktop.
- A touch screen is taken to be on the built-in panel; a touch monitor on
  a laptop that has a built-in panel is mapped onto the panel.
- The vertical blank flush cannot follow the scan of a panel shown upside
  down (it runs from the bottom row up): it copies at the blank without
  knowing where the scan is.
- The system does not ignore the built-in keyboard and touchpad itself in the
  tablet modes; it leaves that to the embedded controller, which was told.
- The sensors are not calibrated (the firmware keeps offsets for Windows'
  driver, `CALS`/`CALK`, in a format of its own): the angle may be a few
  degrees off, which the thresholds allow for.
- Other 2-in-1 designs are not covered: machines that report the mode
  themselves (ACPI `PNP0C60`/`INT33D3` tablet indicators, Intel `VGBS`), the
  `DUAL250E` pairs of other Lenovo machines (which want the angle through a
  `_DSM`), and sensor hubs.
