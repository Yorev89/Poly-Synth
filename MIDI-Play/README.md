# MIDI Play — play MIDI files on the Poly-Synth (phase 1 PoC)

A small laptop app that plays a standard MIDI file (`.mid`) on the Poly-Synth.
The laptop connects by USB cable to the **web controller (ESP32 #1)**, which
relays the notes to the STM32 over its serial link. No Wi-Fi is involved, so
the timing stays within about a millisecond.

```
Laptop (midiplay.py) --USB--> ESP32 #1 (web controller) --UART--> STM32 synth
```

## Setup (once)
1. Flash the web controller with the current `esp32_WEB_Controller.ino`
   (it contains the USB relay, `relayUsbMidi()`).
2. Install Python 3 and the two libraries:
   `pip install -r requirements.txt`
3. Connect the laptop to the web controller's USB port.
   **Close the Arduino serial monitor** — only one program can use the port.

## Playing
```
python midiplay.py song.mid                 # finds the synth automatically
python midiplay.py song.mid --port COM5     # or name the port
python midiplay.py song.mid --info          # describe the file without playing
python midiplay.py --list                   # list serial ports
```
Options: `--tempo 0.8` (speed), `--transpose -12` (semitones), `--velocity 1.2`
(louder/softer), `--start 30` (start at 30 s), `--no-pedal`, `--drums`
(also play channel 10), `--channels 1 2` (only these channels).
Ctrl+C stops playback and releases all notes.

## What it does
- Merges all tracks and follows every tempo change in the file.
- Sends only playing commands (notes, pitch bend). Volume, filter and other
  controller messages in the file are ignored, so a file can't change the
  synth's settings — the relay on the ESP32 enforces this too.
- **Sustain pedal:** the synth has no pedal input yet, so the app emulates it,
  holding note-offs back while the pedal is down.
- Skips the drum channel (10) unless `--drums` is given.

## Limits
- 8 voices: dense piano music, especially with the pedal, will lose some notes
  to voice stealing.
- If the web controller restarts when the app connects (some ESP32 boards do),
  the app waits for it; the synth returns to the Init preset in that case.
- If the synth isn't found, press the web controller's reset button and run the
  app again. Bluetooth serial ports are skipped automatically.

## Recommended preset
**Piano** (factory preset 16) was tuned by ear for piano MIDI files: Bell Chime
with a 10% sub-oscillator, a short release and no delay. Bell Chime and 808 Bass
also work well, since they decay like a struck string.

## Where to find MIDI files
- [piano-midi.de](http://www.piano-midi.de) — hand-shaped classical piano pieces
- [Mutopia Project](https://www.mutopiaproject.org) — public-domain scores with MIDI
- [MAESTRO](https://magenta.tensorflow.org/datasets/maestro) — recorded competition
  performances (a stress test: very dense)
