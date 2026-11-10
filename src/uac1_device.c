#include "uac1_device.h"
#include "usb_descriptors.h"
#include "bridge_config.h"

#include "tusb.h"
#include "device/usbd_pvt.h"
#include "pico/platform.h"
#include "hardware/structs/usb.h"
#include "hardware/sync.h"
#include <string.h>

static audio_ring_t *g_usb_out_ring = NULL; // Host Speaker -> I2S DOUT
static audio_ring_t *g_usb_in_ring  = NULL; // I2S DIN -> Host Mic

static uac1_status_t g_status = {
    .spk_streaming = false,
    .mic_streaming = false,
    .spk_mute      = false,
    .mic_mute      = false,
    .spk_volume    = 0, // 0 dB
    .mic_volume    = 0, // 0 dB
    .usb_rx_packets = 0,
    .usb_tx_packets = 0,
};

static uint8_t g_alt_setting[ITF_NUM_TOTAL];
static uint32_t g_sample_freq = AUDIO_SAMPLE_RATE;

// Endpoint transfer buffers (aligned to 4 bytes)
static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t ep_out_buf[AUDIO_MAX_PACKET_BYTES];
static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t ep_in_buf[AUDIO_MAX_PACKET_BYTES];
static CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t ctrl_buf[16];

void uac1_device_bind_rings(audio_ring_t *usb_to_i2s_ring, audio_ring_t *i2s_to_usb_ring) {
    g_usb_out_ring = usb_to_i2s_ring;
    g_usb_in_ring  = i2s_to_usb_ring;
}

const uac1_status_t *uac1_device_get_status(void) {
    return &g_status;
}

//--------------------------------------------------------------------+
// Internal Helper: Pre-fill and Queue Isochronous IN packet (Pico -> Host Mic)
//--------------------------------------------------------------------+
static void __not_in_flash_func(uac1_prepare_next_mic_packet)(void) {
    if (!g_status.mic_streaming || !g_usb_in_ring) {
        memset(ep_in_buf, 0, AUDIO_NOM_PACKET_BYTES);
        return;
    }

    audio_ring_read_adaptive(g_usb_in_ring, (uint32_t *)ep_in_buf, AUDIO_FRAMES_PER_MS);

    if (g_status.mic_mute) {
        memset(ep_in_buf, 0, AUDIO_NOM_PACKET_BYTES);
    }
}

static void __not_in_flash_func(uac1_queue_next_mic_packet)(uint8_t rhport) {
    if (!g_status.mic_streaming) {
        return;
    }
    // Immediately arm EP 0x82 IN with the pre-filled packet (copies ep_in_buf to USB DPRAM),
    // then pre-fill ep_in_buf for the following 1ms SOF frame.
    usbd_edpt_xfer(rhport, EPNUM_AUDIO_IN, ep_in_buf, AUDIO_NOM_PACKET_BYTES);
    uac1_prepare_next_mic_packet();
}

//--------------------------------------------------------------------+
// TinyUSB Custom Class Driver Callbacks
//--------------------------------------------------------------------+
static void uac1_init(void) {
    memset(g_alt_setting, 0, sizeof(g_alt_setting));
    g_status.spk_streaming = false;
    g_status.mic_streaming = false;
}

static bool uac1_deinit(void) {
    return true;
}

static void uac1_reset(uint8_t rhport) {
    (void)rhport;
    memset(g_alt_setting, 0, sizeof(g_alt_setting));
    g_status.spk_streaming = false;
    g_status.mic_streaming = false;
}

static uint16_t uac1_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    TU_VERIFY(TUSB_CLASS_AUDIO == itf_desc->bInterfaceClass, 0);

    // Open is called once per standard interface (Alt 0) when no IAD is present:
    //   1) ITF_NUM_AUDIO_CONTROL (0)
    //   2) ITF_NUM_AUDIO_STREAMING_SPK (1)
    //   3) ITF_NUM_AUDIO_STREAMING_MIC (2)
    if (itf_desc->bInterfaceNumber == ITF_NUM_AUDIO_STREAMING_SPK) {
        usbd_edpt_iso_alloc(rhport, EPNUM_AUDIO_OUT, AUDIO_MAX_PACKET_BYTES);
    } else if (itf_desc->bInterfaceNumber == ITF_NUM_AUDIO_STREAMING_MIC) {
        usbd_edpt_iso_alloc(rhport, EPNUM_AUDIO_IN, AUDIO_MAX_PACKET_BYTES);
    }

    // Calculate length of this single interface (including all its alternate settings & class descriptors)
    return tu_desc_get_interface_total_len(itf_desc, 1, max_len);
}

