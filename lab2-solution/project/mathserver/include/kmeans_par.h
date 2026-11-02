#ifndef KMEANS_PAR_H
#define KMEANS_PAR_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#include "solver.h"

#define KMEANS_DEFAULT_FILE "kmeans-data.txt"
#define KMEANS_DEFAULT_K 9
#define KMEANS_RESULTS_FILE "kmeans-results.txt" /* standalone program output */

struct kmeans_options {
    char file[PATH_MAX]; /* -f: data file, one "x y" point per line */
    int k;               /* -k: number of clusters */
};

struct kmeans_point {
    float x;
    float y;
    int cluster;
};

struct kmeans_result {
    struct kmeans_point *points;
    int count;
    int iterations;
};

/* Parses argv (argv[0] is the command name). See enum parse_result. */
int kmeans_parse_options(int argc, char **argv, struct kmeans_options *opts,
                         char *msg, size_t msg_size);

/* Loads the data file and clusters it using nthreads threads.
   Returns 0 on success, -1 with a message in err. */
int kmeans_compute(const struct kmeans_options *opts, int nthreads,
                   struct kmeans_result *result, char *err, size_t err_size);

/* Writes "x y cluster" lines, the sequential program's results format. */
void kmeans_write_results(FILE *out, const struct kmeans_result *result);

void kmeans_free_result(struct kmeans_result *result);

/* Server entry point: compute and write the results. 0 on success. */
int kmeans_run(const struct kmeans_options *opts, int nthreads, FILE *out,
               char *err, size_t err_size);

#endif
