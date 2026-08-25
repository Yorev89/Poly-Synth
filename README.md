# Poly-Synth — DIY Polyphonic Synthesizer
**Live site:** [yorev89.github.io/Poly-Synth](https://yorev89.github.io/Poly-Synth)

A homebrew polyphonic synthesizer built from scratch, based on a 
three-MCU architecture. This repository contains all firmware files 
and hardware resources.

## Architecture

| MCU       | Role                                        |
|-----------|---------------------------------------------|
| STM32F411 | Synthesis engine, I2S audio, 5-pin DIN MIDI |
| ESP32 #1  | Web UI, WiFi access point                   |
| ESP32 #2  | BLE MIDI keyboard interface                 |

## Features

### Synthesis Engine
- 8-voice polyphony with adaptive voice scaling
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
- Global low-pass filter, cutoff range 20Hz–18kHz
- LFO modulation of filter cutoff

### LFO System
- Two independent LFOs
- Rate: 0.1–20 Hz each
- Waveforms: Sine, Triangle, Square, Sawtooth Up, Sawtooth Down
- Modulation targets per LFO: pitch (vibrato), filter cutoff, amplitude (tremolo), Osc2 detune

### Effects
- Stereo delay with time, feedback, and mix controls
- 5 delay presets: Off, Slapback, Short, Medium, Rhythmic
- Stereo chorus (4-tap)

### Presets
- 15 factory presets: Init/Default, Fat Bass, TB-303 Acid, Bell Chime, Synth Lead,
  Analog Pad, Robot Voice, String Ensemble, Metallic Pad, Pluck Bass, Wobble Bass,
  Sitar, Ambient Wash, Glide Bass, and one additional preset
- 12 user-saveable preset slots (stored in ESP32 non-volatile memory)

### Connectivity & Control
- WiFi access point — no app required, connect directly from any browser
- 4-tab web interface: Sound, Modulation, FX, Presets
- BLE MIDI keyboard interface — auto-discovers BLE MIDI devices, with preferred
  device priority and automatic fallback to any BLE MIDI device
- Web-settable volume ceiling for venue or parental control
- MIDI keyboard volume (CC7) respects the web-set ceiling

## Status
✅ Hardware complete. Firmware verified on hardware — August 2026. First stable release: v1.0
