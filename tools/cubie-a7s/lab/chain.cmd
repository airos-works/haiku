# air/OS lab: run by the air/OS U-Boot (tools/cubie-a7s/u-boot) after the
# BSP U-Boot started it. Boots Haiku's EFI loader with the air/OS device
# tree; if that fails, the board resets and the card boots Debian.

echo "air/OS lab: booting Haiku"
if load mmc 0:2 ${kernel_addr_r} EFI/airos/haiku_loader.efi; then
	if load mmc 0:2 ${fdt_addr_r} airos/cubie-a7s.dtb; then
		bootefi ${kernel_addr_r} ${fdt_addr_r}
	fi
fi
echo "air/OS lab: Haiku did not start, resetting"
sleep 5
reset
