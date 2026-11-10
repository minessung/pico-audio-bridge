# Raspberry Pi Pico WH Full-Duplex UAC 1.0 ↔ I2S Audio Bridge

두 대의 **Raspberry Pi Pico WH (RP2040)** 를 사용하여 Android 스마트폰과 PC/Mac 사이에서 아날로그 변환(DAC/ADC) 손실 없이 **Full-Duplex 디지털 PCM 오디오 스트림**을 주고받는 USB Audio Class 1.0 (UAC 1.0) 브릿지 펌웨어입니다.

- **단일 펌웨어(Single Firmware)**: `GPIO 14` 핀의 상태(Open vs GND)에 따라 부팅 시 자동으로 **I2S Master (Android 측)** 또는 **I2S Slave (PC/Mac 측)** 로 동작합니다.
- **Android 음성 통화 지원**: USB Audio Control 디스크립터에서 터미널 타입을 **Bidirectional Headset (`0x0402`)** 으로 선언하여 Android 연결 시 외장 통화용 헤드셋(스피커 + 마이크)으로 즉시 인식됩니다.
- **오디오 포맷**: `48,000 Hz` / `16-bit Signed PCM` / `2-ch Stereo` (양방향 동시 전송)
- **클럭 드리프트 보정**: USB Host SOF 클럭과 I2S 클럭 간의 미세한 주파수 편차를 Ring Buffer 수위 기반 적응형 선형 보간(Adaptive Linear Interpolation)으로 실시간 보정합니다.

---

## 1. 하드웨어 배선 (Wiring Guide)

```
[Android Phone]                                             [PC / Mac]
  (USB Host 1)                                             (USB Host 2)
       │                                                        │
       ▼ USB UAC 1.0 (Headset, PID=0x4001)                      ▼ USB UAC 1.0 (Headset, PID=0x4002)
┌──────────────┐            Full-Duplex I2S              ┌──────────────┐
│  Pico WH #1  │ ────── GPIO 10 (BCLK) ────────────────► │  Pico WH #2  │
│ (I2S Master) │ ────── GPIO 11 (LRCLK / WS) ──────────► │ (I2S Slave)  │
│              │ ────── GPIO 12 (DOUT) ───► GPIO 13 ───► │   (DIN)      │
│   (DIN)      │ ◄───── GPIO 13 ◄────────── GPIO 12 ──── │   (DOUT)     │
│              │ ────── GND ──────────────────────────── │              │
│ GPIO 14:OPEN │                                         │ GPIO 14:GND  │
└──────────────┘                                         └──────────────┘
```

### 핀 연결표 (Pico WH #1 ↔ Pico WH #2)

| 신호 기능 | Pico WH #1 (Android 측, Master) | Pico WH #2 (PC/Mac 측, Slave) | 비고 |
| :--- | :--- | :--- | :--- |
| **Role Select** | `GPIO 14` (Pin 19) **연결 안 함 (Open)** | `GPIO 14` (Pin 19) **→ `GND` 연결** | 내부 풀업 사용 (Open=Master, GND=Slave) |
| **I2S BCLK** | `GPIO 10` (Pin 14) | `GPIO 10` (Pin 14) | 1:1 연결 (`1.536 MHz` Bit Clock) |
| **I2S LRCLK (WS)** | `GPIO 11` (Pin 15) | `GPIO 11` (Pin 15) | 1:1 연결 (`48 kHz` Word Select) |
| **Phone → PC 오디오** | `GPIO 12` (Pin 16, `DOUT`) | `GPIO 13` (Pin 17, `DIN`) | **교차 연결** (Master DOUT → Slave DIN) |
| **PC → Phone 오디오** | `GPIO 13` (Pin 17, `DIN`) | `GPIO 12` (Pin 16, `DOUT`) | **교차 연결** (Slave DOUT → Master DIN) |
| **GND** | `GND` (Pin 18, 23, 38 등) | `GND` (Pin 18, 23, 38 등) | 공통 접지 필수 |

### Pico Debug Probe 연결
- **SWD (3핀 디버그 헤더)**: Debug Probe의 `SWCLK`, `SWDIO`, `GND`를 대상 Pico WH의 `DEBUG` 3핀 헤더에 연결합니다.
- **UART (시리얼 로그 출력, `115200 8N1`)**:
  - Pico `GPIO 0` (Pin 1, `UART0 TX`) → Debug Probe `RX`
  - Pico `GPIO 1` (Pin 2, `UART0 RX`) → Debug Probe `TX`
  - Pico `GND` (Pin 3) → Debug Probe `GND`

---

## 2. 빌드 및 플래싱 방법

### 빌드 실행
```bash
./build.sh
```
빌드가 완료되면 `build/pico_uac_i2s_bridge.uf2` 및 `build/pico_uac_i2s_bridge.elf` 파일이 생성됩니다.

### 펌웨어 업로드
1. **Pico Debug Probe (SWD / OpenOCD) 사용 시**:
   ```bash
   ./build.sh flash
   ```
2. **BOOTSEL (UF2 드래그 앤 드롭) 사용 시**:
   - Pico WH의 `BOOTSEL` 버튼을 누른 채 USB를 연결한 뒤 `build/pico_uac_i2s_bridge.uf2` 파일을 `RPI-RP2` 드라이브에 복사합니다. (두 대의 Pico WH 모두 동일한 `.uf2` 파일을 넣으면 됩니다.)

### Debug Probe 시리얼 모니터 확인
```bash
screen /dev/tty.usbmodem* 115200
```
부팅 시 현재 보드가 `MASTER`인지 `SLAVE`인지 표시되며, 매 1초마다 USB 마운트 상태, 스피커/마이크 패킷 카운트, 링버퍼 수위, I2S DMA 블록 카운트가 출력됩니다.
