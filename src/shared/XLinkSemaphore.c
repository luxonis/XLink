// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <errno.h>
#include "XLinkSemaphore.h"
#include "XLinkErrorUtils.h"
#include "XLinkLog.h"

static int xlink_sem_is_initialized(const XLink_sem_t* sem)
{
    return __atomic_load_n(&sem->initialized, __ATOMIC_ACQUIRE);
}

static void xlink_sem_set_initialized(XLink_sem_t* sem, int value)
{
    __atomic_store_n(&sem->initialized, value, __ATOMIC_RELEASE);
}

int XLink_sem_inc(XLink_sem_t* sem)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    XLINK_RET_ERR_IF(!xlink_sem_is_initialized(sem), -1);
    XLINK_RET_IF_FAIL(pthread_mutex_lock(&sem->ref_mutex));
    if (sem->refs < 0) {
        // Semaphore has been already destroyed
        XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));
        return -1;
    }

    sem->refs++;
    XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));

    return 0;
}

int XLink_sem_dec(XLink_sem_t* sem)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    XLINK_RET_ERR_IF(!xlink_sem_is_initialized(sem), -1);
    XLINK_RET_IF_FAIL(pthread_mutex_lock(&sem->ref_mutex));
    if (sem->refs < 1) {
        // Can't decrement reference count if there are no waiters
        // or semaphore has been already destroyed
        XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));
        return -1;
    }

    sem->refs--;
    int ret = pthread_cond_broadcast(&sem->ref_cond);
    XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));

    return ret;
}


int XLink_sem_init(XLink_sem_t* sem, int pshared, unsigned int value)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);

    int created_sync = 0;
    if (!xlink_sem_is_initialized(sem)) {
        XLINK_RET_IF_FAIL(pthread_mutex_init(&sem->ref_mutex, NULL));
        XLINK_RET_IF_FAIL(pthread_cond_init(&sem->ref_cond, NULL));
        xlink_sem_set_initialized(sem, 1);
        created_sync = 1;
    }
    int ret = sem_init(&sem->psem, pshared, value);
    if (ret != 0) {
        if (created_sync) {
            pthread_cond_destroy(&sem->ref_cond);
            pthread_mutex_destroy(&sem->ref_mutex);
            xlink_sem_set_initialized(sem, 0);
        }
        return ret;
    }
    sem->refs = 0;

    return 0;
}

int XLink_sem_destroy(XLink_sem_t* sem)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    if (!xlink_sem_is_initialized(sem)) {
        return 0;
    }

    XLINK_RET_IF_FAIL(pthread_mutex_lock(&sem->ref_mutex));
    if (sem->refs < 0) {
        // Semaphore has been already destroyed
        XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));
        return -1;
    }

    while(sem->refs > 0) {
        if (pthread_cond_wait(&sem->ref_cond, &sem->ref_mutex)) {
            break;
        };
    }
    sem->refs = -1;
    int ret = sem_destroy(&sem->psem);
    XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));
    XLINK_RET_IF_FAIL(pthread_cond_destroy(&sem->ref_cond));
    XLINK_RET_IF_FAIL(pthread_mutex_destroy(&sem->ref_mutex));
    xlink_sem_set_initialized(sem, 0);

    return ret;
}

int XLink_sem_post(XLink_sem_t* sem)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    if (!xlink_sem_is_initialized(sem)) {
        return -1;
    }
    XLINK_RET_IF_FAIL(pthread_mutex_lock(&sem->ref_mutex));
    if (sem->refs < 0) {
        XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));
        return -1;
    }

    int ret = sem_post(&sem->psem);
    XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));

    return ret;
}

int XLink_sem_wait(XLink_sem_t* sem)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);

    XLINK_RET_IF_FAIL(XLink_sem_inc(sem));
    int ret;
    while(((ret = sem_wait(&sem->psem) == -1) && errno == EINTR))
        continue;
    XLINK_RET_IF_FAIL(XLink_sem_dec(sem));

    return ret;
}

int XLink_sem_timedwait(XLink_sem_t* sem, const struct timespec* abstime)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    XLINK_RET_ERR_IF(abstime == NULL, -1);

    XLINK_RET_IF_FAIL(XLink_sem_inc(sem));
    int ret;
    while(((ret = sem_timedwait(&sem->psem, abstime)) == -1) && errno == EINTR)
        continue;
    XLINK_RET_IF_FAIL(XLink_sem_dec(sem));

    return ret;
}

int XLink_sem_trywait(XLink_sem_t* sem)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);

    XLINK_RET_IF_FAIL(XLink_sem_inc(sem));
    int ret = sem_trywait(&sem->psem);
    XLINK_RET_IF_FAIL(XLink_sem_dec(sem));

    return ret;
}

int XLink_sem_set_refs(XLink_sem_t* sem, int refs)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    XLINK_RET_ERR_IF(refs < -1, -1);

    if (!xlink_sem_is_initialized(sem)) {
        sem->refs = refs;
        return 0;
    }

    XLINK_RET_IF_FAIL(pthread_mutex_lock(&sem->ref_mutex));
    sem->refs = refs;
    int ret = pthread_cond_broadcast(&sem->ref_cond);
    XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));

    return ret;
}

int XLink_sem_get_refs(XLink_sem_t* sem, int *sval)
{
    XLINK_RET_ERR_IF(sem == NULL, -1);
    XLINK_RET_ERR_IF(sval == NULL, -1);

    if (!xlink_sem_is_initialized(sem)) {
        *sval = sem->refs;
        return 0;
    }

    XLINK_RET_IF_FAIL(pthread_mutex_lock(&sem->ref_mutex));
    *sval = sem->refs;
    XLINK_RET_IF_FAIL(pthread_mutex_unlock(&sem->ref_mutex));
    return 0;
}
