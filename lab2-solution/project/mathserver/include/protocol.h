#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Wire format shared by the server and the client.
 *
 * Every message is an 8-byte header (type, payload length; both in network
 * byte order) followed by exactly `length` payload bytes.
 *
 *   server -> client  MSG_WELCOME    the client's number as text, e.g. "1"
 *   client -> server  MSG_COMMAND    a command line, e.g. "matinvpar -n 8 -I fast -P 1"
 *   server -> client  MSG_NEED_DATA  k-means only: the -f path the client must upload
 *   client -> server  MSG_DATA       the contents of that file
 *   client -> server  MSG_CANCEL     the client could not read that file
 *   server -> client  MSG_RESULT     "<file name>\n<file contents>"
 *   server -> client  MSG_INFO       text to show the user (help and usage output)
 *   server -> client  MSG_ERROR      why a command could not be run
 */
enum msg_type {
    MSG_WELCOME = 1,
    MSG_COMMAND,
    MSG_NEED_DATA,
    MSG_DATA,
    MSG_CANCEL,
    MSG_RESULT,
    MSG_INFO,
    MSG_ERROR
};

#define MSG_HEADER_SIZE 8u
#define MAX_COMMAND_LEN 4096u                   /* longest accepted command line */
#define MAX_PAYLOAD_LEN (1024u * 1024u * 1024u) /* largest data or result payload */

void encode_header(unsigned char out[MSG_HEADER_SIZE], uint32_t type, uint32_t length);
void decode_header(const unsigned char in[MSG_HEADER_SIZE], uint32_t *type, uint32_t *length);

/*
 * Blocking helpers. They loop over partial transfers and return 0 on
 * success, or -1 on an error or when the peer has closed the connection.
 */
int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);
int send_msg(int fd, uint32_t type, const void *payload, uint32_t length);

/*
 * Receives one message. On success *payload is a malloc'd, NUL-terminated
 * copy of the payload that the caller must free. Fails (and the connection
 * should then be closed) if the payload is longer than max_length.
 */
int recv_msg(int fd, uint32_t *type, char **payload, uint32_t *length, uint32_t max_length);

#endif
