#!/bin/bash
# mkscript.sh <out> <binary> <args...>: a script disk payload that unpacks the
# binary and runs it with the args, logging each output line as "NT ..."
out=$1; bin=$2; shift 2
{
	echo "#NETTEST#"
	echo 'mkdir -p /tmp/nt; cd /tmp/nt'
	echo "cat > /tmp/nt/bin.b64 <<'B64'"
	base64 "$bin"
	echo "B64"
	echo 'base64 -d /tmp/nt/bin.b64 > /tmp/nt/testbin; chmod +x /tmp/nt/testbin'
	echo "/tmp/nt/testbin $* 2>&1 | while read line; do echo \"NT \$line\" > /dev/dprintf; done"
} > "$out"
