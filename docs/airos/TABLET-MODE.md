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

The arithmetic has a host test with made-up poses (folding back and forth
with noisy sensors, a vertical hinge, shaking, the closed-or-folded-back
question): `src/add-ons/kernel/drivers/sensor/bosch_accel/test/run.sh`.

## What it does not do (yet)

- Nothing in the desktop reacts to the mode yet: no on-screen keyboard
  appears, and the screen does not rotate with the tablet.
- The system does not ignore the built-in keyboard and touchpad itself in the
  tablet modes; it leaves that to the embedded controller, which was told.
- The sensors are not calibrated (the firmware keeps offsets for Windows'
  driver, `CALS`/`CALK`, in a format of its own): the angle may be a few
  degrees off, which the thresholds allow for.
- Other 2-in-1 designs are not covered: machines that report the mode
  themselves (ACPI `PNP0C60`/`INT33D3` tablet indicators, Intel `VGBS`), the
  `DUAL250E` pairs of other Lenovo machines (which want the angle through a
  `_DSM`), and sensor hubs.
