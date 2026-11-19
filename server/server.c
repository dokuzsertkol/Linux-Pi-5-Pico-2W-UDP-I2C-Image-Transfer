#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <stdatomic.h>
#include "server.h"

static Client client = {0};
static SendBufferEntry send_buffer[WINDOW_SIZE] = {0};

static uint64_t last_register_time = 0;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint16_t transfer_counter = 0;

static FILE *output_file = NULL;
static bool *output_received = NULL;
static atomic_bool reconnection_handled = false;

static uint16_t output_transfer = 0, output_total = 0, output_count = 0;

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

static void close_output_transfer(void) {
    pthread_mutex_lock(&output_mutex);

    if (output_file != NULL) {
        fclose(output_file);
        output_file = NULL;
    }

    free(output_received);
    output_received = NULL;

    output_transfer = 0;
    output_total = 0;
    output_count = 0;

    pthread_mutex_unlock(&output_mutex);
}

static int start_output_transfer(uint16_t transfer, uint16_t total) {
    pthread_mutex_lock(&output_mutex);

    if (output_file != NULL) {
        fclose(output_file);
        output_file = NULL;
    }

    free(output_received);
    output_received = NULL;

    output_total = 0;
    output_count = 0;
    output_transfer = 0;

    output_received = calloc(total, sizeof(bool));

    if (output_received == NULL) {
        perror("Output calloc");
        pthread_mutex_unlock(&output_mutex);
        return 0;
    }
    
    char source_path[PATH_MAX];
    char output_path[PATH_MAX];

    snprintf(source_path, sizeof(source_path), "%s", __FILE__);

    char *directory = dirname(source_path);

    snprintf(output_path, sizeof(output_path), "%s/output-%u.jpg", directory, transfer);

    output_file = fopen(output_path, "wb+");

    if (output_file == NULL) {
        perror("output.jpg");
        free(output_received);
        output_received = NULL;

        pthread_mutex_unlock(&output_mutex);
        return 0;
    }

    output_transfer = transfer;
    output_total = total;
    output_count = 0;

    pthread_mutex_unlock(&output_mutex);

    printf("[File] initialized: transfer=%u, packets=%u\n", transfer, total);

    return 1;
}

static int save_echo_to_output(const uint8_t *buffer, size_t packet_size) {
    Header header;
    memcpy(&header, buffer, HEADER_SIZE);

    uint16_t packet_counter = ntohs(header.packet_counter);
    uint16_t total_packet = ntohs(header.total_packet);
    uint16_t payload_size = ntohs(header.payload_size);
    uint16_t transfer = ntohs(header.transfer_counter);

    if (packet_size != HEADER_SIZE + payload_size || payload_size > CHUNK_SIZE || total_packet == 0 || packet_counter >= total_packet) {
        printf("[File] invalid packet\n");
        return 0;
    }

    pthread_mutex_lock(&output_mutex);

    if (output_file == NULL || output_transfer != transfer || output_total != total_packet) {
        printf("[File] transfer mismatch\n");
        pthread_mutex_unlock(&output_mutex);
        return 0;
    }

    if (output_received[packet_counter]) {
        pthread_mutex_unlock(&output_mutex);
        return 1;
    }

    off_t offset = (off_t)packet_counter * CHUNK_SIZE;

    if (fseeko(output_file, offset, SEEK_SET) != 0) {
        perror("Output fseek");
        pthread_mutex_unlock(&output_mutex);
        return 0;
    }

    size_t written = fwrite(buffer + HEADER_SIZE, 1, payload_size, output_file);

    if (written != payload_size) {
        perror("Output fwrite");
        pthread_mutex_unlock(&output_mutex);
        return 0;
    }

    if (fflush(output_file) != 0) {
        perror("Output fflush");
        pthread_mutex_unlock(&output_mutex);
        return 0;
    }

    output_received[packet_counter] = true;
    output_count++;

    printf("[File] packet saved: transfer=%u, packet=%u, progress=%u/%u\n", transfer, packet_counter, output_count, output_total);

    if (output_count == output_total) {
        fclose(output_file);
        output_file = NULL;

        free(output_received);
        output_received = NULL;

        output_transfer = 0;
        output_total = 0;
        output_count = 0;

        printf("[File] saved successfully: output-%u.jpg\n", transfer);
    }

    pthread_mutex_unlock(&output_mutex);

    return 1;
}