// Find standard endpoint descriptor for given (interface, alt_setting)
static tusb_desc_endpoint_t const *find_iso_ep_desc(uint8_t itf, uint8_t alt) {
    uint8_t const *p_desc = tud_descriptor_configuration_cb(0);
    uint16_t total_len = ((tusb_desc_configuration_t const *)p_desc)->wTotalLength;
    uint8_t const *p_end = p_desc + total_len;

    bool in_target_itf = false;
    while (p_desc < p_end) {
        uint8_t len = tu_desc_len(p_desc);
        if (len == 0) break;

        if (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE) {
            tusb_desc_interface_t const *d_itf = (tusb_desc_interface_t const *)p_desc;
            in_target_itf = (d_itf->bInterfaceNumber == itf && d_itf->bAlternateSetting == alt);
        } else if (in_target_itf && tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
            return (tusb_desc_endpoint_t const *)p_desc;
        }
        p_desc += len;
    }
    return NULL;
}

// Safely clear any pending RP2040 USB DPRAM buffer control bits before
// deactivating or re-activating an isochronous endpoint.
static void uac1_quiesce_iso_ep(uint8_t ep_addr) {
    uint8_t const epnum = tu_edpt_number(ep_addr);
    uint8_t const dir   = tu_edpt_dir(ep_addr);
    if (epnum == 0 || epnum >= USB_MAX_ENDPOINTS) {
        return;
    }

    if (dir == TUSB_DIR_IN) {
        usb_dpram->ep_buf_ctrl[epnum].in = 0;
        busy_wait_at_least_cycles(12);
        usb_dpram->ep_ctrl[epnum - 1].in = 0;
    } else {
        usb_dpram->ep_buf_ctrl[epnum].out = 0;
        busy_wait_at_least_cycles(12);
        usb_dpram->ep_ctrl[epnum - 1].out = 0;
    }
}

static bool uac1_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    // 1. Standard Interface Requests (GET_INTERFACE / SET_INTERFACE)
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {

        uint8_t const itf = tu_u16_low(request->wIndex);
        uint8_t const alt = tu_u16_low(request->wValue);
        TU_VERIFY(itf < ITF_NUM_TOTAL);

        if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
            if (stage == CONTROL_STAGE_SETUP) {
                return tud_control_xfer(rhport, request, &g_alt_setting[itf], 1);
            }
            return true;
        }

        if (request->bRequest == TUSB_REQ_SET_INTERFACE) {
            if (stage == CONTROL_STAGE_SETUP) {
                g_alt_setting[itf] = alt;

                if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
                    g_status.spk_streaming = false;
                    uac1_quiesce_iso_ep(EPNUM_AUDIO_OUT);
                    if (alt == 1) {
                        if (g_usb_out_ring) {
                            audio_ring_reset(g_usb_out_ring);
                        }
                        tusb_desc_endpoint_t const *desc_ep = find_iso_ep_desc(itf, alt);
                        TU_ASSERT(desc_ep);
                        usbd_edpt_iso_activate(rhport, desc_ep);
                        usbd_edpt_clear_stall(rhport, EPNUM_AUDIO_OUT);
                        g_status.spk_streaming = true;
                        usbd_edpt_xfer(rhport, EPNUM_AUDIO_OUT, ep_out_buf, AUDIO_MAX_PACKET_BYTES);
                    }
                } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
                    g_status.mic_streaming = false;
                    uac1_quiesce_iso_ep(EPNUM_AUDIO_IN);
                    if (alt == 1) {
                        if (g_usb_in_ring) {
                            uint32_t w = g_usb_in_ring->write_idx;
                            uint32_t r = g_usb_in_ring->read_idx;
                            if ((w - r) > AUDIO_RING_TARGET_FRAMES) {
                                g_usb_in_ring->read_idx = w - AUDIO_RING_TARGET_FRAMES;
                                g_usb_in_ring->primed = true;
                                g_usb_in_ring->adapt_cooldown = 64u;
                            }
                        }
                        tusb_desc_endpoint_t const *desc_ep = find_iso_ep_desc(itf, alt);
                        TU_ASSERT(desc_ep);
                        usbd_edpt_iso_activate(rhport, desc_ep);
                        usbd_edpt_clear_stall(rhport, EPNUM_AUDIO_IN);
                        g_status.mic_streaming = true;
                        uac1_prepare_next_mic_packet();
                        uac1_queue_next_mic_packet(rhport);
                    }
                }
                return tud_control_status(rhport, request);
            }
            return true;
        }
    }

    // 2. Audio Class-Specific Requests (UAC 1.0 Feature Unit & Endpoint Frequency Control)
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) {
        uint8_t const ctrl_sel = tu_u16_high(request->wValue);
        uint8_t const entity_id = tu_u16_high(request->wIndex);

        // 2a. Feature Unit Requests (Recipient = Interface)
        if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
            bool is_spk = (entity_id == UAC1_ENTITY_SPK_FEATURE_UNIT);
            bool is_mic = (entity_id == UAC1_ENTITY_MIC_FEATURE_UNIT);

            if (!is_spk && !is_mic) {
                return false;
            }

            // Direction IN: Host reads Mute or Volume (GET_CUR, GET_MIN, GET_MAX, GET_RES)
            if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
                if (stage != CONTROL_STAGE_SETUP) return true;

                if (ctrl_sel == UAC1_FU_MUTE_CONTROL) {
                    ctrl_buf[0] = (uint8_t)(is_spk ? g_status.spk_mute : g_status.mic_mute);
                    return tud_control_xfer(rhport, request, ctrl_buf, 1);
                } else if (ctrl_sel == UAC1_FU_VOLUME_CONTROL) {
                    int16_t val = 0;
                    switch (request->bRequest) {
                        case UAC1_REQ_GET_CUR: val = is_spk ? g_status.spk_volume : g_status.mic_volume; break;
                        case UAC1_REQ_GET_MIN: val = (int16_t)(-60 * 256); break; // -60 dB
                        case UAC1_REQ_GET_MAX: val = (int16_t)(0 * 256);   break; //   0 dB
                        case UAC1_REQ_GET_RES: val = (int16_t)(1 * 256);   break; //   1 dB step
                        default: return false;
                    }
                    memcpy(ctrl_buf, &val, 2);
                    return tud_control_xfer(rhport, request, ctrl_buf, 2);
                }
            } else {
                // Direction OUT: Host sets Mute or Volume (SET_CUR)
                if (stage == CONTROL_STAGE_SETUP) {
                    return tud_control_xfer(rhport, request, ctrl_buf, request->wLength);
                } else if (stage == CONTROL_STAGE_DATA) {
                    if (ctrl_sel == UAC1_FU_MUTE_CONTROL && request->wLength >= 1) {
                        if (is_spk) g_status.spk_mute = (ctrl_buf[0] != 0);
                        else        g_status.mic_mute = (ctrl_buf[0] != 0);
                    } else if (ctrl_sel == UAC1_FU_VOLUME_CONTROL && request->wLength >= 2) {
                        int16_t vol;
                        memcpy(&vol, ctrl_buf, 2);
                        if (is_spk) g_status.spk_volume = vol;
                        else        g_status.mic_volume = vol;
                    }
                }
                return true;
            }
        }

        // 2b. Endpoint Sampling Frequency Requests (Recipient = Endpoint)
        if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_ENDPOINT) {
            if (ctrl_sel == UAC1_EP_SAMPLING_FREQ_CONTROL) {
                if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
                    if (stage == CONTROL_STAGE_SETUP) {
                        ctrl_buf[0] = (uint8_t)(g_sample_freq & 0xFF);
                        ctrl_buf[1] = (uint8_t)((g_sample_freq >> 8) & 0xFF);
                        ctrl_buf[2] = (uint8_t)((g_sample_freq >> 16) & 0xFF);
                        return tud_control_xfer(rhport, request, ctrl_buf, 3);
                    }
                    return true;
                } else {
                    if (stage == CONTROL_STAGE_SETUP) {
                        return tud_control_xfer(rhport, request, ctrl_buf, request->wLength);
                    } else if (stage == CONTROL_STAGE_DATA && request->wLength >= 3) {
                        uint32_t freq = (uint32_t)ctrl_buf[0] |
                                        ((uint32_t)ctrl_buf[1] << 8) |
                                        ((uint32_t)ctrl_buf[2] << 16);
                        if (freq == AUDIO_SAMPLE_RATE) {
                            g_sample_freq = freq;
                        }
                    }
                    return true;
                }
            }
        }
    }

    return false;
}

