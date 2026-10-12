/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Compile the real implementation; intercept only its calls into the driver.
#include "recovery_ioctl.h"
#define ioctl AmduvdRecoveryIoctl
#include "AmdUvdPlugin.cpp"
