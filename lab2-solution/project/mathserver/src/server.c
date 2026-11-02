/*
 * server.c - entry point of the math server.
 *
 *   ./server [-h] [-p port] [-d] [-s fork|muxbasic|muxscale]
 *
 * Opens the listening socket, optionally becomes a daemon, then serves
 * clients with the chosen strategy until it is stopped (Ctrl+C, or kill
 * for a daemon). Unsupported options make it print the usage and exit 3.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "server_util.h"
#include "solver.h"
#include "strategies.h"

#define DEFAULT_PORT 9999
#define EXIT_BAD_OPTION 3

enum strategy { STRATEGY_FORK, STRATEGY_MUXBASIC, STRATEGY_MUXSCALE };

static const char *const STRATEGY_NAMES[] = { "fork", "muxbasic", "muxscale" };

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "Usage: %s [-h] [-p port] [-d] [-s strategy]\n"
            "  -h           print this help text\n"
            "  -p port      listen on this port (default %d)\n"
            "  -d           run as a daemon (prints its process ID)\n"
            "  -s strategy  how to serve many clients at once:\n"
            "               fork      one process per client (default)\n"
            "               muxbasic  one process, poll()\n"
            "               muxscale  one process, epoll()\n",
            program, DEFAULT_PORT);
}

static int parse_strategy(const char *name, enum strategy *out)
{
    for (size_t i = 0; i < sizeof STRATEGY_NAMES / sizeof STRATEGY_NAMES[0]; i++) {
        if (strcmp(name, STRATEGY_NAMES[i]) == 0) {
            *out = (enum strategy)i;
            return 0;
        }
    }
    return -1;
}

/* The mux strategies may hold thousands of sockets: allow as many
   descriptors as the system lets this process have. */
static void raise_descriptor_limit(void)
{
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        setrlimit(RLIMIT_NOFILE, &limit);
    }
}

static int bad_option(const char *program, const char *message, const char *value)
{
    fprintf(stderr, message, value);
    fputc('\n', stderr);
    usage(stderr, program);
    return EXIT_BAD_OPTION;
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    int run_as_daemon = 0;
    enum strategy strategy = STRATEGY_FORK;
    int first_client_number, listen_fd, opt;
    char optstr[2] = { 0 };

    setvbuf(stdout, NULL, _IOLBF, 0);
    opterr = 0; /* we print our own messages */
    while ((opt = getopt(argc, argv, ":hp:ds:")) != -1) {
        optstr[0] = (char)optopt;
        switch (opt) {
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        case 'p':
            if (parse_int_arg(optarg, 1, 65535, &port) != 0)
                return bad_option(argv[0], "Invalid port '%s'.", optarg);
            break;
        case 'd':
            run_as_daemon = 1;
            break;
        case 's':
            if (parse_strategy(optarg, &strategy) != 0)
                return bad_option(argv[0], "Unsupported strategy '%s'.", optarg);
            break;
        case ':':
            return bad_option(argv[0], "Option -%s needs a value.", optstr);
        default:
            return bad_option(argv[0], "Unsupported option -%s.", optstr);
        }
    }
    /* Also accept a bare port ("./server 9999"), as in one handout example. */
    if (optind == argc - 1) {
        if (parse_int_arg(argv[optind], 1, 65535, &port) != 0)
            return bad_option(argv[0], "Unsupported argument '%s'.", argv[optind]);
    } else if (optind < argc) {
        return bad_option(argv[0], "Unsupported argument '%s'.", argv[optind]);
    }

    signal(SIGPIPE, SIG_IGN); /* a vanished client must not kill the server */
    raise_descriptor_limit();
    if (paths_init() < 0)
        return 1;
    first_client_number = paths_next_client_number();

    /* Open the port before daemonising, so errors still reach the terminal. */
    listen_fd = open_listener(port);
    if (listen_fd < 0)
        return 1;
    if (run_as_daemon)
        daemonize(port);

    log_info("Math server on port %d, strategy %s, results in %s",
             port, STRATEGY_NAMES[strategy], paths_results_dir());
    log_info("Listening for clients...");
    switch (strategy) {
    case STRATEGY_MUXBASIC:
        return run_muxbasic_strategy(listen_fd, first_client_number);
    case STRATEGY_MUXSCALE:
        return run_muxscale_strategy(listen_fd, first_client_number);
    default:
        return run_fork_strategy(listen_fd, first_client_number);
    }
}
