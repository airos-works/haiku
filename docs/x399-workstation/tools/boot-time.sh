#!/bin/bash
# Power cycle the X399 under the lab lock and time the boot from the outside:
# when ssh answers, and where the kernel's clock sat at that moment, so that
# syslog time stamps can be placed against the power button. Keeps the
# launch_daemon log and the syslog of that boot in evidence/boot-time/<name>.
#
# usage: boot-time.sh <name>
#
# Reading the result: the syslog line "boot: firmware N ms, boot loader N ms
# before the kernel" splits the time before the kernel; kernel settings
# "syslog_time_stamps true" (and a bigger syslog_buffer_size) put seconds
# since the kernel started in front of every kernel line; `launch_roster log`
# dates every service start. A boot cut short leaves
# ~/config/settings/kernel/drivers/amdgpu_drm.starting behind and the next
# boot runs on the framebuffer, so the marker is removed before cycling.
set -u
name=${1:-boot}
X399=${X399:-/mnt/HaikuWork/x399}
out=$X399/evidence/boot-time/$name; mkdir -p $out
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=3 ws-haiku"
log=$out/timeline.txt
now() { date +%s.%N; }
exec 9>$X399/state/lab.lock
flock 9
echo "lock acquired $(date +%T)" > $log
$SSH 'rm -f /boot/home/config/settings/kernel/drivers/amdgpu_drm.starting; sync' >/dev/null 2>&1
$X399/tools/power-cycle.sh >> $log 2>&1
t_on=$(now); echo "power_on $t_on" >> $log
while :; do
	t=$(now); el=$(python3 -c "print(round($t-$t_on,1))")
	if $SSH 'echo up' >/dev/null 2>&1; then
		echo "ssh_up +$el" >> $log; break
	fi
	python3 -c "import sys; sys.exit(0 if $el > 400 else 1)" && { echo "gave up at +$el" >> $log; break; }
	sleep 1
done
sleep 25
# kernel start on the wall clock: remote now, remote now minus the kernel's
# clock, and local now, so that the clock skew can be taken out
remote=$($SSH 'python3 -c "import time; print(time.time(), time.time()-time.monotonic())"')
echo "remote_now_and_boot $remote local_now $(now)" >> $log
python3 - "$t_on" $remote "$(now)" >> $log <<'PY'
import sys
on, rnow, rboot, lnow = map(float, sys.argv[1:5])
print("kernel_start +%.1f s after power on" % (rboot - (rnow - lnow) - on))
PY
$SSH 'launch_roster log' > $out/launch_log.txt 2>&1
$SSH 'cat /var/log/syslog' > $out/syslog.txt 2>&1
$SSH 'grep -a "boot: firmware\|boot:.*CPU reset" /var/log/syslog | tail -1' >> $log 2>&1
flock -u 9
echo "done $(date +%T)" >> $log
cat $log
