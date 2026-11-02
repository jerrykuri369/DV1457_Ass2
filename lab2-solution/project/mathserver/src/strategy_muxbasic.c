/*
 * strategy_muxbasic.c - one process watches every client with poll().
 *
 * poll() gets the whole list of descriptors on every call and the loop
 * then scans all of them, so the cost grows with the number of clients.
 * The protocol and client logic live in mux_core.c.
 */
#define _GNU_SOURCE
#include "strategies.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "mux.h"
#include "server_util.h"

/* The pollfd array, the tag of each entry, and where each fd sits in it. */
struct poll_set {
    struct pollfd *fds;
    void **tags;
    size_t count, capacity;
    int *slot_of;        /* slot_of[fd] = index in fds, or -1 */
    size_t slot_of_size;
};

struct ready_event {
    void *tag;
    unsigned events;
};

static short to_poll(unsigned interest)
{
    short events = 0;

    if (interest & MUX_READ)
        events |= POLLIN;
    if (interest & MUX_WRITE)
        events |= POLLOUT;
    return events;
}

static unsigned from_poll(short revents)
{
    unsigned events = 0;

    if (revents & POLLIN)
        events |= MUX_READ;
    if (revents & POLLOUT)
        events |= MUX_WRITE;
    if (revents & (POLLERR | POLLHUP | POLLNVAL))
        events |= MUX_ERROR;
    return events;
}

static int slot_of(const struct poll_set *set, int fd)
{
    return fd >= 0 && (size_t)fd < set->slot_of_size ? set->slot_of[fd] : -1;
}

static int poll_add(void *impl, int fd, void *tag, unsigned interest)
{
    struct poll_set *set = impl;

    if ((size_t)fd >= set->slot_of_size) {
        size_t size = set->slot_of_size ? set->slot_of_size : 64;
        int *map;
        while (size <= (size_t)fd)
            size *= 2;
        map = realloc(set->slot_of, size * sizeof *map);
        if (!map)
            return -1;
        for (size_t i = set->slot_of_size; i < size; i++)
            map[i] = -1;
        set->slot_of = map;
        set->slot_of_size = size;
    }
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 64;
        struct pollfd *fds = realloc(set->fds, capacity * sizeof *fds);
        void **tags;
        if (!fds)
            return -1;
        set->fds = fds;
        tags = realloc(set->tags, capacity * sizeof *tags);
        if (!tags)
            return -1;
        set->tags = tags;
        set->capacity = capacity;
    }
    set->fds[set->count] = (struct pollfd){ .fd = fd, .events = to_poll(interest) };
    set->tags[set->count] = tag;
    set->slot_of[fd] = (int)set->count++;
    return 0;
}

static int poll_modify(void *impl, int fd, void *tag, unsigned interest)
{
    struct poll_set *set = impl;
    int slot = slot_of(set, fd);

    (void)tag;
    if (slot < 0) {
        errno = ENOENT;
        return -1;
    }
    set->fds[slot].events = to_poll(interest);
    return 0;
}

static void poll_remove(void *impl, int fd)
{
    struct poll_set *set = impl;
    int slot = slot_of(set, fd);
    size_t last;

    if (slot < 0)
        return;
    last = --set->count;
    if ((size_t)slot != last) { /* move the last entry into the gap */
        set->fds[slot] = set->fds[last];
        set->tags[slot] = set->tags[last];
        set->slot_of[set->fds[slot].fd] = slot;
    }
    set->slot_of[fd] = -1;
}

int run_muxbasic_strategy(int listen_fd, int first_client_number)
{
    struct poll_set set = { 0 };
    const struct mux_backend backend = { &set, poll_add, poll_modify, poll_remove };
    struct mux_server *server = mux_server_create(listen_fd, first_client_number, &backend);
    struct ready_event *ready = NULL;
    size_t ready_capacity = 0;

    if (!server)
        return 1;
    for (;;) {
        size_t count = 0;

        if (poll(set.fds, (nfds_t)set.count, -1) < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll: %s", strerror(errno));
            return 1;
        }
        /* Handlers add and remove descriptors, so act on a copy. */
        if (ready_capacity < set.count) {
            struct ready_event *grown = realloc(ready, set.capacity * sizeof *grown);
            if (!grown) {
                log_error("Out of memory");
                return 1;
            }
            ready = grown;
            ready_capacity = set.capacity;
        }
        for (size_t i = 0; i < set.count; i++)
            if (set.fds[i].revents)
                ready[count++] = (struct ready_event){ set.tags[i], from_poll(set.fds[i].revents) };
        for (size_t i = 0; i < count; i++)
            mux_server_handle(server, ready[i].tag, ready[i].events);
        mux_server_end_batch(server);
    }
}
