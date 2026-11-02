// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>
#include <string.h>

#include "XLinkStream.h"
#include "XLinkErrorUtils.h"
#include "XLinkPrivateFields.h"

#ifdef MVLOG_UNIT_NAME
#undef MVLOG_UNIT_NAME
#define MVLOG_UNIT_NAME xLink
#endif

#include "XLinkLog.h"
#include "XLinkStringUtils.h"

static void clearStreamState(streamDesc_t* stream) {
    memset(stream, 0, offsetof(streamDesc_t, sem));
    stream->id = INVALID_STREAM_ID;
}

XLinkError_t XLinkStreamInitialize(
    streamDesc_t* stream, streamId_t id, const char* name) {
    mvLog(MVLOG_DEBUG, "name: %s, id: %ld\n", name, id);
    ASSERT_XLINK(stream);

    if (XLink_sem_wait(&stream->sem)) {
        return X_LINK_ERROR;
    }
    if (pthread_mutex_lock(&availableXLinksMutex)) {
        XLink_sem_post(&stream->sem);
        return X_LINK_ERROR;
    }
    clearStreamState(stream);
    stream->id = id;
    mv_strncpy(stream->name, MAX_STREAM_NAME_LENGTH,
               name, MAX_STREAM_NAME_LENGTH - 1);
    pthread_mutex_unlock(&availableXLinksMutex);

    return X_LINK_SUCCESS;
}

void XLinkStreamReset(streamDesc_t* stream) {
    if(stream == NULL) {
        return;
    }

    if (pthread_mutex_lock(&availableXLinksMutex)) return;
    clearStreamState(stream);
    pthread_mutex_unlock(&availableXLinksMutex);
}
