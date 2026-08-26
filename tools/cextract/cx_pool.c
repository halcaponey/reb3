/* cx_pool.c -- the parallel-for and the source lock.  See cx_pool.h for the
 * three rules that make this safe; this file is just the mechanism.
 *
 * Threads are created per cx_pool_for() call rather than kept alive between
 * them.  A stage calls this once or twice for a job measured in seconds, so
 * ~30 us of pthread_create per worker is not worth a resident pool and the
 * shutdown ordering one would need against the game's own exit path.
 */
/* -std=c11 alone hides POSIX, and PTHREAD_MUTEX_RECURSIVE / sysconf() are
 * POSIX -- cx_src.c asks for the same level, for the same reason. */
#define _POSIX_C_SOURCE 200809L

#include "cx_pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include <unistd.h>
#endif

#define CX_POOL_MAX 16

/* ------------------------------------------------------- the worker count */
static int g_workers = -1;

int cx_pool_workers(void)
{
    if (g_workers < 0) {
        const char *e = getenv("B3_JOBS");
        int n;
        if (e && *e) {
            n = atoi(e);
        } else {
#ifdef __EMSCRIPTEN__
            /* the link's PTHREAD_POOL_SIZE minus main() -- see cx_pool.h */
            n = 3;
#else
            long c = sysconf(_SC_NPROCESSORS_ONLN);
            n = (c > 0) ? (int)c : 1;
            if (n > 8) n = 8;
#endif
        }
        if (n < 1) n = 1;
        if (n > CX_POOL_MAX) n = CX_POOL_MAX;
        g_workers = n;
    }
    return g_workers;
}

/* ---------------------------------------------------------- the source lock
 * Recursive because cx_vfs_fopen() -> cx_src_map() both take it, and because
 * a stage may already hold it around a small read loop.  PTHREAD_MUTEX_
 * RECURSIVE has no static initialiser, so it is built once under pthread_once
 * rather than assumed. */
static pthread_mutex_t g_io_mtx;
static pthread_once_t  g_io_once = PTHREAD_ONCE_INIT;

static void io_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_io_mtx, &a);
    pthread_mutexattr_destroy(&a);
}

void cx_pool_lock(void)
{
    pthread_once(&g_io_once, io_init);
    pthread_mutex_lock(&g_io_mtx);
}

void cx_pool_unlock(void)
{
    pthread_once(&g_io_once, io_init);
    pthread_mutex_unlock(&g_io_mtx);
}

/* ------------------------------------------------------------ parallel-for */
typedef struct {
    void  (*fn)(void *, int);
    void   *ctx;
    int     n;
    int     next;
    pthread_mutex_t mtx;
} cx_job;

static void *worker(void *arg)
{
    cx_job *j = (cx_job *)arg;
    for (;;) {
        int i;
        pthread_mutex_lock(&j->mtx);
        i = j->next < j->n ? j->next++ : -1;
        pthread_mutex_unlock(&j->mtx);
        if (i < 0)
            break;
        j->fn(j->ctx, i);
    }
    return NULL;
}

void cx_pool_for(int n, void (*fn)(void *ctx, int i), void *ctx)
{
    pthread_t th[CX_POOL_MAX];
    cx_job    j;
    int       w, started = 0, i;

    if (n <= 0 || !fn)
        return;

    w = cx_pool_workers();
    if (w > n) w = n;
    if (w <= 1) {                     /* rule 3: one thread means no thread */
        for (i = 0; i < n; i++)
            fn(ctx, i);
        return;
    }

    j.fn = fn; j.ctx = ctx; j.n = n; j.next = 0;
    pthread_mutex_init(&j.mtx, NULL);

    for (i = 0; i < w; i++)
        if (pthread_create(&th[started], NULL, worker, &j) == 0)
            started++;

    /* A machine that would not give us a single thread still has to do the
     * work; the calling thread is a worker too, which also means a stage
     * never sits idle waiting. */
    worker(&j);

    for (i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    pthread_mutex_destroy(&j.mtx);
}
