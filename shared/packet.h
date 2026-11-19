#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

#define MTU 1500
#define UDP_HEADER 8
#define IPV4_HEADER 20
#define PACKET_SIZE (MTU - IPV4_HEADER - UDP_HEADER)
#define HEADER_SIZE sizeof(Header)
#define CHUNK_SIZE (PACKET_SIZE - HEADER_SIZE)

typedef enum : uint8_t {
    P_REGISTER = 0,
    P_REGISTER_ACK,
    P_DATA,
    P_DATA_ACK,
    P_TRANSFER_ABORT,
} PacketID;

typedef struct __attribute__((packed)) {
    PacketID id;
    uint16_t packet_counter;
    uint16_t total_packet;
    uint16_t payload_size;
    uint16_t transfer_counter;
} Header;

typedef struct  __attribute__((packed)) {
    Header header;
    uint8_t payload[CHUNK_SIZE];
} Packet;

#endif