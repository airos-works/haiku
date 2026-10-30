#!/bin/bash
# Build the NVDEC media add-on on the workstation and install it for the user.
#
# It is built there rather than with the rest of the tree because it needs
# NVIDIA's resource manager headers, which live in the NVK checkout on the
# machine and are not part of this repository.
#
# HAIKU is the tree to build from (the workstation checkout by default),
# REMOTE where on the workstation to build it. With NO_INSTALL=1 the add-on is
# only built. Installing replaces the file with a new one rather than writing
# over it: programs that have it loaded keep the old one.
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
HAIKU=${HAIKU:-$X399/haiku}
REMOTE=${REMOTE:-/boot/home/build/nvdec}
PLUGIN=src/add-ons/media/plugins/nvdec
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
TARGET=/boot/home/config/non-packaged/add-ons/media/plugins
INSTALL="mkdir -p $TARGET && cp nvdec $TARGET/.nvdec.new && mv $TARGET/.nvdec.new $TARGET/nvdec && echo installed $TARGET/nvdec"
[ "${NO_INSTALL:-0}" = 1 ] && INSTALL="echo built $REMOTE/nvdec"

$SSH "mkdir -p $REMOTE"
scp -F $X399/ssh/config -q $HAIKU/$PLUGIN/* ws-haiku:$REMOTE/
# VP9's parser is sunxi_cedar's.
scp -F $X399/ssh/config -q $HAIKU/src/add-ons/media/plugins/sunxi_cedar/{vp9_parse,vp9_tables,bits}.[ch] \
	ws-haiku:$REMOTE/
$SSH "cd $REMOTE \
	&& NVRM=/boot/home/build/src/mesa-nvk/src/nouveau/vulkan/nvkmd/nvrm \
	&& O=\$NVRM/open-gpu-kernel-modules \
	&& INC=\"-I. -I\$NVRM \
		-I\$O/src/common/sdk/nvidia/inc \
		-I\$O/src/nvidia/arch/nvalloc/common/inc \
		-I\$O/src/nvidia/arch/nvalloc/unix/include \
		-I\$O/src/nvidia/inc/kernel \
		-I/boot/home/build/src/mesa-nvk/src/nouveau/headers/nvidia \
		-I/boot/system/develop/headers/private/media\" \
	&& for f in nvdec_engine nvdec_h264 h264_parse nvdec_hevc hevc_parse \
			nvdec_vp9 nvdec_vp9_tables vp9_parse vp9_tables bits nvdec_convert; do \
		gcc -O2 -g -fPIC -c -o \$f.o \$f.c \$INC || exit 1; \
	done \
	&& gcc -O2 -g -fPIC -c -o nvRmApi.o \$NVRM/nvRmApi.c \$INC \
	&& g++ -O2 -g -fPIC -c -o NVDecPlugin.o NVDecPlugin.cpp \$INC \
	&& g++ -shared -o nvdec NVDecPlugin.o nvdec_engine.o nvdec_h264.o \
		h264_parse.o nvdec_hevc.o hevc_parse.o nvdec_vp9.o nvdec_vp9_tables.o \
		vp9_parse.o vp9_tables.o bits.o nvdec_convert.o nvRmApi.o \
		-lbe -lmedia \
	&& $INSTALL"
