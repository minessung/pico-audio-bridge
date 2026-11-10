#ifndef I2S_BRIDGE_H_
#define I2S_BRIDGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "audio_ring.h"

// Initialize and start the Full-Duplex PIO + DMA I2S engine.
// - is_master = true : Drives BCLK (GPIO 10) and LRCLK (GPIO 11), outputs DOUT (GPIO 12), inputs DIN (GPIO 13)
// - is_master = false: Follows BCLK (GPIO 10) and LRCLK (GPIO 11), outputs DOUT (GPIO 12), inputs DIN (GPIO 13)
void i2s_bridge_init(bool is_master, audio_ring_t *usb_to_i2s_ring, audio_ring_t *i2s_to_usb_ring);

// Return total number of 1ms I2S DMA blocks processed
uint32_t i2s_bridge_get_dma_block_count(void);

#endif /* I2S_BRIDGE_H_ */
