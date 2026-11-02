/*
 * mux_core.c - client handling shared by muxbasic (poll) and muxscale (epoll).
 *
 * One thread runs the event loop and does all socket I/O, without ever
 * blocking: every client has a small state machine and its own input and
 * output buffers. Computations would block the loop for seconds, so they
 * go to a queue served by a few worker threads (the solvers then use their
 * own pthreads). A worker reports a finished job by writing the job pointer
 * into a pipe that the loop watches like any other descriptor. Everything
 * stays inside one process.
 */
#define _GNU_SOURCE
#include "mux.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "job.h"
#include "parallel.h"
#include "protocol.h"
#include "server_util.h"

#define KEEP_OUTPUT_BUFFER (1u << 20) /* free larger buffers once sent */

enum stage {
    STAGE_WAIT_COMMAND, /* reading the next command */
    STAGE_WAIT_DATA,    /* k-means: reading the uploaded data file */
    STAGE_COMPUTING     /* a worker thread is running this client's job */
};

struct mux_client {
    int fd;
    struct client_info info;
    enum stage stage;
    struct job *job;      /* job waiting for its data file */
    unsigned interest;    /* events currently requested from the backend */

    unsigned char header[MSG_HEADER_SIZE]; /* incoming message */
    size_t header_got;
    uint32_t body_type, body_len, body_got;
    char *body;

    char *out;            /* outgoing bytes */
    size_t out_len, out_sent, out_cap;

    int closing;          /* close once the output has been sent */
    int closed;           /* socket closed; memory freed later */
    struct mux_client *next_dead;
};

struct job_queue {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    struct job *head, *tail;
};

struct mux_server {
    struct mux_backend backend;
    int listen_fd;
    int done_read, done_write; /* workers write finished jobs here */
    int spare_fd;              /* given up when out of descriptors */
    int next_client_number;
    struct job_queue queue;
    struct mux_client *dead;   /* closed clients, freed after the batch */
    char listener_tag, done_tag; /* their addresses tag these descriptors */
};

static void close_client(struct mux_server *s, struct mux_client *c);

/* ------------------------------------------------------------------ */
/* Client lifetime and output                                          */
/* ------------------------------------------------------------------ */

static void free_client(struct mux_client *c)
{
    if (c->job) {
        job_release(c->job);
        free(c->job);
    }
    free(c->body);
    free(c->out);
    free(c);
}

/* Freed only in mux_server_end_batch(): the current batch of events may
   still mention this client. */
static void bury(struct mux_server *s, struct mux_client *c)
{
    c->next_dead = s->dead;
    s->dead = c;
}

static void close_client(struct mux_server *s, struct mux_client *c)
{
    if (c->closed)
        return;
    s->backend.remove(s->backend.impl, c->fd);
    close(c->fd);
    c->fd = -1;
    c->closed = 1;
    log_info("Client %d disconnected", c->info.number);
    if (c->stage != STAGE_COMPUTING) /* otherwise job_finished() buries it */
        bury(s, c);
}

static unsigned wanted_events(const struct mux_client *c)
{
    if (c->out_sent < c->out_len)
        return MUX_WRITE; /* finish replying before reading the next request */
    if (c->closing || c->stage == STAGE_COMPUTING)
        return 0;
    return MUX_READ;
}

static void update_interest(struct mux_server *s, struct mux_client *c)
{
    unsigned want;

    if (c->closed)
        return;
    want = wanted_events(c);
    if (want == c->interest)
        return;
    if (s->backend.modify(s->backend.impl, c->fd, c, want) < 0) {
        log_error("Cannot watch client %d: %s", c->info.number, strerror(errno));
        close_client(s, c);
        return;
    }
    c->interest = want;
}

