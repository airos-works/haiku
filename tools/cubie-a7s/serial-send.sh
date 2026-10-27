#!/usr/bin/env bash
# Type into the Cubie A7S serial console (the NanoKVM's UART).
#   serial-send.sh 'text'    (printf escapes such as \r and \003 work)
# The output shows up in the serial-logger.py log.
set -euo pipefail
TTY=${CUBIE_KVM_TTY:-/dev/ttyS1}
printf '%s' "$1" | ssh -F "${NANOKVM_SSH_CONFIG:-/mnt/HaikuWork/nanokvm/.ssh/config}" nanokvm \
	"cat > /tmp/serial-send.$$ && printf \"\$(cat /tmp/serial-send.$$)\" > $TTY; rm -f /tmp/serial-send.$$"
