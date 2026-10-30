#!/bin/bash
# Build one of tests/*.c against the NVDEC decoder, on the workstation.
# Usage: build-nvdec.sh <name> [extra gcc arguments...]
# HAIKU is the tree to build from, REMOTE where on the workstation.
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
HAIKU=${HAIKU:-$X399/haiku}
REMOTE=${REMOTE:-/boot/home/build/nvdec}
PLUGIN=src/add-ons/media/plugins/nvdec
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
name=$1; shift
$SSH "mkdir -p $REMOTE"
scp -F $X399/ssh/config -q $HAIKU/docs/x399-workstation/tests/$name.c \
	$HAIKU/$PLUGIN/* ws-haiku:$REMOTE/
# VP9's parser is sunxi_cedar's.
scp -F $X399/ssh/config -q $HAIKU/src/add-ons/media/plugins/sunxi_cedar/{vp9_parse,vp9_tables,bits}.[ch] \
	ws-haiku:$REMOTE/
$SSH "cd $REMOTE \
	&& NVRM=/boot/home/build/src/mesa-nvk/src/nouveau/vulkan/nvkmd/nvrm \
	&& O=\$NVRM/open-gpu-kernel-modules \
	&& gcc -O2 -g -o $name $name.c nvdec_engine.c nvdec_h264.c h264_parse.c \
		nvdec_hevc.c hevc_parse.c nvdec_vp9.c nvdec_vp9_tables.c vp9_parse.c \
		vp9_tables.c bits.c nvdec_convert.c \
		\$NVRM/nvRmApi.c \
		-I. -I\$NVRM \
		-I\$O/src/common/sdk/nvidia/inc \
		-I\$O/src/nvidia/arch/nvalloc/common/inc \
		-I\$O/src/nvidia/arch/nvalloc/unix/include \
		-I\$O/src/nvidia/inc/kernel \
		-I/boot/home/build/src/mesa-nvk/src/nouveau/headers/nvidia \
		$*"
