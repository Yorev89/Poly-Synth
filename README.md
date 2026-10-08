# Poly-Synth — DIY Polyphonic Synthesizer
**Live site:** [yorev89.github.io/Poly-Synth](https://yorev89.github.io/Poly-Synth)

A homebrew polyphonic synthesizer built from scratch, based on a 
three-MCU architecture. This repository contains all firmware files 
and hardware resources.

## Architecture

| MCU       | Role                                        |
|-----------|---------------------------------------------|
| STM32F411 | Synthesis engine, I2S audio (DIN MIDI input wired, currently disabled) |
| ESP32 #1  | Web UI, WiFi access point                   |
| ESP32 #2  | BLE MIDI keyboard interface                 |

## Features

### Synthesis Engine
- 8-voice polyphony with envelope-based voice scaling and click-free voice stealing
- Dual oscillators (Osc1 + Osc2) with 4 waveforms each: Sine, Sawtooth, Square, Triangle
- Sub oscillator: Sine or Square, selectable at -1 or -2 octaves
- White noise generator with adjustable level
- Ring modulator — continuously blendable from additive mix to full ring modulation
- Osc2 independent level and detune control (0–100 cents)
- Portamento (legato glide) with adjustable time (0–500ms), triggers on legato playing only
- Pitch bend with adjustable range

### Envelopes
- Volume ADSR: Attack 1–1000ms, Decay 1–1000ms, Sustain 0–100%, Release 1–3000ms
- Filter ADSR envelope with adjustable amount (0–100%), independently enable/disable

### Filter
- Global resonant low-pass: 2-pole (12 dB/octave) state-variable filter
- Cutoff and resonance controls; filter envelope and LFO modulation
  (sweeps from 60 Hz up to 18 kHz)

### LFO System
- Two independent LFOs
- Rate: 0.1–20 Hz each
- Waveforms: Sine, Triangle, Square, Sawtooth Up, Sawtooth Down
- Modulation targets per LFO: pitch (vibrato), filter cutoff, amplitude (tremolo), Osc2 detune

### Effects
- Stereo output: each voice is panned by pitch (low notes left, high right)
- Stereo delay (10–400ms) with time, feedback, and mix controls
- 4 delay presets (Slapback, Short, Medium, Rhythmic) plus Off; ping-pong mode
- Modulated stereo chorus

### Presets
- 15 factory presets: Init/Default, Fat Bass, TB-303 Acid, Bell Chime, Synth Lead,
  Analog Pad, Robot Voice, String Ensemble, Metallic Pad, Pluck Bass, Wobble Bass,
  Sitar, Ambient Wash, Glide Bass, 808 Bass
- 12 user-saveable preset slots (stored in ESP32 non-volatile memory)

### Connectivity & Control
- WiFi access point — no app required, connect directly from any browser
- 4-tab web interface: Sound, Modulation, FX, Presets
- BLE MIDI keyboard interface — auto-discovers BLE MIDI devices, with preferred
  device priority and automatic fallback to any BLE MIDI device
- Web-settable volume ceiling for venue or parental control, protected by a
  4-digit passcode enforced by the web controller
- MIDI keyboard volume (CC7) respects the web-set ceiling

## Firmware

| File                       | Board     |
|----------------------------|-----------|
| `stm32_Synth_Engine.ino`   | STM32F411 |
| `esp32_WEB_Controller.ino` | ESP32 #1  |
| `esp32_BLE.ino`            | ESP32 #2  |

All three are built with the Arduino IDE.

### Required build settings for the STM32
- Core: STM32duino (STM32 MCU based boards)
- Board: Generic STM32F4 series, **Board part number: BlackPill F411CE**
  (uses the board's 25 MHz crystal for accurate tuning)
- **Tools → Optimize: Fastest (-O3)**. This is required: with the default
  "Smallest (-Os)" the synthesis loop cannot keep up with 8 voices and
  the audio distorts.

## Status
Hardware complete and verified. Firmware is playable and in active
development. Phase 2 bug fixing is complete (tuning, CPU load, clicks,
voice stealing, pitch bend, release tails, BLE MIDI parsing, volume lock);
the freed processor time went into stereo output, a modulated chorus,
ping-pong delay, and a resonant filter. Next: anti-aliased oscillators and
exponential envelope curves.
