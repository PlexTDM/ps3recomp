/*
 * ps3recomp - cellSync2 HLE implementation
 *
 * Extended sync primitives using host OS primitives:
 * pthread_mutex, pthread_cond, ps3_sem_t (runtime/platform/posix_sem.h)
 */

#include "cellSync2.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include "../../runtime/platform/posix_sem.h"   /* portable timed semaphore */
#include <errno.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Internal layout overlaid on opaque buffers
 * -----------------------------------------------------------------------*/

typedef struct Sync2MutexInternal {
    u32 initialized;
    u32 recursive;
    u32 lockCount;    /* for recursive */
    u32 ownerThread;  /* simplified - not truly thread-safe ID */
    pthread_mutex_t mtx;
} Sync2MutexInternal;

typedef struct Sync2CondInternal {
    u32 initialized;
    Sync2MutexInternal* mutex;
    pthread_cond_t cv;
} Sync2CondInternal;

typedef struct Sync2SemInternal {
    u32 initialized;
    s32 value;
    s32 maxValue;
    ps3_sem_t sem;
} Sync2SemInternal;

typedef struct Sync2QueueInternal {
    u32 initialized;
    u32 head;
    u32 tail;
    u32 count;
    u32 depth;
    u32 elemSize;
    u8* buffer;
    pthread_mutex_t mtx;
    pthread_cond_t notFull;
    pthread_cond_t notEmpty;
} Sync2QueueInternal;

/* Compile-time size checks */
_Static_assert(sizeof(Sync2MutexInternal) <= CELL_SYNC2_MUTEX_SIZE,
               "Sync2MutexInternal too large");
_Static_assert(sizeof(Sync2CondInternal) <= CELL_SYNC2_COND_SIZE,
               "Sync2CondInternal too large");
_Static_assert(sizeof(Sync2SemInternal) <= CELL_SYNC2_SEMAPHORE_SIZE,
               "Sync2SemInternal too large");
_Static_assert(sizeof(Sync2QueueInternal) <= CELL_SYNC2_QUEUE_SIZE,
               "Sync2QueueInternal too large");

/* =========================================================================
 * Mutex
 * =====================================================================*/

s32 cellSync2MutexAttributeInitialize(CellSync2MutexAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2MutexAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    attr->recursive = 0;
    return CELL_OK;
}

s32 cellSync2MutexInit(CellSync2Mutex* mutex, const CellSync2MutexAttribute* attr)
{
    attr = GUEST_PTR(attr, const CellSync2MutexAttribute*);
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    memset(m, 0, sizeof(Sync2MutexInternal));
    m->recursive = attr ? attr->recursive : 0;
    m->initialized = 1;

    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    if (m->recursive)
        pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mtx, &mattr);
    pthread_mutexattr_destroy(&mattr);

    return CELL_OK;
}

s32 cellSync2MutexLock(CellSync2Mutex* mutex, u64 timeoutUsec)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    if (timeoutUsec == 0) {
        /* Non-blocking: try once */
        if (pthread_mutex_trylock(&m->mtx) != 0)
            return CELL_SYNC2_ERROR_BUSY;
    } else if (timeoutUsec == (u64)-1) {
        /* Infinite wait */
        pthread_mutex_lock(&m->mtx);
    } else {
        /* Timed wait: spin with yield until timeout */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeoutUsec / 1000000);
        ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (pthread_mutex_timedlock(&m->mtx, &ts) != 0)
            return CELL_SYNC2_ERROR_BUSY;
    }

    return CELL_OK;
}

s32 cellSync2MutexTryLock(CellSync2Mutex* mutex)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    if (pthread_mutex_trylock(&m->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    return CELL_OK;
}

s32 cellSync2MutexUnlock(CellSync2Mutex* mutex)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    pthread_mutex_unlock(&m->mtx);

    return CELL_OK;
}

/* =========================================================================
 * Condition Variable
 * =====================================================================*/

s32 cellSync2CondAttributeInitialize(CellSync2CondAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2CondAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    return CELL_OK;
}

s32 cellSync2CondInit(CellSync2Cond* cond, CellSync2Mutex* mutex,
                      const CellSync2CondAttribute* attr)
{
    (void)attr;

    if (!cond || !mutex) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    memset(c, 0, sizeof(Sync2CondInternal));
    c->mutex = (Sync2MutexInternal*)mutex;
    c->initialized = 1;

    pthread_cond_init(&c->cv, NULL);

    return CELL_OK;
}

s32 cellSync2CondWait(CellSync2Cond* cond, u64 timeoutUsec)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    if (timeoutUsec == 0) {
        pthread_cond_wait(&c->cv, &c->mutex->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
        ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&c->cv, &c->mutex->mtx, &ts);
        if (rc == ETIMEDOUT)
            return CELL_SYNC2_ERROR_TIMEDOUT;
    }

    return CELL_OK;
}

s32 cellSync2CondSignal(CellSync2Cond* cond)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    pthread_cond_signal(&c->cv);

    return CELL_OK;
}

s32 cellSync2CondSignalAll(CellSync2Cond* cond)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    pthread_cond_broadcast(&c->cv);

    return CELL_OK;
}

/* =========================================================================
 * Semaphore
 * =====================================================================*/

s32 cellSync2SemaphoreAttributeInitialize(CellSync2SemaphoreAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2SemaphoreAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    attr->maxValue = 0x7FFFFFFF;
    return CELL_OK;
}

