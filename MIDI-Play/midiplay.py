#!/usr/bin/env python3
"""
MIDI Play for the Poly-Synth - phase 1 proof of concept
https://github.com/Yorev89/Poly-Synth

Plays a standard MIDI file (.mid) on the Poly-Synth. The laptop is connected
by USB to the web controller (ESP32 #1), which relays the notes to the STM32
synthesis engine over its serial link - no Wi-Fi, so the timing stays tight.

Usage examples:
    python midiplay.py song.mid                 # find the synth automatically
    python midiplay.py song.mid --port COM5     # or name the port
    python midiplay.py song.mid --tempo 0.8     # play at 80% speed
    python midiplay.py song.mid --transpose -12 # one octave down
    python midiplay.py --list                   # show serial ports
    python midiplay.py song.mid --info          # describe the file, don't play

Stop playing at any time with Ctrl+C: all notes are released.

Requires: pip install mido pyserial
"""

import argparse
import sys
import time

try:
    import mido
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("Missing libraries. Install them with:  pip install mido pyserial")

BAUD = 115200
PING_REPLY = b"POLYSYNTH_MIDI_OK"
DRUM_CHANNEL = 9          # MIDI channel 10 (counted from 0): percussion


# ---------------------------------------------------------------- timing ----

class PreciseClock:
    """Waits until a given moment with ~1 ms accuracy.

    Windows normally wakes a sleeping program only every ~15.6 ms, which
    would make the rhythm uneven. We ask Windows for a 1 ms timer, sleep
    until shortly before each note and then wait out the last moment exactly.
    """

    def __init__(self):
        self._winmm = None
        if sys.platform == "win32":
            try:
                import ctypes
                self._winmm = ctypes.WinDLL("winmm")
                self._winmm.timeBeginPeriod(1)
            except OSError:
                self._winmm = None
        self.start = time.perf_counter()

    def now(self):
        return time.perf_counter() - self.start

    def wait_until(self, t):
        while True:
            remaining = t - self.now()
            if remaining <= 0:
                return
            if remaining > 0.003:
                time.sleep(remaining - 0.002)   # coarse sleep
            # last ~2 ms: busy-wait for precision

    def close(self):
        if self._winmm:
            self._winmm.timeEndPeriod(1)


# ------------------------------------------------------------ serial link ---

def open_port(name):
    """Opens a serial port without resetting the ESP32.

    Many ESP32 boards reset when a program opens their port (the DTR/RTS
    lines drive the auto-reset circuit). Keeping both lines inactive avoids
    that on most boards.
    """
    ser = serial.Serial()
    ser.port = name
    ser.baudrate = BAUD
    ser.timeout = 0.1
    ser.write_timeout = 1.0
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def ping(ser, wait=3.0):
    """Asks the web controller to identify itself. Allows time for a reboot
    in case opening the port reset the board anyway. Any port error (for
    example a Bluetooth serial port that never accepts data) counts as
    'no answer' instead of stopping the program."""
    deadline = time.time() + wait
    try:
        ser.reset_input_buffer()
        while time.time() < deadline:
            ser.write(b"PING\n")
            t_end = time.time() + 0.4
            while time.time() < t_end:
                line = ser.readline()
                if PING_REPLY in line:
                    return True
    except (serial.SerialException, OSError):
        return False
    return False


def is_bluetooth(port):
    text = f"{port.description} {port.hwid}".lower()
    return "bluetooth" in text or "bthenum" in text


def find_synth():
    """Tries the serial ports and returns the one where the synth answers.

    Bluetooth serial ports are skipped (they hang when written to); USB
    ports are tried first.
    """
    ports = [p for p in serial.tools.list_ports.comports() if not is_bluetooth(p)]
    ports.sort(key=lambda p: 0 if "usb" in f"{p.description} {p.hwid}".lower() else 1)
    for p in ports:
        print(f"  trying {p.device} ({p.description}) ...", end=" ", flush=True)
        try:
            ser = open_port(p.device)
        except (serial.SerialException, OSError):
            print("busy or not usable")   # e.g. the Arduino serial monitor has it
            continue
        if ping(ser):
            print("synth found")
            return ser
        print("no answer")
        try:
            ser.close()
        except (serial.SerialException, OSError):
            pass
    return None


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
    for p in ports:
        print(f"  {p.device:10s} {p.description}")


# --------------------------------------------------------------- the file ---

def load_events(path, transpose, include_drums, channels):
    """Reads a MIDI file and returns (time_in_seconds, message) pairs.

    mido merges all tracks and applies every tempo change for us.
    """
    mid = mido.MidiFile(path)
    events = []
    t = 0.0
    for msg in mid:                       # msg.time = seconds since previous message
        t += msg.time
        if msg.is_meta or not hasattr(msg, "channel"):
            continue
        if msg.channel == DRUM_CHANNEL and not include_drums:
            continue
        if channels and (msg.channel + 1) not in channels:
            continue
        if msg.type in ("note_on", "note_off"):
            n = msg.note + transpose
            if not 0 <= n <= 127:
                continue
            msg = msg.copy(note=n)
            events.append((t, msg))
        elif msg.type == "pitchwheel":
            events.append((t, msg))
        elif msg.type == "control_change" and msg.control == 64:
            events.append((t, msg))       # sustain pedal: handled by the player
    return mid, events