static uint64_t get_time_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void clear_send_buffer(void) {
    pthread_mutex_lock(&send_buffer_mutex);

    memset(send_buffer, 0, sizeof(send_buffer));

    pthread_mutex_unlock(&send_buffer_mutex);
}

static bool check_client_timeout(void) {
    uint64_t now = get_time_ms();
    bool timed_out = false;

    pthread_mutex_lock(&client_mutex);

    if (client.connected && last_register_time != 0 && now - last_register_time >= CLIENT_TIMEOUT_MS) {
        client.connected = false;
        last_register_time = 0;

        timed_out = true;
    }

    pthread_mutex_unlock(&client_mutex);

    if (timed_out) {
        printf("[Registration] client timeout: connection lost\n");

        clear_send_buffer();
        close_output_transfer();

        atomic_store(&reconnection_handled, true);
    }

    return timed_out;
}

static bool transfer_interrupted(void) {
    check_client_timeout();

    return atomic_exchange(&reconnection_handled, false);
}

static int get_client_address(struct sockaddr_in *address, socklen_t *address_len) {
    pthread_mutex_lock(&client_mutex);

    if (!client.connected) {
        pthread_mutex_unlock(&client_mutex);
        return 0;
    }

    *address = client.client_addr;
    *address_len = client.client_len;

    pthread_mutex_unlock(&client_mutex);
    return 1;
}

static int is_registered_client(const struct sockaddr_in *address) {
    int result = 0;

    pthread_mutex_lock(&client_mutex);

    if (client.connected && client.client_addr.sin_addr.s_addr == address->sin_addr.s_addr &&
        client.client_addr.sin_port == address->sin_port) {
        result = 1;
    }

    pthread_mutex_unlock(&client_mutex);
    return result;
}

static int udp_send(int server_fd, const uint8_t *data, size_t size) {
    struct sockaddr_in destination;
    socklen_t destination_len;

    if (!get_client_address(&destination, &destination_len)) {
        printf("[UDP TX] client is not registered\n");
        return 0;
    }

    ssize_t sent = sendto(
        server_fd,
        data,
        size,
        0,
        (struct sockaddr *)&destination,
        destination_len
    );

    if (sent < 0) {
        perror("sendto");
        return 0;
    }

    if ((size_t)sent != size) {
        printf("[UDP TX] incomplete packet send\n");
        return 0;
    }

    return 1;
}

static int get_free_slot(const Packet *packet, size_t packet_size) {
    pthread_mutex_lock(&send_buffer_mutex);

    for (size_t i = 0; i < WINDOW_SIZE; i++) {
        if (!send_buffer[i].active) {
            send_buffer[i].packet = *packet;
            send_buffer[i].packet_size = packet_size;
            send_buffer[i].active = true;
            send_buffer[i].last_sent_time = get_time_ms();

            pthread_mutex_unlock(&send_buffer_mutex);
            return (int)i;
        }
    }

    pthread_mutex_unlock(&send_buffer_mutex);
    return -1;
}

static int get_image_chunk(FILE *file, Packet *packet, uint16_t packet_counter, uint16_t total_packets) {
    size_t bytes_read = fread(
        packet->payload,
        1,
        CHUNK_SIZE,
        file
    );

    if (bytes_read == 0) {
        return 0;
    }

    packet->header.id = P_DATA;
    packet->header.packet_counter = htons(packet_counter);
    packet->header.total_packet = htons(total_packets);
    packet->header.payload_size = htons((uint16_t)bytes_read);
    packet->header.transfer_counter = htons(transfer_counter);

    return 1;
}

static int send_slot(int server_fd, int slot) {
    Packet packet;
    size_t packet_size;

    pthread_mutex_lock(&send_buffer_mutex);

    if (!send_buffer[slot].active) {
        pthread_mutex_unlock(&send_buffer_mutex);
        return 1;
    }

    packet = send_buffer[slot].packet;
    packet_size = send_buffer[slot].packet_size;

    pthread_mutex_unlock(&send_buffer_mutex);

    return udp_send(server_fd, (const uint8_t *)&packet, packet_size);
}

