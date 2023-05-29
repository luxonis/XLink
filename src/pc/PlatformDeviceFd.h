#ifndef _PLATFORM_DEVICE_FD_H_
#define _PLATFORM_DEVICE_FD_H_

#ifdef __cplusplus
#define NOEXCEPT noexcept
extern "C" {
#else
#define NOEXCEPT
#endif

int getPlatformDeviceFdFromKey(void* fdKeyRaw, void** fd) NOEXCEPT;
void* getPlatformDeviceFdFromKeySimple(void* fdKeyRaw) NOEXCEPT;
void* createPlatformDeviceFdKey(void* fd) NOEXCEPT;
int destroyPlatformDeviceFdKey(void* fdKeyRaw) NOEXCEPT;
void* extractPlatformDeviceFdKey(void* fdKeyRaw) NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif