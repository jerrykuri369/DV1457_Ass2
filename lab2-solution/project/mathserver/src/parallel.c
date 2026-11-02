/* Starting and joining the worker threads used by both solvers. */
#include "parallel.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/* Solver threads use little stack; a small stack lets many jobs run at once. */
#define WORKER_STACK_SIZE (256 * 1024)

/* Threads wait here until every thread exists, so a failed pthread_create()
   can still cancel the run before any work has been done. */
struct gate {
    pthread_mutex_t lock;
    pthread_cond_t opened;
    int state; /* 0 = closed, 1 = go, -1 = abort */
};

struct start_info {
    struct gate *gate;
    void *(*fn)(void *);
    void *arg;
};

static void *start_when_open(void *p)
{
    struct start_info *info = p;
    int go;

    pthread_mutex_lock(&info->gate->lock);
    while (info->gate->state == 0)
        pthread_cond_wait(&info->gate->opened, &info->gate->lock);
    go = info->gate->state > 0;
    pthread_mutex_unlock(&info->gate->lock);
    return go ? info->fn(info->arg) : NULL;
}

int run_threads(int count, void *(*fn)(void *), void *args, size_t arg_size)
{
    struct gate gate = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 };
    pthread_t *threads;
    struct start_info *infos;
    pthread_attr_t attr;
    int created = 0;

    if (count <= 1) {
        fn(args);
        return 0;
    }
    threads = malloc((size_t)count * sizeof *threads);
    infos = malloc((size_t)count * sizeof *infos);
    if (threads && infos && pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);
        for (; created < count; created++) {
            infos[created] = (struct start_info){
                &gate, fn, (char *)args + (size_t)created * arg_size
            };
            if (pthread_create(&threads[created], &attr, start_when_open, &infos[created]) != 0)
                break;
        }
        pthread_attr_destroy(&attr);
    }

    pthread_mutex_lock(&gate.lock);
    gate.state = created == count ? 1 : -1;
    pthread_cond_broadcast(&gate.opened);
    pthread_mutex_unlock(&gate.lock);

    for (int i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    pthread_cond_destroy(&gate.opened);
    pthread_mutex_destroy(&gate.lock);
    free(threads);
    free(infos);
    return created == count ? 0 : -1;
}

int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
