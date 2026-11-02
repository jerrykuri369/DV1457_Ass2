#ifndef STRATEGIES_H
#define STRATEGIES_H

/* Each function serves clients on listen_fd until the process is stopped.
   Client numbers start at first_client_number. They return only on a fatal
   error, with the exit status to use. */
int run_fork_strategy(int listen_fd, int first_client_number);
int run_muxbasic_strategy(int listen_fd, int first_client_number);
int run_muxscale_strategy(int listen_fd, int first_client_number);

#endif
