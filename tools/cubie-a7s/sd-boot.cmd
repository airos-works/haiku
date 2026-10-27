# air/OS on the Cubie A7S, boot.scr on the ESP of the SD card image.
#
# Radxa's boot chain (boot0, TF-A, U-Boot 2018) runs this. It starts the
# air/OS U-Boot (u-boot/build.sh) like a kernel; that one provides UEFI and
# boots EFI/BOOT/BOOTAA64.EFI, Haiku's loader, with the air/OS device tree.

# Only on the board: other U-Boots that run boot scripts before EFI (QEMU's)
# go on to EFI/BOOT/BOOTAA64.EFI themselves.
if test "${board}" = "sunxi"; then
	echo "air/OS: starting U-Boot"
	if load ${devtype} ${devnum}:${distro_bootpart} 0x4a000000 airos/u-boot.bin; then
		if load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} dtb/allwinner/sun60i-a733-cubie-a7s.dtb; then
			booti 0x4a000000 - ${fdt_addr_r}
		fi
	fi
	echo "air/OS: could not start U-Boot"
fi