static bool __not_in_flash_func(uac1_xfer_cb)(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;

    if (ep_addr == EPNUM_AUDIO_OUT) {
        // Received PCM audio from USB Host (Speaker path)
        if (xferred_bytes >= AUDIO_FRAME_BYTES && g_usb_out_ring) {
            uint32_t num_frames = xferred_bytes / AUDIO_FRAME_BYTES;
            if (g_status.spk_mute) {
                memset(ep_out_buf, 0, num_frames * AUDIO_FRAME_BYTES);
            }
            audio_ring_write(g_usb_out_ring, (const uint32_t *)ep_out_buf, num_frames);
            g_status.usb_rx_packets++;
        }

        // Re-arm OUT endpoint if Speaker streaming is still active
        if (g_status.spk_streaming) {
            usbd_edpt_xfer(rhport, EPNUM_AUDIO_OUT, ep_out_buf, AUDIO_MAX_PACKET_BYTES);
        }
        return true;
    }

    if (ep_addr == EPNUM_AUDIO_IN) {
        // Completed sending PCM audio to USB Host (Microphone path)
        g_status.usb_tx_packets++;
        if (g_status.mic_streaming) {
            uac1_queue_next_mic_packet(rhport);
        }
        return true;
    }

    return false;
}

//--------------------------------------------------------------------+
// Register Custom UAC 1.0 Driver with TinyUSB USBD
//--------------------------------------------------------------------+
static const usbd_class_driver_t uac1_driver = {
#if CFG_TUSB_DEBUG >= 2
    .name             = "UAC1_HEADSET",
#else
    .name             = NULL,
#endif
    .init             = uac1_init,
    .deinit           = uac1_deinit,
    .reset            = uac1_reset,
    .open             = uac1_open,
    .control_xfer_cb  = uac1_control_xfer_cb,
    .xfer_cb          = uac1_xfer_cb,
    .sof              = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &uac1_driver;
}
