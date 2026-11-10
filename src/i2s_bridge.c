#include "i2s_bridge.h"
#include "bridge_config.h"
#include "i2s_bridge.pio.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include <string.h>

// Ping-pong DMA buffers (2 blocks of 48 stereo frames = 1ms per block)
static uint32_t i2s_tx_buf[2][I2S_DMA_FRAMES_PER_BLOCK] __attribute__((aligned(16)));
static uint32_t i2s_rx_buf[2][I2S_DMA_FRAMES_PER_BLOCK] __attribute__((aligned(16)));

static int dma_chan_tx_a = -1;
static int dma_chan_tx_b = -1;
static int dma_chan_rx_a = -1;
static int dma_chan_rx_b = -1;

static audio_ring_t *g_usb_to_i2s = NULL;
static audio_ring_t *g_i2s_to_usb = NULL;
static volatile uint32_t g_dma_block_count = 0;

// Convert USB PCM stereo frame (low 16-bit = Left, high 16-bit = Right in little-endian uint32_t)
// to I2S 32-bit MSB-first frame (high 16-bit = Left, low 16-bit = Right) and vice versa
static inline void swap_stereo_halfwords_block(uint32_t *buf, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = buf[i];
        buf[i] = (v << 16) | (v >> 16);
    }
}

static void __isr i2s_dma_irq_handler(void) {
    uint32_t ints = dma_hw->ints0;

    // Handle TX Ping-Pong Refill (USB Speaker -> I2S DOUT)
    if (ints & (1u << dma_chan_tx_a)) {
        dma_hw->ints0 = (1u << dma_chan_tx_a);
        audio_ring_read_adaptive(g_usb_to_i2s, i2s_tx_buf[0], I2S_DMA_FRAMES_PER_BLOCK);
        swap_stereo_halfwords_block(i2s_tx_buf[0], I2S_DMA_FRAMES_PER_BLOCK);
        dma_channel_set_read_addr(dma_chan_tx_a, i2s_tx_buf[0], false);
    }
    if (ints & (1u << dma_chan_tx_b)) {
        dma_hw->ints0 = (1u << dma_chan_tx_b);
        audio_ring_read_adaptive(g_usb_to_i2s, i2s_tx_buf[1], I2S_DMA_FRAMES_PER_BLOCK);
        swap_stereo_halfwords_block(i2s_tx_buf[1], I2S_DMA_FRAMES_PER_BLOCK);
        dma_channel_set_read_addr(dma_chan_tx_b, i2s_tx_buf[1], false);
    }

    // Handle RX Ping-Pong Drain (I2S DIN -> USB Microphone)
    if (ints & (1u << dma_chan_rx_a)) {
        dma_hw->ints0 = (1u << dma_chan_rx_a);
        swap_stereo_halfwords_block(i2s_rx_buf[0], I2S_DMA_FRAMES_PER_BLOCK);
        audio_ring_write(g_i2s_to_usb, i2s_rx_buf[0], I2S_DMA_FRAMES_PER_BLOCK);
        dma_channel_set_write_addr(dma_chan_rx_a, i2s_rx_buf[0], false);
        g_dma_block_count++;
    }
    if (ints & (1u << dma_chan_rx_b)) {
        dma_hw->ints0 = (1u << dma_chan_rx_b);
        swap_stereo_halfwords_block(i2s_rx_buf[1], I2S_DMA_FRAMES_PER_BLOCK);
        audio_ring_write(g_i2s_to_usb, i2s_rx_buf[1], I2S_DMA_FRAMES_PER_BLOCK);
        dma_channel_set_write_addr(dma_chan_rx_b, i2s_rx_buf[1], false);
        g_dma_block_count++;
    }
}

