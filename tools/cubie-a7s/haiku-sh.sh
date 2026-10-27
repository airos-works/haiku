#!/usr/bin/env bash
# Runs a command on the Cubie A7S lab board's Haiku through its telnet login
# (the lab overlay's account), with the Pi lab's shell.py.
#   haiku-sh.sh '<command>' [timeout seconds]
# CUBIE_HAIKU_HOST overrides the address (default: its DHCP lease).
exec env RPI4_STATE="${CUBIE_STATE:-/mnt/HaikuWork/cubie/state}" python3 \
	/mnt/HaikuWork/rpi4/haiku/tools/rpi4/shell.py \
	"${CUBIE_HAIKU_HOST:-192.168.1.118}" "$1" "${2:-60}"
