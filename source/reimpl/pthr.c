/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

// Valor sin inicializar = uno de los PTHREAD_*_INITIALIZER estáticos de bionic.
#define PTHR_IS_STATIC(p) ((uintptr_t)(p) == 0 || (uintptr_t)(p) == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER || \
                           (uintptr_t)(p) == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER)

static pthread_mutex_t *_mutex_new(int kind) {
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    if (pthread_mutex_init(m, &mutattr) != 0) { free(m); m = NULL; }
    pthread_mutexattr_destroy(&mutattr);
    return m;
}

// Init perezoso de mutex estáticos (bionic permite usarlos sin pthread_mutex_init).
// CAS: con hilos en núcleos distintos (2026-11-23), dos primeros lock simultáneos
// creaban dos mutex distintos -> sin exclusión -> lista de Vox corrupta -> doble free.
PTHR_INLINE pthread_mutex_t *_mutex_get(pthread_mutex_t_bionic *mutex) {
    pthread_mutex_t *cur = mutex->real_ptr;
    if (!PTHR_IS_STATIC(cur)) return cur;
    int kind = (uintptr_t)cur == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER ? PTHREAD_MUTEX_RECURSIVE
             : (uintptr_t)cur == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER ? PTHREAD_MUTEX_ERRORCHECK
             : PTHREAD_MUTEX_NORMAL;
    pthread_mutex_t *m = _mutex_new(kind);
    if (!m) return NULL;
    if (!__sync_bool_compare_and_swap(&mutex->real_ptr, cur, m)) {
        pthread_mutex_destroy(m); // otro hilo ganó la carrera
        free(m);
    }
    return mutex->real_ptr;
}

PTHR_INLINE pthread_cond_t *_cond_get(pthread_cond_t_bionic *cond) {
    pthread_cond_t *cur = cond->real_ptr;
    if (cur) return cur;
    pthread_cond_t *c = malloc(sizeof(*c));
    if (!c) return NULL;
    if (pthread_cond_init(c, NULL) != 0) { free(c); return NULL; }
    if (!__sync_bool_compare_and_swap(&cond->real_ptr, NULL, c)) {
        pthread_cond_destroy(c);
        free(c);
    }
    return cond->real_ptr;
}

// Reparto de núcleos (2026-11-22): sin máscara, un hilo nuevo hereda el
// núcleo de su creador -> los hilos del motor (Vox, carga, streaming, red)
// competían con el hilo del juego en el núcleo 0. Núcleo 0 = solo el juego
// (main.c); los del motor van a 1-2 (el audio de Vox se fija al 2 en
// audiotrack.c).
typedef struct { void *(*start)(void *); void *param; } thr_tramp;

static void *thr_entry(void *p) {
    thr_tramp t = *(thr_tramp *)p;
    free(p);
    sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF,
                                         SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
    return t.start(t.param);
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;
    thr_tramp *t = malloc(sizeof(*t));
    if (t) {
        t->start = start;
        t->param = param;
        start = thr_entry;
        param = t;
    }

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else{
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }
    if (ret != 0 && t)
        free(t);

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    int kind = PTHREAD_MUTEX_NORMAL;
    if (attr) pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    // Siempre uno nuevo: la memoria puede venir de malloc con basura.
    uid->real_ptr = _mutex_new(kind);
    return uid->real_ptr ? 0 : ENOMEM;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex || PTHR_IS_STATIC(mutex->real_ptr)) return 0;
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = _mutex_get(mutex);
    return m ? pthread_mutex_lock(m) : ENOMEM;
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    pthread_mutex_t *m = _mutex_get(mutex);
    return m ? pthread_mutex_trylock(m) : ENOMEM;
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (PTHR_IS_STATIC(mutex->real_ptr)) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    pthread_cond_t *c = malloc(sizeof(*c));
    if (!c) return ENOMEM;
    int ret = pthread_cond_init(c, attr);
    if (ret != 0) { free(c); c = NULL; }
    cond->real_ptr = c;
    return ret;
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond || !cond->real_ptr) return 0;
    int ret = pthread_cond_destroy(cond->real_ptr);
    free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    pthread_cond_t *c = _cond_get(cond);
    return c ? pthread_cond_signal(c) : ENOMEM;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    pthread_cond_t *c = _cond_get(cond);
    pthread_mutex_t *m = _mutex_get(mutex);
    return c && m ? pthread_cond_timedwait(c, m, abstime) : ENOMEM;
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    pthread_cond_t *c = _cond_get(cond);
    pthread_mutex_t *m = _mutex_get(mutex);
    return c && m ? pthread_cond_wait(c, m) : ENOMEM;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    pthread_cond_t *c = _cond_get(cond);
    return c ? pthread_cond_broadcast(c) : ENOMEM;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) return -1;
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