void i2s_bridge_init(bool is_master, audio_ring_t *usb_to_i2s_ring, audio_ring_t *i2s_to_usb_ring) {
    g_usb_to_i2s = usb_to_i2s_ring;
    g_i2s_to_usb = i2s_to_usb_ring;

    memset(i2s_tx_buf, 0, sizeof(i2s_tx_buf));
    memset(i2s_rx_buf, 0, sizeof(i2s_rx_buf));

    PIO pio = pio0;
    uint sm = 0;

    // Configure GPIO pins for PIO0
    pio_gpio_init(pio, PIN_I2S_DOUT);
    pio_gpio_init(pio, PIN_I2S_DIN);
    gpio_pull_down(PIN_I2S_DIN); // Prevent floating noise if jumper wire is disconnected

    if (is_master) {
        pio_gpio_init(pio, PIN_I2S_BCLK_BASE);
        pio_gpio_init(pio, PIN_I2S_LRCLK);

        uint offset = pio_add_program(pio, &i2s_master_duplex_program);
        pio_sm_config c = i2s_master_duplex_program_get_default_config(offset);

        sm_config_set_out_pins(&c, PIN_I2S_DOUT, 1);
        sm_config_set_in_pins(&c, PIN_I2S_DIN);
        sm_config_set_sideset_pins(&c, PIN_I2S_BCLK_BASE);

        // Shift left (MSB first), autopull = true, threshold = 32 bits
        sm_config_set_out_shift(&c, false, true, 32);
        sm_config_set_in_shift(&c, false, true, 32);

        // Master uses 3 PIO cycles per bit * 32 bits per frame + 2 setup cycles per frame = 98 cycles per frame!
        //Wait: Let's check exact cycle count of i2s_master_duplex_program!
        // In i2s_master_duplex:
        //   set x, 14 (1) + 15 * (out + in + jmp = 3) + out (1) + in (1) = 1 + 45 + 2 = 48 cycles for Left!
        //   set x, 14 (1) + 15 * (out + in + jmp = 3) + out (1) + in (1) = 1 + 45 + 2 = 48 cycles for Right!
        //   Total = 96 PIO cycles per stereo sample frame (48kHz)!
        //   48,000 Hz * 96 cycles = 4,608,000 Hz PIO clock!
        float div = (float)clock_get_hz(clk_sys) / (float)(AUDIO_SAMPLE_RATE * 96u);
        sm_config_set_clkdiv(&c, div);

        pio_sm_set_pins_with_mask(pio, sm, 0,
            (1u << PIN_I2S_BCLK_BASE) | (1u << PIN_I2S_LRCLK) | (1u << PIN_I2S_DOUT));
        pio_sm_set_pindirs_with_mask(pio, sm,
            (1u << PIN_I2S_BCLK_BASE) | (1u << PIN_I2S_LRCLK) | (1u << PIN_I2S_DOUT),
            (1u << PIN_I2S_BCLK_BASE) | (1u << PIN_I2S_LRCLK) | (1u << PIN_I2S_DOUT) | (1u << PIN_I2S_DIN));

        pio_sm_init(pio, sm, offset, &c);
    } else {
        // Slave mode: BCLK (GPIO 10) and LRCLK (GPIO 11) are inputs
        gpio_init(PIN_I2S_BCLK_BASE);
        gpio_set_dir(PIN_I2S_BCLK_BASE, GPIO_IN);
        gpio_pull_down(PIN_I2S_BCLK_BASE);

        gpio_init(PIN_I2S_LRCLK);
        gpio_set_dir(PIN_I2S_LRCLK, GPIO_IN);
        gpio_pull_down(PIN_I2S_LRCLK);

        uint offset = pio_add_program(pio, &i2s_slave_duplex_program);
        pio_sm_config c = i2s_slave_duplex_program_get_default_config(offset);

        sm_config_set_out_pins(&c, PIN_I2S_DOUT, 1);
        sm_config_set_in_pins(&c, PIN_I2S_DIN);

        // Shift left (MSB first), explicit pull/push per LRCLK frame (autopull=false, autopush=false)
        sm_config_set_out_shift(&c, false, false, 32);
        sm_config_set_in_shift(&c, false, false, 32);

        // Run Slave PIO at full clk_sys (125MHz) to sample BCLK/LRCLK edges accurately
        sm_config_set_clkdiv(&c, 1.0f);

        pio_sm_set_pins_with_mask(pio, sm, 0, (1u << PIN_I2S_DOUT));
        pio_sm_set_pindirs_with_mask(pio, sm,
            (1u << PIN_I2S_DOUT),
            (1u << PIN_I2S_BCLK_BASE) | (1u << PIN_I2S_LRCLK) | (1u << PIN_I2S_DOUT) | (1u << PIN_I2S_DIN));

        pio_sm_init(pio, sm, offset, &c);
    }

    // Claim 4 DMA channels for Ping-Pong TX (A/B) and RX (A/B)
    dma_chan_tx_a = dma_claim_unused_channel(true);
    dma_chan_tx_b = dma_claim_unused_channel(true);
    dma_chan_rx_a = dma_claim_unused_channel(true);
    dma_chan_rx_b = dma_claim_unused_channel(true);

    // Configure TX Channel A (chains to TX Channel B)
    dma_channel_config cfg_tx_a = dma_channel_get_default_config(dma_chan_tx_a);
    channel_config_set_transfer_data_size(&cfg_tx_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_tx_a, true);
    channel_config_set_write_increment(&cfg_tx_a, false);
    channel_config_set_dreq(&cfg_tx_a, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&cfg_tx_a, dma_chan_tx_b);
    dma_channel_configure(dma_chan_tx_a, &cfg_tx_a, &pio->txf[sm], i2s_tx_buf[0], I2S_DMA_FRAMES_PER_BLOCK, false);

    // Configure TX Channel B (chains to TX Channel A)
    dma_channel_config cfg_tx_b = dma_channel_get_default_config(dma_chan_tx_b);
    channel_config_set_transfer_data_size(&cfg_tx_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_tx_b, true);
    channel_config_set_write_increment(&cfg_tx_b, false);
    channel_config_set_dreq(&cfg_tx_b, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&cfg_tx_b, dma_chan_tx_a);
    dma_channel_configure(dma_chan_tx_b, &cfg_tx_b, &pio->txf[sm], i2s_tx_buf[1], I2S_DMA_FRAMES_PER_BLOCK, false);

    // Configure RX Channel A (chains to RX Channel B)
    dma_channel_config cfg_rx_a = dma_channel_get_default_config(dma_chan_rx_a);
    channel_config_set_transfer_data_size(&cfg_rx_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_rx_a, false);
    channel_config_set_write_increment(&cfg_rx_a, true);
    channel_config_set_dreq(&cfg_rx_a, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&cfg_rx_a, dma_chan_rx_b);
    dma_channel_configure(dma_chan_rx_a, &cfg_rx_a, i2s_rx_buf[0], &pio->rxf[sm], I2S_DMA_FRAMES_PER_BLOCK, false);

    // Configure RX Channel B (chains to RX Channel A)
    dma_channel_config cfg_rx_b = dma_channel_get_default_config(dma_chan_rx_b);
    channel_config_set_transfer_data_size(&cfg_rx_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_rx_b, false);
    channel_config_set_write_increment(&cfg_rx_b, true);
    channel_config_set_dreq(&cfg_rx_b, pio_get_dreq(pio, sm, false));
    channel_config_set_chain_to(&cfg_rx_b, dma_chan_rx_a);
    dma_channel_configure(dma_chan_rx_b, &cfg_rx_b, i2s_rx_buf[1], &pio->rxf[sm], I2S_DMA_FRAMES_PER_BLOCK, false);

    // Enable DMA IRQ0 for all 4 channels
    uint32_t irq_mask = (1u << dma_chan_tx_a) | (1u << dma_chan_tx_b) |
                        (1u << dma_chan_rx_a) | (1u << dma_chan_rx_b);
    dma_hw->ints0 = irq_mask;
    dma_set_irq0_channel_mask_enabled(irq_mask, true);
    irq_set_exclusive_handler(DMA_IRQ_0, i2s_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Start DMA channel A for both TX and RX, then enable PIO state machine
    dma_start_channel_mask((1u << dma_chan_tx_a) | (1u << dma_chan_rx_a));
    pio_sm_set_enabled(pio, sm, true);
}

uint32_t i2s_bridge_get_dma_block_count(void) {
    return g_dma_block_count;
}
