/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIC8800WIFI_AIC_USB_H
#define AIC8800WIFI_AIC_USB_H


#include <SupportDefs.h>


/*	The AIC8800D80's USB side: its two USB identities (the ROM loader and
	the running firmware), the firmware load, and the running firmware's
	message and data pipes. Plain Haiku code under a small C API, like
	broadcomfmac's SDIO host; the driver proper (dev/aic) is OpenBSD code on
	the compatibility layers.

	The transport never takes the compatibility layer's Giant lock: a
	command's confirmation is matched on the transport's own receive
	threads, so a command may be waited for from any thread. */

#ifdef __cplusplus
extern "C" {
#endif


#define AIC_USB_PIPE_DATA		0
#define AIC_USB_PIPE_MESSAGE	1

#define AIC_USB_TX_MAX			2048	/* largest data frame on the bus */


struct aic_usb_callbacks {
	/* A message from the firmware that no request waits for (an
	   indication); on a receive thread. */
	void	(*message)(void* cookie, uint16 id, const uint8* parameters,
				size_t length);
	/* One received frame (the firmware's 56-byte receive header, 4 bytes,
	   then the 802.11 frame), and the end of a USB transfer's frames; on
	   the receive thread of \a pipe. */
	void	(*data)(void* cookie, int pipe, const uint8* packet,
				size_t length);
	void	(*data_done)(void* cookie, int pipe);
	/* The status of a frame sent with a confirmation index; on a receive
	   thread. */
	void	(*tx_confirm)(void* cookie, uint32 status, uint32 index);
	/* The transmit ring has room again, or the chip has left the bus. Both
	   come from threads that must not block: wake a thread of the
	   driver's. */
	void	(*tx_ready)(void* cookie);
	void	(*gone)(void* cookie);
	/* The chip is back with the firmware running, after it had left (a
	   restart): aic_usb_stop() and aic_usb_start() again, then set it up.
	   From the thread that explores the bus: wake a thread. */
	void	(*back)(void* cookie);
};


/*	At driver load: registers with the USB stack and brings the chip up as
	far as it goes in one step. B_OK when the running firmware is there for
	the driver to attach to. Otherwise the chip is on its way (the firmware
	was loaded, or the chip was sent back to its ROM) and the driver load is
	to fail: the USB stack loads the driver again when the chip comes back.
*/
status_t	aic_usb_init_hardware(void);
void		aic_usb_uninit_hardware(void);

/*	The running firmware's pipes: the receive threads start and stop. */
status_t	aic_usb_start(const struct aic_usb_callbacks* callbacks,
				void* cookie);
void		aic_usb_stop(void);
bool		aic_usb_gone(void);

/*	One message, and its confirmation unless \a confirmId is 0. One at a
	time; \a timeout in microseconds. \a _confirmLength, if given, is set to
	the parameter bytes received. */
status_t	aic_usb_request(uint16 id, uint16 task, const void* parameters,
				uint16 parameterLength, uint16 confirmId, void* confirm,
				size_t confirmCapacity, size_t* _confirmLength,
				bigtime_t timeout);

/*	A data frame for the firmware, bus header included (at most
	AIC_USB_TX_MAX bytes). B_WOULD_BLOCK when the ring is full; tx_ready
	follows when it has room again. */
status_t	aic_usb_send(const void* frame, size_t length);
bool		aic_usb_send_space(void);

/*	The debug watch (driver setting "watch"): the driver's own thread, and
	locks whose waiters it names. */
void		aic_usb_watch_thread(int32 thread);
void		aic_usb_watch_lock(const char* name, const void* object,
				const volatile int32* owner);

/*	What the loader found. */
uint32		aic_usb_chip_register(void);

/*	A file of the firmware directory, with a terminating zero byte added;
	free() it. */
status_t	aic_usb_read_file(const char* name, uint8** _data, size_t* _size);

/*	The firmware no longer answers: delete the marker of a firmware loaded
	in this boot and send the chip back to its ROM, from where it is loaded
	again (the device leaves the bus). */
void		aic_usb_reboot_chip(void);


#ifdef __cplusplus
}
#endif


#endif	/* AIC8800WIFI_AIC_USB_H */
