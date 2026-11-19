#ifndef SERVER_H
#define SERVER_H

#include <netinet/in.h>
#include <stdbool.h>

#include "../shared/packet.h"

#define PORT 3000
#define WINDOW_SIZE 16
#define DATA_ACK_TIMEOUT_MS 2000
#define CLIENT_TIMEOUT_MS 5000

typedef struct {
    int connected;
    struct sockaddr_in client_addr;
    socklen_t client_len;
} Client;

typedef struct {
    Packet packet;
    size_t packet_size;
    bool active;
    uint64_t last_sent_time;
} SendBufferEntry;

typedef struct {
    int server_fd;
} ReadThreadArgs;

#endif