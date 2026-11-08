#!/bin/sh
# kswap.sh <kernel file> <revision> [<package path>=<file> ...]
#   (runs ON the air/OS machine)
# Repacks the active haiku package with this kernel and the given package
# revision and installs it with pkgman: the previous state stays selectable
# in the boot menu ("Select a previous state"). Nothing is rebooted.
set -e
K=$1; REV=$2
W=/boot/home/kswap
rm -rf $W/root; mkdir -p $W/root
name=$(cd /boot/system/packages && ls | grep '^haiku-r1' | head -1)
echo "base $name"
package extract -C $W/root /boot/system/packages/$name
kf=$(cd $W/root && ls | grep '^kernel_')
[ -f "$W/root/$kf" ] || { echo "no kernel in package"; exit 1; }
cat "$K" > "$W/root/$kf"
shift 2
for pair in "$@"; do
	t=${pair%%=*}; f=${pair#*=}
	[ -f "$W/root/$t" ] || { echo "missing $t in package"; exit 1; }
	cat "$f" > "$W/root/$t" && echo "swapped $t"
done
sed -i "s/^version\([ \t]*\)\(r1~beta6_hrev[0-9_]*\)-[0-9]*/version\1\2-$REV/" $W/root/.PackageInfo
grep '^version' $W/root/.PackageInfo
out=$W/$(echo $name | sed "s/-[0-9]*-\([a-z0-9_]*\)\.hpkg\$/-$REV-\1.hpkg/")
rm -f "$out"
package create -C $W/root -i $W/root/.PackageInfo "$out"
rm -rf $W/root
echo "created $out"
pkgman install -y "$out" 2>&1 | tail -4
ls /boot/system/packages | grep '^haiku-r1'
