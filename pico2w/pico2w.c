#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pico/stdlib.h>
#include <hardware/i2c.h>
#include <pico/i2c_slave.h>
#include "../shared/packet.h"
#include "pico2w.h"

static uint8_t rx_buffer[PACKET_SIZE];
static uint8_t tx_buffer[PACKET_SIZE];

static volatile uint16_t rx_len = 0;
static volatile uint16_t tx_len = 0;
static volatile uint16_t tx_pos = 0;

typedef enum {
    SLAVE_IDLE,
    SLAVE_RX,
    SLAVE_TX
} slave_state_t;

static volatile slave_state_t slave_state = SLAVE_IDLE;

static volatile bool response_ready = false;

static volatile uint32_t rx_transactions = 0;
static volatile uint32_t rx_bytes_total = 0;
static volatile uint16_t last_rx_size = 0;

static volatile uint32_t tx_requests = 0;
static volatile uint32_t tx_completed = 0;
static volatile uint16_t last_tx_size = 0;

static volatile uint32_t rx_overflows = 0;
static volatile uint32_t tx_incomplete = 0;
static volatile uint32_t tx_without_response = 0;

static void begin_rx(void) {
    if (slave_state == SLAVE_RX) {
        return;
    }

    slave_state = SLAVE_RX;

    // Önceki yarım kalmış işlemi temizle.
    rx_len = 0;
    tx_len = 0;
    tx_pos = 0;

    response_ready = false;
}


static void receive_pending_bytes(i2c_inst_t *i2c) {
    while (i2c_get_read_available(i2c) > 0) {

        uint8_t data = i2c_read_byte_raw(i2c);

        if (slave_state != SLAVE_RX) {
            begin_rx();
        }

        if (rx_len < PACKET_SIZE) {
            rx_buffer[rx_len++] = data;
            rx_bytes_total++;
        } else {
            rx_overflows++;
        }
    }
}


static void finish_rx(void) {
    if (rx_len > 0) {
        last_rx_size = rx_len;

        memcpy(tx_buffer, rx_buffer, rx_len);

        tx_len = rx_len;
        tx_pos = 0;

        response_ready = true;

        rx_transactions++;
    }

    rx_len = 0;

    slave_state = SLAVE_IDLE;
}

static void begin_tx(void) {
    if (slave_state == SLAVE_TX) {
        return;
    }

    slave_state = SLAVE_TX;

    tx_pos = 0;
}


static void finish_tx(void) {
    if (response_ready && tx_pos >= tx_len) {
        last_tx_size = tx_len;

        tx_completed++;

        tx_len = 0;
        tx_pos = 0;

        response_ready = false;

    } else if (response_ready) {
        tx_incomplete++;
    }

    slave_state = SLAVE_IDLE;
}

static void i2c_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
        case I2C_SLAVE_RECEIVE: {
            receive_pending_bytes(i2c);

            break;
        }
        case I2C_SLAVE_REQUEST: {
            begin_tx();

            tx_requests++;

            uint8_t data = 0;

            if (response_ready && tx_pos < tx_len) {

                data = tx_buffer[tx_pos++];

            } else if (!response_ready) {

                tx_without_response++;

            }

            i2c_write_byte_raw(i2c, data);

            break;
        }
        case I2C_SLAVE_FINISH: {
            if (slave_state == SLAVE_RX) {
                receive_pending_bytes(i2c);

                finish_rx();
            }
            else if (slave_state == SLAVE_TX) {
                finish_tx();
            }
            else if (i2c_get_read_available(i2c) > 0) {
                begin_rx();

                receive_pending_bytes(i2c);

                finish_rx();
            }
            break;
        }
        default: break;
    }
}

int main(void) {
    stdio_init_all();

    i2c_init(I2C_PORT, I2C_BAUDRATE);

    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);

    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    i2c_slave_init(I2C_PORT, I2C_ADDRESS, &i2c_handler);

    printf("[I2C] slave initialized: address=0x%02X, baudrate=%u\n", I2C_ADDRESS, I2C_BAUDRATE);

    uint32_t printed_rx = 0;
    uint32_t printed_tx = 0;
    uint32_t printed_overflows = 0;
    uint32_t printed_incomplete = 0;
    uint32_t printed_without_response = 0;

    while (true) {
        if (rx_transactions != printed_rx) {
            printf("[I2C RX] received: %u bytes (transactions: %lu)\n",
                last_rx_size,
                (unsigned long)rx_transactions
            );

            printed_rx = rx_transactions;
        }

        if (tx_completed != printed_tx) {
            printf("[I2C TX] sent: %u bytes (transactions: %lu)\n",
                last_tx_size,
                (unsigned long)tx_completed
            );

            printed_tx = tx_completed;
        }

        if (rx_overflows != printed_overflows) {
            printf("[I2C RX] overflow count: %lu\n",
                (unsigned long)rx_overflows
            );

            printed_overflows = rx_overflows;
        }

        if (tx_incomplete != printed_incomplete) {
            printf("[I2C TX] incomplete reads: %lu\n",
                (unsigned long)tx_incomplete
            );

            printed_incomplete = tx_incomplete;
        }

        if (tx_without_response != printed_without_response) {
            printf("[I2C TX] requests without response: %lu\n",
                (unsigned long)tx_without_response
            );

            printed_without_response = tx_without_response;
        }

        tight_loop_contents();
    }

    return 0;
}