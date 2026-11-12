/* host build of ACPICA's types for the i2c_hid GPIO test */
#ifndef __ACLINUX_TEST_H__
#define __ACLINUX_TEST_H__
#define ACPI_USE_STANDARD_HEADERS
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_MACHINE_WIDTH 64
#define COMPILER_DEPENDENT_INT64 long long
#define COMPILER_DEPENDENT_UINT64 unsigned long long
#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64
#endif
