#ifndef SERVER_UTIL_H
#define SERVER_UTIL_H

/* ---- logging: stdout/stderr normally, syslog once daemonised ---- */
void log_use_syslog(void);
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- sockets ---- */
/* Returns a socket listening on all interfaces, or -1 (error logged). */
int open_listener(int port);
int set_nonblocking(int fd);

/* ---- result files ---- */
/* Finds <project>/mathserver/computed_results next to the executable and
   creates it if needed. Returns 0 on success. */
int paths_init(void);
/* Absolute path of the computed_results folder. */
const char *paths_results_dir(void);
/* One more than the highest client number already used in computed_results,
   so a restarted server never overwrites earlier files. */
int paths_next_client_number(void);

/* ---- daemon mode ---- */
/* Forks; the parent prints the daemon's PID and exits. The child detaches
   from the terminal and logs to syslog from then on. */
void daemonize(int port);

#endif
