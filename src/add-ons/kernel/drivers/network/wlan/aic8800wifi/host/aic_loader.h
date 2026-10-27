/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIC8800WIFI_AIC_LOADER_H
#define AIC8800WIFI_AIC_LOADER_H


/*	The AIC8800D80's ROM loader (USB a69c:8d80): writes the Bluetooth ROM
	patch and the Wi-Fi firmware into the chip and starts it, after which
	the chip leaves the bus and comes back as a69c:8d81 with the Wi-Fi
	function and the Bluetooth (H2) interfaces.

	Written from the protocol notes of the Cubie A7S port (cubie/evidence/
	wifi/DESIGN.md section 1), not from AICSemi's driver. The caller supplies
	the bulk pipes and the firmware files, so the same code runs in the
	kernel driver and in the lab's userland tool. Everything on the wire is
	little endian and built byte by byte. */


#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


#define AIC_USB_VENDOR				0xa69c
#define AIC_USB_PRODUCT_ROM			0x8d80	/* the ROM loader */
#define AIC_USB_PRODUCT_FIRMWARE	0x8d81	/* the Wi-Fi firmware runs */

/* the first byte after the length of every packet on the bus */
#define AIC_TYPE_DATA				0x00
#define AIC_TYPE_DATA_TX			0x01
#define AIC_TYPE_CONFIG				0x10	/* a flag: not a data frame */
#define AIC_TYPE_MESSAGE			0x11	/* an IPC message */
#define AIC_TYPE_DATA_CONFIRM		0x12	/* a transmitted frame's status */
#define AIC_TYPE_PRINT				0x13	/* firmware log text */

/* the chip's tasks; the host's own is 100 */
#define AIC_TASK_MM					0
#define AIC_TASK_DBG				1
#define AIC_TASK_SCANU				4
#define AIC_TASK_ME					5
#define AIC_TASK_SM					6
#define AIC_TASK_HOST				100

#define AIC_DBG_MEM_READ_REQ		0x0400
#define AIC_DBG_MEM_READ_CFM		0x0401
#define AIC_DBG_MEM_WRITE_REQ		0x0402
#define AIC_DBG_MEM_WRITE_CFM		0x0403
#define AIC_DBG_MEM_BLOCK_WRITE_REQ	0x040b
#define AIC_DBG_MEM_BLOCK_WRITE_CFM	0x040c
#define AIC_DBG_START_APP_REQ		0x040d

#define AIC_START_APP_AUTO			1
#define AIC_START_APP_REBOOT		3

#define AIC_MESSAGE_HEADER			16		/* bus header + message header */
#define AIC_MESSAGE_MAX				1536	/* largest message the chip takes */
#define AIC_LOADER_RX_SIZE			2048

#define AIC_CHIP_ID_REGISTER		0x40500000


struct aic_loader_ops {
	/* one bulk OUT transfer of exactly \a length bytes; 0 or an error */
	int		(*bulk_out)(void* cookie, const uint8_t* buffer, size_t length);
	/* one bulk IN transfer; the byte count or an error (< 0) */
	int		(*bulk_in)(void* cookie, uint8_t* buffer, size_t capacity,
				int timeoutMs);
	/* a firmware file by its name; the size, or < 0. The buffer has
	   1024 bytes of slack after the file, zeroed. */
	long	(*load_file)(void* cookie, const char* name, uint8_t** _data);
	void	(*release_file)(void* cookie, uint8_t* data);
	/* optional */
	void	(*log)(void* cookie, const char* format, ...);
	void	(*sleep_ms)(void* cookie, int milliseconds);
};

struct aic_loader_result {
	uint32_t	chip_register;	/* AIC_CHIP_ID_REGISTER as read */
	uint8_t		chip_id;		/* bits 23-16: 3 = U02, 7 = U03 */
	uint8_t		chip_mcu;		/* bit 25 clear */
	uint32_t	firmware_version;
	const char*	firmware_name;	/* the Wi-Fi image that was started */
	unsigned	messages;
};


/*	The Bluetooth mode the next load configures (AIC's aicbt_btmode_type):
	0 Bluetooth only, with an external switch; 1 Wi-Fi and Bluetooth
	combined; 2 Bluetooth only, without a switch; 5 Bluetooth only on the
	shared antenna, without an external switch (AIC's D80 default). How the
	module's antenna pins are wired decides which one hears anything. Other
	values keep the default. Returns the mode set. */
#define AIC_BT_MODE_DEFAULT			5
int		aic_loader_set_bt_mode(int mode);

/*	The whole loader sequence of a U02/U03 chip, up to and including the
	start request. On success the chip re-enumerates as 0x8d81. */
int		aic_load_firmware_d80(const struct aic_loader_ops* ops, void* cookie,
			struct aic_loader_result* result);

/*	For a chip found running firmware this driver did not start (a warm
	reboot from another system): back to the ROM, which comes back as
	0x8d80. Sent on the message pipe; nothing answers it. */
int		aic_send_reboot(const struct aic_loader_ops* ops, void* cookie);

/*	One request and its confirmation (\a confirmId 0: none awaited). For
	the lab tool's probe of a running firmware. */
int		aic_request(const struct aic_loader_ops* ops, void* cookie,
			uint16_t id, uint16_t task, const void* parameters,
			uint16_t parameterLength, uint16_t confirmId, void* confirm,
			size_t confirmCapacity);

/*	Builds a message for the bus (AIC_MESSAGE_HEADER + parameterLength
	bytes) and returns its length. */
size_t	aic_build_message(uint8_t* buffer, uint16_t id, uint16_t task,
			const void* parameters, uint16_t parameterLength);


#ifdef __cplusplus
}
#endif


#endif	/* AIC8800WIFI_AIC_LOADER_H */
