# Smooth touchpad scrolling

A touchpad's scroll gesture (two fingers, or the edge areas) reaches programs
as `B_MOUSE_WHEEL_CHANGED` messages, like a mouse wheel. `be:wheel_delta_x`
and `be:wheel_delta_y` are whole wheel steps, as before, so existing programs
scroll as they did. Since 2026-10-27 the input server's mouse add-on adds,
for touchpads:

| Field | Type | Meaning |
| --- | --- | --- |
| `be:wheel_precise_delta_x`, `be:wheel_precise_delta_y` | float | The movement since the last message in wheel steps, not rounded: added up over a gesture they give the whole steps plus what was left over. Same signs as `be:wheel_delta_*` (natural scrolling and the Touchpad preferences' step sizes apply). |
| `be:wheel_phase` | int32 | `B_WHEEL_PHASE_BEGAN` (1) for the gesture's first message, `B_WHEEL_PHASE_CHANGED` (2) while it goes on, `B_WHEEL_PHASE_ENDED` (3, deltas 0) when the fingers leave or start moving the pointer instead (`<Input.h>`). |

During a gesture a message comes with every touchpad report that moved (not
for fingers resting on it), also when no whole step was reached; its
`be:wheel_delta_*` are 0 then, which the Interface Kit's views, scroll bars,
menus and tab views ignore. A mouse wheel
sends no precise deltas and no phase. There is no momentum phase: a program
that wants kinetic scrolling continues the movement itself after
`B_WHEEL_PHASE_ENDED`.

```cpp
case B_MOUSE_WHEEL_CHANGED:
{
	float dy;
	int32 phase;
	if (message->FindInt32("be:wheel_phase", &phase) == B_OK
		&& message->FindFloat("be:wheel_precise_delta_y", &dy) == B_OK) {
		ScrollBy(0, dy * kPixelsPerStep);	// smooth
		if (phase == B_WHEEL_PHASE_ENDED)
			StartMomentum();
	} else if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK)
		ScrollBy(0, dy * kPixelsPerStep);	// a mouse wheel
	break;
}
```

Since 2026-11-12 the scrolling follows the fingers' reported positions
(airos-works/summit#200). Before, it was counted in the pointer's whole units:
a report that moved less than a unit was dropped, and the running sum started
over at every report without movement, so fingers slower than about a unit a
report (8 mm a second on the Framework 13) scrolled nothing at all and faster
ones scrolled in jumps of a tenth of a step. Now:

- The precise deltas are the fingers' movement, not rounded, after a
  hysteresis of a unit and a half of the touchpad's own: fingers resting on
  it (whose position flickers by a unit) send nothing, a moving finger
  scrolls as far as it moves, however slowly.
- The whole steps (`be:wheel_delta_*`) are cut from the same movement, so
  programs that only read them scroll slowly too.
- The gain is set so a scroll of 30 to 80 mm a second goes as far as it did.
  Faster than that it goes a little further (the old rounding lost more there).

`touchpad_scroll_test` (src/tests/add-ons/input_server) feeds scroll gestures
to the add-on's gesture code and checks the phases, that the precise deltas
add up to the whole steps, that a slow finger (half a unit a report) scrolls
as far as a steady one, and that a resting one does not scroll. `wheel_log
[seconds]` opens a window that prints the wheel messages it gets.
