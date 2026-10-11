/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDUVD_RECOVERY_IOCTL_H
#define AMDUVD_RECOVERY_IOCTL_H
#include <sys/ioctl.h>
#include <SupportDefs.h>

extern "C" int AmduvdRecoveryIoctl(int fd, unsigned long op, void* data, size_t size);
#endif
