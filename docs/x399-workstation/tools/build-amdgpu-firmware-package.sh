#!/usr/bin/env bash
# Build the exact Polaris10 firmware set used by the WX5100 driver trials.
# Binary inputs stay outside git; hashes and AMD's redistribution notice travel
# with the package. This does not enable the still-unqualified GFX client path.
# Usage: build-amdgpu-firmware-package.sh [firmware-directory [output-directory]]
set -euo pipefail
base=$(cd -- "$(dirname -- "$0")/.." && pwd)
firmware=${1:-/mnt/HaikuWork/x399/firmware/amdgpu}
output=${2:-/mnt/HaikuWork/x399/pkgs/firmware}
tool=/mnt/HaikuWork/x399/build/x86_64/objects/linux/x86_64/release/tools/package/package
[ -x "$tool" ] || { echo "Missing package tool: $tool" >&2; exit 1; }
manifest=$base/firmware/polaris10.sha256
(cd -- "$firmware" && sha256sum --strict -c "$manifest")
mkdir -p /mnt/HaikuWork/tmp "$output"
stage=$(mktemp -d /mnt/HaikuWork/tmp/amdgpu-firmware-XXXXXX)
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/data/firmware/amdgpu" "$stage/data/licenses" "$stage/documentation/packages/amdgpu_polaris10_firmware"
while read -r digest file; do
	cp -- "$firmware/$file" "$stage/data/firmware/amdgpu/$file"
	chmod 0444 "$stage/data/firmware/amdgpu/$file"
done < "$manifest"
(cd -- "$stage/data/firmware/amdgpu" && sha256sum --strict -c "$manifest")
cp -- "$base/firmware/LICENSE.amdgpu" "$stage/data/licenses/AMD Firmware"
cp -- "$manifest" "$stage/documentation/packages/amdgpu_polaris10_firmware/polaris10.sha256"
cat > "$stage/.PackageInfo" <<'PACKAGE'
name amdgpu_polaris10_firmware
version 20261010-3
architecture any
summary "Pinned AMD Polaris10 firmware for the air/OS WX5100 driver"
description "AMD firmware used by the WX5100 native driver: SMC 0x171a00, SDMA 58, RLC 286 and UVD 0x01008210. The matching feature-49 graphics set is CE 140, PFP 254, ME 167 and MEC 730; the earlier feature-46 CE/PFP/ME images are retained for diagnostics. SMC/SDMA and H.264/HEVC sessions load installed firmware. GFX remains an explicit root diagnostic; general graphics clients are not enabled. The binaries are distributed unchanged from linux-firmware. Their SHA-256 manifest is included."
packager "air/OS"
vendor "air/OS"
licenses { "AMD Firmware" }
copyrights { "2023 Advanced Micro Devices, Inc." }
provides { amdgpu_polaris10_firmware = 20261010 }
urls { "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git" }
PACKAGE
package=$output/amdgpu_polaris10_firmware-20261010-3-any.hpkg
(cd -- "$stage" && "$tool" create -q "$package")
sha256sum "$package"
