# Recording what the computer plays

The system audio mixer has a second output, **Mixer Monitor** (source id 1),
next to the one that feeds the sound card (id 0). The monitor carries a copy
of every buffer the mixer sends to the sound card: the mix of all programs,
after the mixer's volume, mute and attenuation, as host-endian floats with
the sound card connection's frame rate, channels and channel mask. A program
connects a `BMediaRecorder` (or its own consumer) to it to record the
computer's sound, for example to share the screen with audio.

```cpp
media_node mixer;
BMediaRoster::Roster()->GetAudioMixer(&mixer);

media_output outputs[4];
int32 count = 0;
BMediaRoster::Roster()->GetAllOutputsFor(mixer, outputs, 4, &count);
media_output* monitor = NULL;
for (int32 i = 0; i < count; i++) {
	if (strcmp(outputs[i].name, "Mixer Monitor") == 0)
		monitor = &outputs[i];
}

media_format format;
format.type = B_MEDIA_RAW_AUDIO;
format.u.raw_audio = media_raw_audio_format::wildcard;

BMediaRecorder recorder("Screen share audio", B_MEDIA_RAW_AUDIO);
recorder.SetAcceptedFormat(format);
recorder.SetHooks(ProcessAudio, NULL, this);
	// ProcessAudio() gets the buffers and recorder.Format()
if (monitor != NULL && recorder.Connect(mixer, monitor, &format) == B_OK)
	recorder.Start();
```

What to expect:

* **One recorder at a time.** A second `Connect()` fails with
  `B_MEDIA_ALREADY_CONNECTED`. A recorder whose team quit without
  disconnecting is dropped by the mixer the next time it sends a buffer or
  someone asks for its outputs.
* **Only while there is a sound card.** The monitor follows the mixer's
  sound card connection; without one, connecting fails with
  `B_MEDIA_NOT_CONNECTED`.
* **Format changes are announced.** When the sound card gets another frame
  rate (Media preferences) or the output moves to another device, the
  recorder's consumer gets `FormatChanged()` and the buffers that follow have
  the new format (`BMediaRecorder::Format()` in the hook).
* **Gaps.** The system mixer stops when nothing plays, and so do the
  buffers. A recorder that does not return its buffers in time loses some;
  the mixer never waits for it. `start_time` is the sound card's performance
  time and can jump back when the output is connected again.
* **Latency.** Buffers leave the mixer ahead of their `start_time` by the
  sound card's latency, so a recorder gets them before they are heard.

Since 2026-10-27 (summit#119, for Summit's screen sharing with audio).
`mixer_monitor_test` (src/tests/kits/media) records the monitor while it
plays a 1 kHz tone; `mixer_monitor_test 1 rate` switches the sound card's
frame rate halfway through, `mixer_monitor_test 1 reconnect` connects the
mixer to it again.
