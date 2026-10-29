/*
 * ps3recomp - Reader-writer lock syscalls (implementation)
 */

#include "sys_rwlock.h"
#include "../memory/vm.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_rwlock_info g_sys_rwlocks[SYS_RWLOCK_MAX];

/* The write lock's owner, or 0. Only the owner ever writes it, and only
 * while it holds the lock, so a reader either sees its own id or somebody
 * else's -- never a stale "it is me". Acquire/release pair it with the
 * pthread lock itself so the id is published with the acquisition. */
static uint64_t rwlock_writer(const sys_rwlock_info* r)
{
    return __atomic_load_n(&r->writer_tid, __ATOMIC_ACQUIRE);
}

static void rwlock_set_writer(sys_rwlock_info* r, uint64_t tid)
{
    __atomic_store_n(&r->writer_tid, tid, __ATOMIC_RELEASE);
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_rwlock_create
 *
 * r3 = pointer to receive rwlock ID (u32*)
 * r4 = pointer to attribute struct
 * -----------------------------------------------------------------------*/
int64_t sys_rwlock_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_RWLOCK_MAX; i++) {
        if (!g_sys_rwlocks[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_rwlock_info* r = &g_sys_rwlocks[slot];
    memset(r, 0, sizeof(*r));
    r->active = 1;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        r->protocol = proto_be;
        memcpy(r->name, attr_raw + 8, 8);
    }

    pthread_rwlock_init(&r->rwl, NULL);
    r->writer_tid = 0;

    uint32_t rwlock_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, rwlock_id);
    }

    return CELL_OK;
}

int64_t sys_rwlock_destroy(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    pthread_rwlock_destroy(&r->rwl);

    r->active = 0;
    return CELL_OK;
}

int64_t sys_rwlock_rlock(ppu_context* ctx)
{
    uint32_t rwlock_id  = LV2_ARG_U32(ctx, 0);
    /* uint64_t timeout = LV2_ARG_U64(ctx, 1); -- timeout not commonly used */

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    pthread_rwlock_rdlock(&r->rwl);

    return CELL_OK;
}

int64_t sys_rwlock_tryrlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    int rc = pthread_rwlock_tryrdlock(&r->rwl);
    if (rc != 0)
        return (int64_t)(int32_t)CELL_EBUSY;

    return CELL_OK;
}

int64_t sys_rwlock_runlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    pthread_rwlock_unlock(&r->rwl);

    return CELL_OK;
}

int64_t sys_rwlock_wlock(ppu_context* ctx)
{
    uint32_t rwlock_id  = LV2_ARG_U32(ctx, 0);
    /* uint64_t timeout = LV2_ARG_U64(ctx, 1); */

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* pthread_rwlock_wrlock by the thread that already holds the lock is
     * undefined behaviour. Some libpthreads do not block or report it; they
     * return 0, which is worse: the guest is told it took a second lock level
     * that no unlock will ever match. Answer EDEADLK before touching the
     * lock. */
    if (rwlock_writer(r) == ctx->thread_id)
        return (int64_t)(int32_t)CELL_EDEADLK;
    pthread_rwlock_wrlock(&r->rwl);
    rwlock_set_writer(r, ctx->thread_id);

    return CELL_OK;
}

int64_t sys_rwlock_trywlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (rwlock_writer(r) == ctx->thread_id)
        return (int64_t)(int32_t)CELL_EDEADLK;
    int rc = pthread_rwlock_trywrlock(&r->rwl);
    if (rc != 0)
        return (int64_t)(int32_t)CELL_EBUSY;
    rwlock_set_writer(r, ctx->thread_id);

    return CELL_OK;
}

int64_t sys_rwlock_wunlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Same guard: pthread_rwlock_unlock does not distinguish a read unlock
     * from a write one, so without the owner check a stray wunlock releases
     * whatever the calling thread happens to hold, or somebody else's write
     * lock entirely. */
    if (rwlock_writer(r) != ctx->thread_id)
        return (int64_t)(int32_t)CELL_EPERM;
    rwlock_set_writer(r, 0);
    pthread_rwlock_unlock(&r->rwl);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_rwlock_init(lv2_syscall_table* tbl)
{
    memset(g_sys_rwlocks, 0, sizeof(g_sys_rwlocks));

    lv2_syscall_register(tbl, SYS_RWLOCK_CREATE,   sys_rwlock_create);
    lv2_syscall_register(tbl, SYS_RWLOCK_DESTROY,  sys_rwlock_destroy);
    lv2_syscall_register(tbl, SYS_RWLOCK_RLOCK,    sys_rwlock_rlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_TRYRLOCK, sys_rwlock_tryrlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_RUNLOCK,  sys_rwlock_runlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_WLOCK,    sys_rwlock_wlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_TRYWLOCK, sys_rwlock_trywlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_WUNLOCK,  sys_rwlock_wunlock);
}
