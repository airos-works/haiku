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

`touchpad_scroll_test` (src/tests/add-ons/input_server) feeds scroll gestures
to the add-on's gesture code and checks the phases and that the precise
deltas add up to the whole steps. `wheel_log [seconds]` opens a window that
prints the wheel messages it gets. On the Framework 13's PixArt touchpad a
step is 15 touchpad units, so precise deltas come in 1/15 steps.
