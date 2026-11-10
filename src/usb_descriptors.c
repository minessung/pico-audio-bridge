#include "usb_descriptors.h"
#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>

static bool g_is_master = true;

void usb_descriptors_set_role(bool is_master) {
    g_is_master = is_master;
}

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00, // Defined at Interface level for UAC 1.0
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = 0x2E8A, // Raspberry Pi
    .idProduct          = 0x4001, // 0x4001 = Master (Phone), 0x4002 = Slave (PC/Mac)
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    desc_device.idProduct = g_is_master ? 0x4001 : 0x4002;
    return (uint8_t const *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor (UAC 1.0 Full-Duplex Headset)
//--------------------------------------------------------------------+
// Total length calculation:
// - Config Header: 9 bytes
// - Standard AC Interface: 9 bytes
// - Class-Specific AC Interface:
//     Header (2 AS interfaces): 10 bytes
//     Speaker Input Terminal (IT 1): 12 bytes
//     Speaker Feature Unit (FU 2, Stereo): 10 bytes
//     Speaker Output Terminal (OT 3): 9 bytes
//     Mic Input Terminal (IT 4): 12 bytes
//     Mic Feature Unit (FU 5, Stereo): 10 bytes
//     Mic Output Terminal (OT 6): 9 bytes
//     -> CS AC Total = 10 + 12 + 10 + 9 + 12 + 10 + 9 = 72 bytes
// - Speaker AS Interface Alt 0 (Zero-Bandwidth): 9 bytes
// - Speaker AS Interface Alt 1 (Operational):
//     Std AS Interface: 9 bytes
//     CS AS General: 7 bytes
//     CS AS Format Type I (1 freq = 48kHz): 11 bytes
//     Std Iso Endpoint (9-byte UAC 1.0 format): 9 bytes
//     CS Iso Endpoint: 7 bytes
//     -> Speaker Alt 1 Total = 9 + 7 + 11 + 9 + 7 = 43 bytes
// - Mic AS Interface Alt 0 (Zero-Bandwidth): 9 bytes
// - Mic AS Interface Alt 1 (Operational):
//     Std AS Interface: 9 bytes
//     CS AS General: 7 bytes
//     CS AS Format Type I (1 freq = 48kHz): 11 bytes
//     Std Iso Endpoint (9-byte UAC 1.0 format): 9 bytes
//     CS Iso Endpoint: 7 bytes
//     -> Mic Alt 1 Total = 9 + 7 + 11 + 9 + 7 = 43 bytes
//
// Grand Total = 9 + 9 + 72 + (9 + 43) + (9 + 43) = 194 bytes.

#define UAC1_CS_AC_TOTAL_LEN        (10 + 12 + 10 + 9 + 12 + 10 + 9)
#define CONFIG_TOTAL_LEN            (9 + 9 + UAC1_CS_AC_TOTAL_LEN + 9 + 43 + 9 + 43)

static uint8_t const desc_configuration[] = {
    // 1. Configuration Descriptor (9 bytes)
    9, TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(CONFIG_TOTAL_LEN),
    ITF_NUM_TOTAL,                  // bNumInterfaces (3)
    1,                              // bConfigurationValue
    0,                              // iConfiguration
    TUSB_DESC_CONFIG_ATT_SELF_POWERED,
    TUSB_DESC_CONFIG_POWER_MA(100),

    // 2. Standard Audio Control (AC) Interface Descriptor (9 bytes)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_CONTROL,          // bInterfaceNumber (0)
    0,                              // bAlternateSetting
    0,                              // bNumEndpoints
    TUSB_CLASS_AUDIO,               // bInterfaceClass (0x01)
    AUDIO_SUBCLASS_CONTROL,         // bInterfaceSubClass (0x01)
    0x00,                           // bInterfaceProtocol (UAC 1.0 = 0x00)
    0,                              // iInterface

    // 3. Class-Specific AC Interface Header Descriptor (10 bytes)
    10, TUSB_DESC_CS_INTERFACE, 0x01, // HEADER subtype (0x01)
    U16_TO_U8S_LE(0x0100),          // bcdADC = 1.00 (UAC 1.0)
    U16_TO_U8S_LE(UAC1_CS_AC_TOTAL_LEN),
    2,                              // bInCollection (2 AudioStreaming interfaces)
    ITF_NUM_AUDIO_STREAMING_SPK,    // baInterfaceNr(1) = Interface 1
    ITF_NUM_AUDIO_STREAMING_MIC,    // baInterfaceNr(2) = Interface 2

    // --- SPEAKER PATH (USB Host -> Pico -> I2S DOUT) ---

    // 4. Speaker Input Terminal Descriptor (12 bytes)
    12, TUSB_DESC_CS_INTERFACE, 0x02, // INPUT_TERMINAL subtype (0x02)
    UAC1_ENTITY_SPK_INPUT_TERMINAL, // bTerminalID (1)
    U16_TO_U8S_LE(0x0101),          // wTerminalType: USB Streaming (0x0101)
    0x00,                           // bAssocTerminal
    AUDIO_NUM_CHANNELS,             // bNrChannels (2)
    U16_TO_U8S_LE(0x0003),          // wChannelConfig: Left Front + Right Front (0x0003)
    0,                              // iChannelNames
    0,                              // iTerminal

    // 5. Speaker Feature Unit Descriptor (10 bytes)
    10, TUSB_DESC_CS_INTERFACE, 0x06, // FEATURE_UNIT subtype (0x06)
    UAC1_ENTITY_SPK_FEATURE_UNIT,   // bUnitID (2)
    UAC1_ENTITY_SPK_INPUT_TERMINAL, // bSourceID (1)
    1,                              // bControlSize (1 byte per bmaControls entry)
    0x03,                           // bmaControls(0) Master: Mute (bit 0) + Volume (bit 1)
    0x00,                           // bmaControls(1) Ch 1
    0x00,                           // bmaControls(2) Ch 2
    0,                              // iFeature

    // 6. Speaker Output Terminal Descriptor (9 bytes)
    // Terminal Type 0x0402 = Headset (Bidirectional Terminal), associated with Mic IT (4)
    // This makes Android recognize the device as a Voice Call + Media Headset!
    9, TUSB_DESC_CS_INTERFACE, 0x03, // OUTPUT_TERMINAL subtype (0x03)
    UAC1_ENTITY_SPK_OUTPUT_TERMINAL,// bTerminalID (3)
    U16_TO_U8S_LE(0x0402),          // wTerminalType: Headset (0x0402)
    UAC1_ENTITY_MIC_INPUT_TERMINAL, // bAssocTerminal (4)
    UAC1_ENTITY_SPK_FEATURE_UNIT,   // bSourceID (2)
    0,                              // iTerminal

    // --- MICROPHONE PATH (I2S DIN -> Pico -> USB Host) ---

    // 7. Microphone Input Terminal Descriptor (12 bytes)
    12, TUSB_DESC_CS_INTERFACE, 0x02, // INPUT_TERMINAL subtype (0x02)
    UAC1_ENTITY_MIC_INPUT_TERMINAL, // bTerminalID (4)
    U16_TO_U8S_LE(0x0402),          // wTerminalType: Headset (0x0402)
    UAC1_ENTITY_SPK_OUTPUT_TERMINAL,// bAssocTerminal (3)
    AUDIO_NUM_CHANNELS,             // bNrChannels (2)
    U16_TO_U8S_LE(0x0003),          // wChannelConfig: Left Front + Right Front (0x0003)
    0,                              // iChannelNames
    0,                              // iTerminal

    // 8. Microphone Feature Unit Descriptor (10 bytes)
    10, TUSB_DESC_CS_INTERFACE, 0x06, // FEATURE_UNIT subtype (0x06)
    UAC1_ENTITY_MIC_FEATURE_UNIT,   // bUnitID (5)
    UAC1_ENTITY_MIC_INPUT_TERMINAL, // bSourceID (4)
    1,                              // bControlSize (1 byte)
    0x03,                           // bmaControls(0) Master: Mute + Volume
    0x00,                           // bmaControls(1) Ch 1
    0x00,                           // bmaControls(2) Ch 2
    0,                              // iFeature

    // 9. Microphone Output Terminal Descriptor (9 bytes)
    9, TUSB_DESC_CS_INTERFACE, 0x03, // OUTPUT_TERMINAL subtype (0x03)
    UAC1_ENTITY_MIC_OUTPUT_TERMINAL,// bTerminalID (6)
    U16_TO_U8S_LE(0x0101),          // wTerminalType: USB Streaming (0x0101)
    0x00,                           // bAssocTerminal
    UAC1_ENTITY_MIC_FEATURE_UNIT,   // bSourceID (5)
    0,                              // iTerminal

    // =========================================================================
    // Interface 1: Audio Streaming - Speaker (Host -> Pico, OUT)
    // =========================================================================

    // 10. Speaker AS Interface Alt 0 (Zero-Bandwidth) (9 bytes)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_SPK,    // bInterfaceNumber (1)
    0,                              // bAlternateSetting (0)
    0,                              // bNumEndpoints (0)
    TUSB_CLASS_AUDIO,               // bInterfaceClass
    AUDIO_SUBCLASS_STREAMING,       // bInterfaceSubClass (0x02)
    0x00,                           // bInterfaceProtocol
    0,                              // iInterface

    // 11. Speaker AS Interface Alt 1 (Operational, 48kHz 16-bit Stereo) (9 bytes)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_SPK,    // bInterfaceNumber (1)
    1,                              // bAlternateSetting (1)
    1,                              // bNumEndpoints (1)
    TUSB_CLASS_AUDIO,               // bInterfaceClass
    AUDIO_SUBCLASS_STREAMING,       // bInterfaceSubClass (0x02)
    0x00,                           // bInterfaceProtocol
    0,                              // iInterface

    // 12. Class-Specific AS General Interface Descriptor (7 bytes)
    7, TUSB_DESC_CS_INTERFACE, 0x01, // AS_GENERAL subtype (0x01)
    UAC1_ENTITY_SPK_INPUT_TERMINAL, // bTerminalLink (Terminal 1)
    1,                              // bDelay (1 frame)
    U16_TO_U8S_LE(0x0001),          // wFormatTag: PCM (0x0001)

    // 13. Class-Specific AS Format Type I Descriptor (11 bytes)
    11, TUSB_DESC_CS_INTERFACE, 0x02, // FORMAT_TYPE subtype (0x02)
    0x01,                           // bFormatType: FORMAT_TYPE_I (0x01)
    AUDIO_NUM_CHANNELS,             // bNrChannels (2)
    AUDIO_BYTES_PER_SAMPLE,         // bSubFrameSize (2 bytes)
    AUDIO_BITS_PER_SAMPLE,          // bBitResolution (16 bits)
    1,                              // bSamFreqType (1 discrete frequency)
    (uint8_t)(AUDIO_SAMPLE_RATE & 0xFF),
    (uint8_t)((AUDIO_SAMPLE_RATE >> 8) & 0xFF),
    (uint8_t)((AUDIO_SAMPLE_RATE >> 16) & 0xFF),

    // 14. Standard AS Isochronous Audio Data Endpoint Descriptor (UAC 1.0 = 9 bytes)
    9, TUSB_DESC_ENDPOINT,
    EPNUM_AUDIO_OUT,                // bEndpointAddress (0x01 OUT)
    TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA, // 0x09 (Isochronous, Adaptive)
    U16_TO_U8S_LE(AUDIO_MAX_PACKET_BYTES), // wMaxPacketSize (196 bytes)
    1,                              // bInterval (1 ms)
    0,                              // bRefresh (0)
    0,                              // bSynchAddress (0)

    // 15. Class-Specific AS Isochronous Audio Data Endpoint Descriptor (7 bytes)
    7, TUSB_DESC_CS_ENDPOINT, 0x01, // EP_GENERAL subtype (0x01)
    0x01,                           // bmAttributes: Sampling Frequency control supported (bit 0)
    0x00,                           // bLockDelayUnits
    U16_TO_U8S_LE(0x0000),          // wLockDelay

    // =========================================================================
    // Interface 2: Audio Streaming - Microphone (Pico -> Host, IN)
    // =========================================================================

    // 16. Mic AS Interface Alt 0 (Zero-Bandwidth) (9 bytes)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_MIC,    // bInterfaceNumber (2)
    0,                              // bAlternateSetting (0)
    0,                              // bNumEndpoints (0)
    TUSB_CLASS_AUDIO,               // bInterfaceClass
    AUDIO_SUBCLASS_STREAMING,       // bInterfaceSubClass (0x02)
    0x00,                           // bInterfaceProtocol
    0,                              // iInterface

    // 17. Mic AS Interface Alt 1 (Operational, 48kHz 16-bit Stereo) (9 bytes)
    9, TUSB_DESC_INTERFACE,
    ITF_NUM_AUDIO_STREAMING_MIC,    // bInterfaceNumber (2)
    1,                              // bAlternateSetting (1)
    1,                              // bNumEndpoints (1)
    TUSB_CLASS_AUDIO,               // bInterfaceClass
    AUDIO_SUBCLASS_STREAMING,       // bInterfaceSubClass (0x02)
    0x00,                           // bInterfaceProtocol
    0,                              // iInterface

    // 18. Class-Specific AS General Interface Descriptor (7 bytes)
    7, TUSB_DESC_CS_INTERFACE, 0x01, // AS_GENERAL subtype (0x01)
    UAC1_ENTITY_MIC_OUTPUT_TERMINAL,// bTerminalLink (Terminal 6)
    1,                              // bDelay (1 frame)
    U16_TO_U8S_LE(0x0001),          // wFormatTag: PCM (0x0001)

    // 19. Class-Specific AS Format Type I Descriptor (11 bytes)
    11, TUSB_DESC_CS_INTERFACE, 0x02, // FORMAT_TYPE subtype (0x02)
    0x01,                           // bFormatType: FORMAT_TYPE_I (0x01)
    AUDIO_NUM_CHANNELS,             // bNrChannels (2)
    AUDIO_BYTES_PER_SAMPLE,         // bSubFrameSize (2 bytes)
    AUDIO_BITS_PER_SAMPLE,          // bBitResolution (16 bits)
    1,                              // bSamFreqType (1 discrete frequency)
    (uint8_t)(AUDIO_SAMPLE_RATE & 0xFF),
    (uint8_t)((AUDIO_SAMPLE_RATE >> 8) & 0xFF),
    (uint8_t)((AUDIO_SAMPLE_RATE >> 16) & 0xFF),

    // 20. Standard AS Isochronous Audio Data Endpoint Descriptor (UAC 1.0 = 9 bytes)
    9, TUSB_DESC_ENDPOINT,
    EPNUM_AUDIO_IN,                 // bEndpointAddress (0x82 IN)
    TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_SYNCHRONOUS | TUSB_ISO_EP_ATT_DATA, // 0x0D (Isochronous, Synchronous to SOF)
    U16_TO_U8S_LE(AUDIO_MAX_PACKET_BYTES), // wMaxPacketSize (196 bytes)
    1,                              // bInterval (1 ms)
    0,                              // bRefresh (0)
    0,                              // bSynchAddress (0)

    // 21. Class-Specific AS Isochronous Audio Data Endpoint Descriptor (7 bytes)
    7, TUSB_DESC_CS_ENDPOINT, 0x01, // EP_GENERAL subtype (0x01)
    0x01,                           // bmAttributes: Sampling Frequency control supported (bit 0)
    0x00,                           // bLockDelayUnits
    U16_TO_U8S_LE(0x0000)           // wLockDelay
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "Configuration descriptor length mismatch!");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
static char serial_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count = 0;

    switch (index) {
        case 0:
            memcpy(&_desc_str[1], "\x09\x04", 2);
            chr_count = 1;
            break;

        case 1: {
            const char *mfg = "AudioDSP Bridge";
            chr_count = strlen(mfg);
            for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = mfg[i];
            break;
        }

        case 2: {
            const char *prod = g_is_master
                ? "Pico UAC1 Headset Bridge (Master/Phone)"
                : "Pico UAC1 Headset Bridge (Slave/PC)";
            chr_count = strlen(prod);
            if (chr_count > 32) chr_count = 32;
            for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = prod[i];
            break;
        }

        case 3: {
            pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
            chr_count = strlen(serial_str);
            if (chr_count > 32) chr_count = 32;
            for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = serial_str[i];
            break;
        }

        default:
            return NULL;
    }

    // First byte is length (including header), second byte is string type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
