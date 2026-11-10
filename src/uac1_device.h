#ifndef UAC1_DEVICE_H_
#define UAC1_DEVICE_H_

#include <stdint.h>
#include <stdbool.h>
#include "audio_ring.h"

typedef struct {
    volatile bool spk_streaming;
    volatile bool mic_streaming;
    volatile bool spk_mute;
    volatile bool mic_mute;
    volatile int16_t spk_volume; // in 1/256 dB units (UAC 1.0 format)
    volatile int16_t mic_volume; // in 1/256 dB units (UAC 1.0 format)
    volatile uint32_t usb_rx_packets;
    volatile uint32_t usb_tx_packets;
} uac1_status_t;

void uac1_device_bind_rings(audio_ring_t *usb_to_i2s_ring, audio_ring_t *i2s_to_usb_ring);
const uac1_status_t *uac1_device_get_status(void);

#endif /* UAC1_DEVICE_H_ */
