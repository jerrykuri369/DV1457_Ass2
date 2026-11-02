/*
 * job.c - turns a client's command into a result file and a reply.
 * Used by every strategy; job_execute() is thread-safe.
 */
#define _GNU_SOURCE
#include "job.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parallel.h"
#include "protocol.h"
#include "server_util.h"

#define MAX_ARGS 64

static enum job_status set_reply(struct job *job, uint32_t type, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static enum job_status set_reply(struct job *job, uint32_t type, const char *fmt, ...)
{
    va_list ap;
    int len;

    free(job->reply);
    va_start(ap, fmt);
    len = vasprintf(&job->reply, fmt, ap);
    va_end(ap);
    if (len < 0) {
        job->reply = NULL;
        len = 0;
    }
    job->reply_type = type;
    job->reply_len = (uint32_t)len;
    return JOB_REPLY_ONLY;
}

void job_fail(struct job *job, const char *message)
{
    set_reply(job, MSG_ERROR, "%s", message);
}

void job_release(struct job *job)
{
    free(job->reply);
    job->reply = NULL;
    job->reply_len = 0;
}

/* Splits line in place into words. Returns the count, or -1 if too many. */
static int split_words(char *line, char **argv, int max)
{
    int argc = 0;
    char *save = NULL;

    for (char *word = strtok_r(line, " \t\r\n", &save); word; word = strtok_r(NULL, " \t\r\n", &save)) {
        if (argc == max)
            return -1;
        argv[argc++] = word;
    }
    argv[argc] = NULL;
    return argc;
}

static int is_command(const char *word, const char *name, const char *alias)
{
    return strcmp(word, name) == 0 || strcmp(word, alias) == 0;
}

enum job_status job_prepare(struct job *job, const char *command, struct client_info *client)
{
    char line[MAX_COMMAND_LEN + 1];
    char *argv[MAX_ARGS + 1];
    char message[1024];
    int argc, rc;

    memset(job, 0, sizeof *job);
    job->client_number = client->number;
    snprintf(line, sizeof line, "%s", command);

    argc = split_words(line, argv, MAX_ARGS);
    if (argc < 0)
        return set_reply(job, MSG_ERROR, "Too many arguments (at most %d).", MAX_ARGS - 1);
    if (argc == 0)
        return set_reply(job, MSG_ERROR, "Empty command.");

    if (is_command(argv[0], "matinvpar", "matinv-par")) {
        job->kind = JOB_MATINV;
        rc = matinv_parse_options(argc, argv, &job->matinv, message, sizeof message);
        if (rc != PARSE_OK)
            return set_reply(job, rc == PARSE_INFO ? MSG_INFO : MSG_ERROR, "%s", message);
        job->solution_number = ++client->matinv_solutions;
        snprintf(job->result_name, sizeof job->result_name, "matinv_client%d_soln%d.txt",
                 client->number, job->solution_number);
        return JOB_READY;
    }
    if (is_command(argv[0], "kmeanspar", "kmeans-par")) {
        job->kind = JOB_KMEANS;
        rc = kmeans_parse_options(argc, argv, &job->kmeans, message, sizeof message);
        if (rc != PARSE_OK)
            return set_reply(job, rc == PARSE_INFO ? MSG_INFO : MSG_ERROR, "%s", message);
        job->solution_number = ++client->kmeans_solutions;
        snprintf(job->result_name, sizeof job->result_name, "kmeans_client%d_soln%d.txt",
                 client->number, job->solution_number);
        return JOB_NEEDS_DATA; /* the data file is on the client's machine */
    }
    return set_reply(job, MSG_ERROR,
                     "Unknown command '%s'. Available commands: matinvpar, kmeanspar.", argv[0]);
}

void job_cancel(struct job *job, struct client_info *client)
{
    if (job->kind == JOB_KMEANS)
        client->kmeans_solutions--;
    else
        client->matinv_solutions--;
    job_release(job);
}

/* Creates `name` in computed_results (never replacing a file) and writes data. */
static int write_new_file(const char *name, const char *data, size_t length,
                          char *path, size_t path_size)
{
    int fd, saved_errno;
    size_t done = 0;

    if (snprintf(path, path_size, "%s/%s", paths_results_dir(), name) >= (int)path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        done += (size_t)n;
    }
    saved_errno = errno;
    if (close(fd) != 0 && done == length) {
        saved_errno = errno;
        done = 0;
    }
    if (done != length) {
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int job_store_data(struct job *job, const char *data, size_t length)
{
    char name[64];

    snprintf(name, sizeof name, "kmeans_client%d_data%d.txt", job->client_number, job->solution_number);
    if (write_new_file(name, data, length, job->kmeans.file, sizeof job->kmeans.file) < 0) {
        set_reply(job, MSG_ERROR, "Could not store the data file: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void job_execute(struct job *job)
{
    char err[512] = "";
    char path[PATH_MAX];
    char *text = NULL;
    size_t text_len = 0;
    size_t name_len = strlen(job->result_name) + 1; /* name and '\n' */
    FILE *mem = open_memstream(&text, &text_len);
    int rc;

    if (!mem) {
        job_fail(job, "The server is out of memory.");
        return;
    }
    /* The reply payload is "<name>\n<contents>"; build it in one buffer. */
    fprintf(mem, "%s\n", job->result_name);
    if (job->kind == JOB_MATINV)
        rc = matinv_run(&job->matinv, cpu_count(), mem, err, sizeof err);
    else
        rc = kmeans_run(&job->kmeans, cpu_count(), mem, err, sizeof err);
    if (fclose(mem) != 0 && rc == 0) {
        rc = -1;
        snprintf(err, sizeof err, "The server is out of memory.");
    }
    if (rc == 0 && text_len > MAX_PAYLOAD_LEN) {
        rc = -1;
        snprintf(err, sizeof err, "The result is too large to send.");
    }
    if (rc == 0 && write_new_file(job->result_name, text + name_len, text_len - name_len,
                                  path, sizeof path) < 0) {
        rc = -1;
        snprintf(err, sizeof err, "Could not save %s: %s", job->result_name, strerror(errno));
    }
    if (rc != 0) {
        free(text);
        job_fail(job, err);
        return;
    }
    free(job->reply);
    job->reply = text;
    job->reply_len = (uint32_t)text_len;
    job->reply_type = MSG_RESULT;
}
