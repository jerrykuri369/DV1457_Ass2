/*
 * matinv-par.c - parallel matrix inversion (Gauss-Jordan elimination).
 *
 * Same options, matrix initialisation and report format as the sequential
 * matrix_inverse.c from the course, and the same algorithm (no pivot
 * search; the generated matrices are diagonally dominant).
 *
 * Parallel strategy: the pivot loop has to run in order, but inside one
 * pivot step every other row can be eliminated independently. Each thread
 * owns a contiguous block of rows; two barriers per pivot keep the threads
 * in step:
 *   1. thread 0 divides the pivot row by the pivot value;
 *      barrier - nobody reads the pivot row before it is ready;
 *   2. every thread eliminates its own rows (all except the pivot row);
 *      barrier - nobody starts the next pivot while others still work.
 * Every element goes through exactly the same operations, in the same
 * order, as in the sequential program, so the results are identical.
 *
 * Built into the server, and on its own (with -DMATINV_STANDALONE) as the
 * matinvpar program.
 */
#define _GNU_SOURCE
#include "matinv_par.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "parallel.h"

#define DEFAULT_SIZE 5
#define DEFAULT_MAXNUM 15
#define DEFAULT_INIT "fast"
#define DEFAULT_PRINT 1
/* The sequential program never calls srand(), so rand() starts at seed 1.
   random_r() with the same seed yields the same numbers, but per job. */
#define RANDOM_SEED 1

/* Element (row, col) of an n x n row-major matrix. */
#define AT(m, n, row, col) ((m)[(size_t)(row) * (size_t)(n) + (size_t)(col)])

static const char USAGE[] =
    "Usage: matinvpar [-n problemsize]\n"
    "           [-D] show default values\n"
    "           [-h] help\n"
    "           [-I init_type] fast/rand\n"
    "           [-m maxnum] max random no\n"
    "           [-P print_switch] 0/1";

struct elimination {
    int n;
    int nthreads;
    double *a;   /* input matrix, becomes the identity */
    double *inv; /* identity, becomes the inverse */
    pthread_barrier_t barrier;
};

struct worker {
    struct elimination *e;
    int id;
};

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

static const char *expected_value(char opt)
{
    switch (opt) {
    case 'n': return "a size from 1 to 4096";
    case 'm': return "a positive integer";
    case 'P': return "0 or 1";
    default:  return "fast or rand";
    }
}

int matinv_parse_options(int argc, char **argv, struct matinv_options *opts,
                         char *msg, size_t msg_size)
{
    opts->size = DEFAULT_SIZE;
    opts->maxnum = DEFAULT_MAXNUM;
    snprintf(opts->init, sizeof opts->init, "%s", DEFAULT_INIT);
    opts->print = DEFAULT_PRINT;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value;
        char opt;
        int bad = 0;

        if (arg[0] != '-' || arg[1] == '\0' || arg[2] != '\0') {
            snprintf(msg, msg_size, "Unexpected argument '%s'.\n%s", arg, USAGE);
            return PARSE_ERROR;
        }
        opt = arg[1];
        switch (opt) {
        case 'h':
            snprintf(msg, msg_size, "HELP: try matinvpar -u");
            return PARSE_INFO;
        case 'u':
            snprintf(msg, msg_size, "%s", USAGE);
            return PARSE_INFO;
        case 'D':
            snprintf(msg, msg_size,
                     "Default:  n         = %d\n"
                     "          Init      = %s\n"
                     "          maxnum    = %d\n"
                     "          P         = %d",
                     DEFAULT_SIZE, DEFAULT_INIT, DEFAULT_MAXNUM, DEFAULT_PRINT);
            return PARSE_INFO;
        case 'n':
        case 'I':
        case 'm':
        case 'P':
            break;
        default:
            snprintf(msg, msg_size, "Unknown option '%s'.\n%s", arg, USAGE);
            return PARSE_ERROR;
        }

        if (i + 1 >= argc) {
            snprintf(msg, msg_size, "Option -%c needs a value.\n%s", opt, USAGE);
            return PARSE_ERROR;
        }
        value = argv[++i];
        switch (opt) {
        case 'n':
            bad = parse_int_arg(value, 1, MATINV_MAX_SIZE, &opts->size) != 0;
            break;
        case 'm':
            bad = parse_int_arg(value, 1, INT_MAX, &opts->maxnum) != 0;
            break;
        case 'P':
            bad = parse_int_arg(value, 0, 1, &opts->print) != 0;
            break;
        default: /* 'I' */
            bad = strcmp(value, "fast") != 0 && strcmp(value, "rand") != 0;
            if (!bad)
                snprintf(opts->init, sizeof opts->init, "%s", value);
            break;
        }
        if (bad) {
            snprintf(msg, msg_size, "Invalid value '%s' for -%c: expected %s.",
                     value, opt, expected_value(opt));
            return PARSE_ERROR;
        }
    }
    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* Computation                                                         */
/* ------------------------------------------------------------------ */

/* Same values, in the same order, as the sequential Init_Matrix(). */
static void init_matrices(const struct matinv_options *opts, double *a, double *inv)
{
    int n = opts->size;
    int use_rand = strcmp(opts->init, "rand") == 0;
    struct random_data rng;
    char rng_state[128];

    memset(&rng, 0, sizeof rng);
    initstate_r(RANDOM_SEED, rng_state, sizeof rng_state, &rng);

    for (int row = 0; row < n; row++) {
        for (int col = 0; col < n; col++) {
            AT(inv, n, row, col) = row == col ? 1.0 : 0.0;
            if (use_rand) {
                int32_t r;
                random_r(&rng, &r);
                /* diagonal dominance: +5 on the diagonal, +1 elsewhere */
                AT(a, n, row, col) = (double)(r % opts->maxnum) + (row == col ? 5.0 : 1.0);
            } else {
                AT(a, n, row, col) = row == col ? 5.0 : 2.0;
            }
        }
    }
}

