/*
 * ESP32 #3 - WiFi Web Controller with UART (Phase 2 - TESTING VERSION)
 * For STM32 MIDI Synthesizer - COMPLETE WEB INTERFACE
 * 
 * Features:
 * - WiFi Access Point with full web interface
 * - UART communication with STM32 (simple & reliable!)
 * - 4-screen tabbed interface (Sound, Modulation, FX, Presets)
 * - 15 factory presets + 8 user preset slots
 * 
 * Hardware Connections:
 * ESP32 #3 GPIO17 (TX) → STM32 PA10 (UART1 RX)
 * GND → GND (CRITICAL!)
 * 
 * IMPORTANT: Remove 5-pin MIDI circuit from PA10 before connecting ESP32 #3!
 * 
 * WiFi:
 * SSID: STM32_Synth
 * Password: synth2024
 * IP: 192.168.4.1
 * 
 * UART Protocol:
 * - Simple one-way UART at 115200 baud
 * - Commands: "PARAM,name,value\n" or "CC,num,value\n"
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

const char* ssid = "STM32_Synth";
const char* password = "synth2024";

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
};

// Forward declarations for user preset functions
void saveUserPreset(int slot, const Preset& preset);
bool loadUserPreset(int slot, Preset& preset);
bool isUserPresetUsed(int slot);
String getUserPresetName(int slot);
void getCurrentSettings(Preset& preset);

const Preset presets[] = {
  // Format: Name, Osc1Wave, Osc2Wave, Osc2Detune, Osc2Level, SubLevel, SubOctave, SubWave, NoiseLevel,
  //         Attack, Decay, Sustain, Release, FilterCutoff,
  //         FEnvEnabled, FEnvA, FEnvD, FEnvS, FEnvR, FEnvAmount,
  //         LFO1Enabled, LFO1Rate, LFO1Wave, LFO1Pitch, LFO1Filter, LFO1Amp, LFO1Detune,
  //         LFO2Enabled, LFO2Rate, LFO2Wave, LFO2Pitch, LFO2Filter, LFO2Amp, LFO2Detune,
  //         DelayMix, ChorusMix, ChorusEnabled
  
    // 1. Init/Default - Clean slate
  {"Init/Default", 1, 1, 9, 89, 0, 1, 0, 0,
   2, 13, 89, 13, 64,
   false, 2, 13, 64, 13, 64,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, false, 0},
  
  // 2. Fat Bass - Sub oscillator showcase
  {"Fat Bass", 1, 1, 6, 89, 89, 1, 0, 6,
   1, 19, 76, 10, 40,
   true, 1, 13, 25, 13, 76,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, false, 0},
  
  // 3. TB-303 Acid - Filter envelope squelch
  {"TB-303 Acid", 1, 1, 4, 89, 51, 1, 0, 0,
   1, 19, 38, 13, 35,
   true, 1, 25, 25, 13, 127,
   true, 6, 1, 0, 38, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, false, 0},
  
  // 4. Bell Chime - Ring mod for metallic overtones
  {"Bell Chime", 0, 3, 25, 89, 0, 1, 0, 0,
   1, 81, 89, 31, 95,
   false, 2, 13, 64, 13, 64,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   32, 38, false, 38, false, 0},
  
  // 5. Synth Lead - Portamento for smooth glides
  {"Synth Lead", 1, 1, 10, 89, 38, 1, 0, 0,
   1, 13, 102, 19, 50,
   false, 2, 13, 64, 13, 64,
   true, 25, 3, 0, 102, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, true, 50},
  
  // 6. Analog Pad - Dual LFOs + chorus
  {"Analog Pad", 1, 3, 19, 89, 25, 1, 0, 6,
   38, 25, 114, 102, 55,
   true, 51, 38, 89, 51, 64,
   true, 4, 0, 0, 51, 19, 0,
   true, 9, 1, 0, 0, 0, 38,
   0, 45, true, 0, false, 0},
  
  // 7. Robot Voice - Ring mod for robotic tones
  {"Robot Voice", 2, 2, 0, 76, 25, 1, 0, 13,
   6, 19, 76, 13, 45,
   true, 1, 25, 0, 13, 64,
   true, 38, 2, 25, 25, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 51, false, 0},
  
  // 8. String Ensemble - Chorus + detune
  {"String Ensemble", 1, 1, 15, 89, 19, 1, 0, 0,
   25, 19, 108, 95, 70,
   false, 2, 13, 64, 13, 64,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   13, 45, true, 0, false, 0},
  
  // 9. Metallic Pad - Ring mod + chorus
  {"Metallic Pad", 1, 3, 19, 76, 38, 1, 0, 6,
   38, 38, 95, 89, 60,
   true, 64, 51, 76, 64, 64,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 45, true, 25, false, 0},
  
  // 10. Pluck Bass - Fast decay + sub
  {"Pluck Bass", 1, 3, 6, 89, 76, 1, 0, 0,
   1, 15, 0, 6, 55,
   true, 1, 10, 0, 6, 76,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, false, 0},
  
  // 11. Wobble Bass - LFO → filter (slow rate)
  {"Wobble Bass", 1, 2, 0, 89, 102, 2, 0, 0,
   1, 6, 127, 13, 40,
   false, 2, 13, 64, 13, 64,
   true, 6, 2, 0, 114, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, false, 0},
  
  // 12. Sitar - Ring mod for eastern sound
  {"Sitar", 1, 1, 12, 89, 38, 1, 0, 0,
   1, 51, 25, 31, 65,
   true, 1, 64, 0, 25, 76,
   true, 76, 0, 19, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   13, 0, false, 32, false, 0},
  
  // 13. Ambient Wash - Long attack + delay + chorus
  {"Ambient Wash", 1, 2, 32, 89, 32, 1, 0, 13,
   51, 38, 95, 121, 50,
   true, 76, 51, 76, 64, 64,
   true, 3, 0, 0, 38, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   25, 45, true, 0, false, 0},
  
  // 14. Glide Bass - Portamento bass
  {"Glide Bass", 1, 1, 6, 89, 76, 1, 0, 6,
   1, 19, 76, 10, 40,
   true, 1, 13, 25, 13, 76,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, true, 80},
  
  // 15. 808 Bass - Sub dominant + click
  {"808 Bass", 0, 0, 0, 25, 114, 1, 0, 6,
   1, 25, 0, 13, 45,
   true, 1, 13, 0, 13, 64,
   false, 41, 0, 0, 0, 0, 0,
   false, 16, 0, 0, 0, 0, 0,
   0, 0, false, 0, false, 0}
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
  
  // Load preset name
  String name = preferences.getString((prefix + "nam").c_str(), "User");
  preset.name = name.c_str();
  
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
            <input type="number" id="attack_num" min="1" max="1000" step="1" value="16" onchange="updateParam('attack', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="attack_val">16 ms</div>
        </div>
        <div class="control">
          <label>Decay (1-1000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="decay" min="0" max="127" value="13" oninput="updateParam('decay', this.value)" style="flex: 1;">
            <input type="number" id="decay_num" min="1" max="1000" step="1" value="103" onchange="updateParam('decay', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
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
            <input type="number" id="fenvattack_num" min="1" max="1000" step="1" value="16" onchange="updateParam('fenvattack', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
          </div>
          <div class="value-display" id="fenvattack_val">16 ms</div>
        </div>
        <div class="control">
          <label>Filter Decay (1-1000ms)</label>
          <div style="display: flex; gap: 10px; align-items: center;">
            <input type="range" id="fenvdecay" min="0" max="127" value="13" oninput="updateParam('fenvdecay', this.value)" style="flex: 1;">
            <input type="number" id="fenvdecay_num" min="1" max="1000" step="1" value="103" onchange="updateParam('fenvdecay', this.value, true)" style="width: 65px; padding: 5px; border-radius: 4px; border: 2px solid #dee2e6; font-size: 0.85em;">
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
      <p><strong>WiFi:</strong> STM32_Synth | <strong>Password:</strong> synth2024</p>
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
          return Math.round(((displayVal - 1) / 999.0) * 127);
          
        case 'release':
        case 'fenvrelease':
          return Math.round(((displayVal - 1) / 2999.0) * 127);

        case 'delaytime':
          return Math.round((displayVal / 500.0) * 127);

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
    
    function updateParam(param, value, fromNumberInput = false) {
      if (updating) return;
      updating = true;
      
      clearPresetHighlight();
      
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
          displayValue = Math.round(1 + (value / 127.0) * 999);
          endpoint = '/param?name=' + param + '&val=' + value;
          break;
        case 'decay':
        case 'fenvdecay':
          displayValue = Math.round(1 + (value / 127.0) * 999);
          endpoint = '/param?name=' + param + '&val=' + value;
          break;
        case 'sustain':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=sustain&val=' + value;
          break;
        case 'release':
        case 'fe)rawliteral";
const char HTML_CHUNK_5[] PROGMEM = R"rawliteral(nvrelease':
          displayValue = Math.round(1 + (value / 127.0) * 2999);
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
          displayValue = Math.round((value / 127.0) * 500);  // 0-500ms matches STM32
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
          displayValue = Math.round(1 + (value / 127.0) * 999);
          endpoint = '/param?name=fenvattack&val=' + value;
          break;
        case 'fenvdecay':
          displayValue = Math.round(1 + (value / 127.0) * 999);
          endpoint = '/param?name=fenvdecay&val=' + value;
          break;
        case 'fenvsustain':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=fenvsustain&val=' + value;
          break;
        case 'fenvrelease':
          displayValue = Math.round(1 + (value / 127.0) * 2999);
          endpoint = '/param?name=fenvrelease&val=' + value;
          break;
        case 'fenvamount':
          displayValue = Math.round((value / 127.0) * 100);
          endpoint = '/param?name=fenvamount&val=' + value;
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
      
      updateTimers[param] = setTimeout(() => {
        fetch(endpoint);
        delete updateTimers[param];
      }, 50);  // 50ms debounce
      
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

    function loadPreset(num) {
      fetch('/preset?num=' + num).then(r => r.json()).then(preset => {
        // Update oscillator waveforms
        osc1wave = preset.osc1;
        osc2wave = preset.osc2;
        document.getElementById('osc1wave').value = osc1wave;
        document.getElementById('osc2wave').value = osc2wave;
        
        // Update oscillator parameters
        updateParam('osc2detune', preset.osc2Detune);
        updateParam('osc2level', preset.osc2Level);
        
        // Update sub oscillator
        updateParam('sublevel', preset.subLevel);
        updateParam('suboctave', preset.subOctave);
        updateParam('subwave', preset.subWave);
        
        // Update noise
        updateParam('noise', preset.noiseLevel);
        
        // Update ADSR
        updateParam('attack', preset.attack);
        updateParam('decay', preset.decay);
        updateParam('sustain', preset.sustain);
        updateParam('release', preset.release);
        
        // Update filter
        updateParam('filter', preset.filterCutoff);
        
        // Update filter envelope state
        filterEnvEnabled = preset.filterEnvEnabled;
        const fenvBtn = document.getElementById('fenvbtn');
        if (filterEnvEnabled) {
          fenvBtn.textContent = 'Filter Env: ON';
          fenvBtn.classList.add('active');
        } else {
          fenvBtn.textContent = 'Filter Env: OFF';
          fenvBtn.classList.remove('active');
        }
        
        // Update LFO1 state
        lfo1Enabled = preset.lfo1Enabled;
        const lfo1Btn = document.getElementById('lfobtn');
        if (lfo1Enabled) {
          lfo1Btn.textContent = 'LFO1: ON';
          lfo1Btn.classList.add('active');
        } else {
          lfo1Btn.textContent = 'LFO1: OFF';
          lfo1Btn.classList.remove('active');
        }
        
        // Update LFO2 state
        lfo2Enabled = preset.lfo2Enabled;
        const lfo2Btn = document.getElementById('lfo2btn');
        if (lfo2Enabled) {
          lfo2Btn.textContent = 'LFO2: ON';
          lfo2Btn.classList.add('active');
        } else {
          lfo2Btn.textContent = 'LFO2: OFF';
          lfo2Btn.classList.remove('active');
        }
        
        // Update chorus state
        chorusEnabled = preset.chorusEnabled;
        const chorusBtn = document.getElementById('chorusbtn');
        if (chorusEnabled) {
          chorusBtn.textContent = 'Chorus: ON';
          chorusBtn.classList.add('active');
        } else {
          chorusBtn.textContent = 'Chorus: OFF';
          chorusBtn.classList.remove('active');
        }
        
        // Update effects
        updateParam('delaymix', preset.delayMix);
        updateParam('chorusmix', preset.chorusMix);
        
        // Update ring mod (always reset to preset value, default 0)
        updateParam('ringmod', preset.ringMod || 0);
        
        // Update portamento
        portamentoEnabled = preset.portamentoOn || false;
        const portaBtn = document.getElementById('portabtn');
        if (portamentoEnabled) {
          portaBtn.textContent = 'Portamento: ON';
          portaBtn.classList.add('active');
        } else {
          portaBtn.textContent = 'Portamento: OFF';
          portaBtn.classList.remove('active');
        }
        fetch('/param?name=portamento&val=' + (portamentoEnabled ? '1' : '0'));
        updateParam('portatime', preset.portamentoTime || 0);
        
        // Highlight the active preset
        highlightPreset(num);
      });
    }

    function loadUserPreset(slot) {
      fetch('/loaduser?slot=' + slot).then(r => {
        if (r.ok) {
          return r.json();
        } else {
          console.log('Preset slot empty');
          throw new Error('Empty slot');
        }
      }).then(preset => {
        // Same loading logic as factory presets
        osc1wave = preset.osc1;
        osc2wave = preset.osc2;
        document.getElementById('osc1wave').value = osc1wave;
        document.getElementById('osc2wave').value = osc2wave;
    )rawliteral";
const char HTML_CHUNK_6[] PROGMEM = R"rawliteral(    
        updateParam('osc2detune', preset.osc2Detune);
        updateParam('osc2level', preset.osc2Level);
        updateParam('sublevel', preset.subLevel);
        updateParam('suboctave', preset.subOctave);
        updateParam('subwave', preset.subWave);
        updateParam('noise', preset.noiseLevel);
        updateParam('attack', preset.attack);
        updateParam('decay', preset.decay);
        updateParam('sustain', preset.sustain);
        updateParam('release', preset.release);
        updateParam('filter', preset.filterCutoff);
        
        filterEnvEnabled = preset.filterEnvEnabled;
        const fenvBtn = document.getElementById('fenvbtn');
        if (filterEnvEnabled) {
          fenvBtn.textContent = 'Filter Env: ON';
          fenvBtn.classList.add('active');
        } else {
          fenvBtn.textContent = 'Filter Env: OFF';
          fenvBtn.classList.remove('active');
        }
        
        lfo1Enabled = preset.lfo1Enabled;
        const lfo1Btn = document.getElementById('lfobtn');
        if (lfo1Enabled) {
          lfo1Btn.textContent = 'LFO1: ON';
          lfo1Btn.classList.add('active');
        } else {
          lfo1Btn.textContent = 'LFO1: OFF';
          lfo1Btn.classList.remove('active');
        }
        
        lfo2Enabled = preset.lfo2Enabled;
        const lfo2Btn = document.getElementById('lfo2btn');
        if (lfo2Enabled) {
          lfo2Btn.textContent = 'LFO2: ON';
          lfo2Btn.classList.add('active');
        } else {
          lfo2Btn.textContent = 'LFO2: OFF';
          lfo2Btn.classList.remove('active');
        }
        
        chorusEnabled = preset.chorusEnabled;
        const chorusBtn = document.getElementById('chorusbtn');
        if (chorusEnabled) {
          chorusBtn.textContent = 'Chorus: ON';
          chorusBtn.classList.add('active');
        } else {
          chorusBtn.textContent = 'Chorus: OFF';
          chorusBtn.classList.remove('active');
        }
        
        updateParam('delaymix', preset.delayMix);
        updateParam('chorusmix', preset.chorusMix);
        
        // Update ring mod (always reset to preset value, default 0)
        updateParam('ringmod', preset.ringMod || 0);
        
        // Update portamento
        portamentoEnabled = preset.portamentoOn || false;
        const portaBtn2 = document.getElementById('portabtn');
        if (portamentoEnabled) {
          portaBtn2.textContent = 'Portamento: ON';
          portaBtn2.classList.add('active');
        } else {
          portaBtn2.textContent = 'Portamento: OFF';
          portaBtn2.classList.remove('active');
        }
        fetch('/param?name=portamento&val=' + (portamentoEnabled ? '1' : '0'));
        updateParam('portatime', preset.portamentoTime || 0);
        
        // Preset loaded silently
      }).catch(err => {
        console.error('Error loading user preset:', err);
      });
    }

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
        // Convert to 0-127 range for sliders (matches STM32 decoding: value/127 * 0.5)
        const timeVal = Math.round(p.time / 0.5 * 127);  // 0-0.5s → 0-127
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
        
        // Display formula matches STM32 decoding: value/127 * 500ms
        const timeDisplay = Math.round((timeVal / 127.0) * 500);
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
    
    function promptVolumePass() {
      const pass = prompt('Enter 4-digit volume passcode:');
      if (pass) {
        fetch('/checkpass?pass=' + pass).then(r => {
          if (r.ok) return r.text();
          else throw new Error('Wrong passcode');
        }).then(savedVol => {
          volumeUnlocked = true;
          document.getElementById('volume').disabled = false;
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
        updateParam('volume', val);
      }
    }
    
    function saveAndLock() {
      const vol = document.getElementById('volume').value;
      fetch('/savevolume?val=' + vol).then(r => {
        if (r.ok) {
          volumeUnlocked = false;
          document.getElementById('volume').disabled = true;
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
      const params = [
        ['osc2detune', 10],
        ['osc2level', 89],
        ['sublevel', 0],
        ['suboctave', 1],
        ['subwave', 0],
        ['noise', 0],
        ['attack', 2],
        ['decay', 13],
        ['sustain', 89],
        ['release', 13],
        ['filter', 64],
        ['delaytime', 0],
        ['delayfb', 0],
        ['delaymix', 0],
        ['chorusmix', 76],
        ['lforate', 41],
        ['lfopitch', 0],
        ['lfofilter', 0],
        ['lfoamp', 0],
        ['lfodetune', 0],
        ['lfo2rate', 16],
        ['lfo2pitch', 0],
        ['lfo2filter', 0],
        ['lfo2amp', 0],
        ['lfo2detune', 0],
        ['fenvattack', 2],
        ['fenvdecay', 13],
        ['fenvsustain', 64],
        ['fenvrelease', 13],
        ['fenvamount', 64],
        ['ringmod', 0],
        ['portatime', 0]
      ];
      
      // Reset portamento toggle
      portamentoEnabled = false;
      const portaBtn = document.getElementById('portabtn');
      if (portaBtn) { portaBtn.textContent = 'Portamento: OFF'; portaBtn.classList.remove('a)rawliteral";
const char HTML_CHUNK_8[] PROGMEM = R"rawliteral(ctive'); }
      fetch('/param?name=portamento&val=0');
      
      let index = 0;
      function sendNext() {
        if (index < params.length) {
          updateParam(params[index][0], params[index][1]);
          index++;
          setTimeout(sendNext, 30);  // 30ms delay between each parameter
        } else {
          // After initialization, check which user preset slots are used
          checkUserPresets();
        }
      }
      
      sendNext();
    };

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

void handleCC() {
  if (server.hasArg("num") && server.hasArg("val")) {
    int cc = server.arg("num").toInt();
    int val = server.arg("val").toInt();
    sendCC(cc, val);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void handleParam() {
  if (server.hasArg("name") && server.hasArg("val")) {
    String param = server.arg("name");
    int val = server.arg("val").toInt();
    sendParam(param, val);
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

void handlePreset() {
  if (server.hasArg("num")) {
    int num = server.arg("num").toInt();
    if (num >= 0 && num < numPresets) {
      const Preset& p = presets[num];
      
      Serial.println("========================================");
      Serial.print("Loading preset: ");
      Serial.println(p.name);
      Serial.println("========================================");
      
      // Oscillators
      sendParam("osc1wave", p.osc1Wave); delay(20);
      sendParam("osc2wave", p.osc2Wave); delay(20);
      sendParam("osc2detune", p.osc2Detune); delay(20);
      sendParam("osc2level", p.osc2Level); delay(20);
      
      // Sub Oscillator
      sendParam("sublevel", p.subLevel); delay(20);
      sendParam("suboctave", p.subOctave); delay(20);
      sendParam("subwave", p.subWave); delay(20);
      
      // Noise Generator
      sendParam("noise", p.noiseLevel); delay(20);
      
      // Volume ADSR
      sendParam("attack", p.attack); delay(20);
      sendParam("decay", p.decay); delay(20);
      sendParam("sustain", p.sustain); delay(20);
      sendParam("release", p.release); delay(20);
      
      // Filter
      sendCC(1, p.filterCutoff); delay(20);
      
      // Filter Envelope
      sendParam("fenvenable", p.filterEnvEnabled ? 1 : 0); delay(20);
      if (p.filterEnvEnabled) {
        sendParam("fenvattack", p.filterEnvAttack); delay(20);
        sendParam("fenvdecay", p.filterEnvDecay); delay(20);
        sendParam("fenvsustain", p.filterEnvSustain); delay(20);
        sendParam("fenvrelease", p.filterEnvRelease); delay(20);
        sendParam("fenvamount", p.filterEnvAmount); delay(20);
      }
      
      // LFO1
      sendParam("lfoenable", p.lfo1Enabled ? 1 : 0); delay(20);
      if (p.lfo1Enabled) {
        sendParam("lforate", p.lfo1Rate); delay(20);
        sendParam("lfowave", p.lfo1Wave); delay(20);
        sendParam("lfopitch", p.lfo1Pitch); delay(20);
        sendParam("lfofilter", p.lfo1Filter); delay(20);
        sendParam("lfoamp", p.lfo1Amp); delay(20);
        sendParam("lfodetune", p.lfo1Detune); delay(20);
      }
      
      // LFO2
      sendParam("lfo2enable", p.lfo2Enabled ? 1 : 0); delay(20);
      if (p.lfo2Enabled) {
        sendParam("lfo2rate", p.lfo2Rate); delay(20);
        sendParam("lfo2wave", p.lfo2Wave); delay(20);
        sendParam("lfo2pitch", p.lfo2Pitch); delay(20);
        sendParam("lfo2filter", p.lfo2Filter); delay(20);
        sendParam("lfo2amp", p.lfo2Amp); delay(20);
        sendParam("lfo2detune", p.lfo2Detune); delay(20);
      }
      
      // Effects
      sendCC(91, p.delayMix); delay(20);
      sendCC(93, p.chorusMix); delay(20);
      
      // Chorus toggle
      if (p.chorusEnabled && !chorusEnabled) {
        sendButton(2);
        chorusEnabled = true;
        delay(20);
      } else if (!p.chorusEnabled && chorusEnabled) {
        sendButton(2);
        chorusEnabled = false;
        delay(20);
      }
      
      Serial.println("Preset loaded!");
      Serial.println("========================================");
      
      // Return comprehensive preset data as JSON
      String json = "{";
      json += "\"name\":\"" + String(p.name) + "\",";
      json += "\"osc1\":" + String(p.osc1Wave) + ",";
      json += "\"osc2\":" + String(p.osc2Wave) + ",";
      json += "\"osc2Detune\":" + String(p.osc2Detune) + ",";
      json += "\"osc2Level\":" + String(p.osc2Level) + ",";
      json += "\"subLevel\":" + String(p.subLevel) + ",";
      json += "\"subOctave\":" + String(p.subOctave) + ",";
      json += "\"subWave\":" + String(p.subWave) + ",";
      json += "\"noiseLevel\":" + String(p.noiseLevel) + ",";
      json += "\"attack\":" + String(p.attack) + ",";
      json += "\"decay\":" + String(p.decay) + ",";
      json += "\"sustain\":" + String(p.sustain) + ",";
      json += "\"release\":" + String(p.release) + ",";
      json += "\"filterCutoff\":" + String(p.filterCutoff) + ",";
      json += "\"filterEnvEnabled\":" + String(p.filterEnvEnabled ? "true" : "false") + ",";
      json += "\"lfo1Enabled\":" + String(p.lfo1Enabled ? "true" : "false") + ",";
      json += "\"lfo2Enabled\":" + String(p.lfo2Enabled ? "true" : "false") + ",";
      json += "\"delayMix\":" + String(p.delayMix) + ",";
      json += "\"chorusMix\":" + String(p.chorusMix) + ",";
      json += "\"chorusEnabled\":" + String(p.chorusEnabled ? "true" : "false");
      json += "}";
      
      server.send(200, "application/json", json);
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
        // Send all parameters to STM32 (same as factory presets)
        sendParam("osc1wave", userPreset.osc1Wave); delay(20);
        sendParam("osc2wave", userPreset.osc2Wave); delay(20);
        sendParam("osc2detune", userPreset.osc2Detune); delay(20);
        sendParam("osc2level", userPreset.osc2Level); delay(20);
        sendParam("sublevel", userPreset.subLevel); delay(20);
        sendParam("suboctave", userPreset.subOctave); delay(20);
        sendParam("subwave", userPreset.subWave); delay(20);
        sendParam("noise", userPreset.noiseLevel); delay(20);
        sendParam("attack", userPreset.attack); delay(20);
        sendParam("decay", userPreset.decay); delay(20);
        sendParam("sustain", userPreset.sustain); delay(20);
        sendParam("release", userPreset.release); delay(20);
        sendCC(1, userPreset.filterCutoff); delay(20);
        sendParam("fenvenable", userPreset.filterEnvEnabled ? 1 : 0); delay(20);
        if (userPreset.filterEnvEnabled) {
          sendParam("fenvattack", userPreset.filterEnvAttack); delay(20);
          sendParam("fenvdecay", userPreset.filterEnvDecay); delay(20);
          sendParam("fenvsustain", userPreset.filterEnvSustain); delay(20);
          sendParam("fenvrelease", userPreset.filterEnvRelease); delay(20);
          sendParam("fenvamount", userPreset.filterEnvAmount); delay(20);
        }
        sendParam("lfoenable", userPreset.lfo1Enabled ? 1 : 0); delay(20);
        if (userPreset.lfo1Enabled) {
          sendParam("lforate", userPreset.lfo1Rate); delay(20);
          sendParam("lfowave", userPreset.lfo1Wave); delay(20);
          sendParam("lfopitch", userPreset.lfo1Pitch); delay(20);
          sendParam("lfofilter", userPreset.lfo1Filter); delay(20);
          sendParam("lfoamp", userPreset.lfo1Amp); delay(20);
          sendParam("lfodetune", userPreset.lfo1Detune); delay(20);
        }
        sendParam("lfo2enable", userPreset.lfo2Enabled ? 1 : 0); delay(20);
        if (userPreset.lfo2Enabled) {
          sendParam("lfo2rate", userPreset.lfo2Rate); delay(20);
          sendParam("lfo2wave", userPreset.lfo2Wave); delay(20);
          sendParam("lfo2pitch", userPreset.lfo2Pitch); delay(20);
          sendParam("lfo2filter", userPreset.lfo2Filter); delay(20);
          sendParam("lfo2amp", userPreset.lfo2Amp); delay(20);
          sendParam("lfo2detune", userPreset.lfo2Detune); delay(20);
        }
        sendCC(91, userPreset.delayMix); delay(20);
        sendCC(93, userPreset.chorusMix); delay(20);
        if (userPreset.chorusEnabled && !chorusEnabled) {
          sendButton(2);
          chorusEnabled = true;
          delay(20);
        } else if (!userPreset.chorusEnabled && chorusEnabled) {
          sendButton(2);
          chorusEnabled = false;
          delay(20);
        }
        sendParam("ringmod", userPreset.ringMod); delay(20);
        sendParam("portamento", userPreset.portamentoOn ? 1 : 0); delay(20);
        sendParam("portatime", userPreset.portamentoTime); delay(20);
        
        // Return preset data as JSON
        String json = "{";
        json += "\"name\":\"User " + String(slot + 1) + "\",";
        json += "\"osc1\":" + String(userPreset.osc1Wave) + ",";
        json += "\"osc2\":" + String(userPreset.osc2Wave) + ",";
        json += "\"osc2Detune\":" + String(userPreset.osc2Detune) + ",";
        json += "\"osc2Level\":" + String(userPreset.osc2Level) + ",";
        json += "\"subLevel\":" + String(userPreset.subLevel) + ",";
        json += "\"subOctave\":" + String(userPreset.subOctave) + ",";
        json += "\"subWave\":" + String(userPreset.subWave) + ",";
        json += "\"noiseLevel\":" + String(userPreset.noiseLevel) + ",";
        json += "\"attack\":" + String(userPreset.attack) + ",";
        json += "\"decay\":" + String(userPreset.decay) + ",";
        json += "\"sustain\":" + String(userPreset.sustain) + ",";
        json += "\"release\":" + String(userPreset.release) + ",";
        json += "\"filterCutoff\":" + String(userPreset.filterCutoff) + ",";
        json += "\"filterEnvEnabled\":" + String(userPreset.filterEnvEnabled ? "true" : "false") + ",";
        json += "\"lfo1Enabled\":" + String(userPreset.lfo1Enabled ? "true" : "false") + ",";
        json += "\"lfo2Enabled\":" + String(userPreset.lfo2Enabled ? "true" : "false") + ",";
        json += "\"delayMix\":" + String(userPreset.delayMix) + ",";
        json += "\"chorusMix\":" + String(userPreset.chorusMix) + ",";
        json += "\"chorusEnabled\":" + String(userPreset.chorusEnabled ? "true" : "false") + ",";
        json += "\"ringMod\":" + String(userPreset.ringMod) + ",";
        json += "\"portamentoOn\":" + String(userPreset.portamentoOn ? "true" : "false") + ",";
        json += "\"portamentoTime\":" + String(userPreset.portamentoTime);
        json += "}";
        
        server.send(200, "application/json", json);
      } else {
        server.send(404, "text/plain", "User preset slot is empty");
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
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing parameter");
  }
}

void handleChorus() {
  sendButton(2);
  chorusEnabled = !chorusEnabled;
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
  sendCC(123, 0);
  server.send(200, "text/plain", "OK");
}

void handleCheckPass() {
  if (server.hasArg("pass")) {
    String inputPass = server.arg("pass");
    preferences.begin("synth", true);
    String correctPass = preferences.getString("volumePass", "2222");
    preferences.end();
    
    if (inputPass == correctPass) {
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

void handleSaveVolume() {
  if (server.hasArg("val")) {
    int vol = server.arg("val").toInt();
    preferences.begin("synth", false);
    preferences.putInt("volume", vol);
    preferences.end();
    
    sendCC(7, vol);
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
    String correctPass = preferences.getString("volumePass", "2222");
    preferences.end();
    
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
  Serial.println("  ESP32 #3 - Web Controller (UART)");
  Serial.println("  Simple & Reliable!");
  Serial.println("==========================================");
  
  // Initialize UART to STM32
  Serial2.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);  // RX=GPIO16, TX=GPIO17
  Serial.println("✓ UART initialized (GPIO17 → STM32 PA10)");
  
  initState();
  
  // Load saved volume and passcode from preferences
  preferences.begin("synth", true);
  int savedVolume = preferences.getInt("volume", 89);  // Default 70%
  String savedPass = preferences.getString("volumePass", "2222");
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
  server.on("/savevolume", handleSaveVolume);
  server.on("/changepass", handleChangePass);
  
  server.begin();
  Serial.println("✓ Web server started");
  Serial.println("==========================================");
  Serial.println("Ready! Open http://192.168.4.1");
  Serial.println();
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
