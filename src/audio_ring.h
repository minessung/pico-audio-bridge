#ifndef AUDIO_RING_H_
#define AUDIO_RING_H_

#include <stdint.h>
#include <stdbool.h>
#include "bridge_config.h"

// Each stereo sample frame is packed as a 32-bit word:
// High 16 bits = Left channel (int16_t), Low 16 bits = Right channel (int16_t)
typedef struct {
    uint32_t buffer[AUDIO_RING_CAPACITY_FRAMES];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    uint32_t last_sample;
    uint32_t adapt_cooldown;
    bool primed;
    uint32_t underrun_count;
    uint32_t overrun_count;
} audio_ring_t;

void audio_ring_init(audio_ring_t *ring);
void audio_ring_reset(audio_ring_t *ring);

static inline uint32_t audio_ring_available_to_read(const audio_ring_t *ring) {
    return (ring->write_idx - ring->read_idx);
}

static inline uint32_t audio_ring_available_to_write(const audio_ring_t *ring) {
    return AUDIO_RING_CAPACITY_FRAMES - (ring->write_idx - ring->read_idx);
}

// Write stereo frames (from USB OUT or I2S RX)
uint32_t audio_ring_write(audio_ring_t *ring, const uint32_t *frames, uint32_t count);

// Read stereo frames (for USB IN or I2S TX)
uint32_t audio_ring_read(audio_ring_t *ring, uint32_t *frames, uint32_t count);

// Read with adaptive drift compensation (drops/duplicates 1 sample gently when fill level drifts)
void audio_ring_read_adaptive(audio_ring_t *ring, uint32_t *out_frames, uint32_t target_out_count);

#endif /* AUDIO_RING_H_ */
