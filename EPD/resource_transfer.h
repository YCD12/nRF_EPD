#ifndef RESOURCE_TRANSFER_H
#define RESOURCE_TRANSFER_H
#include <stdbool.h>
#include <stdint.h>
/* RX only copies bounded bytes; process performs SPI work and notifications. */
void resource_transfer_receive(const uint8_t *data, uint16_t length, uint32_t generation);
void resource_transfer_process(void *context, uint32_t generation, bool connected,
                               bool idle, uint32_t now,
                               uint32_t (*send)(void *, uint32_t, uint8_t *, uint16_t));
#endif
