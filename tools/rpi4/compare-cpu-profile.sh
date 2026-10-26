#!/usr/bin/env bash
# Compare two build directories that differ only in their CPU profile
# (RPI4_CPU_PROFILE, see tools/rpi4/UserBuildConfig): calls to libgcc's
# outlined atomic helpers and code size of the main system binaries.
#   compare-cpu-profile.sh <generic build dir> <tuned build dir>
set -euo pipefail
TOOLS=${CROSS_TOOLS:-/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-}
FILES=(
	system/kernel_arm64
	system/libroot.so
	system/libbe.so
	system/servers/app_server
	system/servers/registrar
	system/servers/input_server
	system/servers/net_server
	system/libnetwork.so
	system/libmedia.so
	system/libtracker.so
)

find_binary()
{
	local found
	found=$(find "$1/objects/haiku/arm64/release" -path "*/$2" -type f 2>/dev/null | head -1)
	[ -n "$found" ] || found=$(find "$1/objects/haiku/arm64/release" \
		-name "$(basename "$2")" -type f 2>/dev/null | head -1)
	echo "$found"
}

helper_calls()
{
	"${TOOLS}objdump" -d --no-show-raw-insn "$1" \
		| grep -c -E 'bl[[:space:]]+[0-9a-f]+ <__aarch64_(cas|swp|ldadd|ldclr|ldeor|ldset)' || true
}

text_size()
{
	"${TOOLS}size" -A "$1" | awk '$1 == ".text" { print $2 }'
}

printf '%-16s %12s %12s %10s %10s %8s\n' binary helper_calls helper_calls text text change
printf '%-16s %12s %12s %10s %10s %8s\n' "" generic tuned generic tuned ""
for file in "${FILES[@]}"; do
	a=$(find_binary "$1" "$file")
	b=$(find_binary "$2" "$file")
	if [ -z "$a" ] || [ -z "$b" ]; then
		echo "$file: not built in both directories" >&2
		continue
	fi
	ta=$(text_size "$a")
	tb=$(text_size "$b")
	printf '%-16s %12s %12s %10s %10s %7.2f%%\n' "$(basename "$file")" \
		"$(helper_calls "$a")" "$(helper_calls "$b")" "$ta" "$tb" \
		"$(echo "scale=4; ($tb - $ta) * 100 / $ta" | bc)"
done