static void flush_output(struct mux_server *s, struct mux_client *c)
{
    while (!c->closed && c->out_sent < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_sent, c->out_len - c->out_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_client(s, c);
            return;
        }
        c->out_sent += (size_t)n;
    }
    if (c->closed)
        return;
    c->out_len = c->out_sent = 0;
    if (c->out_cap > KEEP_OUTPUT_BUFFER) {
        free(c->out);
        c->out = NULL;
        c->out_cap = 0;
    }
    if (c->closing)
        close_client(s, c);
}

/* Appends a message to the client's output and starts sending it. */
static void queue_message(struct mux_server *s, struct mux_client *c, uint32_t type,
                          const void *payload, uint32_t length)
{
    size_t needed;

    if (c->closed)
        return;
    needed = c->out_len + MSG_HEADER_SIZE + length;
    if (needed > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        char *grown;
        while (cap < needed)
            cap *= 2;
        grown = realloc(c->out, cap);
        if (!grown) {
            log_error("Out of memory while replying to client %d", c->info.number);
            close_client(s, c);
            return;
        }
        c->out = grown;
        c->out_cap = cap;
    }
    encode_header((unsigned char *)c->out + c->out_len, type, length);
    if (length > 0)
        memcpy(c->out + c->out_len + MSG_HEADER_SIZE, payload, length);
    c->out_len = needed;
    flush_output(s, c);
}

static void queue_text(struct mux_server *s, struct mux_client *c, uint32_t type, const char *text)
{
    queue_message(s, c, type, text, (uint32_t)strlen(text));
}

static void queue_reply(struct mux_server *s, struct mux_client *c, const struct job *job)
{
    if (job->reply_type == MSG_RESULT)
        log_info("Sending solution: %s", job->result_name);
    queue_message(s, c, job->reply_type, job->reply, job->reply_len);
}

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

static void *job_worker(void *arg)
{
    struct mux_server *s = arg;

    for (;;) {
        struct job *job;
        const char *p;
        size_t left;

        pthread_mutex_lock(&s->queue.lock);
        while (!s->queue.head)
            pthread_cond_wait(&s->queue.not_empty, &s->queue.lock);
        job = s->queue.head;
        s->queue.head = job->next;
        if (!s->queue.head)
            s->queue.tail = NULL;
        pthread_mutex_unlock(&s->queue.lock);

        job_execute(job);

        /* Hand the job back to the event loop (pointer-sized writes to a
           pipe are atomic). */
        p = (const char *)&job;
        left = sizeof job;
        while (left > 0) {
            ssize_t n = write(s->done_write, p, left);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                log_error("Lost a finished job: %s", strerror(errno));
                break;
            }
            p += n;
            left -= (size_t)n;
        }
    }
    return NULL;
}

static void start_job(struct mux_server *s, struct mux_client *c, struct job *job)
{
    job->owner = c;
    job->next = NULL;
    c->stage = STAGE_COMPUTING;

    pthread_mutex_lock(&s->queue.lock);
    if (s->queue.tail)
        s->queue.tail->next = job;
    else
        s->queue.head = job;
    s->queue.tail = job;
    pthread_cond_signal(&s->queue.not_empty);
    pthread_mutex_unlock(&s->queue.lock);
}

static void job_finished(struct mux_server *s, struct job *job)
{
    struct mux_client *c = job->owner;

    c->stage = STAGE_WAIT_COMMAND;
    if (c->closed) {
        bury(s, c); /* the client left while its job was running */
    } else {
        queue_reply(s, c, job);
        update_interest(s, c);
    }
    job_release(job);
    free(job);
}

