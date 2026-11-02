/* Wire format helpers shared by the server and the client. */
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

void encode_header(unsigned char out[MSG_HEADER_SIZE], uint32_t type, uint32_t length)
{
    uint32_t net_type = htonl(type);
    uint32_t net_length = htonl(length);

    memcpy(out, &net_type, 4);
    memcpy(out + 4, &net_length, 4);
}

void decode_header(const unsigned char in[MSG_HEADER_SIZE], uint32_t *type, uint32_t *length)
{
    uint32_t net_type, net_length;

    memcpy(&net_type, in, 4);
    memcpy(&net_length, in + 4, 4);
    *type = ntohl(net_type);
    *length = ntohl(net_length);
}

int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;

    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0)
            return -1; /* peer closed the connection */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int send_msg(int fd, uint32_t type, const void *payload, uint32_t length)
{
    unsigned char header[MSG_HEADER_SIZE];
    struct iovec iov[2] = {
        { .iov_base = header, .iov_len = sizeof header },
        { .iov_base = (void *)payload, .iov_len = length },
    };
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = length > 0 ? 2 : 1 };

    /* Header and payload go out in one call, so small messages are not
       split into two TCP segments (which can add ~40 ms of latency). */
    encode_header(header, type, length);
    while (msg.msg_iovlen > 0) {
        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        while (msg.msg_iovlen > 0 && (size_t)n >= msg.msg_iov->iov_len) {
            n -= (ssize_t)msg.msg_iov->iov_len;
            msg.msg_iov++;
            msg.msg_iovlen--;
        }
        if (msg.msg_iovlen > 0) {
            msg.msg_iov->iov_base = (char *)msg.msg_iov->iov_base + n;
            msg.msg_iov->iov_len -= (size_t)n;
        }
    }
    return 0;
}

int recv_msg(int fd, uint32_t *type, char **payload, uint32_t *length, uint32_t max_length)
{
    unsigned char header[MSG_HEADER_SIZE];
    char *buf;

    *payload = NULL;
    if (recv_all(fd, header, sizeof header) < 0)
        return -1;
    decode_header(header, type, length);
    if (*length > max_length) {
        errno = EMSGSIZE;
        return -1;
    }
    buf = malloc((size_t)*length + 1);
    if (!buf)
        return -1;
    if (recv_all(fd, buf, *length) < 0) {
        free(buf);
        return -1;
    }
    buf[*length] = '\0';
    *payload = buf;
    return 0;
}
