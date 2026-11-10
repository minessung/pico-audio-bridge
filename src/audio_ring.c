#include "audio_ring.h"
#include "pico.h"
#include "hardware/sync.h"
#include <string.h>

void audio_ring_init(audio_ring_t *ring) {
    memset(ring, 0, sizeof(*ring));
}

void audio_ring_reset(audio_ring_t *ring) {
    ring->write_idx = 0;
    ring->read_idx = 0;
    ring->last_sample = 0;
    ring->adapt_cooldown = 0;
    ring->primed = false;
}

uint32_t __not_in_flash_func(audio_ring_write)(audio_ring_t *ring, const uint32_t *frames, uint32_t count) {
    uint32_t w = ring->write_idx;
    uint32_t r = ring->read_idx;
    uint32_t free_space = AUDIO_RING_CAPACITY_FRAMES - (w - r);

    if (count > free_space) {
        // Re-center read_idx to AUDIO_RING_TARGET_FRAMES on overrun so fill starts right at target
        ring->read_idx = (w + count) - AUDIO_RING_TARGET_FRAMES;
        ring->primed = true;
        ring->overrun_count++;
    }

    for (uint32_t i = 0; i < count; i++) {
        ring->buffer[(w + i) & AUDIO_RING_MASK] = frames[i];
    }
    __dmb();
    ring->write_idx = w + count;
    return count;
}

uint32_t __not_in_flash_func(audio_ring_read)(audio_ring_t *ring, uint32_t *frames, uint32_t count) {
    uint32_t w = ring->write_idx;
    uint32_t r = ring->read_idx;
    uint32_t avail = w - r;

    if (avail < count) {
        ring->underrun_count++;
        for (uint32_t i = 0; i < avail; i++) {
            frames[i] = ring->buffer[(r + i) & AUDIO_RING_MASK];
        }
        uint32_t pad = (avail > 0) ? frames[avail - 1] : 0;
        for (uint32_t i = avail; i < count; i++) {
            frames[i] = pad;
        }
        __dmb();
        ring->read_idx = r + avail;
        return avail;
    }

    for (uint32_t i = 0; i < count; i++) {
        frames[i] = ring->buffer[(r + i) & AUDIO_RING_MASK];
    }
    ring->last_sample = frames[count - 1];
    __dmb();
    ring->read_idx = r + count;
    return count;
}

// Helper: linear interpolation of two packed stereo 16-bit samples (50/50 blend)
static inline uint32_t blend_stereo_sample(uint32_t s0, uint32_t s1) {
    int16_t l0 = (int16_t)(s0 >> 16);
    int16_t r0 = (int16_t)(s0 & 0xFFFF);
    int16_t l1 = (int16_t)(s1 >> 16);
    int16_t r1 = (int16_t)(s1 & 0xFFFF);

    int16_t lm = (int16_t)(((int32_t)l0 + (int32_t)l1) >> 1);
    int16_t rm = (int16_t)(((int32_t)r0 + (int32_t)r1) >> 1);

    return ((uint32_t)(uint16_t)lm << 16) | (uint32_t)(uint16_t)rm;
}

void __not_in_flash_func(audio_ring_read_adaptive)(audio_ring_t *ring, uint32_t *out_frames, uint32_t target_out_count) {
    uint32_t w = ring->write_idx;
    uint32_t r = ring->read_idx;
    uint32_t avail = w - r;

    // Wait for buffer to reach target half-capacity before starting playback after underrun
    if (!ring->primed) {
        if (avail < AUDIO_RING_TARGET_FRAMES) {
            memset(out_frames, 0, target_out_count * sizeof(uint32_t));
            return;
        }
        ring->primed = true;
        ring->adapt_cooldown = 64u;
    } else if (avail < target_out_count + 4u) {
        ring->primed = false;
        ring->underrun_count++;
        memset(out_frames, 0, target_out_count * sizeof(uint32_t));
        return;
    }

    if (ring->adapt_cooldown > 0u) {
        ring->adapt_cooldown--;
    }

    // Rate-limited drift compensation: adjust by at most 1 sample every 64 ms (3072 frames = 0.032% = 325 ppm)
    // so crystal drift is tracked smoothly with zero audible pitch shift.
    if (ring->adapt_cooldown == 0u &&
        avail > (AUDIO_RING_TARGET_FRAMES + 384u) &&
        avail >= (target_out_count + 1u)) {
        ring->adapt_cooldown = 64u;
        for (uint32_t i = 0; i < target_out_count - 1u; i++) {
            out_frames[i] = ring->buffer[(r + i) & AUDIO_RING_MASK];
        }
        uint32_t s_a = ring->buffer[(r + target_out_count - 1u) & AUDIO_RING_MASK];
        uint32_t s_b = ring->buffer[(r + target_out_count) & AUDIO_RING_MASK];
        out_frames[target_out_count - 1u] = blend_stereo_sample(s_a, s_b);
        ring->last_sample = s_b;
        __dmb();
        ring->read_idx = r + target_out_count + 1u;
    } else if (ring->adapt_cooldown == 0u &&
               avail < (AUDIO_RING_TARGET_FRAMES - 384u) &&
               target_out_count >= 2u) {
        ring->adapt_cooldown = 64u;
        uint32_t consume = target_out_count - 1u;
        for (uint32_t i = 0; i < consume; i++) {
            out_frames[i] = ring->buffer[(r + i) & AUDIO_RING_MASK];
        }
        uint32_t last = out_frames[consume - 1u];
        out_frames[target_out_count - 1u] = last;
        out_frames[target_out_count - 2u] = blend_stereo_sample(ring->last_sample, last);
        ring->last_sample = last;
        __dmb();
        ring->read_idx = r + consume;
    } else {
        // Exact 1:1 bit-exact read
        for (uint32_t i = 0; i < target_out_count; i++) {
            out_frames[i] = ring->buffer[(r + i) & AUDIO_RING_MASK];
        }
        ring->last_sample = out_frames[target_out_count - 1u];
        __dmb();
        ring->read_idx = r + target_out_count;
    }
}
