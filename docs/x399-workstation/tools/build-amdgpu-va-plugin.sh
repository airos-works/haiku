#!/bin/bash
# Build the AMD VA-API media add-on on the workstation and install it for the
# user as "amduvd" (the name airTime and Summit load AMD decoders by).
#
# It is built there rather than with the rest of the tree because it links
# libavcodec with VA-API statically (the system's FFmpeg has no hardware
# decoding, and a second shared one would clash with it) and needs libva:
# both come from the GPU user land of amdgpu_drm (src/forks/linux-drm,
# haiku/tools/x399-gpu-userland.sh ... libva ffmpeg).
#
# The add-on finds libva in /boot/system/non-packaged/lib through its RPATH:
# Summit's LIBRARY_PATH leaves that directory out.
#
# HAIKU is the tree to build from (this one by default), REMOTE where on the
# workstation to build it. With NO_INSTALL=1 the add-on is only built.
# Installing replaces the file with a new one rather than writing over it:
# programs that have it loaded keep the old one.
set -eu
X399=${X399:-/mnt/HaikuWork/x399}
HAIKU=${HAIKU:-$(cd "$(dirname "$0")/../../.." && pwd)}
REMOTE=${REMOTE:-/boot/home/build/amdgpu_va}
GPU=${GPU:-/boot/home/gpu-userland}
PLUGINS=src/add-ons/media/plugins
SSH="ssh -F $X399/ssh/config -o ConnectTimeout=10 ws-haiku"
TARGET=/boot/home/config/non-packaged/add-ons/media/plugins
INSTALL="mkdir -p $TARGET && cp amduvd $TARGET/.amduvd.new && mv $TARGET/.amduvd.new $TARGET/amduvd && echo installed $TARGET/amduvd"
[ "${NO_INSTALL:-0}" = 1 ] && INSTALL="echo built $REMOTE/amdgpu_va/amduvd"

tar -C "$HAIKU/$PLUGINS" -cf - amdgpu_va amduvd \
	| $SSH "rm -rf $REMOTE && mkdir -p $REMOTE && tar -C $REMOTE -xmf -"
$SSH "cd $REMOTE/amdgpu_va \
	&& F=$GPU/ffmpeg-va P=/boot/system/non-packaged \
	&& INC=\"-I\$F/include -I\$P/develop/headers \
		-I/boot/system/develop/headers/private/media\" \
	&& g++ -O2 -Wall -fPIC -c AmdgpuVaPlugin.cpp \$INC \
	&& g++ -O2 -fPIC -c ../amduvd/H264Output.cpp \$INC \
	&& g++ -O2 -fPIC -c ../amduvd/HevcOutput.cpp \$INC \
	&& g++ -shared -o amduvd AmdgpuVaPlugin.o H264Output.o HevcOutput.o \
		-Wl,--exclude-libs,ALL \$F/lib/libavcodec.a \$F/lib/libavutil.a \
		-L\$P/lib -lva -lva-drm -lbe -lmedia -Wl,-rpath,\$P/lib \
	&& $INSTALL"