def describe(mid, events, path):
    notes = [m for _, m in events if m.type == "note_on" and m.velocity > 0]
    chans = sorted({m.channel + 1 for _, m in events})
    pedal = sum(1 for _, m in events if m.type == "control_change")
    length = events[-1][0] if events else 0
    lo = min((m.note for m in notes), default=0)
    hi = max((m.note for m in notes), default=0)
    print(f"File:      {path}")
    print(f"Length:    {int(length // 60)}:{int(length % 60):02d}")
    print(f"Tracks:    {len(mid.tracks)}   channels used: {chans}")
    print(f"Notes:     {len(notes)}   range: {lo}-{hi}")
    print(f"Pedal:     {'yes (' + str(pedal) + ' events)' if pedal else 'no'}")


# ----------------------------------------------------------------- player ---

class Player:
    """Turns MIDI messages into Poly-Synth commands.

    The synth has no sustain pedal yet, so the pedal is emulated here: while
    it is down, note-offs are held back and sent when it is released.
    """

    def __init__(self, ser, use_pedal, velocity_scale):
        self.ser = ser
        self.use_pedal = use_pedal
        self.velocity_scale = velocity_scale
        self.sounding = set()     # notes the synth is currently playing
        self.held_by_pedal = set()
        self.pedal_down = False
        self.out = []

    def _send(self, text):
        self.out.append(text)

    def flush(self):
        if self.out:
            self.ser.write(("\n".join(self.out) + "\n").encode("ascii"))
            self.out.clear()

    def handle(self, msg):
        if msg.type == "note_on" and msg.velocity > 0:
            n = msg.note
            if n in self.sounding:            # repeated key: restart the note
                self._send(f"NOTEOFF,{n}")
            self.held_by_pedal.discard(n)
            vel = max(1, min(127, round(msg.velocity * self.velocity_scale)))
            self._send(f"NOTE,{n},{vel}")
            self.sounding.add(n)
        elif msg.type in ("note_off", "note_on"):   # note_on with velocity 0 = off
            n = msg.note
            if n not in self.sounding:
                return
            if self.use_pedal and self.pedal_down:
                self.held_by_pedal.add(n)
            else:
                self._send(f"NOTEOFF,{n}")
                self.sounding.discard(n)
        elif msg.type == "control_change" and msg.control == 64 and self.use_pedal:
            down = msg.value >= 64
            if self.pedal_down and not down:
                for n in sorted(self.held_by_pedal):
                    self._send(f"NOTEOFF,{n}")
                    self.sounding.discard(n)
                self.held_by_pedal.clear()
            self.pedal_down = down
        elif msg.type == "pitchwheel":
            v = msg.pitch + 8192                  # 0..16383
            self._send(f"PITCHBEND,{v & 0x7F},{v >> 7}")

    def all_off(self):
        for n in sorted(self.sounding):
            self._send(f"NOTEOFF,{n}")
        self.sounding.clear()
        self.held_by_pedal.clear()
        self._send("PITCHBEND,0,64")              # bend back to centre
        self.flush()


def play(ser, events, tempo, use_pedal, velocity_scale, start_at):
    player = Player(ser, use_pedal, velocity_scale)
    clock = PreciseClock()
    total = events[-1][0] / tempo if events else 0
    events = [(t, m) for t, m in events if t >= start_at]
    offset = start_at / tempo
    last_shown = -1
    try:
        i = 0
        while i < len(events):
            t = events[i][0] / tempo - offset
            clock.wait_until(t)
            # send everything due at (almost) the same moment in one write,
            # so a chord leaves the laptop as one burst
            while i < len(events) and events[i][0] / tempo - offset <= t + 0.0005:
                player.handle(events[i][1])
                i += 1
            player.flush()
            pos = int(t + offset)
            if pos != last_shown:
                last_shown = pos
                print(f"\r  {pos // 60}:{pos % 60:02d} / {int(total // 60)}:{int(total % 60):02d}",
                      end="", flush=True)
        time.sleep(0.5)
        print("\nFinished.")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        player.all_off()
        clock.close()


# ------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser(description="Play a MIDI file on the Poly-Synth.")
    ap.add_argument("file", nargs="?", help="MIDI file (.mid) to play")
    ap.add_argument("--port", help="serial port, e.g. COM5 (default: find the synth)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--info", action="store_true", help="describe the file and exit")
    ap.add_argument("--tempo", type=float, default=1.0, help="speed factor (0.5 = half speed)")
    ap.add_argument("--transpose", type=int, default=0, help="semitones up (+) or down (-)")
    ap.add_argument("--velocity", type=float, default=1.0, help="velocity scale (e.g. 1.2 = louder)")
    ap.add_argument("--start", type=float, default=0.0, help="start at this many seconds")
    ap.add_argument("--no-pedal", action="store_true", help="ignore the sustain pedal")
    ap.add_argument("--drums", action="store_true", help="also play channel 10 (drums)")
    ap.add_argument("--channels", type=int, nargs="+", help="play only these MIDI channels (1-16)")
    a = ap.parse_args()

    if a.list:
        list_ports()
        return
    if not a.file:
        ap.error("give a MIDI file to play (or --list)")
    if a.tempo <= 0:
        ap.error("--tempo must be greater than 0")

    mid, events = load_events(a.file, a.transpose, a.drums, a.channels)
    if not events:
        sys.exit("No playable notes found in this file.")
    describe(mid, events, a.file)
    if a.info:
        return

    if a.port:
        ser = open_port(a.port)
        if not ping(ser):
            print(f"Warning: no answer from the synth on {a.port} - playing anyway.")
    else:
        ser = find_synth()
        if ser is None:
            sys.exit("Synth not found. Is the web controller connected by USB, "
                     "and is the Arduino serial monitor closed?  Try --list and --port.")

    print("Playing - press Ctrl+C to stop.")
    try:
        play(ser, events, a.tempo, not a.no_pedal, a.velocity, a.start)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
