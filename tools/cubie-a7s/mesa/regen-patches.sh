#!/usr/bin/env bash
# Regenerates mesa-haiku-pvr.patch and mesa-haiku-gl.patch from the working
# tree against the pristine unpack (README.md, "To change a patch"), keeping
# each patch's header (the text before its first "diff" line), and records
# them as applied so that build.sh does not revert them.
#   regen-patches.sh [ROOT]      (ROOT: CUBIE_MESA_ROOT or /mnt/HaikuWork/cubie/mesa)
set -euo pipefail
ROOT=${1:-${CUBIE_MESA_ROOT:-/mnt/HaikuWork/cubie/mesa}}
VERSION=26.2.4
SRC=$ROOT/mesa-$VERSION
PRISTINE=$ROOT/pristine/mesa-$VERSION
TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ -d "$PRISTINE" ] || { echo "no pristine tree at $PRISTINE" >&2; exit 1; }

# The OpenGL patch owns EGL, Gallium, the GL state tracker and what they
# needed; the Vulkan patch owns the rest (the PowerVR driver and compiler,
# the DRM headers, the sync provider, the Vulkan runtime).
is_gl_file() {
	case "$1" in
	src/egl/*|src/gallium/*|src/mesa/*|src/c11/*|include/renderdoc_app.h|\
	src/compiler/nir/nir_opt_varyings.c|src/util/disk_cache.c) return 0 ;;
	*) return 1 ;;
	esac
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
(cd "$ROOT" && diff -ruN -x __pycache__ -x '*.pyc' -x '.haiku-*' \
	"pristine/mesa-$VERSION" "mesa-$VERSION" \
	| sed -E -e "s|^diff .* pristine/mesa-$VERSION/([^ ]*) mesa-$VERSION/[^ ]*$|diff -ruN a/\1 b/\1|" \
	      -e "s|^--- pristine/mesa-$VERSION/([^\t]*)\t.*|--- a/\1|" \
	      -e "s|^\+\+\+ mesa-$VERSION/([^\t]*)\t.*|+++ b/\1|" \
	> "$tmp/all.diff") || true

# Split by file.
awk -v out="$tmp" '
/^diff -ruN a\// {
	file = $3; sub(/^a\//, "", file);
	name = file; gsub(/\//, "__", name);
	target = out "/f__" name;
	print file > (out "/files");
}
{ print > target }
' "$tmp/all.diff"

for patch in mesa-haiku-pvr.patch mesa-haiku-gl.patch; do
	awk '/^diff /{exit} {print}' "$TOOLS/$patch" > "$tmp/$patch"
done
while read -r file; do
	name=${file//\//__}
	if is_gl_file "$file"; then
		cat "$tmp/f__$name" >> "$tmp/mesa-haiku-gl.patch"
	else
		cat "$tmp/f__$name" >> "$tmp/mesa-haiku-pvr.patch"
	fi
done < <(sort "$tmp/files")

for patch in mesa-haiku-pvr.patch mesa-haiku-gl.patch; do
	cp "$tmp/$patch" "$TOOLS/$patch"
done
applied=$SRC/.haiku-patches
mkdir -p "$applied"
cp "$TOOLS/mesa-haiku-pvr.patch" "$applied/1-mesa-haiku-pvr.patch"
cp "$TOOLS/mesa-haiku-gl.patch" "$applied/2-mesa-haiku-gl.patch"
(cd "$TOOLS" && cat mesa-haiku-pvr.patch mesa-haiku-gl.patch | sha256sum | cut -d' ' -f1) \
	> "$applied/sha256"
echo "pvr: $(grep -c '^diff ' "$TOOLS/mesa-haiku-pvr.patch") files, gl: $(grep -c '^diff ' "$TOOLS/mesa-haiku-gl.patch") files"
