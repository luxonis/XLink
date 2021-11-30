// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#ifdef __GNUC__
#include <pthread.h>
#else
#ifndef WIN_PTHREADS
#define WIN_PTHREADS

#include <windows.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/timeb.h>
#include <process.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ETIMEDOUT
#define ETIMEDOUT   110
#define ENOTSUP     134
#endif

//State related definition
#define PTHREAD_CANCEL_DISABLE 0
#define PTHREAD_CANCEL_ENABLE 0x01
#define PTHREAD_CREATE_DETACHED 0x04
#define PTHREAD_INHERIT_SCHED 0x08


//Mutex related definition

#define PTHREAD_MUTEX_INITIALIZER {(RTL_CRITICAL_SECTION_DEBUG*)-1,-1,0,0,0,0}


#if (_MSC_VER == 1800)
struct timespec
{
    /* long long in windows is the same as long in unix for 64bit */
    long long tv_sec;
    long long tv_nsec;
};
#elif (_MSC_VER >= 1800)
#include "time.h"
#endif

typedef struct
{
    HANDLE handle;
    void *arg;
    void *(*start_routine)(void *);
    DWORD tid;
    unsigned pthread_state;
}pthread_t;

typedef struct
{
    unsigned pthread_state;
    void *stack;
    size_t stack_size;
}pthread_attr_t;


typedef unsigned pthread_mutexattr_t;
typedef CRITICAL_SECTION pthread_mutex_t;

int pthread_mutex_lock(pthread_mutex_t *mutexm);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_init(pthread_mutex_t *mutex, pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);


unsigned _pthread_get_state(pthread_attr_t *attr, unsigned flag);
int _pthread_set_state(pthread_attr_t *attr, unsigned flag, unsigned val);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_setinheritsched(pthread_attr_t *attr, int flag);
int pthread_attr_getinheritsched(pthread_attr_t *attr, int *flag);
int pthread_attr_destroy(pthread_attr_t *attr);

#define pthread_equal(t1, t2) ((t1).tid == (t2).tid)
unsigned int __stdcall _pthread_start_routine(void *args);
int pthread_create(pthread_t *thread, pthread_attr_t *attr, void *(*func)(void *), void *arg);
#define pthread_join(a, b) _pthread_join(&(a), (b))
int _pthread_join(pthread_t *thread, void **res);
pthread_t pthread_self(void);
void pthread_exit(void *res);
int pthread_setname_np(pthread_t target_thread, const char * name);
int pthread_getname_np (pthread_t target_thread, char *__buf, size_t len);

int pthread_detach(pthread_t thread);


#ifdef __cplusplus
}
#endif

#endif /* WIN_PTHREADS */
#endif

