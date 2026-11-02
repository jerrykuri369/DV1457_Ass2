/*
 * strategy_fork.c - one child process per client.
 *
 * The parent only accepts connections. Each child serves one client with
 * plain blocking I/O and exits when the client leaves. A SIGCHLD handler
 * reaps finished children, so no zombies are left behind.
 */
#define _GNU_SOURCE
#include "strategies.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job.h"
#include "protocol.h"
#include "server_util.h"

static void reap_children(int sig)
{
    int saved_errno = errno; /* waitpid() may change errno */

    (void)sig;
    /* One SIGCHLD can stand for several finished children: reap them all. */
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

static int install_child_reaper(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; /* accept() resumes after the signal */
    return sigaction(SIGCHLD, &sa, NULL);
}

static int send_text(int fd, uint32_t type, const char *text)
{
    return send_msg(fd, type, text, (uint32_t)strlen(text));
}

/* For a k-means job: asks for the data file and stores it.
   Returns 1 when the job can run, 0 when it ended without a result, and
   -1 when the connection broke. */
static int receive_data(int fd, struct job *job, struct client_info *client)
{
    uint32_t type, length;
    char *data;
    int rc;

    if (send_text(fd, MSG_NEED_DATA, job->kmeans.file) < 0)
        return -1;
    if (recv_msg(fd, &type, &data, &length, MAX_PAYLOAD_LEN) < 0)
        return -1;
    if (type == MSG_CANCEL) {
        log_info("Client %d cancelled the upload", client->number);
        job_cancel(job, client);
        rc = 0;
    } else if (type != MSG_DATA) {
        job_fail(job, "Expected the data file.");
        rc = send_msg(fd, job->reply_type, job->reply, job->reply_len) < 0 ? -1 : 0;
        job_release(job);
    } else if (job_store_data(job, data, length) < 0) {
        rc = send_msg(fd, job->reply_type, job->reply, job->reply_len) < 0 ? -1 : 0;
        job_release(job);
    } else {
        log_info("Received the data file from client %d (%u bytes)", client->number, length);
        rc = 1;
    }
    free(data);
    return rc;
}

/* Runs one command. Returns -1 when the connection is no longer usable. */
static int handle_command(int fd, struct client_info *client, const char *command)
{
    struct job job;
    enum job_status status;
    int rc;

    log_info("Client %d commanded: %s", client->number, command);
    status = job_prepare(&job, command, client);
    if (status == JOB_NEEDS_DATA) {
        rc = receive_data(fd, &job, client);
        if (rc <= 0)
            return rc;
        status = JOB_READY;
    }
    if (status == JOB_READY)
        job_execute(&job);
    if (job.reply_type == MSG_RESULT)
        log_info("Sending solution: %s", job.result_name);
    rc = send_msg(fd, job.reply_type, job.reply, job.reply_len);
    job_release(&job);
    return rc;
}

static void serve_client(int fd, int number)
{
    struct client_info client = { .number = number };
    char welcome[16];

    snprintf(welcome, sizeof welcome, "%d", number);
    if (send_text(fd, MSG_WELCOME, welcome) == 0) {
        for (;;) {
            uint32_t type, length;
            char *payload;
            int rc;

            if (recv_msg(fd, &type, &payload, &length, MAX_COMMAND_LEN) < 0)
                break; /* client left, or sent something unacceptable */
            if (type == MSG_COMMAND)
                rc = handle_command(fd, &client, payload);
            else
                rc = send_text(fd, MSG_ERROR, "Expected a command.");
            free(payload);
            if (rc < 0)
                break;
        }
    }
    log_info("Client %d disconnected", number);
}

int run_fork_strategy(int listen_fd, int first_client_number)
{
    int next_number = first_client_number;

    if (install_child_reaper() < 0) {
        log_error("sigaction: %s", strerror(errno));
        return 1;
    }
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        int number;
        pid_t pid;

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            log_error("accept: %s", strerror(errno));
            usleep(100 * 1000); /* e.g. out of descriptors: do not spin */
            continue;
        }

        number = next_number++;
        log_info("Connected with client %d", number);
        fflush(NULL); /* otherwise buffered output would be printed twice */
        pid = fork();
        if (pid < 0) {
            log_error("fork: %s", strerror(errno));
            send_text(fd, MSG_ERROR, "The server is overloaded, please try again later.");
            close(fd);
            continue;
        }
        if (pid == 0) {
            close(listen_fd);
            serve_client(fd, number);
            close(fd);
            _exit(0);
        }
        close(fd); /* the child owns the connection now */
    }
}
