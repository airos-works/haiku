# air/OS lab card on Radxa's boot chain (boot0, TF-A, U-Boot 2018).
#
# U-Boot 2018 runs this boot.scr from the ESP before it finds Debian's
# extlinux.conf on the root partition. When airos-once on the ESP holds "1",
# it is set to "0" here (so that a hang ends in Debian after a power cycle)
# and the air/OS U-Boot, airos/u-boot.bin, is started like a kernel; its
# airos/chain.scr then boots Haiku. airos-default makes that permanent.
# Otherwise this script returns and Debian boots.

setenv airos_chain 0
if load ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} airos-once 1; then
	if itest.b *${scriptaddr} == 0x31; then
		mw.b ${scriptaddr} 0x30 1
		fatwrite ${devtype} ${devnum}:${distro_bootpart} ${scriptaddr} airos-once 1
		setenv airos_chain 1
	fi
fi
if test -e ${devtype} ${devnum}:${distro_bootpart} airos-default; then
	setenv airos_chain 1
fi
if test ${airos_chain} = 1; then
	echo "air/OS lab: starting the air/OS U-Boot"
	if load ${devtype} ${devnum}:${distro_bootpart} 0x4a000000 airos/u-boot.bin; then
		if load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} airos/cubie-a7s.dtb; then
			booti 0x4a000000 - ${fdt_addr_r}
		fi
	fi
	echo "air/OS lab: could not start it"
fi
echo "air/OS lab: booting Debian"