static void collect_finished_jobs(struct mux_server *s)
{
    struct job *done[64];

    for (;;) {
        ssize_t n = read(s->done_read, done, sizeof done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return; /* EAGAIN: nothing more for now */
        for (size_t i = 0; i < (size_t)n / sizeof done[0]; i++)
            job_finished(s, done[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

static void handle_command(struct mux_server *s, struct mux_client *c, const char *command)
{
    struct job *job = calloc(1, sizeof *job);

    log_info("Client %d commanded: %s", c->info.number, command);
    if (!job) {
        queue_text(s, c, MSG_ERROR, "The server is out of memory.");
        return;
    }
    switch (job_prepare(job, command, &c->info)) {
    case JOB_READY:
        start_job(s, c, job);
        return;
    case JOB_NEEDS_DATA:
        c->job = job;
        c->stage = STAGE_WAIT_DATA;
        queue_text(s, c, MSG_NEED_DATA, job->kmeans.file);
        return;
    case JOB_REPLY_ONLY:
        queue_reply(s, c, job);
        job_release(job);
        free(job);
        return;
    }
}

static void handle_message(struct mux_server *s, struct mux_client *c,
                           uint32_t type, const char *body, uint32_t length)
{
    struct job *job;

    if (c->stage == STAGE_WAIT_COMMAND && type == MSG_COMMAND) {
        handle_command(s, c, body);
        return;
    }
    if (c->stage != STAGE_WAIT_DATA || (type != MSG_DATA && type != MSG_CANCEL)) {
        queue_text(s, c, MSG_ERROR, "Unexpected message.");
        return;
    }

    job = c->job;
    c->job = NULL;
    c->stage = STAGE_WAIT_COMMAND;
    if (type == MSG_CANCEL) {
        log_info("Client %d cancelled the upload", c->info.number);
        job_cancel(job, &c->info);
        free(job);
        return;
    }
    if (job_store_data(job, body, length) < 0) {
        queue_reply(s, c, job);
        job_release(job);
        free(job);
        return;
    }
    log_info("Received the data file from client %d (%u bytes)", c->info.number, length);
    start_job(s, c, job);
}

/* Called once the 8-byte header is in: checks the size, allocates the body. */
static int start_body(struct mux_server *s, struct mux_client *c)
{
    uint32_t limit;

    decode_header(c->header, &c->body_type, &c->body_len);
    limit = c->stage == STAGE_WAIT_DATA ? MAX_PAYLOAD_LEN : MAX_COMMAND_LEN;
    if (c->body_len > limit) {
        c->closing = 1; /* the stream cannot be resynchronised */
        queue_text(s, c, MSG_ERROR, "Message too large; closing the connection.");
        return -1;
    }
    c->body = malloc((size_t)c->body_len + 1);
    if (!c->body) {
        close_client(s, c);
        return -1;
    }
    c->body_got = 0;
    return 0;
}

static void complete_message(struct mux_server *s, struct mux_client *c)
{
    char *body = c->body;

    body[c->body_len] = '\0';
    c->body = NULL;
    c->header_got = 0;
    handle_message(s, c, c->body_type, body, c->body_len);
    free(body);
}

/* Returns 1 if n bytes arrived; otherwise stops reading (closing on EOF or error). */
static int received(struct mux_server *s, struct mux_client *c, ssize_t n)
{
    if (n > 0)
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0; /* level-triggered: we will be told again */
    close_client(s, c);
    return 0;
}

/* Reads what is available, one message at a time and never more than the
   current message, so a following request stays queued in the kernel. */
static void read_input(struct mux_server *s, struct mux_client *c)
{
    while (!c->closed && (wanted_events(c) & MUX_READ)) {
        if (c->header_got < MSG_HEADER_SIZE) {
            ssize_t n = recv(c->fd, c->header + c->header_got, MSG_HEADER_SIZE - c->header_got, 0);
            if (!received(s, c, n))
                return;
            c->header_got += (size_t)n;
            if (c->header_got < MSG_HEADER_SIZE)
                continue;
            if (start_body(s, c) < 0)
                return;
        }
        if (c->body_got < c->body_len) {
            ssize_t n = recv(c->fd, c->body + c->body_got, c->body_len - c->body_got, 0);
            if (!received(s, c, n))
                return;
            c->body_got += (uint32_t)n;
            if (c->body_got < c->body_len)
                continue;
        }
        complete_message(s, c);
    }
}

/* ------------------------------------------------------------------ */
/* New connections                                                     */
/* ------------------------------------------------------------------ */

static void add_client(struct mux_server *s, int fd)
{
    struct mux_client *c = calloc(1, sizeof *c);
    char welcome[16];

    if (!c || set_nonblocking(fd) < 0) {
        free(c);
        close(fd);
        return;
    }
    c->fd = fd;
    c->info.number = s->next_client_number++;
    c->stage = STAGE_WAIT_COMMAND;
    if (s->backend.add(s->backend.impl, fd, c, MUX_READ) < 0) {
        log_error("Cannot watch a new client: %s", strerror(errno));
        close(fd);
        free(c);
        return;
    }
    c->interest = MUX_READ;
    log_info("Connected with client %d", c->info.number);
    snprintf(welcome, sizeof welcome, "%d", c->info.number);
    queue_text(s, c, MSG_WELCOME, welcome);
    update_interest(s, c);
}

static void accept_clients(struct mux_server *s)
{
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);

        if (fd >= 0) {
            add_client(s, fd);
            continue;
        }
        if (errno == EINTR || errno == ECONNABORTED)
            continue;
        if ((errno == EMFILE || errno == ENFILE) && s->spare_fd >= 0) {
            /* Out of descriptors: accept with the spare one and hang up,
               instead of leaving the connection pending (which would make
               the loop spin). */
            close(s->spare_fd);
            fd = accept(s->listen_fd, NULL, NULL);
            if (fd >= 0)
                close(fd);
            s->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            log_error("Too many open connections; refused a client");
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            log_error("accept: %s", strerror(errno));
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

static int start_workers(struct mux_server *s)
{
    int wanted = cpu_count() > 2 ? cpu_count() : 2;
    int started = 0;
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) != 0)
        return -1;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (int i = 0; i < wanted; i++) {
        pthread_t thread;
        if (pthread_create(&thread, &attr, job_worker, s) == 0)
            started++;
    }
    pthread_attr_destroy(&attr);
    return started > 0 ? 0 : -1;
}

struct mux_server *mux_server_create(int listen_fd, int first_client_number,
                                     const struct mux_backend *backend)
{
    struct mux_server *s = calloc(1, sizeof *s);
    int pipe_fds[2];

    if (!s || pipe2(pipe_fds, O_CLOEXEC) < 0) {
        log_error("Cannot start the server: %s", strerror(errno));
        free(s);
        return NULL;
    }
    s->backend = *backend;
    s->listen_fd = listen_fd;
    s->done_read = pipe_fds[0];
    s->done_write = pipe_fds[1];
    s->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    s->next_client_number = first_client_number;
    pthread_mutex_init(&s->queue.lock, NULL);
    pthread_cond_init(&s->queue.not_empty, NULL);

    if (set_nonblocking(listen_fd) < 0 || set_nonblocking(s->done_read) < 0
        || backend->add(backend->impl, listen_fd, &s->listener_tag, MUX_READ) < 0
        || backend->add(backend->impl, s->done_read, &s->done_tag, MUX_READ) < 0
        || start_workers(s) < 0) {
        log_error("Cannot start the server: %s", strerror(errno));
        return NULL; /* the process exits; nothing to clean up */
    }
    return s;
}

void mux_server_handle(struct mux_server *s, void *tag, unsigned events)
{
    struct mux_client *c = tag;

    if (tag == &s->listener_tag) {
        accept_clients(s);
        return;
    }
    if (tag == &s->done_tag) {
        collect_finished_jobs(s);
        return;
    }
    if (c->closed)
        return;
    if (events & MUX_ERROR) {
        close_client(s, c);
        return;
    }
    if (events & MUX_WRITE)
        flush_output(s, c);
    if (events & MUX_READ)
        read_input(s, c);
    update_interest(s, c);
}

void mux_server_end_batch(struct mux_server *s)
{
    while (s->dead) {
        struct mux_client *c = s->dead;
        s->dead = c->next_dead;
        free_client(c);
    }
}
