#!/bin/sh
# build-queue.sh [-r V] V... (in the VM, as /build/q.sh) : resume the configured tree of V (-r: make + package only),
# then extract, port and build each release into /build/dist.
export PATH=/build/py27/bin:$PATH
JOBS=${JOBS:-16}
pkg() {	# package an already built tree (build-node.sh's install step)
	SRC=/build/src/node-v$1 NAME=node-v$1-haiku-x64 OUT=/build/dist
	cd $SRC && make -j$JOBS && rm -rf out/stage &&
	make -j$JOBS install DESTDIR=$SRC/out/stage/$NAME PORTABLE=1 &&
	cp README.md LICENSE CHANGELOG.md out/stage/$NAME/ && mkdir -p $OUT &&
	( cd out/stage && tar -cf $OUT/$NAME.tar $NAME ) &&
	gzip -c -f -9 $OUT/$NAME.tar > $OUT/$NAME.tar.gz &&
	xz -c -f -9e -T0 $OUT/$NAME.tar > $OUT/$NAME.tar.xz && rm -f $OUT/$NAME.tar
}
if [ "$1" = -r ]; then
	V=$2; shift 2; L=/build/logs/build-$V.log
	{ echo "== $V resume $(date)"; pkg $V && echo "== $V OK $(date)" || echo "== $V FAILED $(date)"; } >> $L 2>&1
	tail -1 $L
fi
for V in "$@"; do
	L=/build/logs/build-$V.log
	{
	echo "== $V start $(date)"
	cd /build/src && rm -rf node-v$V && tar xJf /build/official/node-v$V.tar.xz &&
	python3.10 /build/port/port-haiku.py node-v$V &&
	JOBS=$JOBS sh /build/tools/build-node.sh /build/src/node-v$V /build/dist &&
	echo "== $V OK $(date)" || echo "== $V FAILED $(date)"
	} > $L 2>&1
	tail -1 $L
done
