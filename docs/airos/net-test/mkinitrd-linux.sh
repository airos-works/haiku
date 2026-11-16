#!/bin/bash
# mkinitrd-linux.sh <out.cpio.gz> <lsend args...>: an initramfs that brings
# up eth0 as 10.10.0.2 and runs lsend with the arguments (the host's static
# busybox; lsend built into $LINUXGUEST, see the README)
D=${LINUXGUEST:-/data1/scratch/linuxguest}; out=$1; shift
R=$(mktemp -d $D/root.XXXX)
mkdir -p $R/bin $R/proc $R/sys $R/dev
cp /usr/bin/busybox $R/bin/busybox
cp $D/lsend $R/lsend
echo "$*" > $R/args
cat > $R/init <<'EOI'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
ip link set lo up
ip link set eth0 up
ip addr add 10.10.0.2/24 dev eth0
sleep 3
echo "NT linux $(uname -r) $(cat /proc/sys/net/ipv4/tcp_congestion_control) wmem $(cat /proc/sys/net/ipv4/tcp_wmem)"
/lsend $(cat /args)
sleep 1
poweroff -f
EOI
chmod +x $R/init
(cd $R && find . | cpio -o -H newc 2>/dev/null | gzip -1) > $out
rm -rf "$R"
