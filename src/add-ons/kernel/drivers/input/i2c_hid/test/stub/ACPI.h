#ifndef _TEST_ACPI_H
#define _TEST_ACPI_H
#include <SupportDefs.h>
extern "C" {
#include "acpi.h"
}
typedef void* acpi_handle;
typedef ACPI_STATUS acpi_status;
typedef ACPI_RESOURCE acpi_resource;
typedef acpi_status (*acpi_walk_resources_callback)(acpi_resource* resource, void* context);
struct acpi_module_info {
	status_t (*get_handle)(acpi_handle parent, const char* pathname, acpi_handle* retHandle);
	status_t (*get_device_info)(const char* path, char** hid, char** cidList, size_t cidListLength, char** uid, char** cls);
	status_t (*walk_resources)(acpi_handle busDeviceHandle, char* method, acpi_walk_resources_callback callback, void* context);
};
#endif
