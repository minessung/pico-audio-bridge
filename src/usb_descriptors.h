#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>
#include <stdbool.h>
#include "bridge_config.h"

//--------------------------------------------------------------------+
// Interface Numbers
//--------------------------------------------------------------------+
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_SPK, // USB Host -> Pico (Speaker / RX)
    ITF_NUM_AUDIO_STREAMING_MIC, // Pico -> USB Host (Microphone / TX)
    ITF_NUM_TOTAL
};

//--------------------------------------------------------------------+
// Endpoint Addresses
//--------------------------------------------------------------------+
#define EPNUM_AUDIO_OUT             0x01 // Isochronous OUT (Host -> Pico)
#define EPNUM_AUDIO_IN              0x82 // Isochronous IN  (Pico -> Host)

//--------------------------------------------------------------------+
// UAC 1.0 Entity IDs (Audio Control Topology)
//--------------------------------------------------------------------+
#define UAC1_ENTITY_SPK_INPUT_TERMINAL      0x01 // USB Streaming IN (from Host)
#define UAC1_ENTITY_SPK_FEATURE_UNIT        0x02 // Speaker Mute / Volume
#define UAC1_ENTITY_SPK_OUTPUT_TERMINAL     0x03 // Headset Speaker Out (0x0402)

#define UAC1_ENTITY_MIC_INPUT_TERMINAL      0x04 // Headset Microphone In (0x0402)
#define UAC1_ENTITY_MIC_FEATURE_UNIT        0x05 // Microphone Mute / Volume
#define UAC1_ENTITY_MIC_OUTPUT_TERMINAL     0x06 // USB Streaming OUT (to Host)

//--------------------------------------------------------------------+
// UAC 1.0 Class-Specific Constants
//--------------------------------------------------------------------+
#define AUDIO_SUBCLASS_CONTROL              0x01
#define AUDIO_SUBCLASS_STREAMING            0x02

// Audio Class-Specific Request Codes (UAC 1.0 Spec A.9)
#define UAC1_REQ_SET_CUR                    0x01
#define UAC1_REQ_SET_MIN                    0x02
#define UAC1_REQ_SET_MAX                    0x03
#define UAC1_REQ_SET_RES                    0x04
#define UAC1_REQ_GET_CUR                    0x81
#define UAC1_REQ_GET_MIN                    0x82
#define UAC1_REQ_GET_MAX                    0x83
#define UAC1_REQ_GET_RES                    0x84

// Feature Unit Control Selectors (UAC 1.0 Spec A.10.2)
#define UAC1_FU_MUTE_CONTROL                0x01
#define UAC1_FU_VOLUME_CONTROL              0x02

// Endpoint Control Selectors (UAC 1.0 Spec A.10.4)
#define UAC1_EP_SAMPLING_FREQ_CONTROL       0x01

// Set role before USB initialization so USB Product ID and Product String
// clearly identify Master (Android side) vs Slave (PC/Mac side)
void usb_descriptors_set_role(bool is_master);

#endif /* USB_DESCRIPTORS_H_ */
