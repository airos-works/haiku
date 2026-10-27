/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	/dev/misc/rpi_gpio: the BCM2711's general purpose I/O pins for programs
	such as AirPins; see <rpi_gpio.h>. On another board the driver finds no
	device and publishes nothing.

	Only the pins of the 40-pin header (GPIO 0 to 27, all in the first of
	the controller's banks) can be claimed, so every register this driver
	writes is a bank 0 register. Other drivers set up pins 30 and up
	(h4bcm: the mini UART for Bluetooth, broadcomfmac: the Wi-Fi SDIO bus)
	with read-modify-write accesses of their own banks' registers; the pull
	register for pins 16 to 31 is shared with h4bcm, which writes it once,
	when the Bluetooth controller is opened. */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <string.h>

#include <bus/FDT.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <rpi_firmware.h>
#include <rpi_gpio.h>


//#define TRACE_GPIO
#ifdef TRACE_GPIO
#	define TRACE(x...)	dprintf("rpi_gpio: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif
#define ERROR(x...)		dprintf("rpi_gpio: " x)

// registers
#define REG_FSEL(n)			(0x00 + 4 * (n))	// ten pins each, three bits
#define REG_SET0			0x1c
#define REG_CLR0			0x28
#define REG_LEV0			0x34
#define REG_LEV1			0x38
#define REG_EDS0			0x40
#define REG_REN0			0x4c
#define REG_FEN0			0x58
#define REG_HEN0			0x64
#define REG_LEN0			0x70
#define REG_AREN0			0x7c
#define REG_AFEN0			0x88
#define REG_PULL(n)			(0xe4 + 4 * (n))	// sixteen pins each, two bits

#define PIN_COUNT			58
#define HEADER_PINS			0x0fffffffu			// GPIO 0 to 27
#define FIRST_UNCLAIMABLE	28

#define EVENT_RING_SIZE		8192
#define WAIT_BATCH			64

// More edges than this within THROTTLE_WINDOW: the inputs are sampled for
// THROTTLE_PAUSE instead.
#define THROTTLE_WINDOW		10000
#define THROTTLE_EVENTS		400
#define THROTTLE_PAUSE		50000
// without the interrupt, inputs are sampled at this period
#define SAMPLE_PERIOD		1000


struct gpio_client {
	mutex			waitLock;
	uint64			readPosition;	// in event numbers
	uint32			claimed;		// pins; changed under sLock + sEventLock
	uint32			lost;
	bool			closed;
	rpi_gpio_event	batch[WAIT_BATCH];
};

struct saved_pin {
	uint8			function;
	uint8			pull;
	uint8			level;
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDeviceNames[] = { "misc/rpi_gpio", NULL };

static device_manager_info* sDeviceManager;

static area_id sRegistersArea = -1;
static volatile uint8* sRegisters;
static uint32 sInterrupt;
static bool sInterruptInstalled;
static uint32 sBoardRevision;
static uint64 sBoardSerial;

// pin ownership and configuration
static mutex sLock = MUTEX_INITIALIZER("rpi_gpio");
static gpio_client* sOwner[FIRST_UNCLAIMABLE];
static saved_pin sSaved[FIRST_UNCLAIMABLE];
static int32 sOpenCount;

// events; everything below is protected by sEventLock (interrupts off)
static spinlock sEventLock = B_SPINLOCK_INITIALIZER;
static ConditionVariable sEventCondition;
static rpi_gpio_event sEvents[EVENT_RING_SIZE];
static uint64 sEventCount;			// events ever recorded
static uint32 sEdgeMask;			// claimed inputs
static uint32 sLastLevels;			// as last recorded
static bool sThrottled;
static bigtime_t sWindowStart;
static uint32 sWindowCount;
static timer sThrottleTimer;
static timer sSampleTimer;
static bool sSampling;


static inline uint32
read_reg(uint32 offset)
{
	return *(volatile uint32*)(sRegisters + offset);
}


static inline void
write_reg(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRegisters + offset) = value;
}


static uint8
get_function(uint32 pin)
{
	return (read_reg(REG_FSEL(pin / 10)) >> ((pin % 10) * 3)) & 7;
}


static void
set_function(uint32 pin, uint8 function)
{
	uint32 shift = (pin % 10) * 3;
	uint32 value = read_reg(REG_FSEL(pin / 10));
	value = (value & ~(7u << shift)) | ((uint32)function << shift);
	write_reg(REG_FSEL(pin / 10), value);
}


static uint8
get_pull(uint32 pin)
{
	return (read_reg(REG_PULL(pin / 16)) >> ((pin % 16) * 2)) & 3;
}


static void
set_pull(uint32 pin, uint8 pull)
{
	uint32 shift = (pin % 16) * 2;
	uint32 value = read_reg(REG_PULL(pin / 16));
	value = (value & ~(3u << shift)) | ((uint32)pull << shift);
	write_reg(REG_PULL(pin / 16), value);
}


static inline uint8
pin_level(uint32 levels, uint32 pin)
{
	return (levels >> pin) & 1;
}


//	#pragma mark - events (sEventLock held)


static void
record_event(uint32 pin, uint8 level, uint8 flags, bigtime_t time)
{
	rpi_gpio_event& event = sEvents[sEventCount % EVENT_RING_SIZE];
	event.time = time;
	event.pin = pin;
	event.level = level;
	event.flags = flags;
	memset(event._reserved, 0, sizeof(event._reserved));
	sEventCount++;

	if (level != 0)
		sLastLevels |= 1u << pin;
	else
		sLastLevels &= ~(1u << pin);
}


/*!	Records the inputs in \a pins that had an edge. One whose level is the
	one last recorded had two: a pulse that ended before it was looked at.
*/
static void
record_edges(uint32 pins, uint32 levels, uint8 flags, bigtime_t time)
{
	while (pins != 0) {
		uint32 pin = __builtin_ctz(pins);
		pins &= pins - 1;
		uint8 level = pin_level(levels, pin);
		if (pin_level(sLastLevels, pin) == level)
			record_event(pin, !level, flags, time);
		record_event(pin, level, flags, time);
	}
}


static void
set_edge_detection(uint32 pin, bool on)
{
	uint32 bit = 1u << pin;
	if (on)
		sEdgeMask |= bit;
	else
		sEdgeMask &= ~bit;

	if (!sInterruptInstalled || (on && sThrottled))
		return;

	if (on) {
		// nothing that happened before counts
		write_reg(REG_EDS0, bit);
		write_reg(REG_REN0, read_reg(REG_REN0) | bit);
		write_reg(REG_FEN0, read_reg(REG_FEN0) | bit);
	} else {
		write_reg(REG_REN0, read_reg(REG_REN0) & ~bit);
		write_reg(REG_FEN0, read_reg(REG_FEN0) & ~bit);
		write_reg(REG_EDS0, bit);
	}
}


/*!	Samples the inputs: the period of a throttled interrupt ends, or there
	is no interrupt at all.
*/
static int32
sample_inputs(timer* timer)
{
	SpinLocker locker(sEventLock);

	if (sThrottled && timer == &sThrottleTimer) {
		sThrottled = false;
		sWindowStart = system_time();
		sWindowCount = 0;
		write_reg(REG_EDS0, sEdgeMask);
		write_reg(REG_REN0, read_reg(REG_REN0) | sEdgeMask);
		write_reg(REG_FEN0, read_reg(REG_FEN0) | sEdgeMask);
	}

	uint32 levels = read_reg(REG_LEV0);
	uint32 changed = (levels ^ sLastLevels) & sEdgeMask;
	if (changed == 0)
		return B_HANDLED_INTERRUPT;

	bigtime_t now = system_time();
	while (changed != 0) {
		uint32 pin = __builtin_ctz(changed);
		changed &= changed - 1;
		record_event(pin, pin_level(levels, pin), RPI_GPIO_EVENT_SAMPLED, now);
	}
	return sEventCondition.NotifyAll() > 0
		? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


static int32
gpio_interrupt(void* data)
{
	SpinLocker locker(sEventLock);

	uint32 status = read_reg(REG_EDS0) & HEADER_PINS;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;
	write_reg(REG_EDS0, status);
	uint32 levels = read_reg(REG_LEV0);
	bigtime_t now = system_time();

	status &= sEdgeMask;
	record_edges(status, levels, 0, now);

	if (now - sWindowStart > THROTTLE_WINDOW) {
		sWindowStart = now;
		sWindowCount = 0;
	}
	sWindowCount += __builtin_popcount(status);
	if (sWindowCount > THROTTLE_EVENTS && !sThrottled) {
		// too fast to report every edge: look again in a while
		sThrottled = true;
		write_reg(REG_REN0, read_reg(REG_REN0) & ~sEdgeMask);
		write_reg(REG_FEN0, read_reg(REG_FEN0) & ~sEdgeMask);
		write_reg(REG_EDS0, sEdgeMask);
		add_timer(&sThrottleTimer, &sample_inputs, THROTTLE_PAUSE,
			B_ONE_SHOT_RELATIVE_TIMER);
	}

	return sEventCondition.NotifyAll() > 0
		? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


//	#pragma mark - pins (sLock held)


static void
restore_pin(gpio_client* client, uint32 pin)
{
	uint32 bit = 1u << pin;
	{
		InterruptsSpinLocker locker(sEventLock);
		set_edge_detection(pin, false);
		client->claimed &= ~bit;
	}
	sOwner[pin] = NULL;

	const saved_pin& saved = sSaved[pin];
	if (saved.function == RPI_GPIO_OUTPUT)
		write_reg(saved.level != 0 ? REG_SET0 : REG_CLR0, bit);
	set_function(pin, saved.function);
	set_pull(pin, saved.pull);
	TRACE("pin %" B_PRIu32 " back to function %u pull %u\n", pin,
		saved.function, saved.pull);
}


static status_t
claim_pin(gpio_client* client, const rpi_gpio_claim& request)
{
	uint32 pin = request.pin;
	if (pin >= FIRST_UNCLAIMABLE
		|| (request.function != RPI_GPIO_INPUT
			&& request.function != RPI_GPIO_OUTPUT)
		|| request.pull > RPI_GPIO_PULL_DOWN
		|| request.level < -1 || request.level > 1
		|| (request.flags & ~RPI_GPIO_CLAIM_DETACH) != 0) {
		return B_BAD_VALUE;
	}
	bool detach = (request.flags & RPI_GPIO_CLAIM_DETACH) != 0;
	uint32 bit = 1u << pin;

	MutexLocker locker(sLock);
	if (sOwner[pin] != NULL && sOwner[pin] != client)
		return B_BUSY;
	bool owned = sOwner[pin] == client;

	if (!owned && !detach) {
		sSaved[pin].function = get_function(pin);
		sSaved[pin].pull = get_pull(pin);
		sSaved[pin].level = pin_level(read_reg(REG_LEV0), pin);
	}

	// An input of ours that only gets another pull keeps reporting its
	// edges: the change of level is one. Anything else reports nothing
	// while it changes.
	bool keepEdges = owned && !detach && request.function == RPI_GPIO_INPUT
		&& get_function(pin) == RPI_GPIO_INPUT;
	if (!keepEdges) {
		InterruptsSpinLocker eventLocker(sEventLock);
		set_edge_detection(pin, false);
	}

	if (request.function == RPI_GPIO_OUTPUT) {
		// the output starts at the level asked for, or the one it has
		int8 level = request.level;
		if (level < 0)
			level = pin_level(read_reg(REG_LEV0), pin);
		write_reg(level != 0 ? REG_SET0 : REG_CLR0, bit);
	}
	set_function(pin, request.function);
	set_pull(pin, request.pull);

	InterruptsSpinLocker eventLocker(sEventLock);
	if (detach) {
		sOwner[pin] = NULL;
		client->claimed &= ~bit;
		return B_OK;
	}

	sOwner[pin] = client;
	client->claimed |= bit;
	if (!keepEdges) {
		if (request.function == RPI_GPIO_INPUT)
			set_edge_detection(pin, true);
		// the level it starts with, or the one it changed to
		uint8 level = pin_level(read_reg(REG_LEV0), pin);
		if (!owned || level != pin_level(sLastLevels, pin)) {
			record_event(pin, level, RPI_GPIO_EVENT_CLAIMED, system_time());
			sEventCondition.NotifyAll();
		}
	}

	TRACE("pin %" B_PRIu32 " claimed: function %u pull %u level %d\n", pin,
		request.function, request.pull, request.level);
	return B_OK;
}


static status_t
release_pin(gpio_client* client, uint32 pin)
{
	MutexLocker locker(sLock);
	if (pin >= FIRST_UNCLAIMABLE || sOwner[pin] != client)
		return B_BAD_VALUE;

	restore_pin(client, pin);
	return B_OK;
}


static status_t
write_pins(gpio_client* client, const rpi_gpio_write& request)
{
	MutexLocker locker(sLock);
	if ((request.mask & ~(uint64)client->claimed) != 0)
		return B_NOT_ALLOWED;

	uint32 mask = (uint32)request.mask;
	for (uint32 pins = mask; pins != 0; pins &= pins - 1) {
		if (get_function(__builtin_ctz(pins)) != RPI_GPIO_OUTPUT)
			return B_NOT_ALLOWED;
	}

	uint32 levels = (uint32)request.levels;
	if ((levels & mask) != 0)
		write_reg(REG_SET0, levels & mask);
	if ((~levels & mask) != 0)
		write_reg(REG_CLR0, ~levels & mask);

	InterruptsSpinLocker eventLocker(sEventLock);
	bigtime_t now = system_time();
	uint32 changed = (levels ^ sLastLevels) & mask;
	while (changed != 0) {
		uint32 pin = __builtin_ctz(changed);
		changed &= changed - 1;
		record_event(pin, pin_level(levels, pin), RPI_GPIO_EVENT_WRITTEN, now);
	}
	sEventCondition.NotifyAll();
	return B_OK;
}


static void
get_state(gpio_client* client, rpi_gpio_state& state)
{
	memset(&state, 0, sizeof(state));

	MutexLocker locker(sLock);
	state.levels = read_reg(REG_LEV0) | ((uint64)read_reg(REG_LEV1) << 32);
	state.time = system_time();
	for (uint32 pin = 0; pin < PIN_COUNT; pin++) {
		state.function[pin] = get_function(pin);
		state.pull[pin] = get_pull(pin);
	}
	for (uint32 pin = 0; pin < FIRST_UNCLAIMABLE; pin++) {
		if (sOwner[pin] == client)
			state.claimed |= 1ull << pin;
		else if (sOwner[pin] != NULL)
			state.claimed_elsewhere |= 1ull << pin;
	}
}


static status_t
wait_events(gpio_client* client, rpi_gpio_wait& request)
{
	if (request.capacity == 0 || request.events == NULL
		|| !IS_USER_ADDRESS(request.events)) {
		return B_BAD_VALUE;
	}
	uint32 capacity = min_c(request.capacity, (uint32)WAIT_BATCH);

	bigtime_t deadline = B_INFINITE_TIMEOUT;
	if (request.timeout <= 0)
		deadline = 0;
	else if (request.timeout != B_INFINITE_TIMEOUT)
		deadline = system_time() + request.timeout;

	MutexLocker waitLocker(client->waitLock);
	InterruptsSpinLocker locker(sEventLock);
	uint32 count = 0;
	status_t status = B_OK;
	while (true) {
		if (client->closed) {
			status = B_FILE_ERROR;
			break;
		}
		if (sEventCount - client->readPosition > EVENT_RING_SIZE) {
			uint64 oldest = sEventCount - EVENT_RING_SIZE;
			client->lost += oldest - client->readPosition;
			client->readPosition = oldest;
		}
		while (client->readPosition < sEventCount && count < capacity) {
			const rpi_gpio_event& event
				= sEvents[client->readPosition % EVENT_RING_SIZE];
			client->readPosition++;
			if ((client->claimed & (1u << event.pin)) != 0)
				client->batch[count++] = event;
		}
		if (count > 0 || deadline == 0
			|| (deadline != B_INFINITE_TIMEOUT && system_time() >= deadline)) {
			break;
		}

		ConditionVariableEntry entry;
		sEventCondition.Add(&entry);
		locker.Unlock();
		status = entry.Wait(B_CAN_INTERRUPT
			| (deadline != B_INFINITE_TIMEOUT ? B_ABSOLUTE_TIMEOUT : 0),
			deadline);
		locker.Lock();
		if (status == B_INTERRUPTED)
			break;
		status = B_OK;
	}

	request.count = count;
	request.lost = client->lost;
	client->lost = 0;
	locker.Unlock();

	if (status != B_OK)
		return status;
	if (count > 0 && user_memcpy(request.events, client->batch,
			count * sizeof(rpi_gpio_event)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


//	#pragma mark - device hooks


static status_t
gpio_open(const char* name, uint32 flags, void** _cookie)
{
	if (sRegisters == NULL)
		return B_DEVICE_NOT_FOUND;

	gpio_client* client = new(std::nothrow) gpio_client;
	if (client == NULL)
		return B_NO_MEMORY;
	mutex_init(&client->waitLock, "rpi_gpio wait");
	client->claimed = 0;
	client->lost = 0;
	client->closed = false;
	{
		InterruptsSpinLocker locker(sEventLock);
		client->readPosition = sEventCount;
	}

	MutexLocker locker(sLock);
	if (sOpenCount++ == 0 && !sInterruptInstalled) {
		sSampling = true;
		add_timer(&sSampleTimer, &sample_inputs, SAMPLE_PERIOD,
			B_PERIODIC_TIMER);
	}

	*_cookie = client;
	return B_OK;
}


static status_t
gpio_close(void* cookie)
{
	gpio_client* client = (gpio_client*)cookie;

	// a thread that waits for events gives up
	InterruptsSpinLocker locker(sEventLock);
	client->closed = true;
	sEventCondition.NotifyAll();
	return B_OK;
}


static status_t
gpio_free(void* cookie)
{
	gpio_client* client = (gpio_client*)cookie;

	MutexLocker locker(sLock);
	for (uint32 pin = 0; pin < FIRST_UNCLAIMABLE; pin++) {
		if (sOwner[pin] == client)
			restore_pin(client, pin);
	}
	if (--sOpenCount == 0 && sSampling) {
		// the hook takes sEventLock only, so waiting for it here is fine
		cancel_timer(&sSampleTimer);
		sSampling = false;
	}
	locker.Unlock();

	mutex_destroy(&client->waitLock);
	delete client;
	return B_OK;
}


static status_t
gpio_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	gpio_client* client = (gpio_client*)cookie;

	if (!IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;

	switch (op) {
		case RPI_GPIO_GET_INFO:
		{
			rpi_gpio_info info;
			memset(&info, 0, sizeof(info));
			info.api_version = RPI_GPIO_API_VERSION;
			info.pin_count = PIN_COUNT;
			info.claimable = HEADER_PINS;
			info.board_revision = sBoardRevision;
			info.board_serial = sBoardSerial;
			info.board = RPI_GPIO_BOARD_RPI4;
			if (sInterruptInstalled)
				info.flags |= RPI_GPIO_INFO_EDGE_INTERRUPTS;
			return user_memcpy(buffer, &info, sizeof(info));
		}

		case RPI_GPIO_GET_STATE:
		{
			rpi_gpio_state state;
			get_state(client, state);
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case RPI_GPIO_CLAIM:
		{
			rpi_gpio_claim request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			return claim_pin(client, request);
		}

		case RPI_GPIO_RELEASE:
		{
			uint32 pin;
			if (user_memcpy(&pin, buffer, sizeof(pin)) != B_OK)
				return B_BAD_ADDRESS;
			return release_pin(client, pin);
		}

		case RPI_GPIO_WRITE:
		{
			rpi_gpio_write request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			return write_pins(client, request);
		}

		case RPI_GPIO_WAIT_EVENTS:
		{
			rpi_gpio_wait request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			status_t status = wait_events(client, request);
			if (status != B_OK && status != B_INTERRUPTED)
				return status;
			if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			return status;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
gpio_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
gpio_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	gpio_open,
	gpio_close,
	gpio_free,
	gpio_control,
	gpio_read,
	gpio_write
};


//	#pragma mark - driver


static status_t
find_controller(phys_addr_t* _registers, size_t* _size, uint32* _interrupt)
{
	device_node* root = sDeviceManager->get_root_node();
	if (root == NULL)
		return B_DEVICE_NOT_FOUND;

	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE,
			{ .string = "brcm,bcm2711-gpio" } },
		{}
	};
	device_node* node = NULL;
	if (sDeviceManager->find_child_node(root, attributes, &node) != B_OK)
		node = NULL;
	sDeviceManager->put_node(root);
	if (node == NULL)
		return B_DEVICE_NOT_FOUND;

	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&fdt, (void**)&device);
	if (status == B_OK) {
		uint64 base, size, interrupt;
		if (fdt->get_reg(device, 0, &base, &size)) {
			*_registers = base;
			*_size = size;
			// the first of the controller's interrupts is bank 0's
			*_interrupt = fdt->get_interrupt(device, 0, NULL, &interrupt)
				? interrupt : 0;
		} else
			status = B_BAD_DATA;
	}

	sDeviceManager->put_node(node);
	return status;
}


static void
read_board_details()
{
	rpi_firmware_module_info* firmware;
	if (get_module(RPI_FIRMWARE_MODULE_NAME, (module_info**)&firmware)
			!= B_OK) {
		return;
	}

	uint32 revision[1] = {};
	if (firmware->property(RPI_FIRMWARE_GET_BOARD_REVISION, revision,
			sizeof(revision)) == B_OK) {
		sBoardRevision = revision[0];
	}
	uint32 serial[2] = {};
	if (firmware->property(RPI_FIRMWARE_GET_BOARD_SERIAL, serial,
			sizeof(serial)) == B_OK) {
		sBoardSerial = serial[0] | ((uint64)serial[1] << 32);
	}

	put_module(RPI_FIRMWARE_MODULE_NAME);
}


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&sDeviceManager);
	if (status != B_OK)
		return status;

	phys_addr_t registers;
	size_t size;
	status = find_controller(&registers, &size, &sInterrupt);
	if (status != B_OK) {
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
		return B_DEVICE_NOT_FOUND;
	}

	phys_addr_t page = registers & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* address;
	sRegistersArea = map_physical_memory("rpi_gpio registers", page,
		ROUNDUP(registers + size - page, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (sRegistersArea < 0) {
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
		return sRegistersArea;
	}
	sRegisters = (volatile uint8*)address + (registers - page);

	read_board_details();
	sEventCondition.Init(sEvents, "rpi_gpio events");

	// Nothing on the header pins reports edges until a program asks.
	write_reg(REG_REN0, read_reg(REG_REN0) & ~HEADER_PINS);
	write_reg(REG_FEN0, read_reg(REG_FEN0) & ~HEADER_PINS);
	write_reg(REG_HEN0, read_reg(REG_HEN0) & ~HEADER_PINS);
	write_reg(REG_LEN0, read_reg(REG_LEN0) & ~HEADER_PINS);
	write_reg(REG_AREN0, read_reg(REG_AREN0) & ~HEADER_PINS);
	write_reg(REG_AFEN0, read_reg(REG_AFEN0) & ~HEADER_PINS);
	write_reg(REG_EDS0, HEADER_PINS);
	sLastLevels = read_reg(REG_LEV0);

	if (sInterrupt != 0) {
		status = install_io_interrupt_handler(sInterrupt, &gpio_interrupt,
			NULL, 0);
		if (status == B_OK)
			sInterruptInstalled = true;
		else {
			ERROR("interrupt %" B_PRIu32 ": %s; inputs are sampled\n",
				sInterrupt, strerror(status));
		}
	}

	dprintf("rpi_gpio: controller at %#" B_PRIxPHYSADDR ", interrupt %"
		B_PRIu32 ", board revision %#" B_PRIx32 "\n", registers, sInterrupt,
		sBoardRevision);
	return B_OK;
}


void
uninit_driver()
{
	if (sInterruptInstalled) {
		{
			InterruptsSpinLocker locker(sEventLock);
			write_reg(REG_REN0, read_reg(REG_REN0) & ~HEADER_PINS);
			write_reg(REG_FEN0, read_reg(REG_FEN0) & ~HEADER_PINS);
			write_reg(REG_EDS0, HEADER_PINS);
		}
		remove_io_interrupt_handler(sInterrupt, &gpio_interrupt, NULL);
		cancel_timer(&sThrottleTimer);
	}
	sInterruptInstalled = false;
	sRegisters = NULL;
	if (sRegistersArea >= 0)
		delete_area(sRegistersArea);
	sRegistersArea = -1;

	put_module(B_DEVICE_MANAGER_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
