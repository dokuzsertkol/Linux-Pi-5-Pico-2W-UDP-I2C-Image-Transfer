#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/i2c-dev.h>
#include "../shared/packet.h"
#include "pi5.h"

static bool *received = NULL;
static uint16_t received_total = 0;
static uint16_t received_count = 0;
static uint16_t transfer_counter = 0;
static bool transfer_active = false;
bool registered = false;
uint64_t last_register_time = 0;
static bool transfer_aborted = false;
static uint16_t aborted_transfer_id = 0;

static void reset_transfer(uint16_t transfer, uint16_t total) {
    free(received);
    received = NULL;

    received = calloc(total, sizeof(bool));
    if (received == NULL) {
        perror("calloc");

        received_total = 0;
        received_count = 0;
        transfer_active = false;

        return;
    }

    transfer_counter = transfer;
    received_total = total;
    received_count = 0;
    transfer_active = true;

    printf("[Transfer] started: id=%u, packets=%u\n", transfer, total);
}

static void send_transfer_abort(int sock, const struct sockaddr_in *server_addr) {
    Header header = {0};
    header.id = P_TRANSFER_ABORT;

    sendto(
        sock,
        &header,
        HEADER_SIZE,
        0,
        (const struct sockaddr *)server_addr,
        sizeof(*server_addr)
    );

    printf("[UDP TX] sent: transfer abort notification\n");
}

static uint64_t get_time_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int send_udp_register(int sock, const struct sockaddr_in *server_addr) {
    Header header = {0};
    header.id = P_REGISTER;

    ssize_t sent = sendto(
        sock,
        &header,
        HEADER_SIZE,
        0,
        (const struct sockaddr *)server_addr,
        sizeof(*server_addr)
    );

    if (sent < 0) {
        perror("REGISTER sendto");
        return -1;
    }

    if (sent != HEADER_SIZE) {
        fprintf(stderr, "[UDP TX] incomplete REGISTER send\n");
        return -1;
    }

    return 0;
}

static int try_register(int sock, const struct sockaddr_in *server_addr) {
    if (send_udp_register(sock, server_addr) < 0) {
        printf("[Registration] failed\n");
        return -1;
    }

    printf("[UDP TX] sent: REGISTER request\n");
    return 0;
}

static int send_udp_data(int sock, const struct sockaddr_in *server_addr, const uint8_t *buffer, size_t packet_size) {
    ssize_t sent = sendto(
        sock,
        buffer,
        packet_size,
        0,
        (const struct sockaddr *)server_addr,
        sizeof(*server_addr)
    );

    if (sent < 0) {
        perror("UDP DATA sendto");
        return -1;
    }

    if ((size_t)sent != packet_size) {
        fprintf(stderr, "[UDP TX] incomplete packet send\n");
        return -1;
    }

    return 0;
}

static int i2c_send_packet(int i2c_fd, const uint8_t *data, size_t size) {
    ssize_t written = write(i2c_fd, data, size);
    if (written < 0) {
        perror("I2C write");
        return -1;
    }
    if ((size_t)written != size) {
        fprintf(stderr, "[I2C TX] incomplete write: %zd/%zu bytes\n", written, size);

        return -1;
    }
    return 0;
}

static int i2c_receive_packet(int i2c_fd, uint8_t *buffer, size_t size) {
    ssize_t received_bytes = read(i2c_fd, buffer, size);
    if (received_bytes < 0) {
        perror("I2C read");
        return -1;
    }
    if ((size_t)received_bytes != size) {
        fprintf(stderr, "[I2C RX] incomplete read: %zd/%zu bytes\n", received_bytes, size);

        return -1;
    }
    return 0;
}

