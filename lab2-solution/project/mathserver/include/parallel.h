#ifndef PARALLEL_H
#define PARALLEL_H

#include <stddef.h>

/*
 * Runs fn on `count` threads and waits for all of them. Thread i receives
 * (char *)args + i * arg_size. Threads only start working once all of them
 * exist: if one cannot be created, the others quit without doing anything
 * and -1 is returned, so the caller can safely retry with fewer threads.
 * With count <= 1, fn runs in the calling thread.
 */
int run_threads(int count, void *(*fn)(void *), void *args, size_t arg_size);

/* Number of online CPU cores (at least 1). */
int cpu_count(void);

#endif