static int retransmit_expired_packets(int server_fd) {
    uint64_t now = get_time_ms();

    for (size_t i = 0; i < WINDOW_SIZE; i++) {
        Packet packet;
        size_t packet_size;
        bool retransmit = false;

        pthread_mutex_lock(&send_buffer_mutex);

        if (send_buffer[i].active &&
            now - send_buffer[i].last_sent_time >= DATA_ACK_TIMEOUT_MS) {

            packet = send_buffer[i].packet;
            packet_size = send_buffer[i].packet_size;

            send_buffer[i].last_sent_time = now;
            retransmit = true;
        }

        pthread_mutex_unlock(&send_buffer_mutex);

        if (!retransmit) {
            continue;
        }

        printf("[UDP TX] retransmitting: transfer=%u, packet=%u\n",
            ntohs(packet.header.transfer_counter),
            ntohs(packet.header.packet_counter)
        );

        if (!udp_send(server_fd, (const uint8_t *)&packet, packet_size)) {
            return 0;
        }
    }

    return 1;
}

static int wait_for_all_acks(int server_fd) {
    while (true) {
        if (transfer_interrupted()) {
            return 0;
        }

        bool all_acked = true;

        pthread_mutex_lock(&send_buffer_mutex);

        for (size_t i = 0; i < WINDOW_SIZE; i++) {
            if (send_buffer[i].active) {
                all_acked = false;
                break;
            }
        }

        pthread_mutex_unlock(&send_buffer_mutex);

        if (all_acked) {
            return 1;
        }

        if (!retransmit_expired_packets(server_fd)) {
            return 0;
        }

        usleep(1000);
    }
}

static int get_image(const char *path, int server_fd) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror("fopen");
        return 0;
    }
    
    check_client_timeout();

    struct sockaddr_in active_client;
    socklen_t active_client_len;

    if (!get_client_address(&active_client, &active_client_len)) {
        printf("[Transfer] cannot start: client is not registered\n");
        fclose(file);
        return 0;
    }

    atomic_store(&reconnection_handled, false);

    if (fseek(file, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(file);
        return 0;
    }

    long file_size_long = ftell(file);
    if (file_size_long < 0) {
        perror("ftell");
        fclose(file);
        return 0;
    }

    rewind(file);

    size_t file_size = (size_t)file_size_long;

    size_t chunk_count = file_size / CHUNK_SIZE + (file_size % CHUNK_SIZE != 0);
    if (chunk_count > UINT16_MAX) {
        printf("[Transfer] file too large for uint16_t packet counters\n");
        fclose(file);
        return 0;
    }

    if (chunk_count == 0) {
        printf("[File] empty file\n");
        fclose(file);
        return 1;
    }

    transfer_counter++;

    printf("[Transfer] started: id=%u\n", transfer_counter);
    printf("[Transfer] file size: %zu bytes\n", file_size);
    printf("[Transfer] total packets: %zu\n", chunk_count);

    if (!start_output_transfer(transfer_counter, (uint16_t)chunk_count)) {
        printf("[File] output initialization failed\n");
        fclose(file);
        return 0;
    }

    Packet packet = {0};

    for (size_t i = 0; i < chunk_count; i++) {
        if (transfer_interrupted()) {
            printf("[Transfer] aborted: client reconnection detected\n");
            clear_send_buffer();
            close_output_transfer();
            fclose(file);
            return 0;
        }
        memset(&packet, 0, sizeof(packet));

        if (!get_image_chunk(file, &packet, (uint16_t)i, (uint16_t)chunk_count)) {
            if (ferror(file)) {
                perror("fread");
            } else {
                printf("[File] unexpected end of file\n");
            }

            clear_send_buffer();
            close_output_transfer();
            fclose(file);
            return 0;
        }

        size_t packet_size = HEADER_SIZE + ntohs(packet.header.payload_size);

        int slot;
        while ((slot = get_free_slot(&packet, packet_size)) == -1) {
            if (transfer_interrupted()) {
                printf("[Transfer] aborted by client\n");

                clear_send_buffer();
                close_output_transfer();
                fclose(file);

                return 0;
            }

            if (!retransmit_expired_packets(server_fd)) {
                clear_send_buffer();
                close_output_transfer();
                fclose(file);

                return 0;
            }

            usleep(1000);
        }

        if (!send_slot(server_fd, slot)) {
            clear_send_buffer();
            close_output_transfer();
            fclose(file);
            return 0;
        }

        printf("[UDP TX] sent: packet=%zu/%zu\n", i + 1, chunk_count);
    }

    fclose(file);

    printf("[Transfer] all packets sent. Waiting for echoes...\n");

    if (!wait_for_all_acks(server_fd)) {
        printf("[Transfer] failed: image transfer unsuccessful\n");
        clear_send_buffer();
        close_output_transfer();
        return 0;
    }

    printf("[Transfer] completed successfully\n");

    return 1;
}