/* Divides pivot row p of both matrices by the pivot value. */
static void normalise_pivot_row(struct elimination *e, int p)
{
    int n = e->n;
    double pivot = AT(e->a, n, p, p);

    for (int col = 0; col < n; col++) {
        AT(e->a, n, p, col) /= pivot;
        AT(e->inv, n, p, col) /= pivot;
    }
}

/* Subtracts a multiple of pivot row p from row r so that A[r][p] becomes 0. */
static void eliminate_row(struct elimination *e, int r, int p)
{
    int n = e->n;
    double multiplier = AT(e->a, n, r, p); /* read before row r changes */
    double *restrict a_row = &AT(e->a, n, r, 0);
    double *restrict inv_row = &AT(e->inv, n, r, 0);
    const double *restrict a_pivot = &AT(e->a, n, p, 0);
    const double *restrict inv_pivot = &AT(e->inv, n, p, 0);

    for (int col = 0; col < n; col++) {
        a_row[col] -= a_pivot[col] * multiplier;
        inv_row[col] -= inv_pivot[col] * multiplier;
    }
}

static void *eliminate_rows(void *arg)
{
    struct worker *w = arg;
    struct elimination *e = w->e;
    int first = (int)((long long)w->id * e->n / e->nthreads);
    int last = (int)((long long)(w->id + 1) * e->n / e->nthreads);

    for (int p = 0; p < e->n; p++) {
        if (w->id == 0)
            normalise_pivot_row(e, p);
        pthread_barrier_wait(&e->barrier); /* pivot row is ready */
        for (int r = first; r < last; r++)
            if (r != p)
                eliminate_row(e, r, p);
        pthread_barrier_wait(&e->barrier); /* pivot p finished everywhere */
    }
    return NULL;
}

static int invert(double *a, double *inv, int n, int nthreads)
{
    struct elimination e = { .n = n, .nthreads = nthreads, .a = a, .inv = inv };
    struct worker *workers = malloc((size_t)nthreads * sizeof *workers);
    int rc;

    if (!workers)
        return -1;
    if (pthread_barrier_init(&e.barrier, NULL, (unsigned)nthreads) != 0) {
        free(workers);
        return -1;
    }
    for (int i = 0; i < nthreads; i++)
        workers[i] = (struct worker){ &e, i };
    rc = run_threads(nthreads, eliminate_rows, workers, sizeof *workers);
    pthread_barrier_destroy(&e.barrier);
    free(workers);
    return rc;
}

double *matinv_compute(const struct matinv_options *opts, int nthreads,
                       char *err, size_t err_size)
{
    size_t cells = (size_t)opts->size * (size_t)opts->size;
    double *a = malloc(cells * sizeof *a);
    double *inv = malloc(cells * sizeof *inv);

    if (!a || !inv) {
        free(a);
        free(inv);
        snprintf(err, err_size, "Not enough memory for a %dx%d matrix", opts->size, opts->size);
        return NULL;
    }
    init_matrices(opts, a, inv);

    if (nthreads > opts->size)
        nthreads = opts->size;
    if (nthreads < 1)
        nthreads = 1;
    /* If the threads could not be started, nothing has been touched yet,
       so falling back to a single thread is safe. */
    if (invert(a, inv, opts->size, nthreads) != 0 && invert(a, inv, opts->size, 1) != 0) {
        free(a);
        free(inv);
        snprintf(err, err_size, "Could not start the worker threads");
        return NULL;
    }
    free(a);
    return inv;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

void matinv_write_report(FILE *out, const struct matinv_options *opts,
                         const double *inverse, int with_matrix)
{
    int n = opts->size;

    fprintf(out, "Matrix Inverse\n");
    fprintf(out, "\nsize      = %dx%d ", n, n);
    fprintf(out, "\nmaxnum    = %d \n", opts->maxnum);
    fprintf(out, "Init\t  = %s \n", opts->init);
    fprintf(out, "Initializing matrix...");
    fprintf(out, "done \n\n");
    if (!with_matrix)
        return;

    fprintf(out, "Inversed Matrix:\n");
    for (int row = 0; row < n; row++) {
        for (int col = 0; col < n; col++)
            fprintf(out, " %5.2f", AT(inverse, n, row, col));
        fputc('\n', out);
    }
    fprintf(out, "\n\n");
}

int matinv_run(const struct matinv_options *opts, int nthreads, FILE *out,
               char *err, size_t err_size)
{
    double *inverse = matinv_compute(opts, nthreads, err, err_size);

    if (!inverse)
        return -1;
    matinv_write_report(out, opts, inverse, 1);
    free(inverse);
    if (ferror(out)) {
        snprintf(err, err_size, "Could not write the result");
        return -1;
    }
    return 0;
}

#ifdef MATINV_STANDALONE
int main(int argc, char **argv)
{
    struct matinv_options opts;
    char msg[1024];
    double *inverse;
    int rc = matinv_parse_options(argc, argv, &opts, msg, sizeof msg);

    if (rc != PARSE_OK) {
        fprintf(rc == PARSE_INFO ? stdout : stderr, "%s\n", msg);
        return rc == PARSE_INFO ? 0 : 1;
    }
    inverse = matinv_compute(&opts, cpu_count(), msg, sizeof msg);
    if (!inverse) {
        fprintf(stderr, "%s\n", msg);
        return 1;
    }
    matinv_write_report(stdout, &opts, inverse, opts.print == 1);
    free(inverse);
    return 0;
}
#endif
