/*
 * ESP32 #1 - Wi-Fi Web Controller for the Poly-Synth
 * https://github.com/Yorev89/Poly-Synth
 *
 * Features:
 * - Wi-Fi access point serving the full web interface
 * - 4-screen tabbed interface (Sound, Modulation, FX, Presets)
 * - 15 factory presets + 12 user preset slots (stored in ESP32 flash, Preferences)
 * - On-screen keyboard page
 * - Volume ceiling protected by a 4-digit passcode, enforced by the server
 *   (5-minute unlock window)
 *
 * Hardware connections:
 * - GPIO17 (TX) -> STM32 PA10 (USART1 RX)
 * - GND -> GND (required)
 * - PA10 was the 5-pin DIN MIDI input; the DIN circuit must stay disconnected
 *
 * Wi-Fi:
 * - Access point at IP 192.168.4.1. Network name, password and the default
 *   volume passcode are in secrets.h (not published - see secrets.example.h)
 *
 * UART protocol (one-way, 115200 baud, one command per line):
 * - PARAM,<name>,<value> | CC,<num>,<value> | NOTE,<note>,<velocity> | NOTEOFF,<note>
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// Private settings live in secrets.h, which is NOT uploaded to GitHub.
// To use your own: copy secrets.example.h to secrets.h (same folder) and edit it.
// Without secrets.h the sketch still builds, using the PUBLIC defaults from
// secrets.example.h - with a warning at compile time and on the serial monitor.
#if __has_include("secrets.h")
#include "secrets.h"
#define USING_DEFAULT_SECRETS 0
#else
#include "secrets.example.h"
#define USING_DEFAULT_SECRETS 1
#warning "secrets.h not found: using the PUBLIC default Wi-Fi password and passcode from secrets.example.h. Copy it to secrets.h and set your own."
#endif
static_assert(sizeof(WIFI_PASSWORD) - 1 >= 8, "WIFI_PASSWORD must be at least 8 characters (WPA2 requirement)");
static_assert(sizeof(DEFAULT_VOLUME_PASSCODE) - 1 == 4, "DEFAULT_VOLUME_PASSCODE must be 4 digits");

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// UART Configuration
#define UART_RX_PIN 16  // Not physically connected, but ESP32 needs a valid pin number
#define UART_TX_PIN 17  // GPIO17 → STM32 PA10 (UART1 RX)

#define NUM_USER_PRESETS 12  // Number of user preset slots

WebServer server(80);
Preferences preferences;  // NVS storage object

// Statistics
unsigned long commandsSent = 0;


struct SynthState {
  // Oscillators
  int osc1Wave;
  int osc2Wave;
  int osc2Detune;
  
  // Envelope
  int attack;
  int decay;
  int sustain;
  int release;
  
  // Filter
  int filterCutoff;
  
  // Effects
  int delayTime;
  int delayFeedback;
  int delayMix;
  int chorusMix;
  int delayPreset;
  
  // Master
  int volume;
} state;

// Chorus state (separate variable)
bool chorusEnabled = false;

void initState() {
  state.osc1Wave = 1;
  state.osc2Wave = 1;
  state.osc2Detune = 10;
  state.attack = 2;
  state.decay = 13;
  state.sustain = 89;
  state.release = 13;
  state.filterCutoff = 64;
  state.delayTime = 0;
  state.delayFeedback = 0;
  state.delayMix = 0;
  state.chorusMix = 76;
  state.delayPreset = 0;
  state.volume = 89;
}

// ========== UART COMMUNICATION ==========
void sendToSTM32(String cmd) {
  Serial2.println(cmd);  // Simple UART send!
  commandsSent++;
  
  Serial.print("→ ");
  Serial.println(cmd);
}


void sendCC(int cc, int value) {
  String cmd = "CC," + String(cc) + "," + String(value);
  sendToSTM32(cmd);
}

void sendParam(String param, int value) {
  String cmd = "PARAM," + param + "," + String(value);
  sendToSTM32(cmd);
}

void sendButton(int buttonNum) {
  String cmd = "BTN," + String(buttonNum);
  sendToSTM32(cmd);
}

// ========== WEB INTERFACE HANDLERS (unchanged) ==========
// ==========================================

// Synth Presets
struct Preset {
  const char* name;
  // Basic Oscillators
  int osc1Wave;
  int osc2Wave;
  int osc2Detune;
  int osc2Level;  // NEW
  // Sub Oscillator
  int subLevel;    // NEW
  int subOctave;   // NEW (1 or 2)
  int subWave;     // NEW (0=SINE, 2=SQUARE)
  // Noise Generator
  int noiseLevel;  // NEW
  // Volume ADSR
  int attack;
  int decay;
  int sustain;
  int release;
  // Filter
  int filterCutoff;
  // Filter Envelope
  bool filterEnvEnabled;  // NEW
  int filterEnvAttack;    // NEW
  int filterEnvDecay;     // NEW
  int filterEnvSustain;   // NEW
  int filterEnvRelease;   // NEW
  int filterEnvAmount;    // NEW
  // LFO1
  bool lfo1Enabled;       // NEW
  int lfo1Rate;           // NEW
  int lfo1Wave;           // NEW
  int lfo1Pitch;          // NEW
  int lfo1Filter;         // NEW
  int lfo1Amp;            // NEW
  int lfo1Detune;         // NEW
  // LFO2
  bool lfo2Enabled;       // NEW
  int lfo2Rate;           // NEW
  int lfo2Wave;           // NEW
  int lfo2Pitch;          // NEW
  int lfo2Filter;         // NEW
  int lfo2Amp;            // NEW
  int lfo2Detune;         // NEW
  // Effects
  int delayMix;
  int chorusMix;
  bool chorusEnabled;
  // Ring mod & Portamento
  int ringMod;           // 0-127
  bool portamentoOn;     // on/off
  int portamentoTime;    // 0-127
  // Added in Phase 2
  int resonance;         // 0-127
  int delayTime;         // 0-127 (10-400 ms)
  int delayFeedback;     // 0-127
  bool pingPong;         // ping-pong delay mode
  int level;             // output level: 0-127, 64 = 100%, 127 = ~200% (+6 dB)
};

// Forward declarations for user preset functions
void saveUserPreset(int slot, const Preset& preset);
bool loadUserPreset(int slot, Preset& preset);
bool isUserPresetUsed(int slot);
String getUserPresetName(int slot);
void getCurrentSettings(Preset& preset);
void applyPreset(const Preset& p);
String presetJson(const Preset& p, const String& name);
// (Functions taking a Preset must be declared here, after the struct: the
//  Arduino IDE puts its automatic prototypes above the struct otherwise.)

// Times (attack, decay, release, filter envelope) use an exponential scale:
// 0 = 1 ms, 64 = ~56 ms, 100 = ~545 ms, 127 = 3000 ms (time to fall 60 dB).
// Retuned in Phase 2 for the exponential envelopes and resonant filter.
const Preset presets[] = {
  // 1. Init - neutral starting point: two slightly detuned saws
  {"Init/Default", 1, 1, 9, 89, 0, 1, 0, 0, 26, 90, 89, 90, 61, false, 26, 90, 64, 90, 64, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, false, 0, 0, 62, 40, false, 64},
  // 2. Fat Bass - detuned saws + sub, low filter with a short envelope 'pluck'
  {"Fat Bass", 1, 1, 6, 89, 89, 1, 0, 0, 17, 101, 76, 88, 15, true, 17, 90, 25, 88, 44, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, false, 0, 19, 62, 40, false, 64},
  // 3. TB-303 Acid - one saw, low cutoff, high resonance, snappy filter envelope
  {"TB-303 Acid", 1, 1, 0, 0, 0, 1, 0, 0, 17, 90, 64, 70, 3, true, 0, 88, 0, 73, 76, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, false, 0, 95, 62, 40, false, 77},
  // 4. Bell Chime - sine + triangle, instant attack, long ringing decay, no sustain
  {"Bell Chime", 0, 3, 4, 127, 0, 1, 0, 0, 0, 124, 0, 124, 99, false, 26, 90, 64, 90, 64, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 25, 44, true, 25, false, 0, 0, 94, 40, true, 115},
  // 5. Synth Lead - detuned saws, vibrato, short glide, a little resonance
  {"Synth Lead", 1, 1, 13, 89, 32, 1, 0, 0, 26, 95, 102, 90, 48, true, 26, 95, 64, 90, 38, true, 34, 0, 32, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 25, 64, false, 0, true, 20, 32, 94, 40, true, 64},
  // 6. Analog Pad - slow swell, gentle filter movement, drifting detune, chorus
  {"Analog Pad", 1, 3, 19, 89, 25, 1, 0, 4, 101, 110, 108, 124, 29, true, 106, 116, 89, 121, 51, true, 4, 0, 0, 51, 19, 0, true, 9, 1, 0, 0, 0, 38, 19, 57, true, 0, false, 0, 25, 111, 47, true, 64},
  // 7. Robot Voice - "Dalek"-style ring modulation: square x sine a semitone apart gives a low
  //    metallic warble (difference tone ~6% of the note); resonant "formant" filter switched by a square LFO
  {"Robot Voice", 2, 0, 127, 127, 0, 1, 0, 0, 26, 90, 114, 76, 13, false, 26, 90, 64, 90, 64, true, 44, 2, 0, 32, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 108, false, 0, 89, 62, 40, false, 83},
  // 8. String Ensemble - detuned saws, slow bow-like attack, light vibrato, chorus
  {"String Ensemble", 1, 1, 15, 102, 0, 1, 0, 0, 90, 110, 108, 116, 55, false, 26, 90, 64, 90, 64, true, 31, 0, 13, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 19, 64, true, 0, false, 0, 0, 78, 33, false, 64},
  // 9. Metallic Pad - ring-modulated pad with resonant filter sweep and chorus
  {"Metallic Pad", 1, 3, 19, 76, 38, 1, 0, 6, 99, 112, 95, 124, 42, true, 106, 112, 76, 116, 64, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 25, 57, true, 32, false, 0, 44, 94, 53, true, 64},
  // 10. Pluck Bass - instant attack, decaying note, resonant filter 'pluck'
  {"Pluck Bass", 1, 3, 6, 89, 76, 1, 0, 0, 0, 101, 0, 95, 8, true, 0, 88, 0, 84, 57, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, false, 0, 38, 62, 40, false, 64},
  // 11. Wobble Bass - smooth sine LFO sweeping a resonant low filter
  {"Wobble Bass", 1, 2, 0, 89, 76, 1, 0, 0, 17, 90, 127, 84, 1, false, 26, 90, 64, 90, 64, true, 15, 0, 0, 102, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, false, 0, 70, 62, 40, false, 64},
  // 12. Sitar - bright plucked attack, long decay, buzzy resonance + ring mod
  {"Sitar", 1, 1, 11, 89, 25, 1, 0, 0, 0, 116, 0, 110, 36, true, 0, 106, 0, 90, 64, true, 76, 0, 0, 19, 0, 0, false, 16, 0, 0, 0, 0, 0, 19, 64, false, 32, false, 0, 57, 62, 40, false, 64},
  // 13. Ambient Wash - very slow swell and fade, wide detune, long ping-pong echoes
  {"Ambient Wash", 1, 2, 32, 89, 32, 1, 0, 13, 121, 121, 102, 127, 29, true, 124, 121, 76, 124, 51, true, 2, 0, 0, 38, 0, 0, false, 16, 0, 0, 0, 0, 0, 44, 64, true, 0, false, 0, 32, 127, 74, true, 64},
  // 14. Glide Bass - Fat Bass with portamento between notes
  {"Glide Bass", 1, 1, 6, 89, 76, 1, 0, 0, 17, 101, 76, 88, 15, true, 17, 90, 25, 88, 44, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, true, 30, 19, 62, 40, false, 64},
  // 15. 808 Bass - pure sine with a long booming decay (no pitch drop: not available)
  {"808 Bass", 0, 0, 0, 0, 51, 1, 0, 0, 0, 124, 0, 116, 23, false, 26, 90, 64, 90, 64, false, 41, 0, 0, 0, 0, 0, false, 16, 0, 0, 0, 0, 0, 0, 64, false, 0, false, 0, 0, 62, 40, false, 96}
};


const int numPresets = 15;

// User Preset Management Functions
void saveUserPreset(int slot, const Preset& preset) {
  if (slot < 0 || slot >= NUM_USER_PRESETS) return;
  
  preferences.begin("synth", false);  // Open in read-write mode
  
  String prefix = "u" + String(slot) + "_";
  
  // Save all preset parameters
  preferences.putInt((prefix + "o1w").c_str(), preset.osc1Wave);
  preferences.putInt((prefix + "o2w").c_str(), preset.osc2Wave);
  preferences.putInt((prefix + "o2d").c_str(), preset.osc2Detune);
  preferences.putInt((prefix + "o2l").c_str(), preset.osc2Level);
  preferences.putInt((prefix + "sub").c_str(), preset.subLevel);
  preferences.putInt((prefix + "sbo").c_str(), preset.subOctave);
  preferences.putInt((prefix + "sbw").c_str(), preset.subWave);
  preferences.putInt((prefix + "noi").c_str(), preset.noiseLevel);
  preferences.putInt((prefix + "atk").c_str(), preset.attack);
  preferences.putInt((prefix + "dec").c_str(), preset.decay);
  preferences.putInt((prefix + "sus").c_str(), preset.sustain);
  preferences.putInt((prefix + "rel").c_str(), preset.release);
  preferences.putInt((prefix + "fil").c_str(), preset.filterCutoff);
  preferences.putBool((prefix + "fen").c_str(), preset.filterEnvEnabled);
  preferences.putInt((prefix + "fea").c_str(), preset.filterEnvAttack);
  preferences.putInt((prefix + "fed").c_str(), preset.filterEnvDecay);
  preferences.putInt((prefix + "fes").c_str(), preset.filterEnvSustain);
  preferences.putInt((prefix + "fer").c_str(), preset.filterEnvRelease);
  preferences.putInt((prefix + "fem").c_str(), preset.filterEnvAmount);
  preferences.putBool((prefix + "l1e").c_str(), preset.lfo1Enabled);
  preferences.putInt((prefix + "l1r").c_str(), preset.lfo1Rate);
  preferences.putInt((prefix + "l1w").c_str(), preset.lfo1Wave);
  preferences.putInt((prefix + "l1p").c_str(), preset.lfo1Pitch);
  preferences.putInt((prefix + "l1f").c_str(), preset.lfo1Filter);
  preferences.putInt((prefix + "l1a").c_str(), preset.lfo1Amp);
  preferences.putInt((prefix + "l1d").c_str(), preset.lfo1Detune);
  preferences.putBool((prefix + "l2e").c_str(), preset.lfo2Enabled);
  preferences.putInt((prefix + "l2r").c_str(), preset.lfo2Rate);
  preferences.putInt((prefix + "l2w").c_str(), preset.lfo2Wave);
  preferences.putInt((prefix + "l2p").c_str(), preset.lfo2Pitch);
  preferences.putInt((prefix + "l2f").c_str(), preset.lfo2Filter);
  preferences.putInt((prefix + "l2a").c_str(), preset.lfo2Amp);
  preferences.putInt((prefix + "l2d").c_str(), preset.lfo2Detune);
  preferences.putInt((prefix + "dlm").c_str(), preset.delayMix);
  preferences.putInt((prefix + "chm").c_str(), preset.chorusMix);
  preferences.putBool((prefix + "che").c_str(), preset.chorusEnabled);
  preferences.putInt((prefix + "rng").c_str(), preset.ringMod);
  preferences.putBool((prefix + "poe").c_str(), preset.portamentoOn);
  preferences.putInt((prefix + "pot").c_str(), preset.portamentoTime);
  preferences.putInt((prefix + "res").c_str(), preset.resonance);
  preferences.putInt((prefix + "dlt").c_str(), preset.delayTime);
  preferences.putInt((prefix + "dlf").c_str(), preset.delayFeedback);
  preferences.putBool((prefix + "png").c_str(), preset.pingPong);
  preferences.putInt((prefix + "lvl").c_str(), preset.level);
  
  // Save preset name (max 20 characters)
  String name = String(preset.name);
  if (name.length() > 20) name = name.substring(0, 20);
  preferences.putString((prefix + "nam").c_str(), name);
  
  // Mark slot as used
  preferences.putBool((prefix + "use").c_str(), true);
  
  preferences.end();
}

bool loadUserPreset(int slot, Preset& preset) {
  if (slot < 0 || slot >= NUM_USER_PRESETS) return false;
  
  preferences.begin("synth", true);  // Open in read-only mode
  
  String prefix = "u" + String(slot) + "_";
  
  // Check if slot is used
  if (!preferences.getBool((prefix + "use").c_str(), false)) {
    preferences.end();
    return false;  // Slot is empty
  }
  
  // Load all parameters
  preset.osc1Wave = preferences.getInt((prefix + "o1w").c_str(), 1);
  preset.osc2Wave = preferences.getInt((prefix + "o2w").c_str(), 1);
  preset.osc2Detune = preferences.getInt((prefix + "o2d").c_str(), 10);
  preset.osc2Level = preferences.getInt((prefix + "o2l").c_str(), 89);
  preset.subLevel = preferences.getInt((prefix + "sub").c_str(), 0);
  preset.subOctave = preferences.getInt((prefix + "sbo").c_str(), 1);
  preset.subWave = preferences.getInt((prefix + "sbw").c_str(), 0);
  preset.noiseLevel = preferences.getInt((prefix + "noi").c_str(), 0);
  preset.attack = preferences.getInt((prefix + "atk").c_str(), 2);
  preset.decay = preferences.getInt((prefix + "dec").c_str(), 13);
  preset.sustain = preferences.getInt((prefix + "sus").c_str(), 89);
  preset.release = preferences.getInt((prefix + "rel").c_str(), 13);
  preset.filterCutoff = preferences.getInt((prefix + "fil").c_str(), 64);
  preset.filterEnvEnabled = preferences.getBool((prefix + "fen").c_str(), false);
  preset.filterEnvAttack = preferences.getInt((prefix + "fea").c_str(), 2);
  preset.filterEnvDecay = preferences.getInt((prefix + "fed").c_str(), 13);
  preset.filterEnvSustain = preferences.getInt((prefix + "fes").c_str(), 64);
  preset.filterEnvRelease = preferences.getInt((prefix + "fer").c_str(), 13);
  preset.filterEnvAmount = preferences.getInt((prefix + "fem").c_str(), 64);
  preset.lfo1Enabled = preferences.getBool((prefix + "l1e").c_str(), false);
  preset.lfo1Rate = preferences.getInt((prefix + "l1r").c_str(), 41);
  preset.lfo1Wave = preferences.getInt((prefix + "l1w").c_str(), 0);
  preset.lfo1Pitch = preferences.getInt((prefix + "l1p").c_str(), 0);
  preset.lfo1Filter = preferences.getInt((prefix + "l1f").c_str(), 0);
  preset.lfo1Amp = preferences.getInt((prefix + "l1a").c_str(), 0);
  preset.lfo1Detune = preferences.getInt((prefix + "l1d").c_str(), 0);
  preset.lfo2Enabled = preferences.getBool((prefix + "l2e").c_str(), false);
  preset.lfo2Rate = preferences.getInt((prefix + "l2r").c_str(), 16);
  preset.lfo2Wave = preferences.getInt((prefix + "l2w").c_str(), 0);
  preset.lfo2Pitch = preferences.getInt((prefix + "l2p").c_str(), 0);
  preset.lfo2Filter = preferences.getInt((prefix + "l2f").c_str(), 0);
  preset.lfo2Amp = preferences.getInt((prefix + "l2a").c_str(), 0);
  preset.lfo2Detune = preferences.getInt((prefix + "l2d").c_str(), 0);
  preset.delayMix = preferences.getInt((prefix + "dlm").c_str(), 0);
  preset.chorusMix = preferences.getInt((prefix + "chm").c_str(), 76);
  preset.chorusEnabled = preferences.getBool((prefix + "che").c_str(), false);
  preset.ringMod = preferences.getInt((prefix + "rng").c_str(), 0);
  preset.portamentoOn = preferences.getBool((prefix + "poe").c_str(), false);
  preset.portamentoTime = preferences.getInt((prefix + "pot").c_str(), 0);
  // Fields added in Phase 2 (older saved presets get these defaults)
  preset.resonance = preferences.getInt((prefix + "res").c_str(), 0);
  preset.delayTime = preferences.getInt((prefix + "dlt").c_str(), 62);
  preset.delayFeedback = preferences.getInt((prefix + "dlf").c_str(), 40);
  preset.pingPong = preferences.getBool((prefix + "png").c_str(), false);
  preset.level = preferences.getInt((prefix + "lvl").c_str(), 64);
  
  // Load preset name. Kept in a static String: the old code pointed the name
  // at a local String that was destroyed when this function returned.
  static String loadedName;
  loadedName = preferences.getString((prefix + "nam").c_str(), "User");
  preset.name = loadedName.c_str();
  
  preferences.end();
  return true;
}

bool isUserPresetUsed(int slot) {
  if (slot < 0 || slot >= NUM_USER_PRESETS) return false;
  
  preferences.begin("synth", true);
  String prefix = "u" + String(slot) + "_";
  bool used = preferences.getBool((prefix + "use").c_str(), false);
  preferences.end();
  
  return used;
}

String getUserPresetName(int slot) {
  if (slot < 0 || slot >= NUM_USER_PRESETS) return "User";
  
  preferences.begin("synth", true);
  String prefix = "u" + String(slot) + "_";
  
  if (!preferences.getBool((prefix + "use").c_str(), false)) {
    preferences.end();
    return "User " + String(slot + 1) + " (Empty)";
  }
  
  String name = preferences.getString((prefix + "nam").c_str(), "User " + String(slot + 1));
  preferences.end();
  
  return name;
}

void getCurrentSettings(Preset& preset) {
  // This function captures current synth state into a Preset struct
  // We'll populate this from the web UI's current values
  // For now, create a placeholder - we'll get values from JavaScript
  preset.name = "User";
}

// ===== KEYBOARD PAGE =====
const char KEYBOARD_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Virtual Keyboard</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
      display: flex;
      flex-direction: column;
      align-items: center;
      touch-action: none;
      -webkit-user-select: none;
      user-select: none;
    }
    .header {
      background: white;
      padding: 15px 20px;
      border-radius: 12px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.15);
      margin-bottom: 20px;
      width: 100%;
      max-width: 900px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .header h1 {
      font-size: 1.5em;
      color: #333;
    }
    .back-link {
      background: #667eea;
      color: white;
      padding: 10px 20px;
      border-radius: 8px;
      text-decoration: none;
      font-weight: 600;
    }
    .controls {
      background: white;
      padding: 20px;
      border-radius: 12px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.15);
      margin-bottom: 20px;
      width: 100%;
      max-width: 900px;
    }
    .control-row {
      display: flex;
      gap: 20px;
      align-items: center;
      margin-bottom: 15px;
    }
    .control-row:last-child { margin-bottom: 0; }
    .control-group {
      flex: 1;
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .control-group label {
      font-weight: 600;
      color: #555;
      min-width: 80px;
    }
    .octave-btns {
      display: flex;
      gap: 10px;
    }
    button {
      background: #667eea;
      color: white;
      border: none;
      padding: 12px 24px;
      border-radius: 8px;
      font-size: 1em;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
    }
    button:active { transform: scale(0.95); background: #5568d3; }
    input[type="range"] {
      flex: 1;
      height: 8px;
      border-radius: 4px;
      background: #ddd;
      outline: none;
      -webkit-appearance: none;
    }
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: #667eea;
      cursor: pointer;
    }
    .value-display {
      min-width: 50px;
      text-align: right;
      font-weight: 600;
      color: #667eea;
    }
    .keyboard-container {
      background: white;
      padding: 30px 20px;
      border-radius: 12px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.2);
      width: 100%;
      max-width: 900px;
      overflow-x: auto;
    }
    .keyboard {
      display: flex;
      justify-content: center;
      position: relative;
      height: 200px;
      min-width: 700px;
    }
    .key {
      cursor: pointer;
      transition: all 0.05s;
      display: flex;
      align-items: flex-end;
      justify-content: center;
      padding-bottom: 10px;
      font-weight: 600;
      font-size: 0.9em;
    }
    .white-key {
      background: white;
      width: 50px;
      height: 200px;
      z-index: 1;
      color: #999;
      border: 2px solid #333;
      border-right: 1px solid #ccc;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    .white-key:active, .white-key.active {
      background: #e0e0e0;
      transform: translateY(2px);
    }
    .black-key {
      background: linear-gradient(to bottom, #222 0%, #000 100%);
      width: 30px;
      height: 110px;
      position: absolute;
      z-index: 2;
      color: #888;
      border: 2px solid #000;
      box-shadow: 0 4px 8px rgba(0,0,0,0.3);
    }
    .black-key:active, .black-key.active {
      background: #555;
      transform: translateY(2px);
    }
    .octave-label {
      margin-top: 10px;
      text-align: center;
      font-weight: 600;
      color: #667eea;
      font-size: 1.2em;
    }
  </style>
</head>
<body>
  <div class="header">
    <h1>🎹 Virtual Keyboard</h1>
    <a href="/" class="back-link">← Controls</a>
  </div>

  <div class="controls">
    <div class="control-row">
      <div class="control-group">
        <label>Octave:</label>
        <div class="octave-btns">
          <button onclick="changeOctave(-1)">◄</button>
          <button onclick="changeOctave(1)">►</button>
        </div>
        <div class="value-display" id="octave-display">C4-C5</div>
      </div>
    </div>
    <div class="control-row">
      <div class="control-group">
        <label>Velocity:</label>
        <input type="range" id="velocity" min="1" max="127" value="100" oninput="updateVelocity(this.value)">
        <div class="value-display" id="vel-display">79%</div>
      </div>
    </div>
  </div>

  <div class="keyboard-container">
    <div class="keyboard" id="keyboard"></div>
    <div class="octave-label" id="octave-label">Octave 4-5</div>
  </div>

  <script>
    let currentOctave = 4; // Start at C4
    let velocity = 100;
    let activeNotes = new Set();

    const whiteNotes = ['C', 'D', 'E', 'F', 'G', 'A', 'B'];
    const blackNotePositions = {
      'C#': 1, 'D#': 2, 'F#': 4, 'G#': 5, 'A#': 6
    };

    function updateVelocity(val) {
      velocity = parseInt(val);
      document.getElementById('vel-display').textContent = Math.round((velocity / 127) * 100) + '%';
    }

    function changeOctave(delta) {
      currentOctave = Math.max(0, Math.min(8, currentOctave + delta));
      updateOctaveDisplay();
    }

    function updateOctaveDisplay() {
      const start = currentOctave;
      const end = currentOctave + 1;
      document.getElementById('octave-display').textContent = `C${start}-C${end}`;
      document.getElementById('octave-label').textContent = `Octave ${start}-${end}`;
    }

    function noteToMidi(note, octave) {
      const noteMap = { C: 0, 'C#': 1, D: 2, 'D#': 3, E: 4, F: 5, 'F#': 6, G: 7, 'G#': 8, A: 9, 'A#': 10, B: 11 };
      return (octave + 1) * 12 + noteMap[note];
    }

    function playNote(note, octave) {
      const midi = noteToMidi(note, octave);
      if (activeNotes.has(midi)) return;
      activeNotes.add(midi);
      fetch(`/note?num=${midi}&vel=${velocity}`);
      
      const keyEl = document.querySelector(`[data-note="${note}${octave}"]`);
      if (keyEl) keyEl.classList.add('active');
    }

    function stopNote(note, octave) {
      const midi = noteToMidi(note, octave);
      activeNotes.delete(midi);
      fetch(`/note?num=${midi}&vel=0`);
      
      const keyEl = document.querySelector(`[data-note="${note}${octave}"]`);
      if (keyEl) keyEl.classList.remove('active');
    }

    function buildKeyboard() {
      const kb = document.getElementById('keyboard');
      kb.innerHTML = '';

      // Draw 2 octaves (C to C)
      for (let oct = 0; oct < 2; oct++) {
        const octave = currentOctave + oct;
        
        for (let i = 0; i < 7; i++) {
          const note = whiteNotes[i];
          const key = document.createElement('div');
          key.className = 'white-key';
          key.dataset.note = note + octave;
          key.textContent = note + octave;
          key.style.left = (oct * 7 * 50 + i * 50) + 'px';
          
          key.addEventListener('mousedown', (e) => { e.preventDefault(); playNote(note, octave); });
          key.addEventListener('mouseup', (e) => { e.preventDefault(); stopNote(note, octave); });
          key.addEventListener('mouseleave', (e) => { e.preventDefault(); stopNote(note, octave); });
          key.addEventListener('touchstart', (e) => { e.preventDefault(); playNote(note, octave); });
          key.addEventListener('touchend', (e) => { e.preventDefault(); stopNote(note, octave); });
          key.addEventListener('touchcancel', (e) => { e.preventDefault(); stopNote(note, octave); });
          
          kb.appendChild(key);
        }
      }

      // Draw black keys
      for (let oct = 0; oct < 2; oct++) {
        const octave = currentOctave + oct;
        
        for (let note in blackNotePositions) {
          const pos = blackNotePositions[note];
          const key = document.createElement('div');
          key.className = 'black-key';
          key.dataset.note = note + octave;
          key.textContent = note + octave;
          key.style.left = (oct * 7 * 50 + pos * 50 - 15) + 'px';
          
          key.addEventListener('mousedown', (e) => { e.preventDefault(); playNote(note, octave); });
          key.addEventListener('mouseup', (e) => { e.preventDefault(); stopNote(note, octave); });
          key.addEventListener('mouseleave', (e) => { e.preventDefault(); stopNote(note, octave); });
          key.addEventListener('touchstart', (e) => { e.preventDefault(); playNote(note, octave); });
          key.addEventListener('touchend', (e) => { e.preventDefault(); stopNote(note, octave); });
          key.addEventListener('touchcancel', (e) => { e.preventDefault(); stopNote(note, octave); });
          
          kb.appendChild(key);
        }
      }

      // Final C
      const finalOct = currentOctave + 2;
      const finalKey = document.createElement('div');
      finalKey.className = 'white-key';
      finalKey.dataset.note = 'C' + finalOct;
      finalKey.textContent = 'C' + finalOct;
      finalKey.style.left = (2 * 7 * 50) + 'px';
      
      finalKey.addEventListener('mousedown', (e) => { e.preventDefault(); playNote('C', finalOct); });
      finalKey.addEventListener('mouseup', (e) => { e.preventDefault(); stopNote('C', finalOct); });
      finalKey.addEventListener('mouseleave', (e) => { e.preventDefault(); stopNote('C', finalOct); });
      finalKey.addEventListener('touchstart', (e) => { e.preventDefault(); playNote('C', finalOct); });
      finalKey.addEventListener('touchend', (e) => { e.preventDefault(); stopNote('C', finalOct); });
      finalKey.addEventListener('touchcancel', (e) => { e.preventDefault(); stopNote('C', finalOct); });
      
      kb.appendChild(finalKey);
    }

    // All notes off on panic
    function allNotesOff() {
      activeNotes.forEach(midi => fetch(`/note?num=${midi}&vel=0`));
      activeNotes.clear();
      document.querySelectorAll('.key').forEach(k => k.classList.remove('active'));
    }

    // Rebuild keyboard when octave changes
    window.addEventListener('DOMContentLoaded', () => {
      buildKeyboard();
      updateOctaveDisplay();
      updateVelocity(velocity);
    });

    // Watch for octave changes
    let lastOctave = currentOctave;
    setInterval(() => {
      if (currentOctave !== lastOctave) {
        allNotesOff();
        buildKeyboard();
        lastOctave = currentOctave;
      }
    }, 100);
  </script>
</body>
</html>
)rawliteral";

// ===== MAIN CONTROL PAGE =====
const char HTML_CHUNK_0[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>STM32 Synth Controller</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 15px;
      color: #333;
    }
    
    .container {
      max-width: 1000px;
      margin: 0 auto;
      background: rgba(255, 255, 255, 0.95);
      border-radius: 20px;
      padding: 25px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
    }
    
    h1 {
      text-align: center;
      color: #667eea;
      margin-bottom: 8px;
      font-size: 2.2em;
      font-weight: 700;
    }
    
    .subtitle {
      text-align: center;
      color: #666;
      margin-bottom: 25px;
      font-size: 0.95em;
    }
    
    .section {
      background: #f8f9fa;
      border-radius: 15px;
      padding: 18px;
      margin-bottom: 18px;
      border: 2px solid #e9ecef;
    }
    
    .section-title {
      font-size: 1.25em;
      color: #667eea;
      margin-bottom: 12px;
      font-weight: 600;
      border-bottom: 2px solid #667eea;
      padding-bottom: 8px;
    }
    
    .control-group {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 15px;
      margin-bottom: 12px;
    }
    
    .control {
      display: flex;
      flex-direction: column;
    }
    
    label {
      font-weight: 600;
      margin-bottom: 6px;
      color: #495057;
      font-size: 0.85em;
    }
    
    input[type="range"] {
      width: 100%;
      height: 7px;
      border-radius: 5px;
      background: #dee2e6;
      outline: none;
      -webkit-appearance: none;
    }
    
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: #667eea;
      cursor: pointer;
      transition: all 0.2s;
      box-shadow: 0 2px 4px rgba(0,0,0,0.2);
    }
    
    input[type="range"]::-webkit-slider-thumb:hover {
      background: #764ba2;
      transform: scale(1.15);
    }
    
    input[type="range"]::-moz-range-thumb {
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: #667eea;
      cursor: pointer;
      border: none;
    }
    
    .value-display {
      text-align: center;
      font-weight: 700;
      color: #667eea;
      margin-top: 6px;
      font-size: 1em;
    }
    
    .button-group {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    
    button {
      flex: 1;
      min-width: 110px;
      padding: 12px 16px;
      background: #667eea;
      color: white;
      border: none;
      border-radius: 10px;
      cursor: pointer;
      font-size: 0.95em;
      font-weight: 600;
      transition: all 0.3s;
      box-shadow: 0 4px 6px rgba(0,0,0,0.1);
    }
    
    button:hover {
      background: #764ba2;
      transform: translateY(-2px);
      box-shadow: 0 6px 12px rgba(0,0,0,0.2);
    }
    
    button:active {
      transform: translateY(0);
    }
    
    .preset-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
      gap: 10px;
    }
    
    .preset-btn {
      background: #17a2b8;
      padding: 14px 10px;
      font-size: 0.9em;
      min-width: 0;
    }
    
    .preset-btn:hover {
      background: #138496;
    }
    
    .toggle-btn {
      background: #6c757d;
    }
    
    .toggle-btn.active {
      background: #28a745;
    }
    
    .wave-btn {
      background: #fd7e14;
    }
    
    .wave-btn:hover {
      background: #e67100;
    }
    
    .emergency-btn {
      background: #dc3545;
      font-size: 1.05em;
      padding: 14px;
    }
    
    .emergency-btn:hover {
      background: #c82333;
    }
    
    .delay-preset-btn {
      background: #6f42c1;
      min-width: 70px;
      padding: 10px;
      font-size: 0.85em;
    }
    
    .delay-preset-btn:hover {
      background: #5a32a3;
    }
    
    .delay-preset-btn.active {
      background: #dc3545;
      box-shadow: 0 0 0 3px rgba(220, 53, 69, 0.3);
    }
    
    .info-box {
      background: #e7f3ff;
      border-left: 4px solid #2196F3;
      padding: 12px;
      margin-top: 18px;
      border-radius: 5px;
      font-size: 0.9em;
    }
    
    .info-box h3 {
      color: #2196F3;
      margin-bottom: 8px;
      font-size: 1em;
    }
    
    .info-box p {
      margin: 4px 0;
      color: #555;
    }
    
    @media (max-width: 768px) {
      .control-group {
        grid-template-columns: 1fr;
      }
      h1 { font-size: 1.8em; }
      .preset-grid {
        grid-template-columns: repeat(auto-fill, minmax(120px, 1fr));
      }
    }
    
    /* Tab Navigation Styles */
    .tab-navigation {
      position: sticky;
      top: 0;
      z-index: 1000;
      background: #667eea;
      border-radius: 15px;
      padding: 10px;
      margin-bottom: 20px;
      display: flex;
      gap: 8px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.15);
    }
    
    .tab-btn {
      flex: 1;
      padding: 12px 20px;
      background: rgba(255,255,255,0.2);
      color: white;
      border: none;
      border-radius: 10px;
      font-size: 1em;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s ease;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    
    .tab-btn:hover {
      background: rgba(255,255,255,0.3);
      transform: translateY(-2px);
    }
    
    .tab-btn.active {
      background: white;
      color: #667eea;
      box-shadow: 0 4px 8px rgba(0,0,0,0.2);
    }
    
    .screen {
      display: none;
      animation: fadeIn 0.3s ease-in;
    }
    
    .screen.active {
      display: block;
    }
    
    @keyframes fadeIn {
      from { opacity: 0; }
      to { opacity: 1; }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>🎹 STM32 Synthesizer</h1>
    <div class="subtitle">WiFi Control Interface with Presets</div>

    <!-- Tab Navigation -->
    <div class="tab-navigation">
      <button class="tab-btn active" onclick="switchScreen('sound')">Sound</button>
      <button class="tab-btn" onclick="switchScreen('modulation')">Modulation</button>
      <button class="tab-btn" onclick="switchScreen('fx')">FX</button>
      <button class="tab-btn" onclick="switchScreen('presets')">Presets</button>
    </div>

    <!-- SOUND SCREEN -->
    <div id="sound-screen" class="screen active">

    <!-- Oscillators + ADSR -->
    <div class="section">
      <div class="section-title">🎵 Oscillators</div>
      <div class="control-group">
        <div class="control">
          <label>Osc1 Waveform</label>
          <select id="osc1wave" onchange="updateOscWave(1, this.value)" style="width: 100%; padding: 8px; border-radius: 5px; border: 2px solid #dee2e6; font-size: 0.9em;">
            <option value="0">Sine</option>
            <option value="1" selected>Saw</option>
            <option value="2">Square</option>
            <option value="3">Triangle</option>
          </select>
        </div>
        <div class="control">
          <label>Osc2 Waveform</label>
          <select id="osc2wave" onchange="updateOscWave(2, this.value)" style="width: 100%; padding: 8px; border-radius: 5px; border: 2px solid #dee2e6; font-size: 0.9em;">
            <option value="0">Sine</option>
            <option value="1" selected>Saw</option>
            <option value="2">Square</option>
            <option value="3">Triangle</option>
          </select>
        </div>
      </div>
      <div class="control-group" style="margin-top: 12px;">
        <div class="control">
          <label>Osc2 Detune (0-100 cents)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="osc2detune" min="0" max="127" value="10" oninput="updateParam('osc2detune', this.value)" style="flex: 1;">
            <input type="number" id="osc2detune_num" min="0" max="100" step="1" value="7" onchange="updateParam('osc2detune', this.value, true)rawliteral";
const char HTML_CHUNK_1[] PROGMEM = R"rawliteral()" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="osc2detune_val">7 cents</div>
        </div>
        <div class="control">
          <label>Osc2 Volume (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="osc2level" min="0" max="127" value="89" oninput="updateParam('osc2level', this.value)" style="flex: 1;">
            <input type="number" id="osc2level_num" min="0" max="100" step="1" value="70" onchange="updateParam('osc2level', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="osc2level_val">70%</div>
        </div>
      </div>
      
      <!-- Sub Oscillator -->
      <div class="section-title" style="margin-top: 20px;">🔊 Sub Oscillator</div>
      <div class="control-group">
        <div class="control">
          <label>Sub Level (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="sublevel" min="0" max="127" value="0" oninput="updateParam('sublevel', this.value)" style="flex: 1;">
            <input type="number" id="sublevel_num" min="0" max="100" step="1" value="0" onchange="updateParam('sublevel', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="sublevel_val">0%</div>
        </div>
        <div class="control">
          <label>Sub Octave</label>
          <select id="suboctave" onchange="updateParam('suboctave', this.value)" style="width: 100%; padding: 8px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.9em; background: white;">
            <option value="1">-1 Octave</option>
            <option value="2">-2 Octaves</option>
          </select>
        </div>
      </div>
      <div class="control-group" style="margin-top: 12px;">
        <div class="control">
          <label>Sub Waveform</label>
          <select id="subwave" onchange="updateParam('subwave', this.value)" style="width: 100%; padding: 8px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.9em; background: white;">
            <option value="0">SINE</option>
            <option value="2">SQUARE</option>
          </select>
        </div>
      </div>
      <div style="font-size: 0.8em; color: #666; margin-top: 8px; padding: 8px; background: #f8f9fa; border-radius: 4px;">
        💡 Sub adds massive bass depth! Try 50-70% level with -1 octave for fat bass sounds.
      </div>
      
      <!-- Noise Generator -->
      <div class="section-title" style="margin-top: 20px;">📢 Noise Generator</div>
      <div class="control-group">
        <div class="control">
          <label>Noise Level (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="noise" min="0" max="127" value="0" oninput="updateParam('noise', this.value)" style="flex: 1;">
            <input type="number" id="noise_num" min="0" max="100" step="1" value="0" onchange="updateParam('noise', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="noise_val">0%</div>
        </div>
      </div>
      <div style="font-size: 0.8em; color: #666; margin-top: 8px; padding: 8px; background: #f8f9fa; border-radius: 4px;">
        💡 Perfect for: Snares (70%), Hi-hats (100% + high filter), Analog warmth (5-10%). Try with short envelopes!
      </div>
      
      <!-- Ring Modulator -->
      <div class="section-title" style="margin-top: 20px;">🔔 Ring Modulator</div>
      <div class="control-group">
        <div class="control">
          <label>Ring Mod Amount (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="ringmod" min="0" max="127" value="0" oninput="updateParam('ringmod', this.value)" style="flex: 1;">
            <input type="number" id="ringmod_num" min="0" max="100" step="1" value="0" onchange="updateParam('ringmod', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="ringmod_val">0%</div>
        </div>
      </div>
      <div style="font-size: 0.8em; color: #666; margin-top: 8px; padding: 8px; background: #f8f9fa; border-radius: 4px;">
        🔔 0%=Normal (Osc1+Osc2), 25%=Shimmer, 50%=Bells, 100%=Full Ring Mod. Try with detuned oscillators for metallic/robotic tones!
      </div>
      
      <!-- Portamento -->
      <div class="section-title" style="margin-top: 20px;">🎸 Portamento / Glide</div>
      <div class="control-group">
        <div class="control">
          <button id="portabtn" onclick="togglePortamento()" class="toggle-btn">Portamento: OFF</button>
        </div>
        <div class="control">
          <label>Glide Time (0-2000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="portatime" min="0" max="127" value="0" oninput="updateParam('portatime', this.value)" style="flex: 1;">
            <input type="number" id="portatime_num" min="0" max="500" step="1" value="0" onchange="updateParam('portatime', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="portatime_val">0 ms</div>
        </div>
      </div>
      <div style="font-size: 0.8em; color: #666; margin-top: 8px; padding: 8px; background: #f8f9fa; border-radius: 4px;">
        🎸 Smooth pitch slide between notes. Short (50-100ms) for subtle, long (500ms+) for expressive leads.
      </div>
      
      <!-- ADSR moved here -->
      <div class="section-title" style="margin-top: 20px;">📊 ADSR Envelope</div>
      <div class="control-group">
        <div class="control">
          <label>Attack (1-1000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="attack" min="0" max="127" value="2" oninput="updateParam('attack', this.value)" style="flex: 1;">
            <input type="number" id="attack_num" min="1" max="3000" step="1" value="16" onchange="updateParam('attack', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="attack_val">16 ms</div>
        </div>
        <div class="control">
          <label>Decay (1-1000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="decay" min="0" max="127" value="13" oninput="updateParam('decay', this.value)" style="flex: 1;">
            <input type="number" id="decay_num" min="1" max="3000" step="1" value="103" onchange="updateParam('decay', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="decay_val">103 ms</div>
        </div>
        <div class="control">
          <label>Sustain (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="sustain" min="0" max="127" value="89" oninput="updateParam('sustain', this.value)" style="flex: 1;">
            <input type="number" id="sustain_num" min="0" max="100" step="1" value="70" onchange="updateParam('sustain', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="sustain_val">70%</div>
        </div>
        <div class="control">
          <label>Release (1-3000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" i)rawliteral";
const char HTML_CHUNK_2[] PROGMEM = R"rawliteral(d="release" min="0" max="127" value="13" oninput="updateParam('release', this.value)" style="flex: 1;">
            <input type="number" id="release_num" min="1" max="3000" step="1" value="103" onchange="updateParam('release', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="release_val">103 ms</div>
        </div>
      </div>
      
      <!-- Filter Envelope -->
      <div class="section-title" style="margin-top: 20px;">🎛️ Filter Envelope</div>
      <div class="control-group">
        <div class="control">
          <label>Filter Attack (1-1000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="fenvattack" min="0" max="127" value="2" oninput="updateParam('fenvattack', this.value)" style="flex: 1;">
            <input type="number" id="fenvattack_num" min="1" max="3000" step="1" value="16" onchange="updateParam('fenvattack', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="fenvattack_val">16 ms</div>
        </div>
        <div class="control">
          <label>Filter Decay (1-1000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="fenvdecay" min="0" max="127" value="13" oninput="updateParam('fenvdecay', this.value)" style="flex: 1;">
            <input type="number" id="fenvdecay_num" min="1" max="3000" step="1" value="103" onchange="updateParam('fenvdecay', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="fenvdecay_val">103 ms</div>
        </div>
        <div class="control">
          <label>Filter Sustain (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="fenvsustain" min="0" max="127" value="64" oninput="updateParam('fenvsustain', this.value)" style="flex: 1;">
            <input type="number" id="fenvsustain_num" min="0" max="100" step="1" value="50" onchange="updateParam('fenvsustain', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="fenvsustain_val">50%</div>
        </div>
        <div class="control">
          <label>Filter Release (1-3000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="fenvrelease" min="0" max="127" value="13" oninput="updateParam('fenvrelease', this.value)" style="flex: 1;">
            <input type="number" id="fenvrelease_num" min="1" max="3000" step="1" value="103" onchange="updateParam('fenvrelease', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="fenvrelease_val">103 ms</div>
        </div>
      </div>
      <div class="control-group" style="margin-top: 12px;">
        <div class="control">
          <label>Envelope Amount (0-100%)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="fenvamount" min="0" max="127" value="64" oninput="updateParam('fenvamount', this.value)" style="flex: 1;">
            <input type="number" id="fenvamount_num" min="0" max="100" step="1" value="50" onchange="updateParam('fenvamount', this.value, true)" style="width: 60px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="fenvamount_val">50%</div>
        </div>
      </div>
      <div class="button-group" style="margin-top: 10px;">
        <button class="toggle-btn" id="fenvbtn" onclick="toggleFilterEnv()">Filter Env: OFF</button>
      </div>
      <div style="margin-top: 8px; font-size: 0.85em; color: #666; line-height: 1.4;">
        💡 Filter envelope modulates filter cutoff over time. Try fast attack + quick decay for plucks, or slow attack for evolving pads!
      </div>
    </div>

    </div> <!-- End Sound Screen -->

    <!-- MODULATION SCREEN -->
    <div id="modulation-screen" class="screen">

    <!-- LFO 1 -->
    <div class="section">
      <div class="section-title">🌊 LFO 1 (Low Frequency Oscillator)</div>
      <div class="control-group">
        <div class="control">
          <label>LFO1 Rate (Hz)</label>
          <input type="range" id="lforate" min="0" max="127" value="41" oninput="updateParam('lforate', this.value)">
          <div class="value-display" id="lforate_val">5.0 Hz</div>
        </div>
        <div class="control">
          <label>LFO1 Waveform</label>
          <select id="lfowave" onchange="updateLFOWave(1, this.value)" style="width: 100%; padding: 8px; border-radius: 5px; border: 2px solid #dee2e6; font-size: 0.9em;">
            <option value="0">Sine</option>
            <option value="1">Triangle</option>
            <option value="2">Square</option>
            <option value="3">Saw Up</option>
            <option value="4">Saw Down</option>
          </select>
        </div>
      </div>
      <div class="control-group" style="margin-top: 12px;">
        <div class="control">
          <label>LFO1 → Pitch (Vibrato)</label>
          <input type="range" id="lfopitch" min="0" max="127" value="0" oninput="updateParam('lfopitch', this.value)">
          <div class="value-display" id="lfopitch_val">0%</div>
        </div>
        <div class="control">
          <label>LFO1 → Filter</label>
          <input type="range" id="lfofilter" min="0" max="127" value="0" oninput="updateParam('lfofilter', this.value)">
          <div class="value-display" id="lfofilter_val">0%</div>
        </div>
        <div class="control">
          <label>LFO1 → Amplitude (Tremolo)</label>
          <input type="range" id="lfoamp" min="0" max="127" value="0" oninput="updateParam('lfoamp', this.value)">
          <div class="value-display" id="lfoamp_val">0%</div>
        </div>
        <div class="control">
          <label>LFO1 → Osc2 Detune</label>
          <input type="range" id="lfodetune" min="0" max="127" value="0" oninput="updateParam('lfodetune', this.value)">
          <div class="value-display" id="lfodetune_val">0%</div>
        </div>
      </div>
      <div class="button-group" style="margin-top: 10px;">
        <button class="toggle-btn" id="lfobtn" onclick="toggleLFO(1)">LFO1: OFF</button>
      </div>
    </div>

    <!-- LFO 2 -->
    <div class="section">
      <div class="section-title">🌊 LFO 2 (Independent Modulation)</div>
      <div class="control-group">
        <div class="control">
          <label>LFO2 Rate (Hz)</label>
          <input type="range" id="lfo2rate" min="0" max="127" value="16" oninput="updateParam('lfo2rate', this.value)">
          <div class="value-display" id="lfo2rate_val">2.0 Hz</div>
        </div>
        <div class="control">
          <label>LFO2 Waveform</label>
          <select id="lfo2wave" onchange="updateLFOWave(2, this.value)" style="width: 100%; padding: 8px; border-radius: 5px; border: 2px solid #dee2e6; font-size: 0.9em;">
            <option value="0">Sine</option>
            <option value="1" selected>Triangle</option>
            <option value="2">Square</option>
            <option value="3">Saw Up</option>
            <option value="4">Saw Down</option>
          </select>
        </div>
      </div>
      <div class="control-group" style="margin-top: 12px;">
        <div class="control">
          <label>LFO2 → Pitch (Vibrato)</label>
          <input type="range" id="lfo2pitch" min="0" max="127" value="0" oninput="updateParam('lfo2pitch', this.value)">
          <div class="value-display" id="lfo2pitch_val">0%</div>
        </div>
        <div class="control">
          <label>LFO2 → Filter</label>
          <input type="range" id="lfo2filter" min="0" max="127" value="0" oninput)rawliteral";
const char HTML_CHUNK_3[] PROGMEM = R"rawliteral(="updateParam('lfo2filter', this.value)">
          <div class="value-display" id="lfo2filter_val">0%</div>
        </div>
        <div class="control">
          <label>LFO2 → Amplitude (Tremolo)</label>
          <input type="range" id="lfo2amp" min="0" max="127" value="0" oninput="updateParam('lfo2amp', this.value)">
          <div class="value-display" id="lfo2amp_val">0%</div>
        </div>
        <div class="control">
          <label>LFO2 → Osc2 Detune</label>
          <input type="range" id="lfo2detune" min="0" max="127" value="0" oninput="updateParam('lfo2detune', this.value)">
          <div class="value-display" id="lfo2detune_val">0%</div>
        </div>
      </div>
      <div class="button-group" style="margin-top: 10px;">
        <button class="toggle-btn" id="lfo2btn" onclick="toggleLFO(2)">LFO2: OFF</button>
      </div>
    </div>

    <!-- Filter -->
    <div class="section">
      <div class="section-title">🎛️ Filter</div>
      <div class="control-group">
        <div class="control">
          <label>Cutoff Frequency</label>
          <input type="range" id="filter" min="0" max="127" value="64" oninput="updateParam('filter', this.value)">
          <div class="value-display" id="filter_val">5000 Hz</div>
        </div>
        <div class="control">
          <label>Resonance</label>
          <input type="range" id="resonance" min="0" max="127" value="0" oninput="updateParam('resonance', this.value)">
          <div class="value-display" id="resonance_val">0%</div>
        </div>
      </div>
    </div>

    </div> <!-- End Modulation Screen -->

    <!-- FX SCREEN -->
    <div id="fx-screen" class="screen">

    <!-- Delay Effect -->
    <div class="section">
      <div class="section-title">⏱️ Delay</div>
      <div style="margin-bottom: 12px;">
        <label style="display: block; margin-bottom: 8px; font-size: 0.85em;">Presets:</label>
        <div class="button-group">
          <button class="delay-preset-btn active" id="dpreset0" onclick="setDelayPreset(0)">OFF</button>
          <button class="delay-preset-btn" id="dpreset1" onclick="setDelayPreset(1)">Slap</button>
          <button class="delay-preset-btn" id="dpreset2" onclick="setDelayPreset(2)">Short</button>
          <button class="delay-preset-btn" id="dpreset3" onclick="setDelayPreset(3)">Medium</button>
          <button class="delay-preset-btn" id="dpreset4" onclick="setDelayPreset(4)">Rhythmic</button>
        </div>
        <div class="button-group" style="margin-top: 10px;">
          <button class="toggle-btn" id="pingpongbtn" onclick="togglePingPong()">Ping-Pong: OFF</button>
        </div>
      </div>
      <div class="control-group">
        <div class="control">
          <label>Time</label>
          <input type="range" id="delaytime" min="0" max="127" value="0" oninput="updateParam('delaytime', this.value)">
          <div class="value-display" id="delaytime_val">0 ms</div>
        </div>
        <div class="control">
          <label>Feedback</label>
          <input type="range" id="delayfb" min="0" max="127" value="0" oninput="updateParam('delayfb', this.value)">
          <div class="value-display" id="delayfb_val">0%</div>
        </div>
        <div class="control">
          <label>Mix</label>
          <input type="range" id="delaymix" min="0" max="127" value="0" oninput="updateParam('delaymix', this.value)">
          <div class="value-display" id="delaymix_val">0%</div>
        </div>
      </div>
    </div>

    <!-- Chorus -->
    <div class="section">
      <div class="section-title">✨ Chorus</div>
      <div class="control-group">
        <div class="control">
          <label>Chorus Mix</label>
          <input type="range" id="chorusmix" min="0" max="127" value="76" oninput="updateParam('chorusmix', this.value)">
          <div class="value-display" id="chorusmix_val">60%</div>
        </div>
      </div>
      <div class="button-group" style="margin-top: 10px;">
        <button class="toggle-btn" id="chorusbtn" onclick="toggleChorus()">Chorus: OFF</button>
      </div>
    </div>

    <!-- Master -->
    <div class="section">
      <div class="section-title">🔊 Master</div>
      <div class="control-group">
        <div class="control">
          <label>Volume <button id="volumeLockBtn" onclick="promptVolumePass()" style="background: #ff6b6b; color: white; border: none; padding: 4px 10px; border-radius: 4px; cursor: pointer; font-size: 0.85em; margin-left: 8px;">🔒 Locked</button></label>
          <input type="range" id="volume" min="0" max="127" value="89" disabled oninput="updateVolume(this.value)">
          <div class="value-display" id="volume_val">70%</div>
        </div>
      </div>
      <div class="button-group" style="margin-top: 12px;">
        <button class="emergency-btn" onclick="allNotesOff()">🛑 All Notes Off</button>
        <button onclick="window.open('/keyboard', '_blank')" style="background: #667eea; color: white; border: none; padding: 10px 20px; border-radius: 8px; cursor: pointer; font-size: 0.9em;">🎹 Open Keyboard</button>
        <button id="saveLockBtn" onclick="saveAndLock()" style="background: #51cf66; color: white; border: none; padding: 10px 20px; border-radius: 8px; cursor: pointer; font-size: 0.9em; display: none;">💾 Save & Lock</button>
        <button onclick="promptChangePass()" style="background: #666; color: white; border: none; padding: 10px 20px; border-radius: 8px; cursor: pointer; font-size: 0.9em;">Change Passcode</button>
      </div>
    </div>

    </div> <!-- End FX Screen -->

    <!-- PRESETS SCREEN -->
    <div id="presets-screen" class="screen">

    <div class="section">
      <div class="section-title">🎼 Factory Presets</div>
      <div class="preset-grid">
        <button class="preset-btn" onclick="loadPreset(0)">Init/Default</button>
        <button class="preset-btn" onclick="loadPreset(1)">Fat Bass</button>
        <button class="preset-btn" onclick="loadPreset(2)">TB-303 Acid</button>
        <button class="preset-btn" onclick="loadPreset(3)">Bell Chime</button>
        <button class="preset-btn" onclick="loadPreset(4)">Synth Lead</button>
        <button class="preset-btn" onclick="loadPreset(5)">Analog Pad</button>
        <button class="preset-btn" onclick="loadPreset(6)">Robot Voice</button>
        <button class="preset-btn" onclick="loadPreset(7)">String Ensemble</button>
        <button class="preset-btn" onclick="loadPreset(8)">Metallic Pad</button>
        <button class="preset-btn" onclick="loadPreset(9)">Pluck Bass</button>
        <button class="preset-btn" onclick="loadPreset(10)">Wobble Bass</button>
        <button class="preset-btn" onclick="loadPreset(11)">Sitar</button>
        <button class="preset-btn" onclick="loadPreset(12)">Ambient Wash</button>
        <button class="preset-btn" onclick="loadPreset(13)">Glide Bass</button>
        <button class="preset-btn" onclick="loadPreset(14)">808 Bass</button>
      </div>
    </div>

    <div class="section">
      <div class="section-title">⭐ User Presets</div>
      <div class="preset-grid">
        <button class="preset-btn" id="user0" onclick="loadUserPreset(0)" style="opacity: 0.5;">User 1 (Empty)</button>
        <button class="preset-btn" id="user1" onclick="loadUserPreset(1)" style="opacity: 0.5;">User 2 (Empty)</button>
        <button class="preset-btn" id="user2" onclick="loadUserPreset(2)" style="opacity: 0.5;">User 3 (Empty)</button>
        <button class="preset-btn" id="user3" onclick="loadUserPreset(3)" style="opacity: 0.5;">User 4 (Empty)</button>
        <button class="preset-btn" id="user4" onclick="loadUserPreset(4)" style="opacity: 0.5;">User 5 (Empty)</button>
        <button class="preset-btn" id="user5" onclick="loadUserPreset(5)" style="opacity: 0.5;">User 6 (Empty)</button>
        <button class="preset-btn" id="user6" onclick="loadUserPreset(6)" style="opacity: 0.5;">User 7 (Empty)</button>
        <button class="preset-btn" id="user7" onclick="loadUserPreset(7)" style="opacity: 0.5;">User 8 (Empty)</button>
        <button class="preset-btn" id="user8" onclick="loadUserPreset(8)" style="opacity: 0.5;">User 9 (Empty)</button>
        <button class="preset-btn" id="user9" onclick="loadUserPreset(9)" style="opacity: 0.5;">User 10 (Empty)</button>
        <button class="preset-btn" id="user10" onclick="loadUserPreset(10)" style="opacity: 0.5;">User 11 (Empty)</button>
        <button class="preset-btn" id="user11" onclick="loadUserPreset(11)" style="opacity: 0.5;">User 12 (Empty)</button>
      </div>
    </div>

    <div class="section">
      <div class="section-title">🔊 Preset Level</div>
      <div class="control">
        <label>Level (balances this sound against the others; unlock the volume with the passcode to change it)</label>
        <input type="range" id="level" min="0" max="127" value="64" disabled oninput="updateParam('level', this.value)">
        <div class="value-display" id="level_val">100%</div>
      </div>
      <div style="display: flex; gap: 10px; margin-top: 10px;">
        <button class="action-btn" id="saveFactoryLevelBtn" onclick="saveFactoryLevel()" disabled>💾 Save level for this factory preset</button>
        <button class="action-btn" id="resetFactoryLevelBtn" onclick="resetFactoryLevel()" disabled>↺ Reset</button>
      </div>
      <p style="margin-top: 10px; font-size: 0.9em; color: #666;" id="factoryLevelNote">
        Load a factory preset to save its level. User presets save their level with all other settings.
      </p>
    </div>

    <div class="section">
      <div class="section-title">💾 Save Current Settings</div>
      <d)rawliteral";
const char HTML_CHUNK_4[] PROGMEM = R"rawliteral(iv style="margin-bottom: 15px;">
        <label style="display: block; margin-bottom: 5px; font-weight: 600; color: #555;">Preset Name (optional):</label>
        <input type="text" id="presetName" placeholder="My Epic Bass" maxlength="20" style="width: 100%; padding: 10px; border-radius: 8px; border: 2px solid #dee2e6; font-size: 1em; box-sizing: border-box;">
      </div>
      <div style="display: flex; gap: 10px; align-items: center;">
        <select id="saveSlot" style="flex: 1; padding: 10px; border-radius: 8px; border: 2px solid #dee2e6; font-size: 1em;">
          <option value="0">User 1</option>
          <option value="1">User 2</option>
          <option value="2">User 3</option>
          <option value="3">User 4</option>
          <option value="4">User 5</option>
          <option value="5">User 6</option>
          <option value="6">User 7</option>
          <option value="7">User 8</option>
          <option value="8">User 9</option>
          <option value="9">User 10</option>
          <option value="10">User 11</option>
          <option value="11">User 12</option>
        </select>
        <button class="action-btn" onclick="saveCurrentPreset()" style="padding: 10px 30px; font-size: 1em;">💾 Save</button>
      </div>
      <p style="margin-top: 10px; font-size: 0.9em; color: #666;">
        <strong>💡 Tip:</strong> Give your preset a custom name, select a slot, and save! If no name is entered, it will be saved as "User X".
      </p>
    </div>

    </div> <!-- End Presets Screen -->

    <div class="info-box">
      <h3>ℹ️ Connection Info</h3>
      <p><strong>WiFi:</strong> the synth's own network (name and password are set in secrets.h)</p>
      <p><strong>URL:</strong> http://192.168.4.1 | <strong>Status:</strong> <span style="color: #28a745; font-weight: bold;">● Connected</span></p>
    </div>
  </div>

  <script>
    const waves = ['SINE', 'SAW', 'SQUARE', 'TRIANGLE'];
    let osc1wave = 1, osc2wave = 1;
    let chorusEnabled = false;
    let lfo1Enabled = false;
    let lfo2Enabled = false;
    let filterEnvEnabled = false;
    let currentDelayPreset = 0;
    let activePreset = -1;  // Track which preset is active (-1 = none)

    let updating = false;  // Prevent update loops
    
    // Create exact lookup tables to avoid rounding errors
    const attackDecayValues = [];
    const releaseValues = [];
    for (let i = 0; i <= 127; i++) {
      attackDecayValues[i] = Math.round(1 + (i / 127.0) * 999);
      releaseValues[i] = Math.round(1 + (i / 127.0) * 2999);
    }
    
    // Convert display value to 0-127
    function displayToValue(param, displayVal) {
      displayVal = parseFloat(displayVal);
      
      switch(param) {
        case 'osc2detune':
        case 'osc2level':
        case 'sublevel':
        case 'noise':
        case 'ringmod':
        case 'sustain':
        case 'lfopitch':
        case 'lfofilter':
        case 'lfoamp':
        case 'lfodetune':
        case 'lfo2pitch':
        case 'lfo2filter':
        case 'lfo2amp':
        case 'lfo2detune':
        case 'volume':
        case 'delayfb':
        case 'delaymix':
        case 'chorusmix':
        case 'fenvsustain':
        case 'fenvamount':
          return Math.round((displayVal / 100.0) * 127);
          
        case 'attack':
        case 'decay':
        case 'fenvattack':
        case 'fenvdecay':
        case 'release':
        case 'fenvrelease':
          return envTimeToVal(displayVal);

        case 'delaytime':
          return Math.max(0, Math.round(((displayVal - 10) / 390.0) * 127));  // 10-400 ms

        case 'portatime':
          return Math.round((displayVal / 500.0) * 127);
          
        default:
          return Math.round(displayVal);
      }
    }
    
    function updateOscWave(oscNum, value) {
      if (oscNum === 1) {
        osc1wave = parseInt(value);
        fetch('/param?name=osc1wave&val=' + value);
      } else if (oscNum === 2) {
        osc2wave = parseInt(value);
        fetch('/param?name=osc2wave&val=' + value);
      }
    }
    
    let updateTimers = {};  // Store timers for each parameter
    
    // Envelope times use an exponential scale, as on most synths:
    // slider 0 = 1 ms, 64 = ~56 ms, 100 = ~545 ms, 127 = 3000 ms
    function envTimeMs(v) { return Math.round(Math.pow(3000, v / 127)); }
    function envTimeToVal(ms) {
      ms = Math.max(1, Math.min(3000, ms));
      return Math.round(Math.log(ms) / Math.log(3000) * 127);
    }

    function updateParam(param, value, fromNumberInput = false, noSend = false) {
      if (updating) return;
      updating = true;
      
      if (!noSend) clearPresetHighlight();
      
      // If from number input with friendly values, convert to 0-127
      if (fromNumberInput) {
        value = displayToValue(param, value);
      }
      
      // Ensure value is a number and in range
      value = parseInt(value);
      if (isNaN(value)) value = 0;
      if (value < 0) value = 0;
      if (value > 127) value = 127;
      
      // Update slider
      const slider = document.getElementById(param);
      if (slider) slider.value = value;
      
      let displayValue = value;
      let endpoint = '';
      
      switch(param) {
        case 'osc2detune':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=osc2detune&val=' + value;
          break;
        case 'osc2level':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=osc2level&val=' + value;
          break;
        case 'sublevel':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=sublevel&val=' + value;
          break;
        case 'suboctave':
          displayValue = value;  // 1 or 2
          endpoint = '/param?name=suboctave&val=' + value;
          break;
        case 'subwave':
          displayValue = value;  // 0=SINE, 2=SQUARE
          endpoint = '/param?name=subwave&val=' + value;
          break;
        case 'noise':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=noise&val=' + value;
          break;
        case 'ringmod':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=ringmod&val=' + value;
          break;
        case 'lforate':
          displayValue = (0.1 + (value / 127.0) * 19.9).toFixed(1);
          endpoint = '/param?name=lforate&val=' + value;
          break;
        case 'lfopitch':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfopitch&val=' + value;
          break;
        case 'lfofilter':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfofilter&val=' + value;
          break;
        case 'lfoamp':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfoamp&val=' + value;
          break;
        case 'lfodetune':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfodetune&val=' + value;
          break;
        case 'lfo2rate':
          displayValue = (0.1 + (value / 127.0) * 19.9).toFixed(1);
          endpoint = '/param?name=lfo2rate&val=' + value;
          break;
        case 'lfo2pitch':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfo2pitch&val=' + value;
          break;
        case 'lfo2filter':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfo2filter&val=' + value;
          break;
        case 'lfo2amp':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfo2amp&val=' + value;
          break;
        case 'lfo2detune':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=lfo2detune&val=' + value;
          break;
        case 'attack':
        case 'fenvattack':
          displayValue = envTimeMs(value);
          endpoint = '/param?name=' + param + '&val=' + value;
          break;
        case 'decay':
        case 'fenvdecay':
          displayValue = envTimeMs(value);
          endpoint = '/param?name=' + param + '&val=' + value;
          break;
        case 'sustain':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=sustain&val=' + value;
          break;
        case 'release':
        case 'fe)rawliteral";
const char HTML_CHUNK_5[] PROGMEM = R"rawliteral(nvrelease':
          displayValue = envTimeMs(value);
          endpoint = '/param?name=' + param + '&val=' + value;
          break;
        case 'filter':
          displayValue = Math.round(200 + (value / 127.0) * 10000);
          endpoint = '/cc?num=1&val=' + value;
          break;
        case 'volume':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/cc?num=7&val=' + value;
          break;
        case 'delaytime':
          displayValue = Math.round(10 + (value / 127.0) * 390);  // 10-400 ms, matches STM32 CC12
          endpoint = '/cc?num=12&val=' + value;
          break;
        case 'delayfb':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/cc?num=13&val=' + value;
          break;
        case 'delaymix':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/cc?num=91&val=' + value;
          break;
        case 'chorusmix':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/cc?num=93&val=' + value;
          break;
        case 'ringmod':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=ringmod&val=' + value;
          break;
        case 'portatime':
          displayValue = Math.round((value / 127.0) * 500);  // 0-500ms
          endpoint = '/param?name=portatime&val=' + value;
          break;
        case 'fenvattack':
          displayValue = envTimeMs(value);
          endpoint = '/param?name=fenvattack&val=' + value;
          break;
        case 'fenvdecay':
          displayValue = envTimeMs(value);
          endpoint = '/param?name=fenvdecay&val=' + value;
          break;
        case 'fenvsustain':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=fenvsustain&val=' + value;
          break;
        case 'fenvrelease':
          displayValue = envTimeMs(value);
          endpoint = '/param?name=fenvrelease&val=' + value;
          break;
        case 'fenvamount':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=fenvamount&val=' + value;
          break;
        case 'resonance':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=resonance&val=' + value;
          break;
        case 'level':
          displayValue = Math.round((value / 64.0) * 100);   // 64 = 100%
          endpoint = '/param?name=level&val=' + value;
          break;
      }
      
      // Update number input
      const numberInput = document.getElementById(param + '_num');
      if (numberInput) {
        numberInput.value = displayValue;
      }
      
      // Update display label
      const displayEl = document.getElementById(param + '_val');
      if (displayEl) {
        let unit = '';
        if (param === 'attack' || param === 'decay' || param === 'release' || param === 'delaytime' || 
            param === 'fenvattack' || param === 'fenvdecay' || param === 'fenvrelease' ||
            param === 'portatime') unit = ' ms';
        else if (param === 'filter') unit = ' Hz';
        else if (param === 'lforate' || param === 'lfo2rate') unit = ' Hz';
        else if (param === 'osc2detune') unit = ' cents';
        else if (param !== 'lforate' && param !== 'lfo2rate' && param !== 'filter' && param !== 'delaytime' &&
                 param !== 'fenvattack' && param !== 'fenvdecay' && param !== 'fenvrelease') unit = '%';
        displayEl.textContent = displayValue + unit;
      }
      
      // Throttle updates - only send after 50ms of no changes (prevents buffer overflow)
      if (updateTimers[param]) {
        clearTimeout(updateTimers[param]);
      }
      
      if (!noSend) {
        updateTimers[param] = setTimeout(() => {
          fetch(endpoint).then(r => {
            if (r.status === 403 && (param === 'volume' || param === 'level')) relockVolumeUI();  // server lock timed out
          });
          delete updateTimers[param];
        }, 50);  // 50ms debounce
      }
      
      updating = false;
    }

    function clearPresetHighlight() {
      if (activePreset >= 0) {
        const btn = document.querySelector(`[onclick="loadPreset(${activePreset})"]`);
        if (btn) btn.style.boxShadow = '';
        activePreset = -1;
      }
    }

    function highlightPreset(num) {
      clearPresetHighlight();
      const btn = document.querySelector(`[onclick="loadPreset(${num})"]`);
      if (btn) {
        btn.style.boxShadow = '0 0 0 3px #28a745';
        activePreset = num;
      }
    }

    // ---- Applying a complete state (preset, user preset, page load) ----
    // Updates every control on the page WITHOUT sending anything: the ESP32
    // has already applied these settings to the synth. (Presets used to update
    // only some controls, so filter-envelope and LFO sliders could show stale
    // values, and ring mod / portamento from factory presets were never applied.)
    const SLIDER_PARAMS = ['osc2detune','osc2level','sublevel','noise','attack','decay','sustain','release',
      'filter','resonance','fenvattack','fenvdecay','fenvsustain','fenvrelease','fenvamount',
      'lforate','lfopitch','lfofilter','lfoamp','lfodetune','lfo2rate','lfo2pitch','lfo2filter','lfo2amp','lfo2detune',
      'delaytime','delayfb','delaymix','chorusmix','ringmod','portatime','level'];
    const SELECT_PARAMS = ['osc1wave','osc2wave','suboctave','subwave','lfowave','lfo2wave'];

    function setToggle(id, on, label) {
      const btn = document.getElementById(id);
      if (!btn) return;
      btn.textContent = label + (on ? ': ON' : ': OFF');
      btn.classList.toggle('active', on);
    }

    function applyState(s) {
      SLIDER_PARAMS.forEach(p => { if (s[p] !== undefined) updateParam(p, s[p], false, true); });
      SELECT_PARAMS.forEach(p => {
        const el = document.getElementById(p);
        if (el && s[p] !== undefined) el.value = s[p];
      });
      osc1wave = s.osc1wave;
      osc2wave = s.osc2wave;
      filterEnvEnabled  = !!s.fenvenable;   setToggle('fenvbtn', filterEnvEnabled, 'Filter Env');
      lfo1Enabled       = !!s.lfoenable;    setToggle('lfobtn', lfo1Enabled, 'LFO1');
      lfo2Enabled       = !!s.lfo2enable;   setToggle('lfo2btn', lfo2Enabled, 'LFO2');
      chorusEnabled     = !!s.chorusenable; setToggle('chorusbtn', chorusEnabled, 'Chorus');
      portamentoEnabled = !!s.portamento;   setToggle('portabtn', portamentoEnabled, 'Portamento');
      pingPongEnabled   = !!s.pingpong;     setToggle('pingpongbtn', pingPongEnabled, 'Ping-Pong');
      for (let i = 0; i < 5; i++) {
        const b = document.getElementById('dpreset' + i);
        if (b) b.classList.remove('active');
      }
    }

    // Preset Level can make the synth louder, so like the volume ceiling it
    // can only be changed while the volume is unlocked with the passcode
    function setLevelControlsEnabled(on) {
      document.getElementById('level').disabled = !on;
      document.getElementById('saveFactoryLevelBtn').disabled = !on || levelFactoryPreset < 0;
      document.getElementById('resetFactoryLevelBtn').disabled = !on || levelFactoryPreset < 0;
    }

    // Factory preset whose level can be saved (-1 = none loaded)
    let levelFactoryPreset = -1;
    function setFactoryLevelTarget(num, name) {
      levelFactoryPreset = num;
      setLevelControlsEnabled(volumeUnlocked);
      document.getElementById('factoryLevelNote').textContent = (num < 0)
        ? 'Load a factory preset to save its level. User presets save their level with all other settings.'
        : 'Level for factory preset "' + name + '"';
    }

    function saveFactoryLevel() {
      if (levelFactoryPreset < 0) return;
      const val = document.getElementById('level').value;
      fetch('/savefactorylevel?num=' + levelFactoryPreset + '&val=' + val)
        .then(r => r.text()).then(msg => alert('🔊 ' + msg));
    }

    function resetFactoryLevel() {
      if (levelFactoryPreset < 0) return;
      fetch('/resetfactorylevel?num=' + levelFactoryPreset)
        .then(r => r.text()).then(v => updateParam('level', parseInt(v), false, true));
    }

    function loadPreset(num) {
      fetch('/preset?num=' + num).then(r => r.json()).then(s => {
        applyState(s);
        highlightPreset(num);
        setFactoryLevelTarget(num, s.name);
      });
    }

    function loadUserPreset(slot) {
      fetch('/loaduser?slot=' + slot).then(r => {
        if (!r.ok) throw new Error('Empty slot');
        return r.json();
      }).then(s => { applyState(s); setFactoryLevelTarget(-1, ''); })
        .catch(() => console.log('Preset slot empty'));
    }

)rawliteral";
const char HTML_CHUNK_6[] PROGMEM = R"rawliteral(
    function saveCurrentPreset() {
      const slot = document.getElementById('saveSlot').value;
      const presetName = document.getElementById('presetName').value.trim();
      
      // Gather all current parameters
      const params = new URLSearchParams();
      params.append('slot', slot);
      params.append('name', presetName);  // Include preset name
      
      params.append('osc1Wave', osc1wave);
      params.append('osc2Wave', osc2wave);
      params.append('osc2Detune', document.getElementById('osc2detune').value);
      params.append('osc2Level', document.getElementById('osc2level').value);
      params.append('subLevel', document.getElementById('sublevel').value);
      params.append('subOctave', document.getElementById('suboctave').value);
      params.append('subWave', document.getElementById('subwave').value);
      params.append('noiseLevel', document.getElementById('noise').value);
      params.append('attack', document.getElementById('attack').value);
      params.append('decay', document.getElementById('decay').value);
      params.append('sustain', document.getElementById('sustain').value);
      params.append('release', document.getElementById('release').value);
      params.append('filterCutoff', document.getElementById('filter').value);
      params.append('filterEnvEnabled', filterEnvEnabled);
      params.append('filterEnvAttack', document.getElementById('fenvattack').value);
      params.append('filterEnvDecay', document.getElementById('fenvdecay').value);
      params.append('filterEnvSustain', document.getElementById('fenvsustain').value);
      params.append('filterEnvRelease', document.getElementById('fenvrelease').value);
      params.append('filterEnvAmount', document.getElementById('fenvamount').value);
      params.append('lfo1Enabled', lfo1Enabled);
      params.append('lfo1Rate', document.getElementById('lforate').value);
      params.append('lfo1Wave', document.getElementById('lfowave').value);
      params.append('lfo1Pitch', document.getElementById('lfopitch').value);
      params.append('lfo1Filter', document.getElementById('lfofilter').value);
      params.append('lfo1Amp', document.getElementById('lfoamp').value);
      params.append('lfo1Detune', document.getElementById('lfodetune').value);
      params.append('lfo2Enabled', lfo2Enabled);
      params.append('lfo2Rate', document.getElementById('lfo2rate').value);
      params.append('lfo2Wave', document.getElementById('lfo2wave').value);
      params.append('lfo2Pitch', document.getElementById('lfo2pitch').value);
      params.append('lfo2Filter', document.getElementById('lfo2filter').value);
      params.append('lfo2Amp', document.getElementById('lfo2amp').value);
      params.append('lfo2Detune', document.getElementById('lfo2detune').value);
      params.append('delayMix', document.getElementById('delaymix').value);
      params.append('chorusMix', document.getElementById('chorusmix').value);
      params.append('chorusEnabled', chorusEnabled);
      params.append('ringMod', document.getElementById('ringmod').value);
      params.append('portamentoOn', portamentoEnabled);
      params.append('portamentoTime', document.getElementById('portatime').value);
      params.append('resonance', document.getElementById('resonance').value);
      params.append('delayTime', document.getElementById('delaytime').value);
      params.append('delayFeedback', document.getElementById('delayfb').value);
      params.append('pingPong', pingPongEnabled);
      params.append('level', document.getElementById('level').value);
      
      fetch('/saveuser', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: params
      }).then(r => r.text()).then(msg => {
        console.log(msg);
        // Fetch the actual saved name from server to ensure it matches
        fetch('/getname?slot=' + slot).then(r => r.text()).then(savedName => {
          const btn = document.getElementById('user' + slot);
          if (!savedName.includes('(Empty)')) {
            btn.textContent = '⭐ ' + savedName;
            btn.style.opacity = '1';
          }
        });
        // Clear the name input
        document.getElementById('presetName').value = '';
      }).catch(err => {
        console.error('Error saving preset:', err);
      });
    }

    function toggleLFO(lfoNum) {
      if (lfoNum === 1) {
        lfo1Enabled = !lfo1Enabled;
        const btn = document.getElementById('lfobtn');
        if (lfo1Enabled) {
          btn.textContent = 'LFO1: ON';
          btn.classList.add('active');
        } else {
          btn.textContent = 'LFO1: OFF';
          btn.classList.remove('active');
        }
        fetch('/param?name=lfoenable&val=' + (lfo1Enabled ? '1' : '0'));
      } else if (lfoNum === 2) {
        lfo2Enabled = !lfo2Enabled;
        const btn = document.getElementById('lfo2btn');
        if (lfo2Enabled) {
          btn.textContent = 'LFO2: ON';
          btn.classList.add('active');
        } else {
          btn.textContent = 'LFO2: OFF';
          btn.classList.remove('active');
        }
        fetch('/param?name=lfo2enable&val=' + (lfo2Enabled ? '1' : '0'));
      }
    }

    function updateLFOWave(lfoNum, value) {
      clearPresetHighlight();
      if (lfoNum === 1) {
        fetch('/param?name=lfowave&val=' + value);
      } else if (lfoNum === 2) {
        fetch('/param?name=lfo2wave&val=' + value);
      }
    }

    function toggleFilterEnv() {
      filterEnvEnabled = !filterEnvEnabled;
      const btn = document.getElementById('fenvbtn');
      if (filterEnvEnabled) {
        btn.t)rawliteral";
const char HTML_CHUNK_7[] PROGMEM = R"rawliteral(extContent = 'Filter Env: ON';
        btn.classList.add('active');
      } else {
        btn.textContent = 'Filter Env: OFF';
        btn.classList.remove('active');
      }
      fetch('/param?name=fenvenable&val=' + (filterEnvEnabled ? '1' : '0'));
    }

    function toggleChorus() {
      chorusEnabled = !chorusEnabled;
      const btn = document.getElementById('chorusbtn');
      if (chorusEnabled) {
        btn.textContent = 'Chorus: ON';
        btn.classList.add('active');
      } else {
        btn.textContent = 'Chorus: OFF';
        btn.classList.remove('active');
      }
      fetch('/param?name=chorusenable&val=' + (chorusEnabled ? '1' : '0'));
    }

    let pingPongEnabled = false;
    function togglePingPong() {
      pingPongEnabled = !pingPongEnabled;
      const btn = document.getElementById('pingpongbtn');
      btn.textContent = pingPongEnabled ? 'Ping-Pong: ON' : 'Ping-Pong: OFF';
      btn.classList.toggle('active', pingPongEnabled);
      fetch('/param?name=pingpong&val=' + (pingPongEnabled ? '1' : '0'));
    }

    let portamentoEnabled = false;
    function togglePortamento() {
      portamentoEnabled = !portamentoEnabled;
      const btn = document.getElementById('portabtn');
      if (portamentoEnabled) {
        btn.textContent = 'Portamento: ON';
        btn.classList.add('active');
      } else {
        btn.textContent = 'Portamento: OFF';
        btn.classList.remove('active');
      }
      fetch('/param?name=portamento&val=' + (portamentoEnabled ? '1' : '0'));
    }

    function setDelayPreset(num) {
      // Remove active class from all delay buttons
      for (let i = 0; i < 5; i++) {
        const btn = document.getElementById('dpreset' + i);
        if (btn) btn.classList.remove('active');
      }
      // Add active class to selected button
      const selectedBtn = document.getElementById('dpreset' + num);
      if (selectedBtn) selectedBtn.classList.add('active');
      
      // Update slider values to match preset
      // Values must match STM32 setDelayPreset() function exactly
      const presets = [
        {time: 0.0,  feedback: 0.0, mix: 0.0},    // OFF
        {time: 0.08, feedback: 0.0, mix: 0.4},    // Slap
        {time: 0.15, feedback: 0.6, mix: 0.4},    // Short
        {time: 0.2,  feedback: 0.5, mix: 0.35},   // Medium
        {time: 0.12, feedback: 0.5, mix: 0.35}    // Rhythmic
      ];
      
      if (num >= 0 && num < presets.length) {
        const p = presets[num];
        // Convert to 0-127 slider range (matches STM32 CC12 decoding: 10 + value/127 * 390 ms)
        const timeVal = Math.max(0, Math.round((p.time * 1000 - 10) / 390 * 127));
        const fbVal = Math.round(p.feedback * 127);
        const mixVal = Math.round(p.mix * 127);
        
        // Update sliders
        document.getElementById('delaytime').value = timeVal;
        document.getElementById('delayfb').value = fbVal;
        document.getElementById('delaymix').value = mixVal;
        
        // Update number inputs
        const timeNum = document.getElementById('delaytime_num');
        const fbNum = document.getElementById('delayfb_num');
        const mixNum = document.getElementById('delaymix_num');
        
        // Display formula matches STM32 CC12 decoding: 10-400 ms
        const timeDisplay = Math.round(10 + (timeVal / 127.0) * 390);
        const fbDisplay = Math.round((fbVal / 127.0) * 100);
        const mixDisplay = Math.round((mixVal / 127.0) * 100);
        
        if (timeNum) timeNum.value = timeDisplay;
        if (fbNum) fbNum.value = fbDisplay;
        if (mixNum) mixNum.value = mixDisplay;
        
        // Update display labels
        const timeLabel = document.getElementById('delaytime_val');
        const fbLabel = document.getElementById('delayfb_val');
        const mixLabel = document.getElementById('delaymix_val');
        
        if (timeLabel) timeLabel.textContent = timeDisplay + ' ms';
        if (fbLabel) fbLabel.textContent = fbDisplay + '%';
        if (mixLabel) mixLabel.textContent = mixDisplay + '%';
      }
      
      // Send to ESP32
      fetch('/delaypreset?num=' + num);
    }

    function allNotesOff() {
      fetch('/panic');
    }


    // Volume lock functions
    let volumeUnlocked = false;
    let lastVolumeActivity = 0;
    const VOLUME_UNLOCK_MS = 5 * 60 * 1000;  // must match the ESP32 server timeout
    
    // Re-lock the slider on the page when the server's 5-minute unlock expires
    setInterval(() => {
      if (volumeUnlocked && Date.now() - lastVolumeActivity > VOLUME_UNLOCK_MS) relockVolumeUI();
    }, 5000);
    
    function promptVolumePass() {
      const pass = prompt('Enter 4-digit volume passcode:');
      if (pass) {
        fetch('/checkpass?pass=' + pass).then(r => {
          if (r.ok) return r.text();
          else throw new Error('Wrong passcode');
        }).then(savedVol => {
          volumeUnlocked = true;
          lastVolumeActivity = Date.now();
          document.getElementById('volume').disabled = false; setLevelControlsEnabled(true);
          document.getElementById('volumeLockBtn').textContent = '🔓 Unlocked';
          document.getElementById('volumeLockBtn').style.background = '#51cf66';
          document.getElementById('volumeLockBtn').onclick = null;
          document.getElementById('saveLockBtn').style.display = 'inline-block';
          document.getElementById('volume').value = savedVol;
          updateParam('volume', savedVol);
        }).catch(err => {
          alert('Wrong passcode!');
        });
      }
    }
    
    function updateVolume(val) {
      if (volumeUnlocked) {
        lastVolumeActivity = Date.now();
        updateParam('volume', val);
      }
    }
    
    // Show the saved volume ceiling when the page opens (the slider used to
    // start at its HTML default of 70% whatever was saved)
    window.addEventListener('load', () => {
      fetch('/getvolume').then(r => r.text()).then(v => {
        const vol = parseInt(v);
        if (!isNaN(vol)) {
          document.getElementById('volume').value = vol;
          document.getElementById('volume_val').textContent = Math.round((vol / 127) * 100) + '%';
        }
      });
    });
    
    // Called when the server refuses a volume change (unlock timed out)
    function relockVolumeUI() {
      volumeUnlocked = false;
      document.getElementById('volume').disabled = true; setLevelControlsEnabled(false);
      document.getElementById('volumeLockBtn').textContent = '🔒 Locked';
      document.getElementById('volumeLockBtn').style.background = '#ff6b6b';
      document.getElementById('volumeLockBtn').onclick = promptVolumePass;
      document.getElementById('saveLockBtn').style.display = 'none';
    }
    
    function saveAndLock() {
      const vol = document.getElementById('volume').value;
      fetch('/savevolume?val=' + vol).then(r => {
        if (r.status === 403) {
          relockVolumeUI();
          alert('🔒 Unlock timed out - enter the passcode again.');
          return;
        }
        if (r.ok) {
          volumeUnlocked = false;
          document.getElementById('volume').disabled = true; setLevelControlsEnabled(false);
          document.getElementById('volumeLockBtn').textContent = '🔒 Locked';
          document.getElementById('volumeLockBtn').style.background = '#ff6b6b';
          document.getElementById('volumeLockBtn').onclick = promptVolumePass;
          document.getElementById('saveLockBtn').style.display = 'none';
          alert('✅ Volume saved and locked!');
        }
      }).catch(() => {
        alert('❌ Failed to save volume!');
      });
    }
    
    function promptChangePass() {
      const oldPass = prompt('Enter current passcode:');
      if (oldPass) {
        const newPass = prompt('Enter new 4-digit passcode:');
        if (newPass && newPass.length === 4 && !isNaN(newPass)) {
          fetch('/changepass?old=' + oldPass + '&new=' + newPass).then(r => {
            if (r.ok) {
              alert('✅ Passcode changed successfully!');
            } else {
              throw new Error('Failed');
            }
          }).catch(() => {
            alert('❌ Wrong current passcode!');
          });
        } else {
          alert('Passcode must be 4 digits!');
        }
      }
    }

    // Screen switching
    let currentScreen = 'sound';

    function switchScreen(screenName) {
      // Hide all screens
      document.querySelectorAll('.screen').forEach(s => {
        s.classList.remove('active');
      });
      
      // Remove active from all tabs
      document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.classList.remove('active');
      });
      
      // Show selected screen
      document.getElementById(screenName + '-screen').classList.add('active');
      
      // Highlight active tab
      document.querySelectorAll('.tab-btn').forEach(btn => {
        if (btn.textContent.toLowerCase() === screenName) {
          btn.classList.add('active');
        }
      });
      
      currentScreen = screenName;
    }

    // Initialize parameters sequentially with delays to prevent buffer overflow
    window.onload = function() {
      // Show the synth's current settings. (The page used to send default
      // values for everything on every load, changing the sound mid-play.)
      fetch('/state').then(r => r.json()).then(st => applyState(st))
        .finally(() => checkUserPresets());
    };

)rawliteral";
const char HTML_CHUNK_8[] PROGMEM = R"rawliteral(
    // Check which user preset slots are used and get their names
    function checkUserPresets() {
      for (let i = 0; i < 12; i++) {
        fetch('/getname?slot=' + i).then(r => r.text()).then(name => {
          const btn = document.getElementById('user' + i);
          // If not empty, show with star and full opacity
          if (!name.includes('(Empty)')) {
            btn.textContent = '⭐ ' + name;
            btn.style.opacity = '1';
          } else {
            btn.textContent = name;  // "User X (Empty)"
            btn.style.opacity = '0.5';
          }
        });
      }
    }
  </script>
</body>
</html>
)rawliteral";







void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(HTML_CHUNK_0);
  server.sendContent(HTML_CHUNK_1);
  server.sendContent(HTML_CHUNK_2);
  server.sendContent(HTML_CHUNK_3);
  server.sendContent(HTML_CHUNK_4);
  server.sendContent(HTML_CHUNK_5);
  server.sendContent(HTML_CHUNK_6);
  server.sendContent(HTML_CHUNK_7);
  server.sendContent(HTML_CHUNK_8);
  server.sendContent("");
}

// ---- Server-side volume lock ----
// The passcode used to be checked only in the browser, so /cc?num=7 and
// /savevolume could be called directly to bypass it. Now the server itself
// refuses volume changes unless the correct passcode was entered recently.
const unsigned long VOLUME_UNLOCK_MS = 5UL * 60UL * 1000UL;  // 5 minutes
unsigned long volumeUnlockedAt = 0;
bool volumeUnlocked = false;

bool isVolumeUnlocked() {
  if (volumeUnlocked && (millis() - volumeUnlockedAt) > VOLUME_UNLOCK_MS) {
    volumeUnlocked = false;  // timed out
  }
  return volumeUnlocked;
}

void handleCC() {
  if (server.hasArg("num") && server.hasArg("val")) {
    int cc = server.arg("num").toInt();
    int val = server.arg("val").toInt();
    if (cc == 7) {                       // volume ceiling
      if (!isVolumeUnlocked()) {
        server.send(403, "text/plain", "Volume locked");
        return;
      }
      volumeUnlockedAt = millis();       // still adjusting: extend the unlock
    }
    sendCC(cc, val);
    trackCC(cc, val);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleParam() {
  if (server.hasArg("name") && server.hasArg("val")) {
    String param = server.arg("name");
    int val = server.arg("val").toInt();
    if (param == "level" && !isVolumeUnlocked()) {   // level can raise loudness
      server.send(403, "text/plain", "Volume locked");
      return;
    }
    sendParam(param, val);
    trackParam(param, val);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleWaveform() {
  if (server.hasArg("osc")) {
    int osc = server.arg("osc").toInt();
    if (osc == 1) {
      sendCC(50, 127);
    } else if (osc == 2) {
      sendCC(51, 127);
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameter");
  }
}

// ---- Current synth state ----
// The web controller keeps a copy of every setting it has sent, so the page
// can show the real state when it is opened or reloaded (it used to reset
// the synth to defaults on every page load).
Preset current;
bool currentIsFactory = true;

// Sends every parameter of a preset to the STM32 and makes it the current state
void applyPreset(const Preset& p) {
  sendParam("osc1wave", p.osc1Wave); delay(15);
  sendParam("osc2wave", p.osc2Wave); delay(15);
  sendParam("osc2detune", p.osc2Detune); delay(15);
  sendParam("osc2level", p.osc2Level); delay(15);
  sendParam("sublevel", p.subLevel); delay(15);
  sendParam("suboctave", p.subOctave); delay(15);
  sendParam("subwave", p.subWave); delay(15);
  sendParam("noise", p.noiseLevel); delay(15);
  sendParam("attack", p.attack); delay(15);
  sendParam("decay", p.decay); delay(15);
  sendParam("sustain", p.sustain); delay(15);
  sendParam("release", p.release); delay(15);
  sendCC(1, p.filterCutoff); delay(15);
  sendParam("resonance", p.resonance); delay(15);
  sendParam("fenvattack", p.filterEnvAttack); delay(15);
  sendParam("fenvdecay", p.filterEnvDecay); delay(15);
  sendParam("fenvsustain", p.filterEnvSustain); delay(15);
  sendParam("fenvrelease", p.filterEnvRelease); delay(15);
  sendParam("fenvamount", p.filterEnvAmount); delay(15);
  sendParam("fenvenable", p.filterEnvEnabled ? 1 : 0); delay(15);
  sendParam("lforate", p.lfo1Rate); delay(15);
  sendParam("lfowave", p.lfo1Wave); delay(15);
  sendParam("lfopitch", p.lfo1Pitch); delay(15);
  sendParam("lfofilter", p.lfo1Filter); delay(15);
  sendParam("lfoamp", p.lfo1Amp); delay(15);
  sendParam("lfodetune", p.lfo1Detune); delay(15);
  sendParam("lfoenable", p.lfo1Enabled ? 1 : 0); delay(15);
  sendParam("lfo2rate", p.lfo2Rate); delay(15);
  sendParam("lfo2wave", p.lfo2Wave); delay(15);
  sendParam("lfo2pitch", p.lfo2Pitch); delay(15);
  sendParam("lfo2filter", p.lfo2Filter); delay(15);
  sendParam("lfo2amp", p.lfo2Amp); delay(15);
  sendParam("lfo2detune", p.lfo2Detune); delay(15);
  sendParam("lfo2enable", p.lfo2Enabled ? 1 : 0); delay(15);
  sendCC(12, p.delayTime); delay(15);
  sendCC(13, p.delayFeedback); delay(15);
  sendCC(91, p.delayMix); delay(15);
  sendParam("pingpong", p.pingPong ? 1 : 0); delay(15);
  sendCC(93, p.chorusMix); delay(15);
  sendParam("chorusenable", p.chorusEnabled ? 1 : 0); delay(15);
  chorusEnabled = p.chorusEnabled;
  sendParam("ringmod", p.ringMod); delay(15);
  sendParam("portatime", p.portamentoTime); delay(15);
  sendParam("portamento", p.portamentoOn ? 1 : 0); delay(15);
  sendParam("level", p.level); delay(15);
  current = p;
}

// All settings as JSON; keys are the web page's control ids
String presetJson(const Preset& p, const String& name) {
  String j = "{";
  j += "\"name\":\"" + name + "\"";
  j += ",\"osc1wave\":" + String(p.osc1Wave);
  j += ",\"osc2wave\":" + String(p.osc2Wave);
  j += ",\"osc2detune\":" + String(p.osc2Detune);
  j += ",\"osc2level\":" + String(p.osc2Level);
  j += ",\"sublevel\":" + String(p.subLevel);
  j += ",\"suboctave\":" + String(p.subOctave);
  j += ",\"subwave\":" + String(p.subWave);
  j += ",\"noise\":" + String(p.noiseLevel);
  j += ",\"attack\":" + String(p.attack);
  j += ",\"decay\":" + String(p.decay);
  j += ",\"sustain\":" + String(p.sustain);
  j += ",\"release\":" + String(p.release);
  j += ",\"filter\":" + String(p.filterCutoff);
  j += ",\"resonance\":" + String(p.resonance);
  j += ",\"fenvenable\":" + String(p.filterEnvEnabled ? 1 : 0);
  j += ",\"fenvattack\":" + String(p.filterEnvAttack);
  j += ",\"fenvdecay\":" + String(p.filterEnvDecay);
  j += ",\"fenvsustain\":" + String(p.filterEnvSustain);
  j += ",\"fenvrelease\":" + String(p.filterEnvRelease);
  j += ",\"fenvamount\":" + String(p.filterEnvAmount);
  j += ",\"lfoenable\":" + String(p.lfo1Enabled ? 1 : 0);
  j += ",\"lforate\":" + String(p.lfo1Rate);
  j += ",\"lfowave\":" + String(p.lfo1Wave);
  j += ",\"lfopitch\":" + String(p.lfo1Pitch);
  j += ",\"lfofilter\":" + String(p.lfo1Filter);
  j += ",\"lfoamp\":" + String(p.lfo1Amp);
  j += ",\"lfodetune\":" + String(p.lfo1Detune);
  j += ",\"lfo2enable\":" + String(p.lfo2Enabled ? 1 : 0);
  j += ",\"lfo2rate\":" + String(p.lfo2Rate);
  j += ",\"lfo2wave\":" + String(p.lfo2Wave);
  j += ",\"lfo2pitch\":" + String(p.lfo2Pitch);
  j += ",\"lfo2filter\":" + String(p.lfo2Filter);
  j += ",\"lfo2amp\":" + String(p.lfo2Amp);
  j += ",\"lfo2detune\":" + String(p.lfo2Detune);
  j += ",\"delaytime\":" + String(p.delayTime);
  j += ",\"delayfb\":" + String(p.delayFeedback);
  j += ",\"delaymix\":" + String(p.delayMix);
  j += ",\"pingpong\":" + String(p.pingPong ? 1 : 0);
  j += ",\"chorusmix\":" + String(p.chorusMix);
  j += ",\"chorusenable\":" + String(p.chorusEnabled ? 1 : 0);
  j += ",\"ringmod\":" + String(p.ringMod);
  j += ",\"portamento\":" + String(p.portamentoOn ? 1 : 0);
  j += ",\"portatime\":" + String(p.portamentoTime);
  j += ",\"level\":" + String(p.level);
  j += "}";
  return j;
}

// Keeps 'current' in step with individual changes made on the page
void trackParam(const String& n, int v) {
  if      (n == "osc1wave")    current.osc1Wave = v;
  else if (n == "osc2wave")    current.osc2Wave = v;
  else if (n == "osc2detune")  current.osc2Detune = v;
  else if (n == "osc2level")   current.osc2Level = v;
  else if (n == "sublevel")    current.subLevel = v;
  else if (n == "suboctave")   current.subOctave = v;
  else if (n == "subwave")     current.subWave = v;
  else if (n == "noise")       current.noiseLevel = v;
  else if (n == "attack")      current.attack = v;
  else if (n == "decay")       current.decay = v;
  else if (n == "sustain")     current.sustain = v;
  else if (n == "release")     current.release = v;
  else if (n == "resonance")   current.resonance = v;
  else if (n == "fenvenable")  current.filterEnvEnabled = (v > 0);
  else if (n == "fenvattack")  current.filterEnvAttack = v;
  else if (n == "fenvdecay")   current.filterEnvDecay = v;
  else if (n == "fenvsustain") current.filterEnvSustain = v;
  else if (n == "fenvrelease") current.filterEnvRelease = v;
  else if (n == "fenvamount")  current.filterEnvAmount = v;
  else if (n == "lfoenable")   current.lfo1Enabled = (v > 0);
  else if (n == "lforate")     current.lfo1Rate = v;
  else if (n == "lfowave")     current.lfo1Wave = v;
  else if (n == "lfopitch")    current.lfo1Pitch = v;
  else if (n == "lfofilter")   current.lfo1Filter = v;
  else if (n == "lfoamp")      current.lfo1Amp = v;
  else if (n == "lfodetune")   current.lfo1Detune = v;
  else if (n == "lfo2enable")  current.lfo2Enabled = (v > 0);
  else if (n == "lfo2rate")    current.lfo2Rate = v;
  else if (n == "lfo2wave")    current.lfo2Wave = v;
  else if (n == "lfo2pitch")   current.lfo2Pitch = v;
  else if (n == "lfo2filter")  current.lfo2Filter = v;
  else if (n == "lfo2amp")     current.lfo2Amp = v;
  else if (n == "lfo2detune")  current.lfo2Detune = v;
  else if (n == "pingpong")    current.pingPong = (v > 0);
  else if (n == "chorusenable") { current.chorusEnabled = (v > 0); chorusEnabled = current.chorusEnabled; }
  else if (n == "ringmod")     current.ringMod = v;
  else if (n == "portamento")  current.portamentoOn = (v > 0);
  else if (n == "portatime")   current.portamentoTime = v;
  else if (n == "level")       current.level = v;
}

void trackCC(int cc, int v) {
  if      (cc == 1)  current.filterCutoff = v;
  else if (cc == 12) current.delayTime = v;
  else if (cc == 13) current.delayFeedback = v;
  else if (cc == 91) current.delayMix = v;
  else if (cc == 93) current.chorusMix = v;
}

void handleState() {
  server.send(200, "application/json", presetJson(current, "Current"));
}

// ---- Factory preset levels ----
// Factory presets live in program flash and can't be edited from the page,
// but their level can: a level saved from the page is stored in the settings
// storage ("fl<n>") and overrides the level in the table above.
int factoryLevel(int num) {
  preferences.begin("synth", true);
  int lv = preferences.getInt(("fl" + String(num)).c_str(), presets[num].level);
  preferences.end();
  return lv;
}

void handleSaveFactoryLevel() {
  if (!isVolumeUnlocked()) { server.send(403, "text/plain", "Volume locked"); return; }
  if (!server.hasArg("num") || !server.hasArg("val")) { server.send(400, "text/plain", "Missing parameters"); return; }
  int num = server.arg("num").toInt();
  int val = constrain(server.arg("val").toInt(), 0, 127);
  if (num < 0 || num >= numPresets) { server.send(400, "text/plain", "Invalid preset"); return; }
  preferences.begin("synth", false);
  preferences.putInt(("fl" + String(num)).c_str(), val);
  preferences.end();
  server.send(200, "text/plain", "Level saved");
}

void handleResetFactoryLevel() {
  if (!isVolumeUnlocked()) { server.send(403, "text/plain", "Volume locked"); return; }
  if (!server.hasArg("num")) { server.send(400, "text/plain", "Missing preset"); return; }
  int num = server.arg("num").toInt();
  if (num < 0 || num >= numPresets) { server.send(400, "text/plain", "Invalid preset"); return; }
  preferences.begin("synth", false);
  preferences.remove(("fl" + String(num)).c_str());
  preferences.end();
  int lv = presets[num].level;
  sendParam("level", lv);
  current.level = lv;
  server.send(200, "text/plain", String(lv));
}

void handlePreset() {
  if (server.hasArg("num")) {
    int num = server.arg("num").toInt();
    if (num >= 0 && num < numPresets) {
      Preset p = presets[num];
      p.level = factoryLevel(num);
      Serial.print("Loading preset: ");
      Serial.println(p.name);
      applyPreset(p);
      server.send(200, "application/json", presetJson(p, String(p.name)));
    } else {
      server.send(400, "text/plain", "Invalid preset number");
    }
  } else {
    server.send(400, "text/plain", "Missing preset number");
  }
}

void handleSaveUserPreset() {
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    
    if (slot >= 0 && slot < NUM_USER_PRESETS) {
      // Create preset from POST data (all current settings)
      Preset userPreset;
      
      // Get preset name from request (or default)
      String presetName;
      if (server.hasArg("name") && server.arg("name").length() > 0) {
        presetName = server.arg("name");
      } else {
        presetName = "User " + String(slot + 1);
      }
      userPreset.name = presetName.c_str();
      
      // Get all parameters from request
      userPreset.osc1Wave = server.arg("osc1Wave").toInt();
      userPreset.osc2Wave = server.arg("osc2Wave").toInt();
      userPreset.osc2Detune = server.arg("osc2Detune").toInt();
      userPreset.osc2Level = server.arg("osc2Level").toInt();
      userPreset.subLevel = server.arg("subLevel").toInt();
      userPreset.subOctave = server.arg("subOctave").toInt();
      userPreset.subWave = server.arg("subWave").toInt();
      userPreset.noiseLevel = server.arg("noiseLevel").toInt();
      userPreset.attack = server.arg("attack").toInt();
      userPreset.decay = server.arg("decay").toInt();
      userPreset.sustain = server.arg("sustain").toInt();
      userPreset.release = server.arg("release").toInt();
      userPreset.filterCutoff = server.arg("filterCutoff").toInt();
      userPreset.filterEnvEnabled = server.arg("filterEnvEnabled") == "true";
      userPreset.filterEnvAttack = server.arg("filterEnvAttack").toInt();
      userPreset.filterEnvDecay = server.arg("filterEnvDecay").toInt();
      userPreset.filterEnvSustain = server.arg("filterEnvSustain").toInt();
      userPreset.filterEnvRelease = server.arg("filterEnvRelease").toInt();
      userPreset.filterEnvAmount = server.arg("filterEnvAmount").toInt();
      userPreset.lfo1Enabled = server.arg("lfo1Enabled") == "true";
      userPreset.lfo1Rate = server.arg("lfo1Rate").toInt();
      userPreset.lfo1Wave = server.arg("lfo1Wave").toInt();
      userPreset.lfo1Pitch = server.arg("lfo1Pitch").toInt();
      userPreset.lfo1Filter = server.arg("lfo1Filter").toInt();
      userPreset.lfo1Amp = server.arg("lfo1Amp").toInt();
      userPreset.lfo1Detune = server.arg("lfo1Detune").toInt();
      userPreset.lfo2Enabled = server.arg("lfo2Enabled") == "true";
      userPreset.lfo2Rate = server.arg("lfo2Rate").toInt();
      userPreset.lfo2Wave = server.arg("lfo2Wave").toInt();
      userPreset.lfo2Pitch = server.arg("lfo2Pitch").toInt();
      userPreset.lfo2Filter = server.arg("lfo2Filter").toInt();
      userPreset.lfo2Amp = server.arg("lfo2Amp").toInt();
      userPreset.lfo2Detune = server.arg("lfo2Detune").toInt();
      userPreset.delayMix = server.arg("delayMix").toInt();
      userPreset.chorusMix = server.arg("chorusMix").toInt();
      userPreset.chorusEnabled = server.arg("chorusEnabled") == "true";
      userPreset.ringMod = server.arg("ringMod").toInt();
      userPreset.portamentoOn = server.arg("portamentoOn") == "true";
      userPreset.portamentoTime = server.arg("portamentoTime").toInt();
      userPreset.resonance = server.arg("resonance").toInt();
      userPreset.delayTime = server.arg("delayTime").toInt();
      userPreset.delayFeedback = server.arg("delayFeedback").toInt();
      userPreset.pingPong = server.arg("pingPong") == "true";
      userPreset.level = server.arg("level").length() ? server.arg("level").toInt() : 64;
      
      saveUserPreset(slot, userPreset);
      server.send(200, "text/plain", "User preset saved to slot " + String(slot));
    } else {
      server.send(400, "text/plain", "Invalid slot number");
    }
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleLoadUserPreset() {
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if (slot >= 0 && slot < NUM_USER_PRESETS) {
      Preset userPreset;
      if (loadUserPreset(slot, userPreset)) {
        applyPreset(userPreset);
        server.send(200, "application/json", presetJson(userPreset, "User " + String(slot + 1)));
      } else {
        server.send(404, "text/plain", "Empty slot");
      }
    } else {
      server.send(400, "text/plain", "Invalid slot number");
    }
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleCheckUserPreset() {
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    bool used = isUserPresetUsed(slot);
    server.send(200, "text/plain", used ? "true" : "false");
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleGetUserPresetName() {
  if (server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    String name = getUserPresetName(slot);
    server.send(200, "text/plain", name);
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleDelayPreset() {
  if (server.hasArg("num")) {
    int preset = server.arg("num").toInt();
    sendParam("delaypreset", preset);  // Send actual preset number
    // Track the resulting delay settings (same values as the page's buttons)
    const float t[]  = {0.0f, 0.08f, 0.15f, 0.2f, 0.12f};
    const float fb[] = {0.0f, 0.0f, 0.6f, 0.5f, 0.5f};
    const float mx[] = {0.0f, 0.4f, 0.4f, 0.35f, 0.35f};
    if (preset >= 0 && preset < 5) {
      int tv = (int)roundf((t[preset] * 1000.0f - 10.0f) / 390.0f * 127.0f);
      current.delayTime = tv < 0 ? 0 : tv;
      current.delayFeedback = (int)roundf(fb[preset] * 127.0f);
      current.delayMix = (int)roundf(mx[preset] * 127.0f);
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameter");
  }
}

void handleChorus() {
  sendButton(2);
  chorusEnabled = !chorusEnabled;
  current.chorusEnabled = chorusEnabled;
  server.send(200, "text/plain", "OK");
}

void handleNote() {
  if (server.hasArg("num") && server.hasArg("vel")) {
    int note = server.arg("num").toInt();
    int vel = server.arg("vel").toInt();
    
    if (vel > 0) {
      // Note on
      String cmd = "NOTE," + String(note) + "," + String(vel) + "\n";
      Serial2.print(cmd);
    } else {
      // Note off
      String cmd = "NOTEOFF," + String(note) + "\n";
      Serial2.print(cmd);
    }
    
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleKeyboard() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(KEYBOARD_PAGE);
  server.sendContent("");
}

void handlePanic() {
  sendCC(120, 0);  // All Sound Off: immediate silence (CC 123 now releases gracefully)
  server.send(200, "text/plain", "OK");
}

void handleCheckPass() {
  if (server.hasArg("pass")) {
    String inputPass = server.arg("pass");
    preferences.begin("synth", true);
    String correctPass = preferences.getString("volumePass", DEFAULT_VOLUME_PASSCODE);
    preferences.end();
    
    if (inputPass == correctPass) {
      volumeUnlocked = true;
      volumeUnlockedAt = millis();
      preferences.begin("synth", true);
      int vol = preferences.getInt("volume", 89);
      preferences.end();
      server.send(200, "text/plain", String(vol));
    } else {
      server.send(403, "text/plain", "Wrong passcode");
    }
  } else {
    server.send(400, "text/plain", "Missing passcode");
  }
}

// Returns the saved volume ceiling (0-127) so the page can display it.
// Read-only: changing the volume still requires the passcode.
void handleGetVolume() {
  preferences.begin("synth", true);
  int vol = preferences.getInt("volume", 89);
  preferences.end();
  server.send(200, "text/plain", String(vol));
}

void handleSaveVolume() {
  if (!isVolumeUnlocked()) {
    server.send(403, "text/plain", "Volume locked");
    return;
  }
  if (server.hasArg("val")) {
    int vol = server.arg("val").toInt();
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    preferences.begin("synth", false);
    preferences.putInt("volume", vol);
    preferences.end();
    
    sendCC(7, vol);
    volumeUnlocked = false;              // "Save & Lock" locks the server too
    server.send(200, "text/plain", "Volume saved");
  } else {
    server.send(400, "text/plain", "Missing value");
  }
}

void handleChangePass() {
  if (server.hasArg("old") && server.hasArg("new")) {
    String oldPass = server.arg("old");
    String newPass = server.arg("new");
    
    preferences.begin("synth", true);
    String correctPass = preferences.getString("volumePass", DEFAULT_VOLUME_PASSCODE);
    preferences.end();
    
    bool newPassValid = (newPass.length() == 4);
    for (unsigned int i = 0; i < newPass.length(); i++) {
      if (!isDigit(newPass[i])) newPassValid = false;
    }
    if (!newPassValid) {
      server.send(400, "text/plain", "Passcode must be 4 digits");
      return;
    }
    
    if (oldPass == correctPass) {
      preferences.begin("synth", false);
      preferences.putString("volumePass", newPass);
      preferences.end();
      server.send(200, "text/plain", "Passcode changed");
    } else {
      server.send(403, "text/plain", "Wrong old passcode");
    }
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void setup() {
  Serial.begin(115200);
  
  delay(1000);
  Serial.println("\n==========================================");
  Serial.println("  ESP32 #1 - Web Controller (UART)");
  Serial.println("  Simple & Reliable!");
  Serial.println("==========================================");
  
  // Initialize UART to STM32
  Serial2.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);  // RX=GPIO16, TX=GPIO17
  Serial.println("✓ UART initialized (GPIO17 → STM32 PA10)");
  
  initState();
  
  // Load saved volume and passcode from preferences
  preferences.begin("synth", true);
  int savedVolume = preferences.getInt("volume", 89);  // Default 70%
  String savedPass = preferences.getString("volumePass", DEFAULT_VOLUME_PASSCODE);
  preferences.end();
  
  Serial.print("✓ Loaded volume: ");
  Serial.print((savedVolume * 100) / 127);
  Serial.println("%");
  
  // Send saved volume to STM32
  delay(100);
  sendCC(7, savedVolume);
  delay(100);
  
  // Initialize WiFi in AP mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  
  IPAddress IP = WiFi.softAPIP();
  
  Serial.print("✓ WiFi AP started: ");
  Serial.println(ssid);
  Serial.print("✓ IP Address: ");
  Serial.println(IP);
  Serial.println("==========================================");
  
  if (MDNS.begin("synth")) {
    Serial.println("✓ mDNS: http://synth.local");
  }
  
  server.on("/", handleRoot);
  server.on("/cc", handleCC);
  server.on("/param", handleParam);
  server.on("/waveform", handleWaveform);
  server.on("/preset", handlePreset);
  server.on("/saveuser", HTTP_POST, handleSaveUserPreset);
  server.on("/loaduser", handleLoadUserPreset);
  server.on("/checkuser", handleCheckUserPreset);
  server.on("/getname", handleGetUserPresetName);
  server.on("/delaypreset", handleDelayPreset);
  server.on("/chorus", handleChorus);
  server.on("/panic", handlePanic);
  server.on("/keyboard", handleKeyboard);
  server.on("/note", handleNote);
  server.on("/checkpass", handleCheckPass);
  server.on("/getvolume", handleGetVolume);
  server.on("/state", handleState);
  server.on("/savefactorylevel", handleSaveFactoryLevel);
  server.on("/resetfactorylevel", handleResetFactoryLevel);
  server.on("/savevolume", handleSaveVolume);
  server.on("/changepass", handleChangePass);
  
  server.begin();
  Serial.println("✓ Web server started");
  Serial.println("==========================================");
#if USING_DEFAULT_SECRETS
  Serial.println("!! WARNING: secrets.h not found - the Wi-Fi password and default");
  Serial.println("!! passcode are the PUBLIC defaults from secrets.example.h.");
  Serial.println("!! Copy secrets.example.h to secrets.h and set your own values.");
#endif
  Serial.println("Ready! Open http://192.168.4.1");
  Serial.println();
  
  // Start from a known sound: send the Init preset so the synth and the
  // page's state agree from power-on
  delay(300);
  Preset initPreset = presets[0];
  initPreset.level = factoryLevel(0);
  applyPreset(initPreset);
  Serial.println("✓ Init preset sent to STM32");
}

void loop() {
  server.handleClient();
  
  // Status update every 30 seconds
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 30000) {
    lastStatus = millis();
    Serial.print("📊 Commands sent: ");
    Serial.println(commandsSent);
  }
  
  delay(2);
}
