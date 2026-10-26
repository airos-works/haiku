# nvidia_rm.accelerant

Accelerant for the `nvidia_rm` kernel driver. It sets display modes through
NVIDIA's kernel mode setting interface (NVKMS).

The sources were imported from X547's nvidia-haiku
(https://github.com/X547/nvidia-haiku, commit
cc1849cb2c5c327af4f7eccf993b8616855ad7c5, MIT License, see `LICENSE.X547`):
`accelerant/` and the C++ client SDK in `sdk/` (from `nvrm_cpp_sdk/`).

## 2D acceleration

`NvEngine2D` puts a graphics channel of the accelerant's own on the card
(`PASCAL_CHANNEL_GPFIFO_A`, else `MAXWELL_CHANNEL_GPFIFO_A`; later cards want
a doorbell this does not ring, and get no engine) with the Fermi 2D class
(`FERMI_TWOD_A`) on it. Work is a list of methods in a push buffer; every
submission ends with a semaphore release, which is what waiting compares
against. A submission that has not finished after 2 s marks the engine lost:
every operation does nothing from then on and callers fall back to the CPU.

What uses it:

- The classic engine and 2D hooks (`B_ACQUIRE_ENGINE` ... `B_SYNC_TO_TOKEN`,
  `B_SCREEN_TO_SCREEN_BLIT`, `B_FILL_RECTANGLE`, `B_INVERT_RECTANGLE`,
  `B_FILL_SPAN`, `B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT`,
  `B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT`), on the frame buffer. The engine
  is pointed at every new frame buffer a mode set makes. These are for
  BWindowScreen programs: app_server dropped them (upstream, 2024).
- A clone (BWindowScreen's, in another team) duplicates the frame buffer
  app_server published (`NV_HAIKU_GET_SCANOUT`), draws on it, follows it
  across mode sets, and offers no mode, layout, DPMS or cursor hooks. Before,
  a clone grabbed NVKMS ownership and applied a layout of its own.
- `B_CREATE_FRAME_BUFFER_SOURCE` / `B_DELETE_FRAME_BUFFER_SOURCE` /
  `B_COPY_TO_FRAME_BUFFER`: memory in the system that the engine reads over
  the bus (snooped, CPU-cached: reading and writing it from the CPU runs at
  heap speed), copied by the engine to the same place on the screen.
  app_server's vertical blank flush keeps its two shadows of the back buffer
  there and hands its rects to the engine instead of copying with the CPU.

Engine quirks found on the GTX 1070 (GP104):

- `SET_COLOR_KEY` alone makes matching pixels transparent black; the "and"
  copy (`SET_OPERATION SRCCOPY_AND`) leaves them out, but it also leaves out
  every pixel whose alpha is zero. Transparent blits therefore read the source
  as `X8R8G8B8`: only the colour key decides, compared on the colour.
- Blits in one list may overlap each other: the engine is pipelined and reads
  through its texture cache, so each one after the first waits for idle and
  invalidates the cache. A single overlapping blit (a scroll) is fine with
  `SET_PIXELS_FROM_MEMORY_SAFE_OVERLAP`.
- Resman brings the card up when its node is opened and takes it down when
  the last one closes: a test without app_server on the card must hold
  `/dev/graphics/nvidia0` open, or allocating the device fails with
  `NV_ERR_INVALID_STATE`.
- System memory with big pages is refused (`NV_ERR_INVALID_STATE`); sources
  use 4 KiB pages.

Settings (`~/config/settings/nvidia_rm_accelerant`): `acceleration 2d off`
turns the engine off (no hooks; app_server copies with the CPU).
`force_edid HDMI-0 <file>` forces a connector on with the EDID in the file, no
monitor needed on a TMDS connector: a test rig. In app_server,
`~/config/settings/app_server/no_device_copy` switches the flush back to CPU
copies while it exists (read every half second).

Tests, none of which need a monitor: `nv2dtest [w h [timings]]` checks every
operation of the engine against the CPU on surfaces of its own and times it;
`nvclone2dtest <accelerant>` checks every hook through a clone (it publishes a
surface of its own as the screen while app_server is not on the card);
`--attach` does the same on app_server's real screen and puts the pixels back;
`--init` stands in for app_server (only while app_server is on another driver).
