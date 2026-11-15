#ifndef XLINK_USB_HOST_MEMORY_H
#define XLINK_USB_HOST_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool usbRegisterReadBufferHandle(void* key, void* handle);
// The caller releases interfaces first. Outstanding frames retain the USB handle.
bool usbCloseReadBufferHandle(void* key);
void* usbAllocateReadBuffer(void* key, uint32_t size, uint32_t alignment);
bool usbFreeReadBuffer(void* data);

#ifdef __cplusplus
}
#endif

#endif
