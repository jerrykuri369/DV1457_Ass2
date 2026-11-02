/*
 * strategy_muxscale.c - one process watches every client with epoll.
 *
 * The kernel keeps the set of watched descriptors, and epoll_wait() returns
 * only the ones that are ready, so the cost depends on activity rather than
 * on the number of connected clients. Level-triggered mode is used: a
 * descriptor is reported again until it has been fully handled. The
 * protocol and client logic live in mux_core.c.
 */
#define _GNU_SOURCE
#include "strategies.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>

#include "mux.h"
#include "server_util.h"

#define MAX_EVENTS 256

static uint32_t to_epoll(unsigned interest)
{
    uint32_t events = 0;

    if (interest & MUX_READ)
        events |= EPOLLIN;
    if (interest & MUX_WRITE)
        events |= EPOLLOUT;
    return events;
}

static unsigned from_epoll(uint32_t events)
{
    unsigned result = 0;

    if (events & EPOLLIN)
        result |= MUX_READ;
    if (events & EPOLLOUT)
        result |= MUX_WRITE;
    if (events & (EPOLLERR | EPOLLHUP))
        result |= MUX_ERROR;
    return result;
}

static int epoll_control(void *impl, int op, int fd, void *tag, unsigned interest)
{
    struct epoll_event event = { .events = to_epoll(interest), .data.ptr = tag };
    return epoll_ctl(*(int *)impl, op, fd, &event);
}

static int epoll_add(void *impl, int fd, void *tag, unsigned interest)
{
    return epoll_control(impl, EPOLL_CTL_ADD, fd, tag, interest);
}

static int epoll_modify(void *impl, int fd, void *tag, unsigned interest)
{
    return epoll_control(impl, EPOLL_CTL_MOD, fd, tag, interest);
}

static void epoll_remove(void *impl, int fd)
{
    epoll_ctl(*(int *)impl, EPOLL_CTL_DEL, fd, NULL);
}

int run_muxscale_strategy(int listen_fd, int first_client_number)
{
    struct epoll_event events[MAX_EVENTS];
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    struct mux_backend backend = { &epoll_fd, epoll_add, epoll_modify, epoll_remove };
    struct mux_server *server;

    if (epoll_fd < 0) {
        log_error("epoll_create1: %s", strerror(errno));
        return 1;
    }
    server = mux_server_create(listen_fd, first_client_number, &backend);
    if (!server)
        return 1;
    for (;;) {
        int count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        if (count < 0) {
            if (errno == EINTR)
                continue;
            log_error("epoll_wait: %s", strerror(errno));
            return 1;
        }
        for (int i = 0; i < count; i++)
            mux_server_handle(server, events[i].data.ptr, from_epoll(events[i].events));
        mux_server_end_batch(server);
    }
}
