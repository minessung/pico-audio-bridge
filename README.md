# Pico Audio Bridge (`pico-audio-bridge`)

A **Full-Duplex USB Audio Class 1.0 (UAC 1.0) ↔ I2S Digital Audio Bridge** built on two **Raspberry Pi Pico WH (RP2040)** microcontrollers. It enables bit-transparent, low-latency bidirectional digital PCM audio streaming between two independent USB hosts (such as an **Android smartphone** and a **Mac/PC**) without any intermediate DAC/ADC conversion.

---

## Project Overview

- **Lossless Full-Duplex Digital Audio Bridge**: Transfers 2-channel stereo `16-bit Signed PCM` audio at `48,000 Hz` simultaneously in both directions (`Host 1 <-> Pico #1 <-> I2S <-> Pico #2 <-> Host 2`).
- **Unified Single-Binary Firmware**: Both boards run the exact same firmware binary (`pico_uac_i2s_bridge.uf2`). The runtime role (**I2S Master** vs **I2S Slave**) and USB Product ID (`0x4001` vs `0x4002`) are automatically selected at boot via `GPIO 14`:
  - `GPIO 14` **OPEN** (internal pull-up HIGH) → **I2S Master** (`PID 0x4001`, generates `BCLK` and `LRCLK`)
  - `GPIO 14` **GND** (strapped LOW) → **I2S Slave** (`PID 0x4002`, synchronizes to external `BCLK` and `LRCLK`)
- **Android Voice Call & Media Ready**: Declares UAC 1.0 Bidirectional Headset terminals (`0x0402`), allowing Android smartphones and desktop operating systems (macOS, Linux, Windows) to natively route both media playback and cellular/VoIP voice call audio without custom drivers.
- **PIO Frame-Synchronized I2S Engine**: Custom RP2040 PIO state machines (`i2s_master_duplex` & `i2s_slave_duplex`) with ping-pong DMA transfers and per-frame `LRCLK` edge synchronization prevent bit-shift noise across hot-plugs or clock restarts.
- **SRAM-Resident Critical Path & Clock Drift Compensation**: All USB Isochronous ISR callbacks and ring buffer operations execute from SRAM (`__not_in_flash_func`) with rate-limited linear interpolation (`±325 ppm` max adjustment) to absorb asynchronous clock drift between two independent USB hosts and the RP2040 crystal oscillator.

---

## System Architecture & Wiring

```text
[Android Phone]                                             [Mac / PC]
  (USB Host 1)                                             (USB Host 2)
       │                                                        │
       ▼ USB UAC 1.0 (Headset, PID=0x4001)                      ▼ USB UAC 1.0 (Headset, PID=0x4002)
┌──────────────┐            Full-Duplex I2S              ┌──────────────┐
│  Pico WH #1  │ ────── GPIO 10 (BCLK, 1.536 MHz) ─────► │  Pico WH #2  │
│ (I2S Master) │ ────── GPIO 11 (LRCLK / WS, 48 kHz) ──► │ (I2S Slave)  │
│              │ ────── GPIO 12 (DOUT) ───► GPIO 13 ───► │   (DIN)      │
│   (DIN)      │ ◄───── GPIO 13 ◄────────── GPIO 12 ──── │   (DOUT)     │
│              │ ────── GND ──────────────────────────── │              │
│ GPIO 14:OPEN │                                         │ GPIO 14:GND  │
└──────────────┘                                         └──────────────┘
```

### Pinout Table (Pico WH #1 ↔ Pico WH #2)

| Signal | Pico WH #1 (I2S Master) | Pico WH #2 (I2S Slave) | Notes |
| :--- | :--- | :--- | :--- |
| **Role Select** | `GPIO 14` (Pin 19) **OPEN** | `GPIO 14` (Pin 19) **→ `GND`** | Internal pull-up (`OPEN` = Master, `GND` = Slave) |
| **I2S BCLK** | `GPIO 10` (Pin 14) | `GPIO 10` (Pin 14) | Direct 1:1 connection (`1.536 MHz` Bit Clock) |
| **I2S LRCLK (WS)** | `GPIO 11` (Pin 15) | `GPIO 11` (Pin 15) | Direct 1:1 connection (`48 kHz` Word Select) |
| **Master → Slave Audio** | `GPIO 12` (Pin 16, `DOUT`) | `GPIO 13` (Pin 17, `DIN`) | **Cross-connected** (`Master DOUT` → `Slave DIN`) |
| **Slave → Master Audio** | `GPIO 13` (Pin 17, `DIN`) | `GPIO 12` (Pin 16, `DOUT`) | **Cross-connected** (`Slave DOUT` → `Master DIN`) |
| **Ground** | `GND` (Pin 18, 23, 38, etc.) | `GND` (Pin 18, 23, 38, etc.) | Common ground required |

### Debug Probe Connection (Optional)
- **SWD**: Connect `SWCLK`, `SWDIO`, and `GND` from the Raspberry Pi Debug Probe to the Pico 3-pin `DEBUG` header.
- **UART0 (`115200 8N1`)**:
  - Pico `GPIO 0` (Pin 1, `UART0 TX`) → Debug Probe `RX`
  - Pico `GPIO 1` (Pin 2, `UART0 RX`) → Debug Probe `TX`
  - Pico `GND` (Pin 3) → Debug Probe `GND`

---

## Repository Structure

- `src/bridge_config.h`: Hardware pin mappings, audio format constants (`48 kHz`, `16-bit`, `2-ch`), and ring buffer sizing.
- `src/usb_descriptors.c` / `.h`: UAC 1.0 Audio Control & Isochronous Audio Streaming descriptors (`EP 0x01 OUT`, `EP 0x82 IN`).
- `src/uac1_device.c` / `.h`: Custom TinyUSB UAC 1.0 class driver supporting simultaneous full-duplex Speaker + Microphone streaming.
- `src/i2s_bridge.pio` / `.c` / `.h`: RP2040 PIO state machines and ping-pong DMA engine for full-duplex I2S Master and Slave modes.
- `src/audio_ring.c` / `.h`: Lock-free SPSC ring buffer with rate-limited adaptive linear interpolation for clock drift compensation.
- `src/main.c`: Role detection, initialization, and real-time SRAM main loop.
- `tools/e2e_audio_test.c`: macOS CoreAudio end-to-end full-duplex sine-wave verification utility.
- `tools/monitor_slave_to_smsl.c`: Real-time low-latency macOS CoreAudio monitor routing bridge audio to an external USB DAC.

---

## Build & Flash Instructions

### 1. Build Firmware
```bash
./build.sh
```
Generates `build/pico_uac_i2s_bridge.uf2`, `build/pico_uac_i2s_bridge.elf`, and `build/pico_uac_i2s_bridge.bin`.

### 2. Flash Firmware
- **Via Raspberry Pi Debug Probe (OpenOCD / SWD)**:
  ```bash
  ./build.sh flash
  ```
- **Via USB BOOTSEL Mode**:
  Hold the `BOOTSEL` button while plugging the Pico into USB, then copy `build/pico_uac_i2s_bridge.uf2` to the mounted `RPI-RP2` volume.
