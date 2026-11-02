/* Logging, sockets, result-file paths and daemon mode for the server. */
#define _GNU_SOURCE
#include "server_util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static int use_syslog;

void log_use_syslog(void)
{
    openlog("mathserver", LOG_PID, LOG_DAEMON);
    use_syslog = 1;
}

static void log_line(int priority, FILE *stream, const char *fmt, va_list ap)
{
    char line[1024];

    /* Format first and write once, so lines from threads do not mix. */
    vsnprintf(line, sizeof line, fmt, ap);
    if (use_syslog) {
        syslog(priority, "%s", line);
    } else {
        fprintf(stream, "%s\n", line);
        fflush(stream);
    }
}

void log_info(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_line(LOG_INFO, stdout, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_line(LOG_ERR, stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

int open_listener(int port)
{
    struct sockaddr_in addr = { 0 };
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }
    /* Lets a restarted server reuse the port right away. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* reachable from other machines */
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, SOMAXCONN) < 0) {
        log_error("Cannot listen on port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/* Result files                                                        */
/* ------------------------------------------------------------------ */

static char results_dir[PATH_MAX];

static int make_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    log_error("Cannot create %s: %s", path, strerror(errno));
    return -1;
}

int paths_init(void)
{
    char project[PATH_MAX];
    char mathserver[PATH_MAX + 16];
    ssize_t len = readlink("/proc/self/exe", project, sizeof project - 1);
    char *slash;

    if (len < 0) {
        log_error("Cannot locate the server executable: %s", strerror(errno));
        return -1;
    }
    project[len] = '\0';
    slash = strrchr(project, '/');
    if (slash)
        *slash = '\0'; /* the server binary lives in the project folder */

    snprintf(mathserver, sizeof mathserver, "%s/mathserver", project);
    if (snprintf(results_dir, sizeof results_dir, "%s/computed_results", mathserver)
        >= (int)sizeof results_dir) {
        log_error("The project path is too long");
        return -1;
    }
    return make_dir(mathserver) == 0 && make_dir(results_dir) == 0 ? 0 : -1;
}

const char *paths_results_dir(void)
{
    return results_dir;
}

int paths_next_client_number(void)
{
    DIR *dir = opendir(results_dir);
    struct dirent *entry;
    int highest = 0;

    if (!dir)
        return 1;
    while ((entry = readdir(dir)) != NULL) {
        int number;
        /* matches matinv_client7_soln1.txt, kmeans_client7_data2.txt, ... */
        if (sscanf(entry->d_name, "%*[a-z]_client%d_", &number) == 1 && number > highest)
            highest = number;
    }
    closedir(dir);
    return highest + 1;
}

/* ------------------------------------------------------------------ */
/* Daemon mode                                                         */
/* ------------------------------------------------------------------ */

void daemonize(int port)
{
    pid_t pid;
    int null_fd;

    fflush(NULL); /* do not let the child inherit unwritten output */
    pid = fork();
    if (pid < 0) {
        log_error("fork: %s", strerror(errno));
        exit(1);
    }
    if (pid > 0) {
        printf("Server running as a daemon with PID %d (port %d). Stop it with: kill %d\n",
               (int)pid, port, (int)pid);
        exit(0);
    }

    setsid();   /* new session: no controlling terminal */
    umask(022);
    if (chdir("/") < 0) {
        /* not fatal: every path the server uses is absolute */
    }
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO)
            close(null_fd);
    }
    log_use_syslog();
}
