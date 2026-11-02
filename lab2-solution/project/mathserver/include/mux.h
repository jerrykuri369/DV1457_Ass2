#ifndef MUX_H
#define MUX_H

/*
 * Core shared by the two multiplexing strategies (muxbasic = poll,
 * muxscale = epoll). The core owns all client state and protocol logic;
 * a backend only tells it which descriptors are ready.
 */

#define MUX_READ 1u
#define MUX_WRITE 2u
#define MUX_ERROR 4u

/* Functions a backend provides so the core can change what is watched. */
struct mux_backend {
    void *impl;
    int (*add)(void *impl, int fd, void *tag, unsigned interest);
    int (*modify)(void *impl, int fd, void *tag, unsigned interest);
    void (*remove)(void *impl, int fd);
};

struct mux_server;

/* Registers the listening socket and the job-completion pipe with the
   backend and starts the job worker threads. NULL on failure (logged). */
struct mux_server *mux_server_create(int listen_fd, int first_client_number,
                                     const struct mux_backend *backend);

/* Handles readiness `events` (MUX_*) for the descriptor registered with tag. */
void mux_server_handle(struct mux_server *server, void *tag, unsigned events);

/* Frees clients closed while handling the last batch of events. Call after
   every batch, never in the middle of one. */
void mux_server_end_batch(struct mux_server *server);

#endif
