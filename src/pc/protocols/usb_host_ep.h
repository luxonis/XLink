// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "XLink/XLink.h"
#include "XLink/XLinkPlatform.h"

int usbEpInitialize();

int usbEpPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd);
int usbEpPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd);
int usbEpPlatformClose(void *fd);

int usbEpPlatformRead(void *fd, void *data, int size);
int usbEpPlatformWrite(void *fd, void *data, int size);

#ifdef __cplusplus
}
#endif
