# Clamshell mode

A laptop with its lid closed keeps working on external displays: plug in a
dock or a monitor, close the lid, and the desktop carries on on the external
displays with the laptop's own panel off. Without an external display,
closing the lid does what the Power preferences say (sleep, by default).

## What happens

- **Lid closed, an external display connected:** app_server turns the
  built-in panel off and leaves it out of the desktop, as if it had been
  unplugged. Its windows move to the other displays; if it was the main
  display (with the Deskbar), another one takes over. power_daemon does not
  put the machine to sleep and does not turn the displays off.
- **Lid opened:** the panel comes back where it was in the arrangement,
  with its windows, and is the main display again if it was before.
- **External displays gone while the lid is closed** (unplugged, or the
  dock was): after 10 seconds without one (a dock that reconnects drops its
  monitors for a moment), the lid action applies, normally sleep.
- **Woken with the lid closed** (power button) while a display is connected,
  for example after plugging in a dock: the machine stays awake on it.
- **Booted with the lid closed** on a dock: app_server reads the lid when it
  starts, so the panel never lights up.

Changes made in the Screen preferences while the lid is closed are kept
(scale, resolution, a display turned on or off), but the places of the
displays and the main display are those of the whole arrangement, with the
panel in it: the closed-lid arrangement is only for the time being.

## Settings

Power preferences, *Lid*: **Keep working on external displays** (on by
default). Turned off, closing the lid sleeps (or turns the displays off) with
external displays connected, too. In `~/config/settings/system/power` it is
`lid_closed_external_display awake` or `same`. With *When the lid is
closed: Do nothing*, the panel is still turned off while an external display
is connected.

## How it works

- `power_daemon` (`src/servers/power/power_policy.cpp`) tells app_server
  about the lid with `BPrivate::set_display_layout()` and a request holding
  only a `"lid closed"` bool, then looks at `get_display_layout()` for an
  enabled display that is not `"internal"`. While the lid is closed on
  external displays it checks again every 2 seconds.
- app_server (`DisplayLayout::ReadOutputs()`): with the lid closed, a
  connected built-in panel is reported as not connected (and `"lid closed"`)
  while another monitor is connected, so the arrangement, the window moves
  and the settings treat it like an unplugged monitor.
  `DisplayLayout::Store()` keeps the remembered places meanwhile.
- A built-in panel is an output the accelerant flags
  `B_DISPLAY_OUTPUT_INTERNAL` (amdgpu_kms: eDP, LVDS, DSI connectors), or one
  whose connector name starts with `eDP`, `LVDS` or `DSI`.
- Graphics drivers without display layouts (one monitor) are not affected:
  power_daemon cannot tell an external display there, and sleeps as before.

Logs: `power_daemon: lid closed on an external display, staying awake`,
`app_server: lid closed, built-in display off` in the syslog.
