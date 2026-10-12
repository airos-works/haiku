#!/bin/bash
# Works the WX 5100's whole GPU stack (amdgpu_drm, radeonsi, RADV, UVD
# through VA-API) at once, and in the ways programs leave it, then checks
# that the machine is still there: nothing new from the driver in the
# syslog, no kernel memory gone, and pictures still drawn and decoded
# exactly.
#
# - everything at once: three OpenGL windows, H.264 and HEVC Main 10 decoded
#   over and over (each run's MD5s compared with the references in
#   gpu-userland/tests/media), Vulkan compute, fill and copies;
# - OpenGL windows opened and closed, and killed while they draw;
# - decoders killed in the middle of a picture;
# - Vulkan windows on the Wayland server, started and ended.
#
# usage: gpu-soak.sh [seconds of everything at once]
set -u
SSH="ssh -F /mnt/HaikuWork/x399/ssh/config -o ConnectTimeout=10 ws-haiku"
T=${1:-120}
$SSH "set -u
	G=/boot/home/gpu-userland
	M=\$G/tests/media
	cd /boot/home/tests
	# A blanked desktop shows no windows, so nothing would be presented.
	kill \$(ps | grep '[s]creen_blanker' | awk '{print \$2}') 2>/dev/null
	# By team name: a ps pattern also matches this shell's own command line.
	reap() {
		for p in gltest vadecode vkbench vkwayland; do
			kill -9 \$(pidof \$p) 2>/dev/null
		done
		sleep 2
	}
	used() { sysinfo -mem | sed -n 's/.*used\/max \([0-9]*\).*/\1/p'; }
	kernel_areas() { listarea 1 | wc -l; }
	# decode STREAM CODEC: 0 when every picture matches the reference
	decode() {
		\$G/tests/vadecode \$2 \$M/\$1 2>/dev/null | cmp -s - \$M/\$1.va.md5
	}
	reap
	before_lines=\$(wc -l < /var/log/syslog)
	before_used=\$(used)
	before_areas=\$(kernel_areas)

	echo '== everything at once for $T s'
	GLTEST_POS=60,60   GLTEST_SIZE=800x600 ./gltest $T 2 >/tmp/soak1.log 2>&1 &
	GLTEST_POS=900,80  GLTEST_SIZE=600x400 ./gltest $T 2 >/tmp/soak2.log 2>&1 &
	GLTEST_POS=500,560 GLTEST_SIZE=700x450 ./gltest $T 2 >/tmp/soak3.log 2>&1 &
	for s in 'bbb.h264 h264' 'hevc10.hevc hevc'; do
		(set -- \$s; end=\$((SECONDS + $T)); runs=0; bad=0
		while [ \$SECONDS -lt \$end ]; do
			decode \$1 \$2 || bad=\$((bad + 1)); runs=\$((runs + 1))
		done
		echo \"  \$1: \$runs decodes, \$bad not exact\") &
	done
	(cd \$G/vktests; end=\$((SECONDS + $T)); runs=0; failed=0
	while [ \$SECONDS -lt \$end ]; do
		./vkbench compute fill copy >/tmp/soak-vk.log 2>&1 || failed=\$((failed + 1))
		runs=\$((runs + 1))
	done
	echo \"  vkbench: \$runs runs, \$failed failed\") &
	wait
	for f in /tmp/soak1.log /tmp/soak2.log /tmp/soak3.log; do
		grep -ao '[0-9.]* fps.*' \$f || echo \"  [!] \$f: no frame rate\"
	done
	reap

	echo '== OpenGL windows opened and closed, over and over'
	for i in \$(seq 20); do
		GLTEST_POS=\$((100 + i * 20)),\$((100 + i * 10)) ./gltest 0.4 1 >/dev/null 2>&1
	done
	echo '  20 windows came and went'
	reap

	echo '== killed while the GPU draws or decodes'
	for i in \$(seq 5); do
		(GLTEST_POS=300,300 GLTEST_SIZE=800x600 ./gltest 30 2 >/dev/null 2>&1 &)
		(\$G/tests/vadecode hevc \$M/hevc10.hevc >/dev/null 2>&1 &)
		sleep 3
		reap
	done
	echo '  5 OpenGL windows and 5 decoders killed'

	echo '== Vulkan windows on the Wayland server'
	ok=0
	for i in \$(seq 10); do
		\$G/vktests/vkwayland 60 640 480 mailbox >/dev/null 2>&1 && ok=\$((ok + 1))
	done
	echo \"  \$ok of 10 ended cleanly (the server's exit race takes some)\"
	reap

	echo '== afterwards'
	sleep 5
	echo \"  memory in use: \$(( (\$(used) - before_used) / 1048576 )) MiB more than before\"
	echo \"  kernel areas: \$((\$(kernel_areas) - before_areas)) more than before\"
	echo '  anything new in the log the driver is unhappy about:'
	tail -n +\$before_lines /var/log/syslog \
		| grep -ai 'amdgpu\|drm' \
		| grep -ai 'error\|fail\|timeout\|reset\|fault\|hang\|lockup' | tail -10
	echo \"  \$(uptime)\"
	echo '== and it still works'
	GLTEST_SIZE=800x600 ./gltest 3 2 2>&1 | grep -ao '[0-9.]* fps.*'
	decode hevc10.hevc hevc && echo '  HEVC Main 10 decodes exactly' \
		|| echo '  [!] HEVC Main 10 no longer decodes exactly'
	reap"
