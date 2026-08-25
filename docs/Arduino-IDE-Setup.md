# Arduino IDE Setup Guide

## Required Board Libraries

Install these via Arduino IDE → Board Manager:

- **ESP32** by Espressif Systems
- **STM32 MCU based boards** by STMicroelectronics

---

## STM32 Settings (F411CEU6)

**Tools menu:**

| Setting | Value |
|---------|-------|
| Board | STM32 MCU based boards → Generic STM32F4 Series |
| Board part number | Generic F411CEUx |
| Upload method | STM32CubeProgrammer (DFU) or (SWD) |
| U(S)ART support | Enabled (generic Serial) |
| USB support | CDC (generic Serial supersede U(S)ART) |
| Optimize | Fastest (-O3) |
| Port | Whichever COM/dev port appears when connected |

**Entering DFU mode (USB upload, no ST-Link):**

1. Hold BOOT0 button
2. Press and release RESET
3. Release BOOT0
4. Upload from IDE

> If using an ST-Link, no button sequence needed — connect and upload via SWD.

---

## ESP32 Settings (NodeMCU-32S)

**Tools menu:**

| Setting | Value |
|---------|-------|
| Board | ESP32 Arduino → ESP32 Dev Module |
| Upload Speed | 921600 |
| CPU Frequency | 240MHz (WiFi/BT) |
| Flash Frequency | 80MHz |
| Flash Mode | QIO |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Default 4MB with spiffs |
| Core Debug Level | None |
| PSRAM | Disabled |
| Port | Whichever COM/dev port appears when connected |

> NodeMCU-32S has auto-reset built in — no button sequence needed, just hit Upload.