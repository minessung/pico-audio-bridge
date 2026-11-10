#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/platform.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/structs/sio.h"
#include "bsp/board_api.h"
#include "tusb.h"

#include "bridge_config.h"
#include "usb_descriptors.h"
#include "audio_ring.h"
#include "i2s_bridge.h"
#include "uac1_device.h"

// Two Full-Duplex Audio Ring Buffers:
// 1. usb_to_i2s_ring: USB Speaker OUT (Host -> Pico) ===> I2S DOUT (GPIO 12)
// 2. i2s_to_usb_ring: I2S DIN (GPIO 13)              ===> USB Mic IN (Pico -> Host)
static audio_ring_t usb_to_i2s_ring;
static audio_ring_t i2s_to_usb_ring;

static bool detect_master_role(void) {
    gpio_init(PIN_ROLE_SELECT);
    gpio_set_dir(PIN_ROLE_SELECT, GPIO_IN);
    gpio_pull_up(PIN_ROLE_SELECT);
    sleep_us(50); // Allow internal pull-up to settle

    // GPIO 14 HIGH (Open) = Master (Android Phone side)
    // GPIO 14 LOW  (GND)  = Slave  (PC / Mac side)
    return gpio_get(PIN_ROLE_SELECT);
}

static void __not_in_flash_func(bridge_main_loop)(bool is_master) {
    uint32_t last_hb_us = timer_hw->timelr;

    while (true) {
        // Service TinyUSB device events in a tight SRAM loop with zero blocking
        tud_task();

        // Check 32-bit hardware microsecond counter (1 CPU cycle, no 64-bit division!)
        uint32_t now_us = timer_hw->timelr;
        if ((now_us - last_hb_us) >= 1000000u) {
            last_hb_us = now_us;
            sio_hw->gpio_togl = (1u << PIN_STATUS_LED);

            // Never call printf() while USB audio streaming is active so tud_task()
            // never suffers XIP flash cache eviction or UART formatting latency!
            const uac1_status_t *st = uac1_device_get_status();
            if (!st->spk_streaming && !st->mic_streaming) {
                printf("%c M%u S0 C0\r\n",
                       is_master ? 'M' : 'S',
                       tud_mounted() ? 1u : 0u);
            }
        }
    }
}

int main(void) {
    // Initialize board & UART0 stdio (115200 8N1 on GPIO 0 TX / GPIO 1 RX for Pico Debug Probe)
    board_init();
    stdio_init_all();

    // Optional external status LED on GPIO 15
    gpio_init(PIN_STATUS_LED);
    gpio_set_dir(PIN_STATUS_LED, GPIO_OUT);
    gpio_put(PIN_STATUS_LED, 0);

    bool is_master = detect_master_role();
    usb_descriptors_set_role(is_master);

    printf("\r\n========================================================\r\n");
    printf(" Pico WH Full-Duplex UAC 1.0 <-> I2S Audio Bridge\r\n");
    printf("========================================================\r\n");
    printf(" Role (GPIO %u) : %s\r\n", PIN_ROLE_SELECT,
           is_master ? "I2S MASTER (Android Phone Side, PID=0x4001)"
                     : "I2S SLAVE  (PC / Mac Side,      PID=0x4002)");
    printf(" Audio Format  : %u Hz, %u-bit, %u-ch Stereo (UAC 1.0 Headset)\r\n",
           AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, AUDIO_NUM_CHANNELS);
    printf(" I2S Pinout    : BCLK=GP%u, LRCLK=GP%u, DOUT=GP%u, DIN=GP%u\r\n",
           PIN_I2S_BCLK_BASE, PIN_I2S_LRCLK, PIN_I2S_DOUT, PIN_I2S_DIN);
    printf("========================================================\r\n");

    // Initialize ring buffers
    audio_ring_init(&usb_to_i2s_ring);
    audio_ring_init(&i2s_to_usb_ring);

    // Bind ring buffers to UAC 1.0 USB driver and start PIO+DMA I2S engine
    uac1_device_bind_rings(&usb_to_i2s_ring, &i2s_to_usb_ring);
    i2s_bridge_init(is_master, &usb_to_i2s_ring, &i2s_to_usb_ring);

    // Initialize TinyUSB device stack on roothub port 0
    tusb_init();

    bridge_main_loop(is_master);
    return 0;
}
