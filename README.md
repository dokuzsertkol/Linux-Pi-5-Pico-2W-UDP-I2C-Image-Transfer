# Linux Pi 5 Pico 2W UDP I2C Image Transfer System

A three node C networking project that transfers an image from a Linux desktop/server to a Raspberry Pi 5 over UDP and forwards it through I2C to a Raspberry Pi Pico 2 W. The echoed packet is verified by the bridge and server, and a reconstructed copy is saved on the server side.

The project is split into independently buildable targets:

- **Linux Server:** reads an image file, splits it into packets, sends it over UDP, tracks acknowledgements, retransmits lost packets, and reconstructs the received echo into an output file.
- **Raspberry Pi 5 bridge:** registers with the server over UDP, forwards packets to the Pico over I2C, checks the I2C echo, and sends the verified packet back to the server.
- **Pico 2 W I2C slave:** receives raw packet bytes from the Pi, temporarily stores them, and echoes them back to the Pi.

## What This Project Demonstrates

- Building several executable targets for different hardware platforms with CMake
- Sharing a binary packet protocol between embedded and Linux applications
- UDP socket setup, registration, receive loops, addressing, and timeouts
- I2C master/slave communication on Linux and the Raspberry Pi Pico SDK
- Interrupt-driven byte handling on an embedded device
- Custom framing for image and file transfers
- Sliding-window reliability using a send buffer
- Packet retransmission and timeout handling
- Duplicate packet detection and transfer state tracking
- Data verification through end-to-end I2C echoing
- Network byte-order conversion
- Packed binary structures and payload-size validation
- Mutexes and atomics for thread-safe server state
- Partial file writing by packet index
- Graceful handling of client reconnection and transfer aborts
- Structured runtime logging for debugging distributed hardware systems

## Repository Layout

```text
.
├── CMakeLists.txt          # Selects the pico2w, pi5, or server target
├── shared/
│   └── packet.h            # Shared packet format and protocol constants
├── server/
│   ├── server.c            # UDP sender, reliability logic, and file reconstruction
│   └── server.h            # Server constants and state structures
├── pi5/
│   ├── pi5.c               # UDP/I2C bridge application
│   └── pi5.h               # Network, I2C, and timeout configuration
├── pico2w/
│   ├── pico2w.c            # I2C slave implementation
│   └── pico2w.h            # I2C configuration
└── linux-build-src/        # Linux/server CMake entry point
```

## Shared Packet Protocol

All nodes include `shared/packet.h`, which defines the protocol data structures.

The system reserves packet sizes below the normal IPv4/UDP MTU limit:

- Ethernet MTU: 1500 bytes
- IPv4 header: 20 bytes
- UDP header: 8 bytes
- Maximum UDP payload: 1472 bytes
- Packet header: 9 bytes
- Payload chunk: 1463 bytes

### Header

```c
typedef struct __attribute__((packed)) {
    PacketID id;
    uint16_t packet_counter;
    uint16_t total_packet;
    uint16_t payload_size;
    uint16_t transfer_counter;
} Header;
```

| Field              | Meaning                                         |
| ------------------ | ----------------------------------------------- |
| `id`               | Packet type                                     |
| `packet_counter`   | Zero-based index of this packet in the transfer |
| `total_packet`     | Number of packets in the transfer               |
| `payload_size`     | Number of valid bytes in the payload            |
| `transfer_counter` | Unique ID for the current transfer              |

The header is packed to avoid compiler-added padding. Multi-byte fields are explicitly converted to network byte order with `hton*` on send and `ntoh*` on receive.

### Packet Types

- `P_REGISTER` — Bridge registration request
- `P_REGISTER_ACK` — Server acknowledgement of registration
- `P_DATA` — Image payload or verified echo
- `P_DATA_ACK` — Defined acknowledgement packet type
- `P_TRANSFER_ABORT` — Notification that the I2C bridge could not complete a transfer

## Transfer Mechanics

### 1. Client Registration

The Raspberry Pi 5 repeatedly sends `P_REGISTER` packets to the server every 2 seconds. The server stores the source IP address and UDP port of the registered bridge.

The server answers with `P_REGISTER_ACK`. The Pi remains registered for 10 seconds after the latest acknowledgement and must renew its connection state while the server is unavailable or after a timeout.

Registration also provides a basic form of source validation: UDP data from an unregistered address is ignored by the server.

### 2. File Splitting

When a file path is entered in the server terminal, the server:

1. Opens the file in binary mode.
2. Finds its size.
3. Calculates the number of packets using `CHUNK_SIZE`.
4. Rejects transfers larger than the 16-bit packet counter range.
5. Increments the transfer ID.
6. Creates an output file named `output-<transfer-id>.jpg`.

The server then reads sequential chunks and constructs packets containing:

- A transfer ID
- The current packet number
- The total packet count
- The exact payload size
- The file bytes

The final packet can contain fewer than `CHUNK_SIZE` bytes; `payload_size` identifies how many bytes are valid.

### 3. UDP Send Window

The server does not send every packet and immediately forget it. It maintains a send buffer with `WINDOW_SIZE = 16` entries.

Each entry contains:

- The complete packet
- Its encoded size
- An active/incomplete flag
- The last-send timestamp

Before adding another packet, the server checks for an available window slot. If the window is full, it retransmits expired packets and continues waiting until a slot becomes free.

### 4. Retransmission

Every active send-buffer entry has a retransmission timeout of 2 seconds.

If a slot is still active after that interval, the server resends the packet and updates its last-send time. This is a stop-and-retry mechanism around a bounded window, rather than TCP-style flow control or congestion management.

The bridge also periodically refreshes registration even when no packets are being exchanged, allowing the server to detect connection state changes.

