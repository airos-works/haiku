/*
 * Copyright 2008-2013, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */

/*! Provides the networking stack notification service. */

#include <net_notifications.h>

#include <kernel.h>
#include <generic_syscall.h>
#include <Notifications.h>
#include <util/KMessage.h>

//#define TRACE_NOTIFICATIONS
#ifdef TRACE_NOTIFICATIONS
#	define TRACE(x...) dprintf("\33[32mnet_notifications:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif


class NetNotificationService : public DefaultUserNotificationService {
public:
								NetNotificationService();
	virtual						~NetNotificationService();

			void				Notify(const KMessage& event);

protected:
	virtual	void				LastReferenceReleased();
	virtual	void				FirstAdded();
	virtual	void				LastRemoved();
};


static NetNotificationService sNotificationService;


//	#pragma mark - NetNotificationService


NetNotificationService::NetNotificationService()
	:
	DefaultUserNotificationService("network")
{
}


NetNotificationService::~NetNotificationService()
{
}


void
NetNotificationService::Notify(const KMessage& event)
{
	uint32 opcode = event.GetInt32("opcode", 0);
	if (opcode == 0)
		return;

	TRACE("notify for %lx\n", opcode);

	DefaultUserNotificationService::Notify(event, opcode);
}


void
NetNotificationService::LastReferenceReleased()
{
	// don't delete us here
}


void
NetNotificationService::FirstAdded()
{
	// The reference counting doesn't work for us, as we'll have to
	// ensure our module stays loaded.
	module_info* dummy;
	get_module(NET_NOTIFICATIONS_MODULE_NAME, &dummy);
}


void
NetNotificationService::LastRemoved()
{
	// Give up the reference _AddListener()
	put_module(NET_NOTIFICATIONS_MODULE_NAME);
}


//	#pragma mark - User generic syscall


static status_t
net_notifications_control(const char *subsystem, uint32 function, void *buffer,
	size_t bufferSize)
{
	struct net_notifications_control control;
	if (bufferSize != sizeof(struct net_notifications_control)
		|| function != NET_NOTIFICATIONS_CONTROL_WATCHING)
		return B_BAD_VALUE;
	if (!IS_USER_ADDRESS(buffer) || user_memcpy(&control, buffer,
			sizeof(struct net_notifications_control)) < B_OK)
		return B_BAD_ADDRESS;

	if (control.flags != 0) {
		return sNotificationService.UpdateUserListener(control.flags,
			control.port, control.token);
	}

	return sNotificationService.RemoveUserListeners(control.port,
		control.token);
}


//	#pragma mark - exported module API


static status_t
send_notification(const KMessage* event)
{
	sNotificationService.Notify(*event);
	return B_OK;
}


static status_t
notifications_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			TRACE("init\n");

			new(&sNotificationService) NetNotificationService();
			status_t result = sNotificationService.Register();
			if (result != B_OK)
				return result;

			register_generic_syscall(NET_NOTIFICATIONS_SYSCALLS,
				net_notifications_control, 1, 0);
			return B_OK;
		}
		case B_MODULE_UNINIT:
			TRACE("uninit\n");

			// Only at shutdown: the module is B_KEEP_LOADED (see below).
			unregister_generic_syscall(NET_NOTIFICATIONS_SYSCALLS, 1);

			sNotificationService.Unregister();

			// we need to release the reference that was acquired
			// on our behalf by the NotificationManager.
			sNotificationService.ReleaseReference();
			sNotificationService.~NetNotificationService();
			return B_OK;

		default:
			return B_ERROR;
	}
}


// air/OS: kept loaded. Unloading it ran unregister_generic_syscall(), which
// waits for the callers inside the syscall, under the kernel's module lock
// (put_module() of the stack module, when its last socket is closed). A
// caller inside net_notifications_control() adding the first listener needs
// that lock too (FirstAdded() calls get_module()): a deadlock early in the
// boot, when net_server closes the socket it probes the address families
// with while mount_server starts watching the network. Nothing past that
// point started (no app_server), about one start in ten.
net_notifications_module_info sNotificationsModule = {
	{
		NET_NOTIFICATIONS_MODULE_NAME,
		B_KEEP_LOADED,
		notifications_std_ops
	},

	send_notification
};

module_info* modules[] = {
	(module_info*)&sNotificationsModule,
	NULL
};
