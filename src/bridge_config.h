#ifndef BRIDGE_CONFIG_H_
#define BRIDGE_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

//--------------------------------------------------------------------+
// Audio Format Configuration (UAC 1.0 Full-Duplex Headset)
//--------------------------------------------------------------------+
#define AUDIO_SAMPLE_RATE           48000u
#define AUDIO_NUM_CHANNELS          2u
#define AUDIO_BITS_PER_SAMPLE       16u
#define AUDIO_BYTES_PER_SAMPLE      (AUDIO_BITS_PER_SAMPLE / 8u)
#define AUDIO_FRAME_BYTES           (AUDIO_NUM_CHANNELS * AUDIO_BYTES_PER_SAMPLE) // 4 bytes per stereo frame

// Nominal samples (stereo frames) per 1ms USB Full-Speed SOF frame
#define AUDIO_FRAMES_PER_MS         (AUDIO_SAMPLE_RATE / 1000u)                   // 48 frames
// Allow +/- 1 stereo frame per ms for clock drift adjustment (47, 48, or 49 frames)
#define AUDIO_MAX_FRAMES_PER_MS     (AUDIO_FRAMES_PER_MS + 1u)                    // 49 frames
#define AUDIO_MIN_FRAMES_PER_MS     (AUDIO_FRAMES_PER_MS - 1u)                    // 47 frames

#define AUDIO_NOM_PACKET_BYTES      (AUDIO_FRAMES_PER_MS * AUDIO_FRAME_BYTES)     // 192 bytes
#define AUDIO_MAX_PACKET_BYTES      (AUDIO_MAX_FRAMES_PER_MS * AUDIO_FRAME_BYTES) // 196 bytes

//--------------------------------------------------------------------+
// Hardware Pin Assignment (Single Firmware for Master & Slave)
//--------------------------------------------------------------------+
// Debug Probe UART0:
//   GPIO 0 -> UART0 TX (Pin 1)
//   GPIO 1 -> UART0 RX (Pin 2)

// Role Selection Pin:
//   GPIO 14 (Pin 19) with internal Pull-Up enabled
//   - Leave OPEN (HIGH)      -> Pico #1: I2S Master (Android Phone side)
//   - Tie to GND (LOW)       -> Pico #2: I2S Slave  (PC / Mac side)
#define PIN_ROLE_SELECT             14u

// External Status / Activity LED (optional, safe on both Pico and Pico WH)
#define PIN_STATUS_LED              15u

// Inter-Pico Full-Duplex I2S Bus Pins:
//   BCLK  : GPIO 10 (Pin 14) - Driven by Master, Input on Slave
//   LRCLK : GPIO 11 (Pin 15) - Driven by Master, Input on Slave (must be BCLK + 1 for PIO sideset)
//   Master TX -> Slave RX :
//     - Master outputs on PIN_I2S_DOUT (GPIO 12, Pin 16)
//     - Slave  outputs on PIN_I2S_DOUT (GPIO 12, Pin 16)
//     - Master inputs  on PIN_I2S_DIN  (GPIO 13, Pin 17)
//     - Slave  inputs  on PIN_I2S_DIN  (GPIO 13, Pin 17)
//   Wait: To make wiring 1:1 or crossover simple and unambiguous:
//   Each Pico uses:
//     GPIO 10 = BCLK  (connect GPIO 10 <-> GPIO 10)
//     GPIO 11 = LRCLK (connect GPIO 11 <-> GPIO 11)
//     GPIO 12 = DOUT  (this Pico's I2S TX -> connect to other Pico's GPIO 13 DIN)
//     GPIO 13 = DIN   (this Pico's I2S RX <- connect from other Pico's GPIO 12 DOUT)
#define PIN_I2S_BCLK_BASE           10u // GPIO 10 = BCLK, GPIO 11 = LRCLK (WS)
#define PIN_I2S_LRCLK               11u
#define PIN_I2S_DOUT                12u // I2S Data Out (USB Host Speaker -> Other Pico)
#define PIN_I2S_DIN                 13u // I2S Data In  (Other Pico -> USB Host Mic)

//--------------------------------------------------------------------+
// Ring Buffer & Clock Drift Compensation Parameters
//--------------------------------------------------------------------+
// Power-of-2 stereo frame capacity (2048 stereo frames = ~42.67 ms buffer, target fill = 1024 frames = ~21.33 ms)
#define AUDIO_RING_CAPACITY_FRAMES  2048u
#define AUDIO_RING_MASK             (AUDIO_RING_CAPACITY_FRAMES - 1u)
#define AUDIO_RING_TARGET_FRAMES    (AUDIO_RING_CAPACITY_FRAMES / 2u) // 1024 frames

// I2S DMA block size in stereo frames (48 frames = 1ms per DMA interrupt)
#define I2S_DMA_FRAMES_PER_BLOCK    48u

#endif /* BRIDGE_CONFIG_H_ */
