# Poli-Synth — DIY Polyphonic Synthesizer

A homebrew polyphonic synthesizer built from scratch, based on a 
three-MCU architecture. This repository contains all firmware files 
and hardware resources.

## Architecture

| MCU       | Role                                        |
|-----------|---------------------------------------------|
| STM32F411 | Synthesis engine, I2S audio, 5-pin DIN MIDI |
| ESP32 #1  | Web UI, WiFi access point                   |
| ESP32 #2  | BLE MIDI keyboard interface                 |
|-----------|---------------------------------------------|

## Features
- 8-voice polyphony
- Dual oscillators per voice + sub and noise oscillators
- 2 LFOs, amplitude and filter ADSRs
- Effects: chorus, delay, ring modulator
- 16 factory presets + 10 user presets
- Web-based UI for real-time control
- 3D printed housing

## Firmware Files
- `Firmware/STM32/stm32_volume_ceiling.ino`
- `Firmware/ESP32-Web/esp32_3_keyboard.ino`
- `Firmware/ESP32-BLE/esp32_1_ble_improved.ino`

> ⚠️ These firmware versions are believed final but are pending 
> hardware verification. A stable release tag will be added once 
> confirmed.

## Status
Work in progress. Hardware complete. Firmware verification 
scheduled for August 2026.
