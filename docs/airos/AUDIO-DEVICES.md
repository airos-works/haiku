# Audio devices in the Deskbar

The volume control in the Deskbar (the speaker icon) says where the sound
goes to and comes from, and lets you change it. Since branch `audio-devices`
(2026-10-15).

* A **left click** opens the volume panel; under the slider it names the
  output and input in use: `Output: Realtek HD Audio   Input: Realtek HD Audio`.
* A **right click** opens a menu with *Output* and *Input* submenus. Each
  lists the physical audio devices the media server knows: sound cards, a
  graphics card's HDMI or DisplayPort audio, USB headsets, the microphone of
  a USB webcam, a Bluetooth A2DP sink. The device in use is marked; choosing
  another makes it the default for every application. A new output takes
  over while sound is playing: the media server moves the system mixer's
  connection to it, so Amp or airTime play on without a restart. A sound
  card with several stereo pairs is listed once per pair (`Card: output 2`).
* The **tool tip** over the icon shows the volume and both devices.
* A **middle click** mutes, the mouse wheel changes the volume, as before.

Devices plugged in after boot appear in the menus as soon as their driver
publishes them: the `multi_audio` add-on watches `/dev/audio/hmulti` and
tells the media server about new and gone devices, as the webcam add-on does
for cameras. A device that is unplugged while it is the default is replaced
by the media server's own choice (the speakers before HDMI audio).

## From a shell

```
desklink --audio-devices                  the outputs and inputs, * = default
desklink --audio-output=<node>[:<input>]  make that node (input) the output
desklink --audio-input=<node>             make that node the input
```

The node numbers are those of the listing. The same calls are available to
programs through `BMediaRoster::SetAudioOutput()` and `SetAudioInput()`.

## How it is built

```
src/bin/desklink/AudioDevices.cpp       lists the live physical audio nodes and the defaults, sets them
src/bin/desklink/MediaReplicant.cpp     the Deskbar icon: menus, tool tip, watches B_MEDIA_DEFAULT_CHANGED
src/bin/desklink/VolumeWindow.cpp       the volume panel with the device labels
src/servers/media/media_server.cpp      sends B_MEDIA_DEFAULT_CHANGED after SetAudioOutput() & co. and after a rescan
src/servers/media/DefaultManager.cpp    reconnects the mixer when the default output changes (unchanged)
src/add-ons/media/media-add-ons/multi_audio/MultiAudioAddOn.cpp   device hot plug, flavor IDs that are not list indices
src/add-ons/kernel/drivers/audio/hda/hda_multi_audio.cpp          device names by codec maker, "(HDMI)" for display audio
```

`B_MEDIA_DEFAULT_CHANGED` carries `default` (the media server's node_type:
audio output, audio input, video output, video input) and `node` (the
media_node_id of the new default, -1 for none). Anything that shows the
defaults can `StartWatching()` it.

HDA devices are named after their codec's maker (`Realtek HD Audio`,
`AMD HD Audio (HDMI)`), so that a machine's sound card and its graphics
card's audio can be told apart. The media server prefers a device without
`(HDMI)` in its name when the user has not chosen one, and codecs made by
AMD, NVIDIA and Intel count as display audio whatever their widgets say.
