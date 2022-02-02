#ifndef _PLATFORM_DEVICE_FD_H_
#define _PLATFORM_DEVICE_FD_H_

#ifdef __cplusplus
extern "C"
{
#endif

int getPlatformDeviceFdFromKey(void* fdKeyRaw, void** fd);
void* createPlatformDeviceFdKey(void* fd);
int destroyPlatformDeviceFdKey(void* fdKeyRaw);

#ifdef __cplusplus
}
#endif

#endif