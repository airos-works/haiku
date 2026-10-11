# Bluetooth audio (A2DP source)

air/OS plays sound through Bluetooth speakers and headsets: the Advanced
Audio Distribution Profile, source side, with the SBC codec. It was written
on the Cubie A7S (AIC8800D80 controller) but nothing in it is specific to the
board; the X399 and the ROCK 5 use the same stack.

## State (2026-10-11)

Built for arm64 and x86_64. What has been checked:

- The SBC encoder, against FFmpeg's independent decoder: gain 1.000,
  delay 73 samples (37 with 4 subbands), 62-65 dB SNR on tones and sweeps
  at 44.1 kHz joint stereo, bitpool 53 (328 kbit/s); all channel modes,
  block and subband counts decode. The analysis window tables check out as
  a power-complementary prototype (within 0.1 %, 76 dB stop band).
- On the Cubie: the HCI module takes the controller's packet sizes (ACL
  1021 bytes x 9 buffers, LE 251 x 14) from the server's start-up queries;
  LE scanning still works; the bluetooth_server restarts cleanly. With
  the cubie-a7s merge of 2026-10-11, the Bluetooth preferences list nearby
  LE devices and the Deskbar's Bluetooth menu opens.
- With a device set (`bt_a2dp --use`), the media add-on publishes
  "Bluetooth: <name>", the node becomes the system audio output and the
  mixer connects to it at 44.1 kHz stereo; when MediaPlayer or airTime
  play, it tries to connect the speaker.

Not yet checked over the air. On 2026-10-10 the lab Cubie had no antenna:
paging the lab host ended in Page Timeout after 10 s, an inquiry found no
device although the host was discoverable, and the host did not find the
Cubie.

On 2026-10-11, with an antenna on J1 (the FCU760K module's shared
Wi-Fi/BT pin; its Bluetooth-only pin is not wired), Wi-Fi scans find
14-34 networks at 2.4 GHz (21 on average, 8 before), but the Bluetooth
link to the lab host still fails both ways: Page Timeout, empty
inquiries, and the host neither pages nor finds the Cubie. The vendor Linux stack on the same board
(Radxa Debian, aic_btusb + BlueZ) behaves the same, and every LE advertiser
the Cubie hears is at -93 dBm or weaker, so the board and the host are out
of Bluetooth range of each other. While the controller pages or inquires,
2.4 GHz Wi-Fi scans find nothing (the antenna is Bluetooth's then). Of the
loader's Bluetooth modes (`bt_mode` in the aic8800wifi driver settings),
0 and 5 (the default) hear the same advertisers, 1 and 2 none. Pairing,
AVDTP and streaming have therefore not run against a real sink yet; they
need a sink within range of the board. Evidence: `cubie/evidence/a2dp/` on
the lab disk (`antenna-check.txt`).

## Using it

- Pair and connect the speaker in the Bluetooth preferences. A device whose
  class says it is audio becomes the sound output once it is connected: the
  preferences store it as the Bluetooth audio device, and the media
  add-on makes its output the system's.
- The Deskbar's Bluetooth menu lists paired audio devices with "Play sound
  here" (or "Stop playing sound here"); that connects the speaker, or gives
  the sound back to the previous output.
- Disconnecting or removing the speaker in the preferences gives the sound
  back as well.
- The Media preferences list the output as "Bluetooth: <name>".
- From a shell, `bt_a2dp` connects, configures and streams a test signal,
  and `bt_a2dp --use <address>` makes a device the audio output.

The speaker is connected when sound starts, so it does not need to be on
all the time. After 5 s of silence the stream is suspended, after a minute
the speaker is let go; the next sound connects again (about a second).

## Pieces

- `src/kits/bluetooth/A2dpSource.cpp`: the profile. SDP lookup of the Audio
  Sink record, AVDTP 1.3 signalling as initiator (discover, capabilities,
  configuration, open, start, suspend, close) and as acceptor for what a
  sink may ask on its own (discover, capabilities, start, suspend, close,
  abort, delay reports), and the RTP media packets.
- `src/kits/bluetooth/SbcEncoder.cpp`: an SBC encoder written from the A2DP
  1.3 specification, appendix B (analysis filter bank, scale factors, bit
  allocation, joint stereo, frame syntax and CRC). All channel modes, 4 or
  8 subbands, 4-16 blocks, loudness or SNR allocation.
- `src/kits/bluetooth/AudioSinkSetting.cpp`: the chosen device, in
  `~/config/settings/bluetooth_audio`.
- `src/add-ons/media/media-add-ons/bluetooth_audio`: the Media Kit output.
  It takes 44.1 kHz stereo 16-bit from the mixer (the mixer resamples), and
  reports the sink's delay as its latency.
- `src/bin/bt_a2dp.cpp`: the test tool.

Changes to the shared Bluetooth stack it needed, each in its own commit:

- L2CAP: refused and pending connection responses, feature mask answers;
  signalling timeouts; dynamic channels closed when the link goes;
  `getsockopt()` for the channel MTUs.
- HCI: ACL data cut at the controller's packet size instead of 48 bytes,
  and held back until the controller has a buffer (Number Of Completed
  Packets flow control).
- bluetooth_server: link keys written when they arrive and read back
  correctly, so pairings survive a restart; pairing events handled even
  when no request registered for them; "Just Works" accepted for a device
  without display or input that we connected to; the encryption state in
  the connection state.

## Licences

The SBC encoder and the AVDTP code are new, MIT licensed, written from the
Bluetooth specifications (A2DP 1.3, AVDTP 1.3, Core 5.x). The analysis
filter coefficients are the tables of the A2DP specification.