static void handle_data_packet(int sock, const struct sockaddr_in *server_addr, int i2c_fd, const uint8_t *buffer, size_t packet_size) {
    Header header;
    memcpy(&header, buffer, HEADER_SIZE);

    uint16_t packet_counter = ntohs(header.packet_counter);
    uint16_t total_packet = ntohs(header.total_packet);
    uint16_t payload_size = ntohs(header.payload_size);
    uint16_t incoming_transfer = ntohs(header.transfer_counter);

    if (header.id != P_DATA || total_packet == 0 || packet_counter >= total_packet ||
        payload_size > CHUNK_SIZE || packet_size != HEADER_SIZE + payload_size) {

        printf("[UDP RX] invalid DATA packet\n");
        return;
    }

    printf("[UDP RX] received: transfer=%u, packet=%u, progress=%u/%u\n", 
        incoming_transfer, packet_counter, received_count, received_total
    );

    if (transfer_aborted) {
        if (incoming_transfer == aborted_transfer_id) {
            return;
        }

        transfer_aborted = false;
    }

    if (!transfer_active || transfer_counter != incoming_transfer) {
        reset_transfer(incoming_transfer, total_packet);
    }

    if (!transfer_active) {
        printf("[Transfer] initialization failed\n");
        return;
    }

    if (received_total != total_packet) {
        printf("[Transfer] inconsistent packet count\n");
        reset_transfer(incoming_transfer, total_packet);

        if (!transfer_active) {
            printf("[Transfer] reinitialization failed\n");
            return;
        }
    }

    if (received[packet_counter]) {
        printf("[UDP RX] duplicate packet: transfer=%u, packet=%u. Resending echo.\n", incoming_transfer, packet_counter);
        
        send_udp_data(sock, server_addr, buffer, packet_size);
        return;
    }

    if (i2c_send_packet(i2c_fd, buffer, packet_size) < 0) {
        printf("[I2C TX] forwarding failed: packet=%u\n", packet_counter);

        aborted_transfer_id = incoming_transfer;
        transfer_aborted = true;
        transfer_active = false;

        send_transfer_abort(sock, server_addr);
        return;
    }

    uint8_t response[PACKET_SIZE];
    if (i2c_receive_packet(i2c_fd, response, packet_size) < 0) {
        printf("[I2C RX] receive failed\n");

        aborted_transfer_id = incoming_transfer;
        transfer_aborted = true;
        transfer_active = false;

        send_transfer_abort(sock, server_addr);
        return;
    }

    if (memcmp(buffer, response, packet_size) != 0) {
        printf("[I2C RX] echo mismatch: transfer=%u, packet=%u\n", incoming_transfer, packet_counter);
        return;
    }

    if (send_udp_data(sock, server_addr, buffer, packet_size) < 0) {
        return;
    }

    received[packet_counter] = true;
    received_count++;

    printf("[UDP TX] echo sent: transfer=%u, packet=%u, progress=%u/%u\n", 
        incoming_transfer, packet_counter, received_count, received_total
    );

    if (received_count == received_total) {
        printf("[Transfer] completed: all packets echoed\n");
    }
}

int main(void) {
    // udp init
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("UDP socket");
        return 1;
    }

    struct sockaddr_in server_addr = {0};

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "[UDP] invalid server IP\n");
        close(udp_fd);
        return 1;
    }

    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0
    };

    if (setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("UDP SO_RCVTIMEO");
        close(udp_fd);
        return 1;
    }

    printf("[UDP] initialized: server=%s:%d\n", SERVER_IP, SERVER_PORT);

    // i2c init
    int i2c_fd = open(I2C_DEVICE, O_RDWR);
    if (i2c_fd < 0) {
        perror("I2C open");
        close(udp_fd);
        return 1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDRESS) < 0) {
        perror("I2C_SLAVE");
        close(i2c_fd);
        close(udp_fd);
        return 1;
    }

    printf("[I2C] configured: device=%s, address=0x%02X\n", I2C_DEVICE, I2C_ADDRESS);

    bool registered = false;
    uint64_t last_register_time = 0;
    uint64_t last_ack_time = 0;

    uint8_t buffer[PACKET_SIZE];

    // receive loop
    while (1) {
        uint64_t now = get_time_ms();

        if (registered && now - last_ack_time >= SERVER_TIMEOUT_MS) {
            registered = false;
            printf("[Registration] server connection lost\n");
        }

        if (now - last_register_time >= REGISTER_INTERVAL_MS) {
            if (try_register(udp_fd, &server_addr) == 0) {
                last_register_time = now;
            }
        }

        ssize_t n = recvfrom(udp_fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            perror("UDP recvfrom");
            registered = false;
            continue;
        }

        if (n < (ssize_t)HEADER_SIZE) {
            continue;
        }

        Header header;
        memcpy(&header, buffer, HEADER_SIZE);

        switch (header.id) {
            case P_REGISTER_ACK:
                if (!registered) {
                    printf("[Registration] server connection established\n");
                }
                else {
                    printf("[UDP RX] received: REGISTER_ACK\n");
                }

                registered = true;
                last_ack_time = get_time_ms();
                break;
            case P_DATA:
                if (registered) {
                    handle_data_packet(udp_fd, &server_addr, i2c_fd, buffer, (size_t)n);
                }
                break;

            default: break;
        }
    }

    free(received);
    close(i2c_fd);
    close(udp_fd);

    return 0;
}

