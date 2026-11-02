/*
 * kmeans-par.c - parallel k-means clustering.
 *
 * Same algorithm, initial centroids and results format as the sequential
 * kmeans.c from the course, plus the -f (data file) and -k (clusters)
 * options and any number of points.
 *
 * Parallel strategy - every iteration has two phases, each followed by a
 * barrier:
 *   1. assignment: each thread finds the nearest centroid for its own
 *      contiguous slice of the points and records whether any changed;
 *   2. update: each thread recomputes the centroids it owns (clusters
 *      id, id + T, id + 2T, ...), scanning the points in order. The float
 *      sums are therefore added in exactly the same order as in the
 *      sequential program, so centroids and clusters are identical.
 * Thread 0 also counts the iteration and decides whether to stop. Each
 * value is written by one thread only, so no locks are needed.
 *
 * Built into the server, and on its own (with -DKMEANS_STANDALONE) as the
 * kmeanspar program.
 */
#define _GNU_SOURCE
#include "kmeans_par.h"

#include <float.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "parallel.h"

#define MAX_CLUSTERS 1000000
/* The sequential program calls srand(0) before choosing the centroids. */
#define RANDOM_SEED 0

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)

static const char USAGE[] =
    "Usage: kmeanspar [-f file] [-k clusters]\n"
    "           [-f file]     data file, one \"x y\" point per line (default " KMEANS_DEFAULT_FILE ")\n"
    "           [-k clusters] number of clusters (default " TO_STRING(KMEANS_DEFAULT_K) ")\n"
    "           [-h] help";

struct clustering {
    struct kmeans_point *points;
    int n;
    int k;
    int nthreads;
    float *cx, *cy;       /* centroids */
    float *sum_x, *sum_y; /* per-cluster sums, written by the owner thread */
    int *members;         /* points per cluster, written by the owner thread */
    int *changed;         /* per thread: did any of its points move? */
    int done;             /* written by thread 0 between the barriers */
    int iterations;
    pthread_barrier_t barrier;
};

struct worker {
    struct clustering *c;
    int id;
};

/* ------------------------------------------------------------------ */
/* Options and input                                                   */
/* ------------------------------------------------------------------ */

int kmeans_parse_options(int argc, char **argv, struct kmeans_options *opts,
                         char *msg, size_t msg_size)
{
    snprintf(opts->file, sizeof opts->file, "%s", KMEANS_DEFAULT_FILE);
    opts->k = KMEANS_DEFAULT_K;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "-u") == 0) {
            snprintf(msg, msg_size, "%s", USAGE);
            return PARSE_INFO;
        }
        if (strcmp(arg, "-f") != 0 && strcmp(arg, "-k") != 0) {
            snprintf(msg, msg_size, "Unknown option '%s'.\n%s", arg, USAGE);
            return PARSE_ERROR;
        }
        if (i + 1 >= argc) {
            snprintf(msg, msg_size, "Option %s needs a value.\n%s", arg, USAGE);
            return PARSE_ERROR;
        }
        value = argv[++i];
        if (arg[1] == 'f') {
            size_t len = strlen(value);
            if (len >= sizeof opts->file) {
                snprintf(msg, msg_size, "The file name is too long.");
                return PARSE_ERROR;
            }
            memcpy(opts->file, value, len + 1);
        } else if (parse_int_arg(value, 1, MAX_CLUSTERS, &opts->k) != 0) {
            snprintf(msg, msg_size, "Invalid value '%s' for -k: expected a positive number of clusters.",
                     value);
            return PARSE_ERROR;
        }
    }
    return PARSE_OK;
}