s32 cellSync2SemaphoreInit(CellSync2Semaphore* sem, s32 initialValue,
                           const CellSync2SemaphoreAttribute* attr)
{
    attr = GUEST_PTR(attr, const CellSync2SemaphoreAttribute*);
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    memset(s, 0, sizeof(Sync2SemInternal));
    s->value = initialValue;
    s->maxValue = attr ? attr->maxValue : 0x7FFFFFFF;
    s->initialized = 1;

    if (ps3_sem_init(&s->sem, (unsigned)initialValue) != 0)
        return CELL_SYNC2_ERROR_STAT;

    return CELL_OK;
}

s32 cellSync2SemaphoreAcquire(CellSync2Semaphore* sem, u32 count,
                              u64 timeoutUsec)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    for (u32 i = 0; i < count; i++) {
        if (timeoutUsec == 0) {
            ps3_sem_wait(&s->sem);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            if (ps3_sem_timedwait(&s->sem, &ts) != 0) {
                if (errno == ETIMEDOUT) return CELL_SYNC2_ERROR_TIMEDOUT;
                return CELL_SYNC2_ERROR_STAT;
            }
        }
    }

    return CELL_OK;
}

s32 cellSync2SemaphoreTryAcquire(CellSync2Semaphore* sem, u32 count)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    for (u32 i = 0; i < count; i++) {
        if (ps3_sem_trywait(&s->sem) != 0) {
            if (errno == EAGAIN) return CELL_SYNC2_ERROR_BUSY;
            return CELL_SYNC2_ERROR_STAT;
        }
    }

    return CELL_OK;
}

s32 cellSync2SemaphoreRelease(CellSync2Semaphore* sem, u32 count)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    for (u32 i = 0; i < count; i++) {
        ps3_sem_post(&s->sem);
    }

    return CELL_OK;
}

s32 cellSync2SemaphoreGetValue(const CellSync2Semaphore* sem, s32* value)
{
    if (!sem || !value) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    int v;
    ps3_sem_getvalue(&s->sem, &v);
    vm_write32((u32)(uintptr_t)value, (u32)v);

    return CELL_OK;
}

/* =========================================================================
 * Queue
 * =====================================================================*/

s32 cellSync2QueueAttributeInitialize(CellSync2QueueAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2QueueAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    /* ponytail: writes the correct 128-byte ABI layout, but like the rest of this
     * module it does NOT translate the guest EA (GUEST_PTR) or byte-swap to
     * big-endian -- cellSync2QueueInit ignores attr, so nothing consumes these bytes
     * yet. Add GUEST_PTR + be_t writes here (and module-wide) if a title starts
     * reading the attribute back. */
    memset(attr, 0, sizeof(*attr));
    attr->maxPushWaiters = 32;
    attr->maxPopWaiters  = 32;
    return CELL_OK;
}

s32 cellSync2QueueInit(CellSync2Queue* queue, void* buffer,
                       u32 elemSize, u32 depth,
                       const CellSync2QueueAttribute* attr)
{
    buffer = GUEST_PTR(buffer, void*);
    (void)attr;

    if (!queue || !buffer) return CELL_SYNC2_ERROR_NULL_POINTER;
    if (elemSize == 0 || depth == 0) return CELL_SYNC2_ERROR_INVAL;

    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    memset(q, 0, sizeof(Sync2QueueInternal));
    q->depth = depth;
    q->elemSize = elemSize;
    q->buffer = (u8*)buffer;
    q->initialized = 1;

    memset(buffer, 0, (size_t)elemSize * depth);

    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->notFull, NULL);
    pthread_cond_init(&q->notEmpty, NULL);

    return CELL_OK;
}

s32 cellSync2QueuePush(CellSync2Queue* queue, const void* data,
                       u64 timeoutUsec)
{
    data = GUEST_PTR(data, const void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    pthread_mutex_lock(&q->mtx);
    while (q->count >= q->depth) {
        if (timeoutUsec == 0) {
            pthread_cond_wait(&q->notFull, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&q->notFull, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return CELL_SYNC2_ERROR_TIMEDOUT;
            }
        }
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    pthread_cond_signal(&q->notEmpty);
    pthread_mutex_unlock(&q->mtx);

    return CELL_OK;
}

s32 cellSync2QueueTryPush(CellSync2Queue* queue, const void* data)
{
    data = GUEST_PTR(data, const void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    if (pthread_mutex_trylock(&q->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count >= q->depth) {
        pthread_mutex_unlock(&q->mtx);
        return CELL_SYNC2_ERROR_OVERFLOW;
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    pthread_cond_signal(&q->notEmpty);
    pthread_mutex_unlock(&q->mtx);

    return CELL_OK;
}

s32 cellSync2QueuePop(CellSync2Queue* queue, void* data,
                      u64 timeoutUsec)
{
    data = GUEST_PTR(data, void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        if (timeoutUsec == 0) {
            pthread_cond_wait(&q->notEmpty, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&q->notEmpty, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return CELL_SYNC2_ERROR_TIMEDOUT;
            }
        }
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mtx);

    return CELL_OK;
}

s32 cellSync2QueueTryPop(CellSync2Queue* queue, void* data)
{
    data = GUEST_PTR(data, void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    if (pthread_mutex_trylock(&q->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return CELL_SYNC2_ERROR_EMPTY;
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mtx);

    return CELL_OK;
}

s32 cellSync2QueueSize(const CellSync2Queue* queue, u32* size)
{
    if (!queue || !size) return CELL_SYNC2_ERROR_NULL_POINTER;
    const Sync2QueueInternal* q = (const Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    vm_write32((u32)(uintptr_t)size, q->count);
    return CELL_OK;
}
