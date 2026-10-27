#!/usr/bin/env bash
# One hardware iteration on the Cubie A7S lab: QEMU smoke test, back to the
# lab card's Debian (power cycle if Haiku runs), deploy the image, boot it
# once and print the serial lines that match a pattern.
#
#   lab-run.sh [-n] [bfs image] [pattern] [seconds]
#
#   -n         skip the QEMU smoke test (already run for this image)
#   pattern    extended regex for the lines to show (default: driver
#              messages, panics); the wait ends at the first DP-1 picture,
#              a kernel debugger entry or the fallback to Debian
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ -z ${CUBIE_BOARD_LOCKED:-} ]]; then
	exec "$HERE/board-lock.sh" "$0" "$@"
fi
smoke=1
if [[ ${1:-} == -n ]]; then
	smoke=0
	shift
fi
BUILD=${CUBIE_BUILD:-/mnt/HaikuWork/cubie/build}
IMAGE=${1:-$BUILD/haiku-cubie-minimum.image}
PATTERN=${2:-'sunxi_display: (DP-1|link)|powervr|sunxi_regulator|PANIC|Debugging Land'}
WAIT=${3:-300}
CUBIE_STATE=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}
LOG=${CUBIE_SERIAL_LOG:-/mnt/HaikuWork/cubie/evidence/serial.log}
POWER=(env RPI4_STATE="$CUBIE_STATE" python3
	/mnt/HaikuWork/rpi4/haiku/tools/rpi4/power.py)
SSH=(ssh -F "$CUBIE_STATE/ssh_config" cubie-recovery)

if (( smoke )); then
	"$HERE/qemu-smoke.sh" "$IMAGE"
fi
if ! "${SSH[@]}" true 2>/dev/null; then
	# the plug drops off Home Assistant now and then (Zigbee route):
	# wait for it to come back rather than fail the cycle
	deadline=$((SECONDS + 1200))
	until "${POWER[@]}" status 2>/dev/null | grep -q -E '"switch": "(on|off)"'; do
		(( SECONDS < deadline )) || { echo "the plug stays unavailable" >&2; exit 1; }
		echo "waiting for the plug"
		sleep 30
	done
	echo "power cycling into Debian"
	"${POWER[@]}" cycle > /dev/null
	deadline=$((SECONDS + 240))
	until "${SSH[@]}" true 2>/dev/null; do
		if (( SECONDS >= deadline )); then
			# after a thermal power-off the board comes back only from a
			# long plug-off; a 5 s cycle leaves it dark
			if [[ -n ${long_cycle:-} ]]; then
				echo "Debian does not come up" >&2
				exit 1
			fi
			long_cycle=1
			echo "no Debian: power cycling with a 60 s plug-off"
			RPI4_OFF_SECONDS=60 "${POWER[@]}" cycle > /dev/null
			deadline=$((SECONDS + 240))
		fi
		sleep 3
	done
fi

start=$(($(wc -l < "$LOG") + 1))
"$HERE/deploy-haiku.sh" "$IMAGE" | tail -1
deadline=$((SECONDS + WAIT))
end='DP-1: [0-9]+x[0-9]+[ ,]|no picture|Kernel Debugging Land|booting Debian'
until tail -n +"$start" "$LOG" | grep -a -q -E "$end"; do
	if (( SECONDS > deadline )); then
		echo "(no end marker within $WAIT s)"
		break
	fi
	sleep 3
done
sleep 5
# the powervr firmware stage can end after the picture: wait for its summary
done_gpu='powervr: (.*firmware stage (passed|FAILED|off)|disabled by)|Kernel Debugging Land'
deadline=$((SECONDS + 60))
until tail -n +"$start" "$LOG" | grep -a -q -E "$done_gpu"; do
	(( SECONDS < deadline )) || break
	sleep 2
done
tail -n +"$start" "$LOG" | cut -c14- | grep -a -E "$PATTERN" || true
