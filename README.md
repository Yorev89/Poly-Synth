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
- Band-limited wavetables (one per octave, 2048 samples): alias-free high notes
- Sub oscillator: Sine or Square, selectable at -1 or -2 octaves
- White noise generator with adjustable level
- Ring modulator — continuously blendable from additive mix to full ring modulation
- Osc2 independent level and detune control (0–100 cents)
- Portamento (legato glide) with adjustable time (0–500ms), triggers on legato playing only
- Pitch bend with adjustable range

### Envelopes
- Volume ADSR with analog-style exponential curves; attack, decay and release
  1 ms–3 s each (exponential control scale; release/decay time = time to fall 60 dB)
- Filter ADSR envelope (same curves and ranges) with adjustable amount, on/off

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
- 16 factory presets: Init/Default, Fat Bass, TB-303 Acid, Bell Chime, Synth Lead,
  Analog Pad, Robot Voice, String Ensemble, Metallic Pad, Pluck Bass, Wobble Bass,
  Sitar, Ambient Wash, Glide Bass, 808 Bass, Piano (tuned for MIDI Play)
- 12 user-saveable preset slots (stored in ESP32 non-volatile memory)
- Presets store every setting, including resonance, delay time/feedback, ping-pong
  and a per-preset output level; factory preset levels can be adjusted and saved
  from the web page (changing a level requires the volume passcode)

### Connectivity & Control
- WiFi access point — no app required, connect directly from any browser
- 4-tab web interface: Sound, Modulation, FX, Presets; the page always shows the
  synth's current settings (opening or reloading it never changes the sound)
- BLE MIDI keyboard interface — auto-discovers BLE MIDI devices, with preferred
  device priority and automatic fallback to any BLE MIDI device
- Web-settable volume ceiling for venue or parental control, protected by a
  4-digit passcode enforced by the web controller
- MIDI keyboard volume (CC7) respects the web-set ceiling
- **MIDI Play:** a laptop app plays standard MIDI files on the synth over the web
  controller's USB port (no Wi-Fi, ~1 ms timing), with sustain-pedal emulation.
  See [`MIDI-Play/`](MIDI-Play/README.md)

### Protection
- Subsonic protection: 30 Hz output high-pass, and the sub-oscillator fades out
  below 45 Hz, so very low notes can't overdrive amplifiers and speakers

## Firmware

| File                       | Board     | Also needs (same folder)                 |
|----------------------------|-----------|------------------------------------------|
| `Firmware/STM32/stm32_Synth_Engine.ino`   | STM32F411 | `wavetables.h` (generated oscillator tables) |
| `Firmware/ESP32-Web/esp32_WEB_Controller.ino` | ESP32 #1  | `secrets.h` (your own, see below)        |
| `Firmware/ESP32-BLE/esp32_BLE.ino`            | ESP32 #2  | —                                        |
| `MIDI-Play/midiplay.py`    | Laptop (Python 3) | `pip install mido pyserial`      |

The three firmware files are built with the Arduino IDE.

### Diagnostics (STM32 serial monitor, 115200 baud, line ending "Newline")
- `stats` — toggles a once-per-second load report: worst-case render time against
  the 2.9 ms deadline, late buffers, active voices
- `clock` — prints the clock configuration and the actual sample rate

### Wi-Fi password and passcode (`secrets.h`)
The web controller reads its Wi-Fi network name, Wi-Fi password and default
volume passcode from `secrets.h`, which is deliberately **not** in this
repository. Copy `secrets.example.h` to `secrets.h` in the same folder and set
your own values (Wi-Fi password at least 8 characters, passcode 4 digits).
Without `secrets.h` the sketch still builds using the public defaults, and
warns you at compile time and on the serial monitor.

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
ping-pong delay, a resonant filter, alias-free oscillators, analog-style
envelopes and rebuilt presets. MIDI Play (phase 1) plays MIDI files from a
laptop. Next: hardware upgrades (first candidate: the 5-pin DIN MIDI input).
