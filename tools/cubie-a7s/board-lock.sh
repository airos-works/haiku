#!/usr/bin/env bash
# Run a command while holding the Cubie A7S lab board, so that only one
# deploy-and-test runs at a time. Wrap the whole iteration (lab-run.sh and
# the checks after it) when several sessions share the board:
#
#   board-lock.sh bash -c 'lab-run.sh image; haiku-sh.sh "..."'
#
# lab-run.sh takes the lock itself when it is run on its own. Nested calls
# (CUBIE_BOARD_LOCKED set) run the command directly. Waits up to
# CUBIE_BOARD_WAIT seconds (default 3600).
set -euo pipefail
if [[ -n ${CUBIE_BOARD_LOCKED:-} ]]; then
	exec "$@"
fi
state=${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}
lock=$state/board.lock
exec 9>> "$lock"
# First come, first served: flock alone lets any waiter in, and a session
# could wait an hour while later ones went ahead. Each waiter queues a
# ticket (time.pid) and takes the lock only when its ticket is the oldest.
queue=$state/board.queue
mkdir -p "$queue"
ticket=$queue/$(date +%s%N).$$
echo "${CUBIE_BOARD_OWNER:-$(whoami)}" > "$ticket"
trap 'rm -f "$ticket"' EXIT
deadline=$((SECONDS + ${CUBIE_BOARD_WAIT:-3600}))
waiting=
while :; do
	tickets=()
	for t in "$queue"/*; do
		# drop the tickets of waiters that went away
		if [[ -e $t ]] && ! kill -0 "${t##*.}" 2>/dev/null; then
			rm -f "$t"
		elif [[ -e $t ]]; then
			tickets+=("$t")
		fi
	done
	if [[ ${tickets[0]:-} == "$ticket" ]] && flock -n 9; then
		break
	fi
	if [[ -z $waiting ]]; then
		ahead=0
		while [[ ${tickets[ahead]:-$ticket} != "$ticket" ]]; do
			ahead=$((ahead + 1))
		done
		echo "board-lock: waiting for the board ($(cat "$lock.owner" \
			2>/dev/null); $ahead queued ahead)" >&2
		waiting=1
	fi
	if (( SECONDS >= deadline )); then
		echo "board-lock: timed out" >&2
		exit 75
	fi
	sleep 5
done
rm -f "$ticket"
echo "$$ ${CUBIE_BOARD_OWNER:-$(whoami)} since $(date -u +%H:%MZ)" > "$lock.owner"
export CUBIE_BOARD_LOCKED=1
"$@"
