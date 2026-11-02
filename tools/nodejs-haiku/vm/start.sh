#!/bin/sh
# Haiku (air/OS CI install) VM for building Node.js. mcp_server on 127.0.0.1:47781, ssh on :47722.
D=$(dirname "$(readlink -f "$0")")
exec qemu-system-x86_64 -name node-haiku -enable-kvm -cpu host -m ${MEM:-20480} -smp ${CPUS:-16} \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=$D/OVMF_VARS.fd -vga std \
  -netdev user,id=net,hostfwd=tcp:127.0.0.1:47781-:7780,hostfwd=tcp:127.0.0.1:47722-:22 \
  -device e1000,netdev=net -device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -device usb-tablet,bus=usb.0 \
  -display none -monitor none -serial file:$D/serial.log \
  -qmp unix:$D/qmp.sock,server=on,wait=off -pidfile $D/qemu.pid -daemonize \
  -drive file=$D/disk.img,if=none,id=disk,format=raw,cache=unsafe -device nvme,drive=disk,serial=airos-disk,bootindex=0 \
  -drive file=$D/build.img,if=none,id=build,format=raw,cache=unsafe -device nvme,drive=build,serial=node-build
