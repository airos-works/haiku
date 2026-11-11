#!/bin/sh
# Synthetic pictures through nvenctest at the sizes given, compared with
# the source (luma PSNR).
#   [CODEC=h264|hevc] [N=pictures] [QP=24] [KEY=30] syn.sh 352x288 1920x1080 ...
# Needs `nvenctest` (test/build-nvenctest.sh) and `mediaconvert` beside it;
# NVENC_ADDON names the add-on. Even sizes only: the comparison reads the
# source as 4:2:0 with whole chroma lines.
cd "$(dirname "$0")"
mkdir -p out
addon=${NVENC_ADDON:-/boot/system/non-packaged/add-ons/media/encoders/nvenc}
codec=${CODEC:-h264}
n=${N:-60}
cd out
for s in "$@"; do
	w=${s%x*}; h=${s#*x}
	../nvenctest "$addon" $codec $w $h $n syn_$s.264 ${QP:-24} 0 ${KEY:-30} syn_$s.yuv > syn_$s.log 2>&1
	rc=$?
	fs=$((w * h * 3 / 2))
	{
		printf 'YUV4MPEG2 W%d H%d F30:1 Ip A1:1 C420jpeg\n' $w $h
		i=0
		while [ $i -lt $n ]; do
			printf 'FRAME\n'
			dd if=syn_$s.yuv bs=$fs skip=$i count=1 2>/dev/null
			i=$((i + 1))
		done
	} > syn_$s.y4m
	rm -f syn_$s.yuv
	echo "$s rc=$rc $(tail -n 1 syn_$s.log) | $(COMPARE_BY_ORDER=1 ../mediaconvert --compare syn_$s.y4m syn_$s.264 2>&1 | tail -1)"
	rm -f syn_$s.y4m
done