### 5. I2C Forwarding

The Raspberry Pi 5 receives a UDP packet and validates:

- The packet is at least as large as the header
- The packet ID is `P_DATA`
- The packet counter is in range
- The payload size fits in one packet
- The exact UDP packet size matches the header plus payload size

It then writes the complete packet to the Pico through Linux's `/dev/i2c-1` interface.

The I2C slave address is `0x09`, and communication is configured at 400 kHz.

### 6. Pico 2 W I2C Slave

The Pico acts as an interrupt-driven I2C slave. It uses separate receive, transmit, and idle states to track the active bus transaction.

The handler processes Pico SDK I2C events:

- `I2C_SLAVE_RECEIVE` — drains bytes available in the I2C hardware and stores them in a receive buffer.
- `I2C_SLAVE_REQUEST` — sends buffered response bytes back to the master.
- `I2C_SLAVE_FINISH` — finalizes receive or transmit state and prepares the echo.

On receive completion, the Pico copies the received bytes to a transmit buffer. When the Pi performs a read, the Pico returns the exact same bytes. Counters track:

- Received transactions and byte totals
- Completed transmissions
- Receive overflow
- Incomplete transmissions
- Reads requested with no prepared response

### 7. Echo Verification

The Pi reads exactly the same number of bytes it wrote and compares the I2C response with the original UDP packet using `memcmp`.

If the I2C write or read fails, or the returned bytes do not match, the bridge sends `P_TRANSFER_ABORT` to the server and marks the transfer inactive.

If the echo matches, the Pi sends the original packet back to the server as a UDP `P_DATA` echo.

### 8. Server Verification and Reconstruction

The server matches an incoming echo against an active send-buffer entry using:

- Transfer ID
- Packet counter
- Packet size
- Byte-for-byte packet contents

If all checks succeed, it writes the payload at:

```text
packet_counter × CHUNK_SIZE
```

This makes packet loss and reordering safe because each payload is written to its exact file offset rather than at the end of the file.

The server then removes that packet from the send window. When all 16 entries—or, for this implementation, all active entries—are cleared and the expected packets have been saved, the transfer is complete.

### 9. Duplicate and Reconnection Handling

The implementation tracks received packet indices on both the bridge and server:

- Duplicate bridge echoes are sent again without forwarding the packet to I2C twice.
- Duplicate server packets are ignored during reconstruction.
- A mismatched packet count for a transfer causes transfer state to reset.
- A new I2C or UDP source identity can invalidate the previous transfer.
- A client timeout clears the send window and closes incomplete output files.
- A transfer abort stops all work on the current transfer and resets the state.

## Threading and Synchronization

The server creates a detached UDP reader thread so network reception can occur independently from terminal input and file transfers.

Shared state is protected with mutexes:

- `client_mutex` — registered client address and connection state
- `send_buffer_mutex` — pending packets and retransmission state
- `output_mutex` — output file and reconstruction progress

An atomic reconnection flag signals the transfer loop when it should stop and release resources.

## Build Configuration

The root CMake configuration selects a target using `BUILD_TARGET`:

```text
pico2w  -> Raspberry Pi Pico 2 W firmware
pi5     -> Raspberry Pi 5 bridge executable
server  -> Linux/desktop server executable
```

### Build the Server

A dedicated Linux build directory is available in the workspace:

```bash
cmake -S linux-build-src -B build-linux
cmake --build build-linux -j
```

The executable is generated under:

```text
build-linux/server/server
```

### Build the Raspberry Pi 5 Bridge

```bash
cmake -S . -B build-pi5 -DBUILD_TARGET=pi5
cmake --build build-pi5 -j
```

Run the result on the Pi 5:

```bash
./build-pi5/pi5/pi5
```

### Build the Pico 2 W Firmware

Set `PICO_SDK_PATH` and configure with the Pico 2 W target:

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build-pico2w -DBUILD_TARGET=pico2w
cmake --build build-pico2w -j
```

The build creates `build-pico2w/pico2w/pico2w.uf2`. Copy the UF2 file to the Pico 2 W while it is in BOOTSEL mode.

## Hardware Setup

| Pico pin  | Raspberry Pi 5 connection |
| --------- | ------------------------- |
| GP6 / SDA | I2C SDA (`/dev/i2c-1`)    |
| GP7 / SCL | I2C SCL (`/dev/i2c-1`)    |
| GND       | Ground                    |

Both I2C lines use pull-ups:

- Pico address: `0x09`
- Speed: 400 kHz
- Pi I2C device: `/dev/i2c-1`

The network server address and UDP port are configured in `pi5/pi5.h`:

```c
#define SERVER_IP "192.168.250.168"
#define SERVER_PORT 3000
```

Update this value to match the server's IPv4 address. The server listens on all network interfaces with port `3000`.

## Running an Transfer

1. Start the Pico 2 W firmware.
2. Start the Raspberry Pi 5 bridge.
3. Start the server executable on the network host.
4. Wait for the Pi to register with the server.
5. Enter an image path in the server terminal.

Example:

```text
[Server] enter image file path: /path/to/image.jpg
```

The server creates a reconstructed file with a name such as:

```text
server/output-1.jpg
```

The transfer's progress and errors are printed through the command line on all three nodes.

## Debugging Features

The implementation includes extensive runtime diagnostics:

- UDP registration and acknowledgement events
- Packet transfer IDs and packet indices
- I2C write/read success and failure
- Echo mismatch reporting
- Retransmission events
- Send-window progress
- Client timeout and reconnection events
- File initialization and write progress
- Pico receive and transmit counters
- I2C overflow and incomplete transaction counters

These logs are useful for separating network failures, I2C failures, packet corruption, and file reconstruction problems.
