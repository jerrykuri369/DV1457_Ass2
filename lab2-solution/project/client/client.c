/*
 * client.c - command-line client for the math server.
 *
 *   ./client -ip <server address> -p <port>
 *
 * Reads commands from the terminal (or a pipe), sends them to the server,
 * uploads the k-means data file when the server asks for it, and saves
 * every received solution in the results folder next to this program,
 * never overwriting an earlier file. Ends at end of input, on "quit", or
 * with Ctrl+C.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "protocol.h"

#define EXIT_BAD_OPTION 3
#define MAX_NAME_ATTEMPTS 10000

static void usage(FILE *out, const char *program)
{
    fprintf(out, "Usage: %s -ip <server address> -p <port>\n", program);
}

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints = { 0 }, *list;
    int fd = -1, last_errno = 0;
    int rc;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "Cannot find server %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    for (struct addrinfo *ai = list; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            last_errno = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(list);
    if (fd < 0)
        fprintf(stderr, "Cannot connect to %s:%s: %s\n", host, port, strerror(last_errno));
    return fd;
}

/* <folder of this program>/results, created if needed. */
static int find_results_dir(char *dir, size_t size)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;

    if (len < 0)
        return -1;
    exe[len] = '\0';
    slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';
    if (snprintf(dir, size, "%s/results", exe) >= (int)size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return mkdir(dir, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * Saves data as dir/name without replacing an existing file: if the name is
 * taken, "_2", "_3", ... is added before the extension. The name actually
 * used is stored in saved.
 */
static int save_new_file(const char *dir, const char *name, const char *data, size_t len,
                         char *saved, size_t saved_size)
{
    const char *base = strrchr(name, '/');
    const char *dot;
    int stem_len;

    base = base ? base + 1 : name; /* never write outside the results folder */
    if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        base = "solution.txt";
    dot = strrchr(base, '.');
    stem_len = dot ? (int)(dot - base) : (int)strlen(base);

    for (int copy = 1; copy <= MAX_NAME_ATTEMPTS; copy++) {
        char path[PATH_MAX];
        int fd, ok;

        if (copy == 1)
            snprintf(saved, saved_size, "%s", base);
        else
            snprintf(saved, saved_size, "%.*s_%d%s", stem_len, base, copy, dot ? dot : "");
        if (snprintf(path, sizeof path, "%s/%s", dir, saved) >= (int)sizeof path) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return -1;
        }
        ok = write_all(fd, data, len) == 0;
        if (close(fd) != 0)
            ok = 0;
        if (!ok) {
            unlink(path);
            return -1;
        }
        return 0;
    }
    errno = EEXIST;
    return -1;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *in = fopen(path, "rb");
    struct stat st;
    char *data;
    int problem = 0;

    if (!in)
        return NULL;
    if (fstat(fileno(in), &st) < 0)
        problem = errno;
    else if (!S_ISREG(st.st_mode))
        problem = EINVAL;
    else if (st.st_size > (off_t)MAX_PAYLOAD_LEN)
        problem = EFBIG;
    if (problem) {
        fclose(in);
        errno = problem;
        return NULL;
    }
    data = malloc((size_t)st.st_size + 1);
    if (!data || fread(data, 1, (size_t)st.st_size, in) != (size_t)st.st_size) {
        free(data);
        fclose(in);
        errno = EIO;
        return NULL;
    }
    fclose(in);
    *len = (size_t)st.st_size;
    return data;
}

/* Sends the file the server asked for. Returns 0 if sent (a result will
   follow), 1 if the upload was cancelled, -1 if the connection broke. */
static int upload(int fd, const char *path)
{
    size_t len;
    char *data = read_file(path, &len);
    int rc;

    if (!data) {
        printf("Cannot read the data file '%s': %s\n", path, strerror(errno));
        return send_msg(fd, MSG_CANCEL, NULL, 0) < 0 ? -1 : 1;
    }
    rc = send_msg(fd, MSG_DATA, data, (uint32_t)len);
    free(data);
    if (rc < 0)
        return -1;
    printf("Sent the data file: %s (%zu bytes)\n", path, len);
    return 0;
}

/* A RESULT payload is "<file name>\n<file contents>". */
static void store_solution(const char *results_dir, char *payload, uint32_t len)
{
    char *newline = memchr(payload, '\n', len);
    char saved[NAME_MAX + 1];

    if (!newline) {
        printf("The server sent a malformed solution.\n");
        return;
    }
    *newline = '\0';
    if (save_new_file(results_dir, payload, newline + 1, len - (size_t)(newline + 1 - payload),
                      saved, sizeof saved) < 0)
        printf("Could not save the solution %s: %s\n", payload, strerror(errno));
    else
        printf("Received the solution: %s\n", saved);
}

/* Handles the server's answer to one command. -1 if the connection broke. */
static int await_answer(int fd, const char *results_dir)
{
    for (;;) {
        uint32_t type, len;
        char *payload;
        int finished = 1, rc = 0;

        if (recv_msg(fd, &type, &payload, &len, MAX_PAYLOAD_LEN) < 0)
            return -1;
        switch (type) {
        case MSG_NEED_DATA:
            rc = upload(fd, payload);
            finished = rc != 0;
            break;
        case MSG_RESULT:
            store_solution(results_dir, payload, len);
            break;
        case MSG_INFO:
            printf("%s\n", payload);
            break;
        case MSG_ERROR:
            printf("Server error: %s\n", payload);
            break;
        default:
            printf("Unexpected message (type %u) from the server.\n", type);
            break;
        }
        free(payload);
        if (rc < 0)
            return -1;
        if (finished)
            return 0;
    }
}

/* Removes leading and trailing white space in place. */
static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && strchr(" \t\r\n", end[-1]))
        *--end = '\0';
    return s;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "ip", required_argument, NULL, 'i' },
        { "p", required_argument, NULL, 'p' },
        { "h", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *host = NULL, *port = NULL;
    char results_dir[PATH_MAX];
    char line[MAX_COMMAND_LEN + 2];
    uint32_t type, len;
    char *welcome;
    int opt, fd;

    /* getopt() would read "-ip" as -i with value "p"; getopt_long_only()
       accepts multi-letter options after a single dash. */
    opterr = 0;
    while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
        switch (opt) {
        case 'i':
            host = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            fprintf(stderr, "Unsupported or incomplete option.\n");
            usage(stderr, argv[0]);
            return EXIT_BAD_OPTION;
        }
    }
    if (!host || !port || optind < argc) {
        usage(stderr, argv[0]);
        return EXIT_BAD_OPTION;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (find_results_dir(results_dir, sizeof results_dir) < 0) {
        perror("Cannot prepare the results folder");
        return 1;
    }
    fd = connect_to_server(host, port);
    if (fd < 0)
        return 1;
    if (recv_msg(fd, &type, &welcome, &len, 64) < 0 || type != MSG_WELCOME) {
        fprintf(stderr, "The server did not answer.\n");
        close(fd);
        return 1;
    }
    printf("Connected to server (you are client %s)\n", welcome);
    free(welcome);

    for (;;) {
        char *command;

        printf("Enter a command for the server: ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            putchar('\n');
            break;
        }
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
            printf("The command is too long (at most %u characters).\n", MAX_COMMAND_LEN);
            continue;
        }
        command = trim(line);
        if (*command == '\0')
            continue;
        if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0)
            break;
        if (send_msg(fd, MSG_COMMAND, command, (uint32_t)strlen(command)) < 0
            || await_answer(fd, results_dir) < 0) {
            printf("Lost the connection to the server.\n");
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}
