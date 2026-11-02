#ifndef MATINV_PAR_H
#define MATINV_PAR_H

#include <stddef.h>
#include <stdio.h>

#include "solver.h"

#define MATINV_MAX_SIZE 4096 /* same limit as the sequential program */

struct matinv_options {
    int size;     /* -n: the matrix is size x size */
    int maxnum;   /* -m: bound for random entries */
    char init[8]; /* -I: "fast" or "rand" */
    int print;    /* -P: 1 prints the inverse (standalone program) */
};

/* Parses argv (argv[0] is the command name). See enum parse_result. */
int matinv_parse_options(int argc, char **argv, struct matinv_options *opts,
                         char *msg, size_t msg_size);

/*
 * Builds the input matrix and inverts it using nthreads threads.
 * Returns the size x size inverse (row-major, caller frees), or NULL with a
 * message in err.
 */
double *matinv_compute(const struct matinv_options *opts, int nthreads,
                       char *err, size_t err_size);

/* Writes the sequential program's report; the matrix only if with_matrix. */
void matinv_write_report(FILE *out, const struct matinv_options *opts,
                         const double *inverse, int with_matrix);

/* Server entry point: computes and writes the full report, always with the
   matrix. Returns 0 on success, -1 with a message in err. */
int matinv_run(const struct matinv_options *opts, int nthreads, FILE *out,
               char *err, size_t err_size);

#endif
