#!/usr/bin/env bash
# Build standalone pixel checks against the same pinned EGL/GLES sysroot.
set -euo pipefail
BASE=${RPI4_MESA_BASE:-/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z}
ROOT=${RPI4_MESA_ROOT:-/mnt/HaikuWork/rpi4/mesa}
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CXX=${RPI4_CXX:-/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-g++}
mkdir -p "$ROOT/probes"
for probe in gl texture readback window cache_growth glview glview_present; do
    libraries=(-lEGL -lGLESv2 -lbe)
    [[ "$probe" != glview* ]] || libraries=(-lEGL -lGL -lbe)
    "$CXX" --sysroot="$BASE/sysroot" -std=c++17 -O2 \
        -I"$BASE/libglvnd-v1.7.0/src/HGL" \
        -I"$BASE/glvnd-install/boot/system/develop/headers/os/opengl" \
        -I"$BASE/sysroot/boot/system/develop/headers/os/opengl" \
        "$TOOLS/${probe}_probe.cpp" -L"$BASE/glvnd-install/boot/system/lib" \
        "${libraries[@]}" -o "$ROOT/probes/rpi4_${probe}_probe"
done

# The driver's CPU cost (compile, draw calls, texture uploads), for comparing
# Mesa builds with different code generation.
"$CXX" --sysroot="$BASE/sysroot" -std=c++17 -O2 \
    -I"$BASE/sysroot/boot/system/develop/headers/os/opengl" \
    "$TOOLS/cpu_bench.cpp" -L"$BASE/glvnd-install/boot/system/lib" \
    -lEGL -lGLESv2 -lbe -o "$ROOT/probes/rpi4_mesa_cpu_bench"