static void *udp_read(void *arg) {
    ReadThreadArgs *args = (ReadThreadArgs *)arg;
    int server_fd = args->server_fd;

    uint8_t buffer[PACKET_SIZE];

    while (1) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        ssize_t bytes_received = recvfrom(
            server_fd,
            buffer,
            sizeof(buffer),
            0,
            (struct sockaddr *)&sender_addr,
            &sender_len
        );

        if (bytes_received < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            continue;
        }

        if ((size_t)bytes_received < HEADER_SIZE) {
            printf("[UDP RX] invalid packet: header too small\n");
            continue;
        }

        Header header;
        memcpy(&header, buffer, HEADER_SIZE);

        uint16_t packet_counter = ntohs(header.packet_counter);
        uint16_t transfer_id = ntohs(header.transfer_counter);
        uint16_t payload_size = ntohs(header.payload_size);

        if ((size_t)bytes_received != HEADER_SIZE + payload_size) {
            printf("[UDP RX] invalid packet size\n");
            continue;
        }

        switch (header.id) {
            case P_REGISTER: {
                if (payload_size != 0) {
                    printf("[UDP RX] invalid REGISTER packet\n");
                    break;
                }

                pthread_mutex_lock(&client_mutex);

                bool new_client = !client.connected || client.client_addr.sin_addr.s_addr != sender_addr.sin_addr.s_addr
                    || client.client_addr.sin_port != sender_addr.sin_port;

                if (new_client) {
                    client.client_addr = sender_addr;
                    client.client_len = sender_len;
                    client.connected = true;
                }

                last_register_time = get_time_ms();

                pthread_mutex_unlock(&client_mutex);

                if (new_client) {
                    printf("[Registration] new client registered\n");

                    clear_send_buffer();
                    close_output_transfer();

                    atomic_store(&reconnection_handled, true);
                } else {
                    // printf("[Registration] existing client request received\n");
                }

                Packet response = {0};
                response.header.id = P_REGISTER_ACK;

                ssize_t sent = sendto(
                    server_fd,
                    &response.header,
                    HEADER_SIZE,
                    0,
                    (struct sockaddr *)&sender_addr,
                    sender_len
                );

                if (sent < 0) {
                    perror("REGISTER_ACK sendto");
                    break;
                }

                if (sent != HEADER_SIZE) {
                    printf("[UDP TX] incomplete REGISTER_ACK send\n");
                    break;
                }

                // printf("[UDP TX] REGISTER_ACK sent successfully\n");
                fflush(stdout);

                break;
            }
            case P_DATA: {
                if (!is_registered_client(&sender_addr)) {
                    printf("[UDP RX] DATA from unknown client ignored\n");
                    break;
                }

                if (payload_size > CHUNK_SIZE) {
                    printf("[UDP RX] invalid echo payload size\n");
                    break;
                }

                int matched_slot = -1;
                bool found = false;
                bool matched = false;

                pthread_mutex_lock(&send_buffer_mutex);

                for (size_t slot = 0; slot < WINDOW_SIZE; slot++) {
                    if (!send_buffer[slot].active) {
                        continue;
                    }

                    Packet *original = &send_buffer[slot].packet;

                    uint16_t original_counter = ntohs(original->header.packet_counter);

                    uint16_t original_transfer = ntohs(original->header.transfer_counter);

                    if (original_counter != packet_counter || original_transfer != transfer_id) {
                        continue;
                    }

                    found = true;

                    if (send_buffer[slot].packet_size == (size_t)bytes_received &&
                        memcmp(&send_buffer[slot].packet, buffer, (size_t)bytes_received) == 0) {

                        matched = true;
                        matched_slot = (int)slot;
                    }
                    break;
                }

                pthread_mutex_unlock(&send_buffer_mutex);

                if (!found) break;

                if (!matched) {
                    printf("[UDP RX] echo mismatch: transfer=%u, packet=%u\n", transfer_id, packet_counter);
                    break;
                }

                if (!save_echo_to_output(buffer, (size_t)bytes_received)) {
                    printf("[File] could not save echo: transfer=%u, packet=%u\n", transfer_id, packet_counter);
                    break;
                }

                pthread_mutex_lock(&send_buffer_mutex);

                if (matched_slot >= 0 && send_buffer[matched_slot].active &&
                    ntohs(send_buffer[matched_slot].packet.header.packet_counter) == packet_counter &&
                    ntohs(send_buffer[matched_slot].packet.header.transfer_counter) == transfer_id
                ) {
                    send_buffer[matched_slot].active = false;
                }

                pthread_mutex_unlock(&send_buffer_mutex);

                printf("[UDP RX] echo verified: transfer=%u, packet=%u\n", transfer_id, packet_counter);
                break;
            }
            case P_DATA_ACK: {
                if (payload_size != 0) {
                    printf("[UDP RX] invalid DATA_ACK packet\n");
                    break;
                }

                if (!is_registered_client(&sender_addr)) {
                    printf("[UDP RX] ACK from unknown client ignored\n");
                    break;
                }

                bool found = false;

                pthread_mutex_lock(&send_buffer_mutex);

                for (size_t slot = 0; slot < WINDOW_SIZE; slot++) {
                    if (send_buffer[slot].active &&
                        ntohs(send_buffer[slot].packet.header.packet_counter) == packet_counter &&
                        ntohs(send_buffer[slot].packet.header.transfer_counter) == transfer_id
                    ) {
                        send_buffer[slot].active = false;
                        found = true;
                        break;
                    }
                }

                pthread_mutex_unlock(&send_buffer_mutex);

                if (found) {
                    printf("[UDP RX] ACK received: transfer=%u, packet=%u\n", transfer_id, packet_counter);
                }

                break;
            }
            case P_TRANSFER_ABORT: {
                if (!is_registered_client(&sender_addr)) {
                    printf("[UDP RX] TRANSFER_ABORT from unknown client ignored\n");
                    break;
                }

                if (payload_size != 0) {
                    printf("[UDP RX] invalid TRANSFER_ABORT packet\n");
                    break;
                }

                printf("[Transfer] client aborted current transfer\n");

                atomic_store(&reconnection_handled, true);

                break;
            }
            default:
                printf("[UDP RX] unexpected packet ID: %u\n", header.id);
                break;
        }

        fflush(stdout);
    }

    return NULL;
}

int main(void) {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr = {0};

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    printf("[UDP] server listening: port=%d\n", PORT);

    ReadThreadArgs args = {
        .server_fd = server_fd
    };

    pthread_t read_thread;

    int result = pthread_create(&read_thread, NULL, udp_read, &args);
    if (result != 0) {
        printf("[Thread] UDP read thread creation failed: %s\n", strerror(result));
        close(server_fd);
        return 1;
    }

    pthread_detach(read_thread);

    printf("[Registration] waiting for client...\n");

    while (1) {
        pthread_mutex_lock(&client_mutex);
        int connected = client.connected;
        pthread_mutex_unlock(&client_mutex);

        if (connected) {
            atomic_store(&reconnection_handled, false);
            break;
        }

        usleep(10000);
    }

    printf("[Registration] client connected\n");

    while (1) {
        char buffer[100];

        printf("[Server] enter image file path: ");
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break;
        }

        buffer[strcspn(buffer, "\n")] = '\0';

        get_image(buffer, server_fd);
    }

    close(server_fd);
    return 0;
}