static int load_points(const char *path, struct kmeans_result *result,
                       char *err, size_t err_size)
{
    FILE *in = fopen(path, "r");
    size_t count = 0, capacity = 1024;
    struct kmeans_point *points;
    float x, y;
    int rc = 0, read_error;

    if (!in) {
        snprintf(err, err_size, "Cannot open the data file: %s", strerror(errno));
        return -1;
    }
    points = malloc(capacity * sizeof *points);
    while (points && (rc = fscanf(in, "%f %f", &x, &y)) == 2) {
        if (count == capacity) {
            struct kmeans_point *grown = NULL;
            if (capacity < (size_t)INT_MAX / 2)
                grown = realloc(points, 2 * capacity * sizeof *points);
            if (!grown) {
                free(points);
                points = NULL;
                break;
            }
            points = grown;
            capacity *= 2;
        }
        points[count++] = (struct kmeans_point){ x, y, -1 };
    }
    read_error = ferror(in);
    fclose(in);

    if (!points) {
        snprintf(err, err_size, "Not enough memory for the data points");
        return -1;
    }
    if (rc != EOF || read_error) {
        free(points);
        snprintf(err, err_size,
                 "Malformed data file: expected \"x y\" pairs (stopped after %zu points)", count);
        return -1;
    }
    if (count == 0) {
        free(points);
        snprintf(err, err_size, "The data file contains no points");
        return -1;
    }
    result->points = points;
    result->count = (int)count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clustering                                                          */
/* ------------------------------------------------------------------ */

/* Same choice as the sequential program: k random points (they may repeat). */
static void choose_initial_centroids(struct clustering *c)
{
    struct random_data rng;
    char rng_state[128];

    memset(&rng, 0, sizeof rng);
    initstate_r(RANDOM_SEED, rng_state, sizeof rng_state, &rng);
    for (int j = 0; j < c->k; j++) {
        int32_t r;
        random_r(&rng, &r);
        c->cx[j] = c->points[r % c->n].x;
        c->cy[j] = c->points[r % c->n].y;
    }
}

static int nearest_centroid(const struct clustering *c, const struct kmeans_point *p)
{
    int nearest = 0;
    double min_dist = DBL_MAX;

    for (int j = 0; j < c->k; j++) {
        /* float differences widened to double, as in the sequential code */
        double xdist = p->x - c->cx[j];
        double ydist = p->y - c->cy[j];
        double dist = xdist * xdist + ydist * ydist;
        if (dist <= min_dist) { /* "<=": ties go to the later cluster */
            min_dist = dist;
            nearest = j;
        }
    }
    return nearest;
}

static int assign_points(struct clustering *c, int first, int last)
{
    int changed = 0;

    for (int i = first; i < last; i++) {
        int nearest = nearest_centroid(c, &c->points[i]);
        if (nearest != c->points[i].cluster) {
            c->points[i].cluster = nearest;
            changed = 1;
        }
    }
    return changed;
}

/* Recomputes the centroids owned by thread `id`. */
static void update_centroids(struct clustering *c, int id)
{
    for (int j = id; j < c->k; j += c->nthreads) {
        c->sum_x[j] = 0.0f;
        c->sum_y[j] = 0.0f;
        c->members[j] = 0;
    }
    for (int i = 0; i < c->n; i++) {
        int j = c->points[i].cluster;
        if (j % c->nthreads != id)
            continue;
        c->sum_x[j] += c->points[i].x;
        c->sum_y[j] += c->points[i].y;
        c->members[j]++;
    }
    for (int j = id; j < c->k; j += c->nthreads) {
        if (c->members[j] > 0) { /* an empty cluster keeps its centroid */
            c->cx[j] = c->sum_x[j] / c->members[j];
            c->cy[j] = c->sum_y[j] / c->members[j];
        }
    }
}

static void *cluster_worker(void *arg)
{
    struct worker *w = arg;
    struct clustering *c = w->c;
    int first = (int)((long long)w->id * c->n / c->nthreads);
    int last = (int)((long long)(w->id + 1) * c->n / c->nthreads);

    for (;;) {
        c->changed[w->id] = assign_points(c, first, last);
        pthread_barrier_wait(&c->barrier); /* every point has its cluster */

        update_centroids(c, w->id);
        if (w->id == 0) {
            int any_changed = 0;
            for (int t = 0; t < c->nthreads; t++)
                any_changed |= c->changed[t];
            c->iterations++;
            c->done = !any_changed;
        }
        pthread_barrier_wait(&c->barrier); /* centroids and 'done' are ready */
        if (c->done)
            return NULL;
    }
}

static int cluster(struct clustering *c, int nthreads)
{
    struct worker *workers = malloc((size_t)nthreads * sizeof *workers);
    int rc;

    if (!workers)
        return -1;
    if (pthread_barrier_init(&c->barrier, NULL, (unsigned)nthreads) != 0) {
        free(workers);
        return -1;
    }
    c->nthreads = nthreads;
    c->iterations = 0;
    c->done = 0;
    for (int i = 0; i < nthreads; i++)
        workers[i] = (struct worker){ c, i };
    rc = run_threads(nthreads, cluster_worker, workers, sizeof *workers);
    pthread_barrier_destroy(&c->barrier);
    free(workers);
    return rc;
}

int kmeans_compute(const struct kmeans_options *opts, int nthreads,
                   struct kmeans_result *result, char *err, size_t err_size)
{
    struct clustering c = { 0 };
    int rc = -1;

    memset(result, 0, sizeof *result);
    if (load_points(opts->file, result, err, err_size) < 0)
        return -1;
    if (opts->k > result->count) {
        snprintf(err, err_size, "Cannot make %d clusters from %d points", opts->k, result->count);
        kmeans_free_result(result);
        return -1;
    }
    if (nthreads > result->count)
        nthreads = result->count;
    if (nthreads < 1)
        nthreads = 1;

    c.points = result->points;
    c.n = result->count;
    c.k = opts->k;
    c.cx = malloc((size_t)c.k * sizeof *c.cx);
    c.cy = malloc((size_t)c.k * sizeof *c.cy);
    c.sum_x = malloc((size_t)c.k * sizeof *c.sum_x);
    c.sum_y = malloc((size_t)c.k * sizeof *c.sum_y);
    c.members = malloc((size_t)c.k * sizeof *c.members);
    c.changed = malloc((size_t)nthreads * sizeof *c.changed);

    if (c.cx && c.cy && c.sum_x && c.sum_y && c.members && c.changed) {
        choose_initial_centroids(&c);
        /* If the threads could not be started, nothing has been touched yet. */
        rc = cluster(&c, nthreads);
        if (rc != 0)
            rc = cluster(&c, 1);
        if (rc != 0)
            snprintf(err, err_size, "Could not start the worker threads");
    } else {
        snprintf(err, err_size, "Not enough memory for %d clusters", c.k);
    }

    free(c.cx);
    free(c.cy);
    free(c.sum_x);
    free(c.sum_y);
    free(c.members);
    free(c.changed);
    if (rc != 0) {
        kmeans_free_result(result);
        return -1;
    }
    result->iterations = c.iterations;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

void kmeans_write_results(FILE *out, const struct kmeans_result *result)
{
    for (int i = 0; i < result->count; i++) {
        const struct kmeans_point *p = &result->points[i];
        fprintf(out, "%.2f %.2f %d\n", p->x, p->y, p->cluster);
    }
}

void kmeans_free_result(struct kmeans_result *result)
{
    free(result->points);
    result->points = NULL;
    result->count = 0;
}

int kmeans_run(const struct kmeans_options *opts, int nthreads, FILE *out,
               char *err, size_t err_size)
{
    struct kmeans_result result;

    if (kmeans_compute(opts, nthreads, &result, err, err_size) != 0)
        return -1;
    kmeans_write_results(out, &result);
    kmeans_free_result(&result);
    if (ferror(out)) {
        snprintf(err, err_size, "Could not write the result");
        return -1;
    }
    return 0;
}

#ifdef KMEANS_STANDALONE
int main(int argc, char **argv)
{
    struct kmeans_options opts;
    struct kmeans_result result;
    char msg[1024];
    FILE *out;
    int rc = kmeans_parse_options(argc, argv, &opts, msg, sizeof msg);

    if (rc != PARSE_OK) {
        fprintf(rc == PARSE_INFO ? stdout : stderr, "%s\n", msg);
        return rc == PARSE_INFO ? 0 : 1;
    }
    if (kmeans_compute(&opts, cpu_count(), &result, msg, sizeof msg) != 0) {
        fprintf(stderr, "%s\n", msg);
        return 1;
    }
    printf("Read the problem data!\n");
    printf("Number of iterations taken = %d\n", result.iterations);
    printf("Computed cluster numbers successfully!\n");

    out = fopen(KMEANS_RESULTS_FILE, "w");
    if (!out) {
        perror("Cannot open the file");
        kmeans_free_result(&result);
        return 1;
    }
    kmeans_write_results(out, &result);
    rc = fclose(out);
    kmeans_free_result(&result);
    if (rc != 0) {
        perror("Cannot write the results");
        return 1;
    }
    printf("Wrote the results to a file!\n");
    return 0;
}
#endif
