#ifndef JOB_H
#define JOB_H

#include <limits.h>
#include <stdint.h>

#include "kmeans_par.h"
#include "matinv_par.h"

enum job_kind { JOB_MATINV, JOB_KMEANS };

/* Per-connection counters; they give every result file a unique name. */
struct client_info {
    int number;
    int matinv_solutions;
    int kmeans_solutions;
};

enum job_status {
    JOB_READY,      /* parsed: call job_execute() */
    JOB_NEEDS_DATA, /* k-means: ask the client for kmeans.file first */
    JOB_REPLY_ONLY  /* nothing to compute: send the INFO/ERROR reply */
};

/* One client request, from parsing to the reply that goes back. */
struct job {
    enum job_kind kind;
    int client_number;
    int solution_number;
    struct matinv_options matinv;
    struct kmeans_options kmeans;
    char result_name[64]; /* e.g. matinv_client1_soln2.txt */
    uint32_t reply_type;  /* MSG_RESULT, MSG_INFO or MSG_ERROR */
    char *reply;          /* malloc'd reply payload */
    uint32_t reply_len;
    void *owner;          /* used by the multiplexing strategies */
    struct job *next;     /* link in the multiplexing job queue */
};

/* Parses a client command. Counts the solution when the job is accepted. */
enum job_status job_prepare(struct job *job, const char *command,
                            struct client_info *client);

/* Undoes job_prepare() for a k-means job whose upload was cancelled. */
void job_cancel(struct job *job, struct client_info *client);

/* Saves the uploaded k-means data and points the job at the saved copy.
   Returns 0, or -1 with an ERROR reply set. */
int job_store_data(struct job *job, const char *data, size_t length);

/* Runs the solver, saves the result file and sets a RESULT or ERROR reply.
   Safe to call from any thread. */
void job_execute(struct job *job);

/* Replaces the reply with an error message. */
void job_fail(struct job *job, const char *message);

/* Frees the reply. */
void job_release(struct job *job);

#endif
