# Interfaces of libraries MediaConverter opens at run time

MediaConverter does not link these libraries. It opens them with `dlopen()`
when an encode needs them, so it starts without them and says which package
is missing. These are the headers of the versions air/OS ships:

| Directory | Library | Package | Licence |
| --- | --- | --- | --- |
| `va/` | libva 2.24.1 (VA-API 1.24.0), from jmgasper/libva | the AMD GPU user land (`system/non-packaged/amdgpu`) | MIT (`va/COPYING`) |
| `x264/` | x264 0.164 | HaikuPorts `x264` | GPL-2.0-or-later |
| `x265/` | x265 3.5 (build 199) | HaikuPorts `x265` | GPL-2.0-or-later |
| `svt-av1/` | SVT-AV1 3.0.2 | HaikuPorts `svt_av13` | BSD-3-Clause-Clear |

`va/va_version.h` is generated from libva's `va_version.h.in` for 1.24.0.

The library versions are part of the interfaces: x264 exports
`x264_encoder_open_164`, x265 `x265_api_get_199`, SVT-AV1 has soname
`libSvtAv1Enc.so.3`. When a package moves to a new major version, update the
header here with it.
