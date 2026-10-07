/*
 * STM32F411CEU6 Polyphonic Synthesizer - Poly-Synth synthesis engine
 * https://github.com/Yorev89/Poly-Synth
 *
 * Features:
 * - 8-voice polyphony with adaptive voice scaling and click-free voice stealing
 * - Dual oscillators + sub oscillator (4 waveforms each), ring modulator
 * - ADSR amplitude envelope per voice + filter envelope
 * - Global one-pole low-pass filter with LFO modulation
 * - Delay (10-400 ms, presets) + 4-tap chorus (output currently mono)
 * - 2x LFO (pitch, filter, amplitude, detune modulation)
 * - 15 factory presets + 12 user preset slots (stored on ESP32 #1)
 * - I2S sample rate computed at boot; tuning compensated to the real rate
 * - Voice gain scaled by summed envelope levels (smooth chords and release tails)
 *
 * Hardware:
 * - STM32F411CEU6 (WeAct Black Pill V3.1), 25 MHz HSE, 96 MHz core
 * - PCM5102A DAC via I2S (PB12=WS, PB13=BCK, PB15=DIN)
 * - ESP32 #1 (web controller, Wi-Fi AP) -> UART RX on PA10 (WebSerial)
 * - ESP32 #2 (BLE MIDI receiver)        -> UART RX on PA3, TX on PA2 (ESP32Serial)
 * - 5-pin DIN MIDI input: wired, but disabled - its UART pin (PA10) now serves ESP32 #1
 *
 * MIDI flow:
 * BLE MIDI keyboard -> ESP32 #2 -> UART -> STM32
 * Commands: NOTE,<note>,<velocity> | NOTEOFF,<note> | PITCHBEND,<lsb>,<msb> | CC,<num>,<val>
 *
 * Build: Arduino IDE, STM32 core, Board part number: BlackPill F411CE
 *        (uses the 25 MHz crystal), Tools -> Optimize -> Fastest (-O3) REQUIRED
 * (with the default -Os the render loop misses its deadline and audio distorts)
 */

#include <Arduino.h>

#define TARGET_SAMPLE_RATE 44100.0f
// Real output sample rate, set by configureI2SClock() at the start of setup().
// All pitch/time calculations use this value, so tuning stays correct even if
// the clock tree can't produce exactly 44100 Hz.
float SAMPLE_RATE = TARGET_SAMPLE_RATE;
uint16_t i2sPrescaler = 0;  // Value for SPI2->I2SPR: I2SDIV | (ODD << 8)
// DIAGNOSTICS (temporary): render timing
uint32_t diagMaxRenderCycles = 0;
uint32_t diagMissedBuffers = 0;
volatile uint32_t diagHalfStamp = 0, diagFullStamp = 0;  // DWT time of DMA interrupt
uint32_t diagMaxLatencyCycles = 0;  // interrupt -> render finished
uint32_t diagLateBuffers = 0;       // renders that finished after their deadline
#define AUDIO_BUFFER_SIZE 512
#define WAVE_TABLE_SIZE 256
#define MAX_VOICES 8
#define MIDI_BAUD_RATE 31250

#define MIDI_NOTE_OFF 0x80
#define MIDI_NOTE_ON 0x90
#define MIDI_CONTROL_CHANGE 0xB0
#define MIDI_PITCH_BEND 0xE0

enum WaveType { WAVE_SINE, WAVE_SAW, WAVE_SQUARE, WAVE_TRIANGLE };
enum EnvStage { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

int16_t sineTable[WAVE_TABLE_SIZE];
int16_t sawTable[WAVE_TABLE_SIZE];
int16_t squareTable[WAVE_TABLE_SIZE];
int16_t triangleTable[WAVE_TABLE_SIZE];

int16_t audioBuffer[AUDIO_BUFFER_SIZE];
DMA_HandleTypeDef hdma_spi2_tx;
volatile uint32_t halfComplete = 0;
volatile uint32_t fullComplete = 0;

// HardwareSerial MidiSerial(PA10, PA9);  // COMMENTED OUT - PA10 used by WebSerial
HardwareSerial ESP32Serial(PA3, PA2);
HardwareSerial WebSerial(PA10, PA9);  // ESP32 #1 Web Control
uint8_t midiChannel = 0xFF;

struct Oscillator {
  uint32_t phase;
  uint32_t phaseInc;
  WaveType waveType;
  float detune;
};

struct Envelope {
  EnvStage stage;
  float level;
  float attackRate;
  float decayRate;
  float sustainLevel;
  float releaseRate;
};

struct Voice {
  bool active;
  uint8_t note;
  Oscillator osc1;
  Oscillator osc2;
  Oscillator sub;
  Envelope env;
  Envelope filterEnv;
  float velocity;
  unsigned long noteOnTime;
  // Portamento (cheap phaseInc-based approach)
  bool portaActive;
  float portaRate;           // Multiply phaseInc by this each sample
  uint32_t portaTargetInc1;  // Target phaseInc for osc1
  uint32_t portaTargetInc2;  // Target phaseInc for osc2
  uint32_t portaTargetSubInc;// Target phaseInc for sub
  // Float accumulators - avoid uint32_t truncation during slow glides
  float portaInc1f;
  float portaInc2f;
  float portaSubIncf;
};

Voice voices[MAX_VOICES];

struct GlobalFilter {
  float z1L;
  float z1R;
  float a;
} globalFilter;

// ==================== SYNTH PARAMETERS ====================

float osc1Detune = 0.0;
float osc2Detune = 7.0;
float osc2Level = 0.7;  // Adjustable via WiFi
WaveType osc1Wave = WAVE_SAW;
WaveType osc2Wave = WAVE_SAW;

// Sub oscillator
float subLevel = 0.0;      // 0-100% (default OFF)
int subOctave = 1;         // 1 = -1 octave, 2 = -2 octaves
WaveType subWave = WAVE_SINE;  // SINE or SQUARE (classic choices)

// Ring Modulator
float ringModAmount = 0.0;  // 0-100% (0=additive, 100=full ring mod)

// Portamento
bool portamentoEnabled = false;
float portamentoTime = 0.1;   // 0-2 seconds
float lastPlayedFreq = 0.0;   // Source frequency for glide

// Noise generator
float noiseLevel = 0.0;    // 0-100% (default OFF)

// ADSR - now adjustable via WiFi!
float attackTime = 0.005;    // 1-1000 ms
float decayTime = 0.05;      // 1-1000 ms
float sustainLevel = 0.7;    // 0-100%
float releaseTime = 0.05;    // 1-3000 ms

// Filter Envelope
bool filterEnvEnabled = false;
float filterEnvAttack = 0.005;    // 1-1000 ms
float filterEnvDecay = 0.05;      // 1-1000 ms
float filterEnvSustain = 0.5;     // 0-100%
float filterEnvRelease = 0.05;    // 1-3000 ms
float filterEnvAmount = 0.5;      // 0-100% - how much envelope affects filter

float filterCutoff = 5000.0;

// LFO System
enum LFOWaveType { LFO_SINE, LFO_TRIANGLE, LFO_SQUARE, LFO_SAW_UP, LFO_SAW_DOWN };
struct LFO {
  uint32_t phase;
  uint32_t phaseInc;
  LFOWaveType waveType;
  float rate;  // Hz (0.1 - 20 Hz)
  
  // Modulation depths (0.0 - 1.0)
  float pitchDepth;      // Vibrato (0-100 cents)
  float filterDepth;     // Filter mod (0-3000 Hz)
  float ampDepth;        // Tremolo
  float osc2DetuneDepth; // Osc2 detune mod (0-50 cents)
} lfo1, lfo2;  // Two independent LFOs!

bool lfo1Enabled = false;
bool lfo2Enabled = false;

// 400 ms at 44.1 kHz, stored as int16 (same RAM as the old 200 ms float buffers).
// Range +/-2.0 maps to the full int16 range (DELAY_SCALE = 16384).
#define DELAY_BUFFER_SIZE 17640
#define DELAY_SCALE 16384.0f
int16_t delayBufferL[DELAY_BUFFER_SIZE];
int16_t delayBufferR[DELAY_BUFFER_SIZE];
int delayWritePos = 0;
float delayTime = 0.0;       // 0 seconds (off)
float delayFeedback = 0.0;
float delayMix = 0.0;
int delayPreset = 0;
int feedbackPreset = 1;


#define CHORUS_BUFFER_SIZE 1323
float chorusBufferL[CHORUS_BUFFER_SIZE];
float chorusBufferR[CHORUS_BUFFER_SIZE];
int chorusWritePos = 0;
float chorusMix = 0.6;
bool chorusEnabled = false;

const int chorusTaps[4] = {44, 88, 132, 176};

float masterVolume = 1.0;  // Maximum (was 0.7 = +43% gain)
float volumeCeiling = 1.0;  // 0-1: Web-locked maximum volume (parent control)
float keyboardVolumeCC = 1.0;  // 0-1: MIDI keyboard's volume knob position
float pitchBendAmount = 0.0;
float pitchBendRange = 2.0;

// ==========================================================

const char* getWaveName(WaveType wave) {
  switch(wave) {
    case WAVE_SINE: return "SINE";
    case WAVE_SAW: return "SAW";
    case WAVE_SQUARE: return "SQUARE";
    case WAVE_TRIANGLE: return "TRIANGLE";
    default: return "UNKNOWN";
  }
}

// ==================== I2S CLOCK CONFIGURATION ====================
// Searches PLLI2S M/N/R and I2SDIV/ODD for the rate closest to targetRate,
// programs PLLI2S, and returns the sample rate actually produced.
// I2S mode: 16-bit data, 16-bit channel (32 bits per stereo frame), no MCLK:
//   Fs = I2SCLK / (32 * (2*I2SDIV + ODD))
// Limits (RM0383 / F411 datasheet): PLL input 0.95-2.1 MHz, VCO 100-432 MHz,
// I2SCLK <= 192 MHz, N 50-432, R 2-7, I2SDIV 2-255.
float configureI2SClock(float targetRate) {
  bool useHSE = (RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) != 0;  // PLLI2S shares the PLL source
  float srcHz = useHSE ? (float)HSE_VALUE : (float)HSI_VALUE;

#ifdef RCC_PLLI2SCFGR_PLLI2SM
  const uint32_t mMin = 2, mMax = 63;   // F411: PLLI2S has its own input divider
#else
  uint32_t mFixed = RCC->PLLCFGR & RCC_PLLCFGR_PLLM;  // otherwise shares main PLLM
  const uint32_t mMin = mFixed, mMax = mFixed;
#endif

  float bestErr = 1e12f, bestRate = 0.0f;
  uint32_t bestM = 0, bestN = 0, bestR = 0, bestK = 0;

  for (uint32_t m = mMin; m <= mMax; m++) {
    float pllIn = srcHz / m;
    if (pllIn < 0.95e6f || pllIn > 2.1e6f) continue;
    for (uint32_t n = 50; n <= 432; n++) {
      float vco = pllIn * n;
      if (vco < 100e6f || vco > 432e6f) continue;
      for (uint32_t r = 2; r <= 7; r++) {
        float i2sclk = vco / r;
        if (i2sclk > 192e6f) continue;
        uint32_t k = (uint32_t)(i2sclk / (32.0f * targetRate) + 0.5f);  // k = 2*DIV + ODD
        if (k < 4 || k > 511) continue;
        float rate = i2sclk / (32.0f * k);
        float err = fabsf(rate - targetRate);
        if (err < bestErr) {
          bestErr = err; bestRate = rate;
          bestM = m; bestN = n; bestR = r; bestK = k;
        }
      }
    }
  }

  // Program PLLI2S (must be off while changing it)
  RCC->CR &= ~RCC_CR_PLLI2SON;
  while (RCC->CR & RCC_CR_PLLI2SRDY);
  uint32_t cfg = (bestN << RCC_PLLI2SCFGR_PLLI2SN_Pos) |
                 (bestR << RCC_PLLI2SCFGR_PLLI2SR_Pos);
#ifdef RCC_PLLI2SCFGR_PLLI2SM
  cfg |= (bestM << RCC_PLLI2SCFGR_PLLI2SM_Pos);
#endif
  RCC->PLLI2SCFGR = cfg;
  RCC->CR |= RCC_CR_PLLI2SON;
  while (!(RCC->CR & RCC_CR_PLLI2SRDY));

  i2sPrescaler = (uint16_t)((bestK >> 1) | ((bestK & 1) << 8));

  Serial.print("I2S clock: src=");   Serial.print(useHSE ? "HSE " : "HSI ");
  Serial.print(srcHz / 1e6f, 3);     Serial.print(" MHz  M=");  Serial.print(bestM);
  Serial.print(" N=");  Serial.print(bestN);
  Serial.print(" R=");  Serial.print(bestR);
  Serial.print(" DIV="); Serial.print(bestK >> 1);
  Serial.print(" ODD="); Serial.println(bestK & 1);
  Serial.print("Sample rate: "); Serial.print(bestRate, 2);
  Serial.print(" Hz (error "); Serial.print(1200.0f * log2f(bestRate / targetRate), 2);
  Serial.println(" cents, compensated)");

  return bestRate;
}

void generateWavetables() {
  for (int i = 0; i < WAVE_TABLE_SIZE; i++) {
    float phase = (float)i / WAVE_TABLE_SIZE;
    sineTable[i] = (int16_t)(sin(2.0 * PI * phase) * 32767);
    sawTable[i] = (int16_t)((2.0 * phase - 1.0) * 32767);
    squareTable[i] = (phase < 0.5) ? 32767 : -32767;
    triangleTable[i] = (int16_t)((phase < 0.5 ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase)) * 32767);
  }
}

void setDelayPreset(int preset) {
  switch(preset) {
    case 0:
      delayMix = 0.0;
      delayTime = 0.0;       // OFF = no delay time
      delayFeedback = 0.0;
      Serial.println("Delay: OFF");
      break;
    case 1:
      delayTime = 0.08;
      delayFeedback = 0.0;
      delayMix = 0.4;
      Serial.println("Delay: SLAPBACK");
      break;
    case 2:
      delayTime = 0.15;
      delayFeedback = 0.6;
      delayMix = 0.4;
      Serial.println("Delay: SHORT");
      break;
    case 3:
      delayTime = 0.2;
      delayFeedback = 0.5;
      delayMix = 0.35;
      Serial.println("Delay: MEDIUM");
      break;
    case 4:
      delayTime = 0.12;
      delayFeedback = 0.5;
      delayMix = 0.35;
      Serial.println("Delay: RHYTHMIC");
      break;
  }
}

void setFeedbackPreset(int preset) {
  switch(preset) {
    case 0:
      delayFeedback = 0.2;
      Serial.println("Feedback: LOW");
      break;
    case 1:
      delayFeedback = 0.5;
      Serial.println("Feedback: MEDIUM");
      break;
    case 2:
      delayFeedback = 0.8;
      Serial.println("Feedback: HIGH");
      break;
  }
}

inline int16_t getWaveSample(WaveType type, uint32_t phase) {
  uint32_t index = (phase >> 24) & 0xFF;
  switch(type) {
    case WAVE_SINE: return sineTable[index];
    case WAVE_SAW: return sawTable[index];
    case WAVE_SQUARE: return squareTable[index];
    case WAVE_TRIANGLE: return triangleTable[index];
    default: return 0;
  }
}

float noteToFreq(uint8_t note) {
  return 440.0 * pow(2.0, (note - 69) / 12.0);
}

uint32_t freqToPhaseInc(float freq, float detune) {
  float detuneRatio = pow(2.0, detune / 1200.0);
  float actualFreq = freq * detuneRatio;
  return (uint32_t)((actualFreq * 4294967296.0) / SAMPLE_RATE);
}

void updateLFORate(int lfoNum, float hz) {
  if (hz < 0.1) hz = 0.1;
  if (hz > 20.0) hz = 20.0;
  
  if (lfoNum == 1) {
    lfo1.rate = hz;
    lfo1.phaseInc = (uint32_t)((hz * 4294967296.0) / SAMPLE_RATE);
  } else if (lfoNum == 2) {
    lfo2.rate = hz;
    lfo2.phaseInc = (uint32_t)((hz * 4294967296.0) / SAMPLE_RATE);
  }
}

// Simple white noise generator using linear congruential generator (LCG)
inline int16_t generateNoise() {
  static uint32_t noiseSeed = 12345;  // Starting seed
  noiseSeed = (noiseSeed * 1103515245 + 12345) & 0x7fffffff;  // LCG algorithm
  return (int16_t)((noiseSeed >> 16) - 16384);  // Convert to -16384 to +16384 range
}

void initVoice(Voice* v) {
  v->active = false;
  v->note = 0;
  v->velocity = 0;
  v->osc1.phase = 0;
  v->osc2.phase = 0;
  v->env.stage = ENV_IDLE;
  v->env.level = 0;
}

void updateGlobalFilter(float cutoffHz) {
  if (cutoffHz < 20.0f) cutoffHz = 20.0f;
  if (cutoffHz > 18000.0f) cutoffHz = 18000.0f;
  
  float fc = cutoffHz / SAMPLE_RATE;
  if (fc > 0.49f) fc = 0.49f;
  if (fc < 0.0001f) fc = 0.0001f;
  
  globalFilter.a = 1.0f - (2.0f * (float)PI * fc);
  
  if (globalFilter.a < 0.001f) globalFilter.a = 0.001f;
  if (globalFilter.a > 0.999f) globalFilter.a = 0.999f;
  
  if (isnan(globalFilter.a) || isinf(globalFilter.a)) {
    globalFilter.a = 0.95f;
    globalFilter.z1L = 0;
    globalFilter.z1R = 0;
  }
}

// Bend ratio currently applied to the voices (1.0 = no bend)
float pitchBendRatioApplied = 1.0f;

// Applies the current pitchBendAmount to every active voice: osc1, osc2 AND the
// sub oscillator (the sub used to stay unbent and drift out of tune). A voice
// that is mid-glide keeps gliding: its current pitch and its target are both
// shifted by the bend change (bend used to be overwritten by the glide).
void applyPitchBend() {
  float ratio = powf(2.0f, pitchBendAmount / 12.0f);
  float change = ratio / pitchBendRatioApplied;
  pitchBendRatioApplied = ratio;
  
  for (int v = 0; v < MAX_VOICES; v++) {
    if (!voices[v].active) continue;
    float freq = noteToFreq(voices[v].note) * ratio;
    uint32_t inc1   = freqToPhaseInc(freq, osc1Detune);
    uint32_t inc2   = freqToPhaseInc(freq, osc2Detune);
    uint32_t incSub = freqToPhaseInc(freq / (float)(1 << subOctave), 0.0);
    
    if (voices[v].portaActive) {
      voices[v].portaTargetInc1   = inc1;
      voices[v].portaTargetInc2   = inc2;
      voices[v].portaTargetSubInc = incSub;
      voices[v].portaInc1f   *= change;
      voices[v].portaInc2f   *= change;
      voices[v].portaSubIncf *= change;
    } else {
      voices[v].osc1.phaseInc = inc1;
      voices[v].osc2.phaseInc = inc2;
      voices[v].sub.phaseInc  = incSub;
    }
  }
}

void noteOn(uint8_t note, float velocity) {
  // Releasing voices keep their programmed release time. (A forced 200 ms
  // fade used to cut all tails whenever a new note started; it masked the
  // gain jumps of the old count-based voice scaling, now envelope-based.)
  
  Voice* targetVoice = NULL;
  
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!voices[i].active) {
      targetVoice = &voices[i];
      break;
    }
  }
  
  if (!targetVoice) {
    // Find the QUIETEST releasing voice to minimize click
    float lowestLevel = 1.0;
    for (int i = 0; i < MAX_VOICES; i++) {
      if (voices[i].env.stage == ENV_RELEASE && voices[i].env.level < lowestLevel) {
        lowestLevel = voices[i].env.level;
        targetVoice = &voices[i];
      }
    }
  }
  
  if (!targetVoice) {
    // All voices held: steal the OLDEST note (was: always voices[0])
    targetVoice = &voices[0];
    for (int i = 1; i < MAX_VOICES; i++) {
      if (voices[i].noteOnTime < targetVoice->noteOnTime) targetVoice = &voices[i];
    }
  }
  
  // A stolen voice keeps its current envelope level and the new note's attack
  // starts from there. Oscillator phases are continuous, so the waveform has no
  // step and the steal is click-free (was: level reset to 0 = audible click).
  bool stolen = targetVoice->active;
  
  // Don't activate yet - set all parameters first
  targetVoice->note = note;
  targetVoice->velocity = velocity;
  targetVoice->noteOnTime = millis();
  
  float freq = noteToFreq(note);
  float pitchBendRatio = pow(2.0, pitchBendAmount / 12.0);
  freq = freq * pitchBendRatio;
  float subFreq = freq / (float)(1 << subOctave);
  
  // Calculate target phase increments
  uint32_t targetInc1   = freqToPhaseInc(freq, osc1Detune);
  uint32_t targetInc2   = freqToPhaseInc(freq, osc2Detune);
  uint32_t targetSubInc = freqToPhaseInc(subFreq, 0.0);
  
  // Legato detection: only glide if at least one other voice is currently active
  // (playing non-releasing). Chords from silence jump directly to pitch.
  bool anyVoiceHeld = false;
  for (int v = 0; v < MAX_VOICES; v++) {
    if (&voices[v] != targetVoice && voices[v].active &&
        voices[v].env.stage != ENV_RELEASE && voices[v].env.stage != ENV_IDLE) {
      anyVoiceHeld = true;
      break;
    }
  }
  
  // Portamento: only trigger in legato playing
  if (portamentoEnabled && portamentoTime > 0.0f && lastPlayedFreq > 0.0f && 
      lastPlayedFreq != freq && anyVoiceHeld) {
    uint32_t startInc1 = freqToPhaseInc(lastPlayedFreq, osc1Detune);
    float numSamples = portamentoTime * SAMPLE_RATE;
    float freqRatio = freq / lastPlayedFreq;
    
    targetVoice->osc1.phaseInc   = startInc1;
    targetVoice->osc2.phaseInc   = freqToPhaseInc(lastPlayedFreq, osc2Detune);
    targetVoice->sub.phaseInc    = freqToPhaseInc(lastPlayedFreq / (float)(1 << subOctave), 0.0);
    targetVoice->portaTargetInc1   = targetInc1;
    targetVoice->portaTargetInc2   = targetInc2;
    targetVoice->portaTargetSubInc = targetSubInc;
    targetVoice->portaRate   = pow(freqRatio, 1.0f / numSamples);
    targetVoice->portaActive = true;
    targetVoice->portaInc1f   = (float)targetVoice->osc1.phaseInc;
    targetVoice->portaInc2f   = (float)targetVoice->osc2.phaseInc;
    targetVoice->portaSubIncf = (float)targetVoice->sub.phaseInc;
  } else {
    // No portamento: jump directly to target
    targetVoice->osc1.phaseInc = targetInc1;
    targetVoice->osc2.phaseInc = targetInc2;
    targetVoice->sub.phaseInc  = targetSubInc;
    targetVoice->portaActive   = false;
    targetVoice->portaRate     = 1.0f;
  }
  
  lastPlayedFreq = freq;  // Remember for next note's glide source
  
  targetVoice->osc1.waveType = osc1Wave;
  targetVoice->osc2.waveType = osc2Wave;
  targetVoice->sub.waveType  = subWave;
  
  // Initialize volume envelope
  targetVoice->env.stage = ENV_ATTACK;
  if (!stolen) targetVoice->env.level = 0;
  targetVoice->env.attackRate = 1.0 / (attackTime * SAMPLE_RATE);
  targetVoice->env.sustainLevel = sustainLevel;
  targetVoice->env.decayRate = (1.0 - targetVoice->env.sustainLevel) / (decayTime * SAMPLE_RATE);
  
  // Release rate calculation:
  // For normal sustain levels, use sustainLevel
  // For very low sustain (pluck/drum sounds), use a fixed fast release
  if (targetVoice->env.sustainLevel < 0.01) {
    // Fast release for drums/plucks - always release in specified time from level 0.05
    targetVoice->env.releaseRate = 0.05 / (releaseTime * SAMPLE_RATE);
  } else {
    targetVoice->env.releaseRate = targetVoice->env.sustainLevel / (releaseTime * SAMPLE_RATE);
  }
  
  // Initialize filter envelope
  if (filterEnvEnabled) {
    targetVoice->filterEnv.stage = ENV_ATTACK;
    if (!stolen) targetVoice->filterEnv.level = 0;
    targetVoice->filterEnv.attackRate = 1.0 / (filterEnvAttack * SAMPLE_RATE);
    targetVoice->filterEnv.decayRate = (1.0 - filterEnvSustain) / (filterEnvDecay * SAMPLE_RATE);
    targetVoice->filterEnv.sustainLevel = filterEnvSustain;
    targetVoice->filterEnv.releaseRate = filterEnvSustain / (filterEnvRelease * SAMPLE_RATE);
  } else {
    targetVoice->filterEnv.stage = ENV_IDLE;
    targetVoice->filterEnv.level = 1.0;  // Full open when disabled
  }
  
  // CRITICAL: Activate voice LAST after all parameters are set
  // This prevents race condition where DMA renders with partial initialization
  targetVoice->active = true;
}

void noteOff(uint8_t note) {
  for (int i = 0; i < MAX_VOICES; i++) {
    if (voices[i].active && voices[i].note == note) {
      if (voices[i].env.stage != ENV_RELEASE && voices[i].env.stage != ENV_IDLE) {
        
        // For drum/pluck sounds (sustain < 1%), recalculate release rate from CURRENT level
        // This ensures quick release even if key is released during attack/decay
        if (voices[i].env.sustainLevel < 0.01) {
          float currentLevel = voices[i].env.level;
          if (currentLevel < 0.01) currentLevel = 0.01; // Minimum for calculation
          voices[i].env.releaseRate = currentLevel / (releaseTime * SAMPLE_RATE);
        }
        
        voices[i].env.stage = ENV_RELEASE;
        if (filterEnvEnabled) {
          voices[i].filterEnv.stage = ENV_RELEASE;
        }
      }
    }
  }
}

void allNotesOff() {
  for (int i = 0; i < MAX_VOICES; i++) {
    voices[i].active = false;
    voices[i].env.stage = ENV_IDLE;
    voices[i].env.level = 0;
    voices[i].filterEnv.stage = ENV_IDLE;
    voices[i].filterEnv.level = 1.0;
  }
  
  for (int i = 0; i < DELAY_BUFFER_SIZE; i++) {
    delayBufferL[i] = 0;
    delayBufferR[i] = 0;
  }
}

inline void processEnvelope(Envelope* env) {
  switch(env->stage) {
    case ENV_ATTACK:
      env->level += env->attackRate;
      if (env->level >= 1.0f) {
        env->level = 1.0f;
        env->stage = ENV_DECAY;
      }
      break;
    case ENV_DECAY:
      env->level -= env->decayRate;
      if (env->level <= env->sustainLevel) {
        env->level = env->sustainLevel;
        env->stage = ENV_SUSTAIN;
      }
      break;
    case ENV_SUSTAIN:
      env->level = env->sustainLevel;
      break;
    case ENV_RELEASE:
      env->level -= env->releaseRate;
      if (env->level <= 0.0f) {
        env->level = 0.0f;
        env->stage = ENV_IDLE;
      }
      break;
    case ENV_IDLE:
      env->level = 0.0f;
      break;
  }
}

// Advances an envelope by n samples in one step (control-rate processing).
// Stage changes happen at the block boundary; error is at most one block.
inline void processEnvelopeN(Envelope* env, int n) {
  switch(env->stage) {
    case ENV_ATTACK:
      env->level += env->attackRate * n;
      if (env->level >= 1.0f) { env->level = 1.0f; env->stage = ENV_DECAY; }
      break;
    case ENV_DECAY:
      env->level -= env->decayRate * n;
      if (env->level <= env->sustainLevel) { env->level = env->sustainLevel; env->stage = ENV_SUSTAIN; }
      break;
    case ENV_SUSTAIN:
      env->level = env->sustainLevel;
      break;
    case ENV_RELEASE:
      env->level -= env->releaseRate * n;
      if (env->level <= 0.0f) { env->level = 0.0f; env->stage = ENV_IDLE; }
      break;
    case ENV_IDLE:
      env->level = 0.0f;
      break;
  }
}

void renderAudio(int16_t* buffer, int startIdx, int numSamples) {
  
  // Update LFO1 once per buffer
  float lfo1Value = 0.0f;
  if (lfo1Enabled) {
    uint32_t index = (lfo1.phase >> 24) & 0xFF;
    
    switch(lfo1.waveType) {
      case LFO_SINE:
        lfo1Value = sineTable[index] / 32768.0f;
        break;
      case LFO_TRIANGLE:
        lfo1Value = (index < 128) ? (index / 64.0f - 1.0f) : (3.0f - index / 64.0f);
        break;
      case LFO_SQUARE:
        lfo1Value = (index < 128) ? 1.0f : -1.0f;
        break;
      case LFO_SAW_UP:
        lfo1Value = (index / 128.0f) - 1.0f;
        break;
      case LFO_SAW_DOWN:
        lfo1Value = 1.0f - (index / 128.0f);
        break;
    }
  }
  
  // Update LFO2 once per buffer
  float lfo2Value = 0.0f;
  if (lfo2Enabled) {
    uint32_t index = (lfo2.phase >> 24) & 0xFF;
    
    switch(lfo2.waveType) {
      case LFO_SINE:
        lfo2Value = sineTable[index] / 32768.0f;
        break;
      case LFO_TRIANGLE:
        lfo2Value = (index < 128) ? (index / 64.0f - 1.0f) : (3.0f - index / 64.0f);
        break;
      case LFO_SQUARE:
        lfo2Value = (index < 128) ? 1.0f : -1.0f;
        break;
      case LFO_SAW_UP:
        lfo2Value = (index / 128.0f) - 1.0f;
        break;
      case LFO_SAW_DOWN:
        lfo2Value = 1.0f - (index / 128.0f);
        break;
    }
  }
  
  // Calculate combined LFO modulations (LFO1 + LFO2)
  float lfoPitchMod = (lfo1Value * lfo1.pitchDepth) + (lfo2Value * lfo2.pitchDepth);
  float lfoFilterMod = (lfo1Value * lfo1.filterDepth * 1500.0f) + (lfo2Value * lfo2.filterDepth * 1500.0f);
  float lfoAmpMod = 1.0f + (lfo1Value * lfo1.ampDepth * 0.3f) + (lfo2Value * lfo2.ampDepth * 0.3f);
  float lfoDetuneMod = (lfo1Value * lfo1.osc2DetuneDepth * 25.0f) + (lfo2Value * lfo2.osc2DetuneDepth * 25.0f);
  
  // Apply LFO to filter (once per buffer, not per sample!)
  float currentFilterCutoff = filterCutoff + lfoFilterMod;
  if (currentFilterCutoff < 200.0f) currentFilterCutoff = 200.0f;
  if (currentFilterCutoff > 10000.0f) currentFilterCutoff = 10000.0f;
  
  // Filter envelope at control rate (once per buffer, was per sample - the
  // single most expensive feature). Filter stays global: the highest
  // filter-envelope level of all voices drives it.
  int frames = numSamples / 2;
  if (filterEnvEnabled) {
    float maxFilterEnv = 0.0f;
    for (int v = 0; v < MAX_VOICES; v++) {
      if (voices[v].active) {
        processEnvelopeN(&voices[v].filterEnv, frames);
        if (voices[v].filterEnv.level > maxFilterEnv) maxFilterEnv = voices[v].filterEnv.level;
      }
    }
    currentFilterCutoff += maxFilterEnv * filterEnvAmount * 6000.0f;  // 0 to +6000 Hz
    if (currentFilterCutoff > 10000.0f) currentFilterCutoff = 10000.0f;
  }
  
  // Coefficient ramps linearly across the buffer from the previous value to
  // the new one, so cutoff changes are smooth (no steps, no zipper noise)
  float filterA = globalFilter.a;
  updateGlobalFilter(currentFilterCutoff);
  float filterAStep = (globalFilter.a - filterA) / (float)frames;
  
  // Convert pitch mod to multiplier (once per buffer)
  static float smoothedPitchMult = 1.0f;  // Static to preserve between buffers
  static float smoothedDetuneMult = 1.0f;
  static float smoothedAmpMod = 1.0f;
  
  float pitchModMultiplier = 1.0f;
  if ((lfo1Enabled && lfo1.pitchDepth > 0.0f) || (lfo2Enabled && lfo2.pitchDepth > 0.0f)) {
    pitchModMultiplier = powf(2.0f, (lfoPitchMod * 50.0f) / 1200.0f);  // Max ±50 cents
  }
  
  // Smooth the multipliers with low-pass filter to reduce zipper noise
  // Coefficient 0.3 = moderately smooth (balances responsiveness vs smoothness)
  float smoothCoeff = 0.3f;
  smoothedPitchMult = smoothedPitchMult + smoothCoeff * (pitchModMultiplier - smoothedPitchMult);
  
  // Convert detune mod to multiplier (once per buffer)
  float detuneModMultiplier = 1.0f;
  if ((lfo1Enabled && lfo1.osc2DetuneDepth > 0.0f) || (lfo2Enabled && lfo2.osc2DetuneDepth > 0.0f)) {
    detuneModMultiplier = powf(2.0f, lfoDetuneMod / 1200.0f);
  }
  smoothedDetuneMult = smoothedDetuneMult + smoothCoeff * (detuneModMultiplier - smoothedDetuneMult);
  
  // Smooth amplitude modulation
  smoothedAmpMod = smoothedAmpMod + smoothCoeff * (lfoAmpMod - smoothedAmpMod);
  
  // Voice load = sum of the amplitude-envelope levels of ALL sounding voices,
  // including releasing ones. (Was: count of held voices only - on chord
  // release the count dropped to 0 and the gain jumped up, e.g. +67% for a
  // 3-note chord, causing an audible volume swell during the release tails.)
  // With scale = 1/(load+2) the summed output L/(L+2) can only fall as the
  // envelopes fall, so releases decay smoothly and the mix cannot exceed 1.
  float voiceLoad = 0.0f;
  for (int v = 0; v < MAX_VOICES; v++) {
    if (voices[v].active) voiceLoad += voices[v].env.level;
  }
  if (voiceLoad < 1.0f) voiceLoad = 1.0f;  // single voice: same gain as before (1/3)
  
  float targetScale = 1.0f / (voiceLoad + 2.0f);
  
  // Asymmetric smoothing: follow quickly when the load rises (new notes, avoids
  // overload), slowly when it falls (keeps tails steady)
  static float smoothedScale = 0.33f;
  float smoothFactor = (targetScale < smoothedScale) ? 0.3f : 0.02f;
  
  smoothedScale = smoothedScale + smoothFactor * (targetScale - smoothedScale);
  float voiceScale = smoothedScale;
  
  for (int i = 0; i < numSamples / 2; i++) {
    int32_t mixL = 0;
    int32_t mixR = 0;
    
    for (int v = 0; v < MAX_VOICES; v++) {
      Voice* voice = &voices[v];
      
      if (voice->active) {
        int16_t sample1 = getWaveSample(voice->osc1.waveType, voice->osc1.phase);
        int16_t sample2 = getWaveSample(voice->osc2.waveType, voice->osc2.phase);
        int16_t sampleSub = getWaveSample(voice->sub.waveType, voice->sub.phase);
        int16_t sampleNoise = generateNoise();
        
        // ── FULL FLOAT SIGNAL CHAIN ──────────────────────────────────
        // Convert samples to normalized float (-1.0 to +1.0) ONCE
        float sample1f  = (float)sample1  / 32768.0f;
        float sample2f  = (float)sample2  / 32768.0f;
        float sampleSubf = (float)sampleSub / 32768.0f;
        float sampleNoisef = (float)sampleNoise / 32768.0f;
        
        // --- Oscillator mix (ring mod or additive) ---
        float osc1_2_mixf;
        if (ringModAmount > 0.0f) {
          float additive = sample1f + (sample2f * osc2Level);
          float ringMod  = sample1f * sample2f * osc2Level * 3.0f;  // 3× compensates amplitude loss
          osc1_2_mixf = additive * (1.0f - ringModAmount) + ringMod * ringModAmount;
        } else {
          osc1_2_mixf = sample1f + (sample2f * osc2Level);
        }
        
        // --- Sub and noise mix ---
        float voiceSamplef = (osc1_2_mixf +
                              sampleSubf   * subLevel +
                              sampleNoisef * noiseLevel) / 4.0f;
        
        // --- Envelope, velocity, LFO amp, voiceScale (all in float) ---
        voiceSamplef *= voice->env.level;
        voiceSamplef *= voice->velocity;
        
        if ((lfo1Enabled && lfo1.ampDepth > 0.0f) || (lfo2Enabled && lfo2.ampDepth > 0.0f)) {
          voiceSamplef *= smoothedAmpMod;
        }
        
        voiceSamplef *= voiceScale;
        
        // --- Single conversion back to int32 at the end ---
        int32_t voiceSample = (int32_t)(voiceSamplef * 32768.0f);
        
        mixL += voiceSample;
        mixR += voiceSample;
        
        // Apply modulations to phase increment (use smoothed values)
        
        // Portamento: accumulate in float to avoid uint32_t stepping artifacts
        if (portamentoEnabled && voice->portaActive) {
          voice->portaInc1f   *= voice->portaRate;
          voice->portaInc2f   *= voice->portaRate;
          voice->portaSubIncf *= voice->portaRate;
          // Write to oscillators as uint32_t
          voice->osc1.phaseInc = (uint32_t)voice->portaInc1f;
          voice->osc2.phaseInc = (uint32_t)voice->portaInc2f;
          voice->sub.phaseInc  = (uint32_t)voice->portaSubIncf;
          // Stop when reached target
          bool arrived = (voice->portaRate >= 1.0f) ?
            (voice->portaInc1f >= (float)voice->portaTargetInc1) :
            (voice->portaInc1f <= (float)voice->portaTargetInc1);
          if (arrived) {
            voice->osc1.phaseInc = voice->portaTargetInc1;
            voice->osc2.phaseInc = voice->portaTargetInc2;
            voice->sub.phaseInc  = voice->portaTargetSubInc;
            voice->portaActive   = false;
          }
        }
        
        uint32_t phaseInc1 = voice->osc1.phaseInc;
        uint32_t phaseInc2 = voice->osc2.phaseInc;
        
        if ((lfo1Enabled && lfo1.pitchDepth > 0.0f) || (lfo2Enabled && lfo2.pitchDepth > 0.0f)) {
          phaseInc1 = (uint32_t)(phaseInc1 * smoothedPitchMult);
          phaseInc2 = (uint32_t)(phaseInc2 * smoothedPitchMult);
        }
        
        if ((lfo1Enabled && lfo1.osc2DetuneDepth > 0.0f) || (lfo2Enabled && lfo2.osc2DetuneDepth > 0.0f)) {
          phaseInc2 = (uint32_t)(phaseInc2 * smoothedDetuneMult);
        }
        
        voice->osc1.phase += phaseInc1;
        voice->osc2.phase += phaseInc2;
        voice->sub.phase += voice->sub.phaseInc;  // Sub not affected by LFO pitch mod
        
        processEnvelope(&voice->env);
        
        // (Filter envelope is processed once per buffer, before this loop)
        
        // Auto-release for pluck sounds: if sustain is very low (<1%) and we're in sustain stage
        // This allows pluck sounds (low/zero sustain) to finish naturally without holding the key
        if (voice->env.stage == ENV_SUSTAIN && voice->env.sustainLevel < 0.01f) {
          voice->env.stage = ENV_RELEASE;
        }
        
        // Only deactivate voices that are in RELEASE and have faded out, or are IDLE
        if (voice->env.stage == ENV_IDLE) {
          voice->active = false;
          voice->env.level = 0;
        }
        else if (voice->env.stage == ENV_RELEASE && voice->env.level <= 0.0001f) {
          voice->active = false;
          voice->env.level = 0;
          voice->env.stage = ENV_IDLE;
        }
      }
    }
    
    // Per-voice scaling already applied above - no post-mix scaling needed
    
    float mixLf = mixL / 32768.0f;
    float mixRf = mixR / 32768.0f;
    
    // Simple one-pole low-pass filter (original, working version)
    filterA += filterAStep;
    globalFilter.z1L = filterA * globalFilter.z1L + (1.0f - filterA) * mixLf;
    globalFilter.z1R = filterA * globalFilter.z1R + (1.0f - filterA) * mixRf;
    
    if (isnan(globalFilter.z1L) || isinf(globalFilter.z1L)) globalFilter.z1L = 0;
    if (isnan(globalFilter.z1R) || isinf(globalFilter.z1R)) globalFilter.z1R = 0;
    
    if (globalFilter.z1L > 2.0f) globalFilter.z1L = 2.0f;
    if (globalFilter.z1L < -2.0f) globalFilter.z1L = -2.0f;
    if (globalFilter.z1R > 2.0f) globalFilter.z1R = 2.0f;
    if (globalFilter.z1R < -2.0f) globalFilter.z1R = -2.0f;
    
    int32_t outL, outR;  // computed after delay and chorus
    
    // Delay
    int delaySamples = (int)(delayTime * SAMPLE_RATE);
    if (delaySamples >= DELAY_BUFFER_SIZE) delaySamples = DELAY_BUFFER_SIZE - 1;
    if (delaySamples < 1) delaySamples = 1;
    
    int delayReadPos = delayWritePos - delaySamples;
    if (delayReadPos < 0) delayReadPos += DELAY_BUFFER_SIZE;
    
    float delayOutL = delayBufferL[delayReadPos] * (1.0f / DELAY_SCALE);
    float delayOutR = delayBufferR[delayReadPos] * (1.0f / DELAY_SCALE);
    
    float delayInL = globalFilter.z1L + (delayOutL * delayFeedback);
    float delayInR = globalFilter.z1R + (delayOutR * delayFeedback);
    if (delayInL >  1.9999f) delayInL =  1.9999f;
    if (delayInL < -1.9999f) delayInL = -1.9999f;
    if (delayInR >  1.9999f) delayInR =  1.9999f;
    if (delayInR < -1.9999f) delayInR = -1.9999f;
    delayBufferL[delayWritePos] = (int16_t)(delayInL * DELAY_SCALE);
    delayBufferR[delayWritePos] = (int16_t)(delayInR * DELAY_SCALE);
    
    delayWritePos++;
    if (delayWritePos >= DELAY_BUFFER_SIZE) delayWritePos = 0;
    
    float finalL = globalFilter.z1L * (1.0f - delayMix) + delayOutL * delayMix;
    float finalR = globalFilter.z1R * (1.0f - delayMix) + delayOutR * delayMix;
    
    // Chorus
    if (chorusEnabled) {
      float chorusOutL = 0.0f;
      float chorusOutR = 0.0f;
      
      for (int t = 0; t < 4; t++) {
        int readPos = chorusWritePos - chorusTaps[t];
        if (readPos < 0) readPos += CHORUS_BUFFER_SIZE;
        
        chorusOutL += chorusBufferL[readPos];
        chorusOutR += chorusBufferR[readPos];
      }
      
      chorusOutL = (chorusOutL / 4.0f);  // Removed 1.2× gain
      chorusOutR = (chorusOutR / 4.0f);
      
      // Soft clip input to chorus buffer to prevent buildup
      float chorusInputL = finalL;
      float chorusInputR = finalR;
      if (chorusInputL > 1.0f) chorusInputL = 1.0f;
      if (chorusInputL < -1.0f) chorusInputL = -1.0f;
      if (chorusInputR > 1.0f) chorusInputR = 1.0f;
      if (chorusInputR < -1.0f) chorusInputR = -1.0f;
      
      chorusBufferL[chorusWritePos] = chorusInputL;
      chorusBufferR[chorusWritePos] = chorusInputR;
      
      chorusWritePos++;
      if (chorusWritePos >= CHORUS_BUFFER_SIZE) chorusWritePos = 0;
      
      finalL = finalL * (1.0f - chorusMix) + chorusOutL * chorusMix;  // Removed 1.1× gain
      finalR = finalR * (1.0f - chorusMix) + chorusOutR * chorusMix;
    }
    
    outL = (int32_t)(finalL * masterVolume * 32768.0f);
    outR = (int32_t)(finalR * masterVolume * 32768.0f);
    
    // No 3x digital gain - prevents clipping with ring mod compensation
    // (Ring mod already has 3× compensation, no need for output gain)
    
    // Soft clipping to prevent harsh distortion
    if (outL > 32767) outL = 32767;
    if (outL < -32768) outL = -32768;
    if (outR > 32767) outR = 32767;
    if (outR < -32768) outR = -32768;
    
    buffer[startIdx + i * 2] = (int16_t)outL;
    buffer[startIdx + i * 2 + 1] = (int16_t)outR;
  }
  
  // Advance LFO phases (once per buffer of numSamples/2 stereo samples)
  if (lfo1Enabled) {
    lfo1.phase += lfo1.phaseInc * (numSamples / 2);
  }
  if (lfo2Enabled) {
    lfo2.phase += lfo2.phaseInc * (numSamples / 2);
  }
}

void handleCC(uint8_t controller, uint8_t value, bool fromMIDI = false) {
  switch(controller) {
    case 1:
      filterCutoff = 200.0 + (value / 127.0) * 10000.0;
      Serial.print("Filter: ");
      Serial.print((int)filterCutoff);
      Serial.println(" Hz");
      break;
    case 7:
      if (fromMIDI) {
        // MIDI keyboard volume control - scale by web-locked ceiling
        keyboardVolumeCC = value / 127.0;
        masterVolume = keyboardVolumeCC * volumeCeiling;
        Serial.print("🎹 Keyboard Vol: ");
        Serial.print((int)(keyboardVolumeCC * 100));
        Serial.print("% × Ceiling ");
        Serial.print((int)(volumeCeiling * 100));
        Serial.print("% = ");
        Serial.print((int)(masterVolume * 100));
        Serial.println("%");
      } else {
        // Web UI volume - sets the ceiling
        volumeCeiling = value / 127.0;
        masterVolume = keyboardVolumeCC * volumeCeiling;
        Serial.print("🌐 Web Ceiling: ");
        Serial.print((int)(volumeCeiling * 100));
        Serial.print("% × Keyboard ");
        Serial.print((int)(keyboardVolumeCC * 100));
        Serial.print("% = ");
        Serial.print((int)(masterVolume * 100));
        Serial.println("%");
      }
      break;
    case 12:
      delayTime = 0.01f + (value / 127.0f) * 0.39f;  // 10-400 ms
      Serial.print("Delay Time: ");
      Serial.print((int)(delayTime * 1000));
      Serial.println(" ms");
      break;
    case 13:
      delayFeedback = value / 127.0 * 0.95;
      Serial.print("Delay FB: ");
      Serial.print((int)(delayFeedback * 100));
      Serial.println("%");
      break;
    case 91:
      delayMix = value / 127.0;
      Serial.print("Delay Mix: ");
      Serial.print((int)(delayMix * 100));
      Serial.println("%");
      break;
    case 93:
      chorusMix = value / 127.0;
      Serial.print("Chorus Mix: ");
      Serial.print((int)(chorusMix * 100));
      Serial.println("%");
      break;
    case 94:
      ringModAmount = value / 127.0;
      Serial.print("Ring Mod: ");
      Serial.print((int)(ringModAmount * 100));
      Serial.println("%");
      break;
    case 123:
    case 120:
      allNotesOff();
      Serial.println("All Notes Off");
      break;
    case 50:
      if (value > 0) {
        osc1Wave = (WaveType)((osc1Wave + 1) % 4);
        Serial.print("Osc1: ");
        Serial.print(getWaveName(osc1Wave));
        Serial.print(" | Osc2: ");
        Serial.println(getWaveName(osc2Wave));
      }
      break;
    case 51:
      if (value > 0) {
        osc2Wave = (WaveType)((osc2Wave + 1) % 4);
        Serial.print("Osc1: ");
        Serial.print(getWaveName(osc1Wave));
        Serial.print(" | Osc2: ");
        Serial.println(getWaveName(osc2Wave));
      }
      break;
  }
}

void processMIDI() {
  // 5-pin MIDI commented out - PA10 used by WebSerial for ESP32 #1
  /*
  static uint8_t midiState = 0;
  static uint8_t midiData[3];
  static uint8_t midiDataIndex = 0;
  
  while (MidiSerial.available()) {
    uint8_t data = MidiSerial.read();
    
    if (data == 0xFF || data == 0xFE || data == 0x00) continue;
    
    if (data >= 0x80) {
      if (data >= 0xF8) continue;
      
      if ((data & 0xF0) == 0xD0 || (data & 0xF0) == 0xC0) {
        midiState = data & 0xF0;
        uint8_t channel = data & 0x0F;
        if (channel != midiChannel && midiChannel != 0xFF) {
          midiState = 0;
          continue;
        }
        midiDataIndex = 0;
        continue;
      }
      
      uint8_t newState = data & 0xF0;
      uint8_t channel = data & 0x0F;
      
      if (channel != midiChannel && midiChannel != 0xFF) {
        midiState = 0;
        continue;
      }
      
      if (newState != midiState) {
        midiState = newState;
        midiDataIndex = 0;
      }
    }
    else {
      if (midiState == 0) continue;
      
      midiData[midiDataIndex++] = data;
      
      if (midiState == MIDI_NOTE_ON && midiDataIndex == 2) {
        uint8_t note = midiData[0];
        uint8_t velocity = midiData[1];
        
        if (velocity > 0) {
          noteOn(note, velocity / 127.0);
        } else {
          noteOff(note);
        }
        midiDataIndex = 0;
      }
      else if (midiState == MIDI_NOTE_OFF && midiDataIndex == 2) {
        noteOff(midiData[0]);
        midiDataIndex = 0;
      }
      else if (midiState == MIDI_PITCH_BEND && midiDataIndex == 2) {
        uint16_t bendValue = midiData[0] | (midiData[1] << 7);
        float bendNorm = (bendValue - 8192) / 8192.0;
        pitchBendAmount = bendNorm * pitchBendRange;
        
        applyPitchBend();  // osc1, osc2 and sub
        
        midiDataIndex = 0;
      }
      else if (midiState == 0xD0 && midiDataIndex == 1) {
        midiDataIndex = 0;
      }
      else if (midiState == MIDI_CONTROL_CHANGE && midiDataIndex == 2) {
        handleCC(midiData[0], midiData[1], true);  // fromMIDI = true
        midiDataIndex = 0;
      }
    }
  }
  */  // End of commented 5-pin MIDI code
}

// Non-blocking line reader: collects bytes as they arrive and returns true once
// a complete line is in buf. It never waits for the rest of a line - the old
// readStringUntil() did, ~1 ms per message at 115200 baud, which delayed audio
// rendering past its deadline during chords (random clicks).
bool readLineNB(Stream& s, char* buf, size_t size, size_t& pos) {
  while (s.available()) {
    char c = s.read();
    if (c == '\n') { buf[pos] = 0; pos = 0; return true; }
    if (c == '\r') continue;
    if (pos < size - 1) buf[pos++] = c;   // overlong lines are truncated
  }
  return false;
}

// Both ESP32s send the same text protocol, so one function handles all
// commands (the two former parsers were ~340-line near-identical copies).
//   port:     where replies go (HELLO -> STM32_READY)
//   fromMIDI: true for the BLE keyboard path, so CC7 volume obeys the ceiling
void processCommand(String& cmd, Stream& port, bool fromMIDI) {
  cmd.trim();
  if (cmd.length() == 0) return;
  
  int comma1 = cmd.indexOf(',');
  if (comma1 == -1) {
    if (cmd == "HELLO") {
      port.println("STM32_READY");
    }
    return;
  }
  
  String cmdType = cmd.substring(0, comma1);
  
  if (cmdType == "NOTE") {
    // Handle Note On from ESP32 MIDI relay
    int comma2 = cmd.indexOf(',', comma1 + 1);
    if (comma2 != -1) {
      uint8_t note = cmd.substring(comma1 + 1, comma2).toInt();
      uint8_t velocity = cmd.substring(comma2 + 1).toInt();
      if (velocity > 0) {
        noteOn(note, velocity / 127.0);
      } else {
        noteOff(note);
      }
    }
  }
  else if (cmdType == "NOTEOFF") {
    // Handle Note Off from ESP32 MIDI relay
    uint8_t note = cmd.substring(comma1 + 1).toInt();
    noteOff(note);
  }
  else if (cmdType == "PITCHBEND") {
    // Handle Pitch Bend from ESP32 MIDI relay
    int comma2 = cmd.indexOf(',', comma1 + 1);
    if (comma2 != -1) {
      uint8_t lsb = cmd.substring(comma1 + 1, comma2).toInt();
      uint8_t msb = cmd.substring(comma2 + 1).toInt();
      uint16_t bendValue = lsb | (msb << 7);
      float bendNorm = (bendValue - 8192) / 8192.0;
      pitchBendAmount = bendNorm * pitchBendRange;
      
      applyPitchBend();  // osc1, osc2 and sub
    }
  }
  else if (cmdType == "CC") {
    int comma2 = cmd.indexOf(',', comma1 + 1);
    if (comma2 != -1) {
      int cc = cmd.substring(comma1 + 1, comma2).toInt();
      int value = cmd.substring(comma2 + 1).toInt();
      handleCC(cc, value, fromMIDI);  // true for BLE keyboard: volume ceiling applies
    }
  }
  else if (cmdType == "BTN") {
    int btnNum = cmd.substring(comma1 + 1).toInt();
    if (btnNum == 0) {
      delayPreset = (delayPreset + 1) % 5;
      setDelayPreset(delayPreset);
    }
    else if (btnNum == 1) {
      feedbackPreset = (feedbackPreset + 1) % 3;
      setFeedbackPreset(feedbackPreset);
    }
    else if (btnNum == 2) {
      chorusEnabled = !chorusEnabled;
      Serial.print("Chorus: ");
      Serial.println(chorusEnabled ? "ON" : "OFF");
    }
  }
  else if (cmdType == "PARAM") {
    int comma2 = cmd.indexOf(',', comma1 + 1);
    if (comma2 != -1) {
      String param = cmd.substring(comma1 + 1, comma2);
      int value = cmd.substring(comma2 + 1).toInt();
      
      if (param == "osc1wave") {
        // Direct waveform setting (0-3)
        if (value >= 0 && value <= 3) {
          osc1Wave = (WaveType)value;
          Serial.print("Osc1: ");
          Serial.print(getWaveName(osc1Wave));
          Serial.print(" | Osc2: ");
          Serial.println(getWaveName(osc2Wave));
        }
      }
      else if (param == "osc2wave") {
        // Direct waveform setting (0-3)
        if (value >= 0 && value <= 3) {
          osc2Wave = (WaveType)value;
          Serial.print("Osc1: ");
          Serial.print(getWaveName(osc1Wave));
          Serial.print(" | Osc2: ");
          Serial.println(getWaveName(osc2Wave));
        }
      }
      else if (param == "osc2detune") {
        osc2Detune = (value / 127.0) * 100.0;  // 0-100 cents
        Serial.print("Osc2 Detune: ");
        Serial.print((int)osc2Detune);
        Serial.println(" cents");
      }
      else if (param == "attack") {
        attackTime = 0.001 + (value / 127.0) * 0.999;  // 1-1000ms
        Serial.print("✓ Attack: ");
        Serial.print((int)(attackTime * 1000));
        Serial.print(" ms (value=");
        Serial.print(value);
        Serial.println(")");
      }
      else if (param == "decay") {
        decayTime = 0.001 + (value / 127.0) * 0.999;  // 1-1000ms
        Serial.print("✓ Decay: ");
        Serial.print((int)(decayTime * 1000));
        Serial.print(" ms (value=");
        Serial.print(value);
        Serial.println(")");
      }
      else if (param == "sustain") {
        sustainLevel = value / 127.0;  // 0-100%
        Serial.print("✓ Sustain: ");
        Serial.print((int)(sustainLevel * 100));
        Serial.print("% (value=");
        Serial.print(value);
        Serial.println(")");
      }
      else if (param == "release") {
        releaseTime = 0.001 + (value / 127.0) * 2.999;  // 1-3000ms
        Serial.print("✓ Release: ");
        Serial.print((int)(releaseTime * 1000));
        Serial.print(" ms (value=");
        Serial.print(value);
        Serial.println(")");
      }
      else if (param == "osc2level") {
        osc2Level = value / 127.0;  // 0-100%
        Serial.print("✓ Osc2 Level: ");
        Serial.print((int)(osc2Level * 100));
        Serial.println("%");
      }
      else if (param == "lforate") {
        float rate = 0.1 + (value / 127.0) * 19.9;  // 0.1-20 Hz
        updateLFORate(1, rate);
        Serial.print("✓ LFO1 Rate: ");
        Serial.print(rate, 2);
        Serial.println(" Hz");
      }
      else if (param == "lfopitch") {
        lfo1.pitchDepth = value / 127.0;
        Serial.print("✓ LFO1→Pitch: ");
        Serial.print((int)(lfo1.pitchDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfofilter") {
        lfo1.filterDepth = value / 127.0;
        Serial.print("✓ LFO1→Filter: ");
        Serial.print((int)(lfo1.filterDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfoamp") {
        lfo1.ampDepth = value / 127.0;
        Serial.print("✓ LFO1→Amp: ");
        Serial.print((int)(lfo1.ampDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfodetune") {
        lfo1.osc2DetuneDepth = value / 127.0;
        Serial.print("✓ LFO1→Detune: ");
        Serial.print((int)(lfo1.osc2DetuneDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfowave") {
        if (value >= 0 && value <= 4) {
          lfo1.waveType = (LFOWaveType)value;
          const char* waveNames[] = {"SINE", "TRIANGLE", "SQUARE", "SAW_UP", "SAW_DOWN"};
          Serial.print("✓ LFO1 Wave: ");
          Serial.println(waveNames[value]);
        }
      }
      else if (param == "lfoenable") {
        lfo1Enabled = (value > 0);
        Serial.print("✓ LFO1: ");
        Serial.println(lfo1Enabled ? "ON" : "OFF");
      }
      // LFO2 Parameters
      else if (param == "lfo2rate") {
        float rate = 0.1 + (value / 127.0) * 19.9;  // 0.1-20 Hz
        updateLFORate(2, rate);
        Serial.print("✓ LFO2 Rate: ");
        Serial.print(rate, 2);
        Serial.println(" Hz");
      }
      else if (param == "lfo2pitch") {
        lfo2.pitchDepth = value / 127.0;
        Serial.print("✓ LFO2→Pitch: ");
        Serial.print((int)(lfo2.pitchDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfo2filter") {
        lfo2.filterDepth = value / 127.0;
        Serial.print("✓ LFO2→Filter: ");
        Serial.print((int)(lfo2.filterDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfo2amp") {
        lfo2.ampDepth = value / 127.0;
        Serial.print("✓ LFO2→Amp: ");
        Serial.print((int)(lfo2.ampDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfo2detune") {
        lfo2.osc2DetuneDepth = value / 127.0;
        Serial.print("✓ LFO2→Detune: ");
        Serial.print((int)(lfo2.osc2DetuneDepth * 100));
        Serial.println("%");
      }
      else if (param == "lfo2wave") {
        if (value >= 0 && value <= 4) {
          lfo2.waveType = (LFOWaveType)value;
          const char* waveNames[] = {"SINE", "TRIANGLE", "SQUARE", "SAW_UP", "SAW_DOWN"};
          Serial.print("✓ LFO2 Wave: ");
          Serial.println(waveNames[value]);
        }
      }
      else if (param == "lfo2enable") {
        lfo2Enabled = (value > 0);
        Serial.print("✓ LFO2: ");
        Serial.println(lfo2Enabled ? "ON" : "OFF");
      }
      // Filter Envelope Parameters
      else if (param == "fenvattack") {
        filterEnvAttack = 0.001 + (value / 127.0) * 0.999;  // 1-1000ms
        Serial.print("✓ Filter Env Attack: ");
        Serial.print((int)(filterEnvAttack * 1000));
        Serial.println(" ms");
      }
      else if (param == "fenvdecay") {
        filterEnvDecay = 0.001 + (value / 127.0) * 0.999;  // 1-1000ms
        Serial.print("✓ Filter Env Decay: ");
        Serial.print((int)(filterEnvDecay * 1000));
        Serial.println(" ms");
      }
      else if (param == "fenvsustain") {
        filterEnvSustain = value / 127.0;  // 0-100%
        Serial.print("✓ Filter Env Sustain: ");
        Serial.print((int)(filterEnvSustain * 100));
        Serial.println("%");
      }
      else if (param == "fenvrelease") {
        filterEnvRelease = 0.001 + (value / 127.0) * 2.999;  // 1-3000ms
        Serial.print("✓ Filter Env Release: ");
        Serial.print((int)(filterEnvRelease * 1000));
        Serial.println(" ms");
      }
      else if (param == "fenvamount") {
        filterEnvAmount = value / 127.0;  // 0-100%
        Serial.print("✓ Filter Env Amount: ");
        Serial.print((int)(filterEnvAmount * 100));
        Serial.println("%");
      }
      else if (param == "fenvenable") {
        filterEnvEnabled = (value > 0);
        Serial.print("✓ Filter Envelope: ");
        Serial.println(filterEnvEnabled ? "ON" : "OFF");
      }
      else if (param == "chorusenable") {
        chorusEnabled = (value > 0);
        Serial.print("✓ Chorus: ");
        Serial.println(chorusEnabled ? "ON" : "OFF");
      }
      else if (param == "sublevel") {
        subLevel = value / 127.0;  // 0-100%
        Serial.print("✓ Sub Level: ");
        Serial.print((int)(subLevel * 100));
        Serial.println("%");
      }
      else if (param == "suboctave") {
        subOctave = value;  // 1 or 2
        Serial.print("✓ Sub Octave: -");
        Serial.println(subOctave);
      }
      else if (param == "subwave") {
        subWave = (WaveType)value;  // 0=SINE, 1=SQUARE
        Serial.print("✓ Sub Wave: ");
        Serial.println(getWaveName(subWave));
      }
      else if (param == "noise") {
        noiseLevel = value / 127.0;  // 0-100%
        if (noiseLevel < 0.05) noiseLevel = 0.0;  // Threshold at 5%
        Serial.print("✓ Noise Level: ");
        Serial.print((int)(noiseLevel * 100));
        Serial.println("%");
      }
      else if (param == "ringmod") {
        ringModAmount = value / 127.0;  // 0-100%
        if (ringModAmount < 0.05) ringModAmount = 0.0;  // Threshold at 5%
        Serial.print("✓ Ring Mod: ");
        Serial.print((int)(ringModAmount * 100));
        Serial.println("%");
      }
      else if (param == "portamento") {
        portamentoEnabled = (value > 0);
        Serial.print("✓ Portamento: ");
        Serial.println(portamentoEnabled ? "ON" : "OFF");
      }
      else if (param == "portatime") {
        portamentoTime = value / 127.0 * 0.5;  // 0-500ms
        Serial.print("✓ Portamento Time: ");
        Serial.print((int)(portamentoTime * 1000));
        Serial.println(" ms");
      }
      else if (param == "delaytime") {
        delayTime = 0.01f + (value / 127.0f) * 0.39f;  // 10-400 ms (same as CC12)
        Serial.print("✓ Delay Time: ");
        Serial.print((int)(delayTime * 1000));
        Serial.println(" ms");
      }
      else if (param == "delayfb") {
        delayFeedback = value / 127.0;  // 0-100%
        Serial.print("✓ Delay Feedback: ");
        Serial.print((int)(delayFeedback * 100));
        Serial.println("%");
      }
      else if (param == "delaymix") {
        delayMix = value / 127.0;  // 0-100%
        if (delayMix < 0.01) delayMix = 0.0;  // Threshold
        Serial.print("✓ Delay Mix: ");
        Serial.print((int)(delayMix * 100));
        Serial.println("%");
      }
      else if (param == "delaypreset") {
        setDelayPreset(value);  // Call the preset function
      }
    }
  }
}

void processESP32Commands() {
  static char lineBuf[128];
  static size_t linePos = 0;
  while (readLineNB(ESP32Serial, lineBuf, sizeof(lineBuf), linePos)) {
    String cmd = lineBuf;
    processCommand(cmd, ESP32Serial, true);   // ESP32 #2: BLE MIDI keyboard
  }
}


void processWebControl() {
  static char lineBuf[128];
  static size_t linePos = 0;
  while (readLineNB(WebSerial, lineBuf, sizeof(lineBuf), linePos)) {
    String cmd = lineBuf;
    processCommand(cmd, WebSerial, false);    // ESP32 #1: web controller
  }
}


void setup() {
  Serial.begin(115200);
  ESP32Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== STM32 Synth + WiFi + Presets ===");
  
  // Must run before anything that uses SAMPLE_RATE (filter, LFOs, envelopes)
  SAMPLE_RATE = configureI2SClock(TARGET_SAMPLE_RATE);
  
  // --- DIAGNOSTICS (temporary): clock tree as actually configured ---
  {
    uint32_t pc = RCC->PLLCFGR;
    uint32_t pi = RCC->PLLI2SCFGR;
    Serial.print("SystemCoreClock: "); Serial.print(SystemCoreClock); Serial.println(" Hz");
    Serial.print("HSE_VALUE: "); Serial.println((uint32_t)HSE_VALUE);
    Serial.print("Main PLL: M="); Serial.print(pc & 0x3F);
    Serial.print(" N="); Serial.print((pc >> 6) & 0x1FF);
    Serial.print(" P="); Serial.print((((pc >> 16) & 0x3) + 1) * 2);
    Serial.print(" SRC="); Serial.println((pc & RCC_PLLCFGR_PLLSRC) ? "HSE" : "HSI");
    Serial.print("PLLI2SCFGR readback: 0x"); Serial.print(pi, HEX);
    Serial.print("  M="); Serial.print(pi & 0x3F);
    Serial.print(" N="); Serial.print((pi >> 6) & 0x1FF);
    Serial.print(" R="); Serial.println((pi >> 28) & 0x7);
  }
  // Cycle counter for render-time measurement
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  
  Serial.println("Ready!");
  
  // MidiSerial.begin(MIDI_BAUD_RATE);  // COMMENTED OUT - using WebSerial
  WebSerial.begin(115200);  // ESP32 #1 Web Control on PA10
  
  for (int i = 0; i < MAX_VOICES; i++) {
    initVoice(&voices[i]);
  }
  
  generateWavetables();
  
  globalFilter.z1L = 0;
  globalFilter.z1R = 0;
  updateGlobalFilter(filterCutoff);
  
  // Initialize LFO1
  lfo1.phase = 0;
  lfo1.rate = 5.0;  // 5 Hz default
  updateLFORate(1, 5.0);
  lfo1.waveType = LFO_SINE;
  lfo1.pitchDepth = 0.0;
  lfo1.filterDepth = 0.0;
  lfo1.ampDepth = 0.0;
  lfo1.osc2DetuneDepth = 0.0;
  lfo1Enabled = false;
  
  // Initialize LFO2
  lfo2.phase = 0;
  lfo2.rate = 2.0;  // 2 Hz default (different from LFO1)
  updateLFORate(2, 2.0);
  lfo2.waveType = LFO_TRIANGLE;  // Different waveform
  lfo2.pitchDepth = 0.0;
  lfo2.filterDepth = 0.0;
  lfo2.ampDepth = 0.0;
  lfo2.osc2DetuneDepth = 0.0;
  lfo2Enabled = false;
  
  for (int i = 0; i < DELAY_BUFFER_SIZE; i++) {
    delayBufferL[i] = 0;
    delayBufferR[i] = 0;
  }
  
  for (int i = 0; i < CHORUS_BUFFER_SIZE; i++) {
    chorusBufferL[i] = 0;
    chorusBufferR[i] = 0;
  }
  
  // PA0/PA1 unused (old breadboard buttons removed); PA2 is ESP32Serial TX
  
  // PLLI2S already configured by configureI2SClock() at the top of setup()
  
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_SPI2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  
  SPI2->I2SCFGR = 0;
  SPI2->I2SPR = i2sPrescaler;  // Computed by configureI2SClock()
  SPI2->I2SCFGR = (1 << 11) | (2 << 8);
  
  hdma_spi2_tx.Instance = DMA1_Stream4;
  hdma_spi2_tx.Init.Channel = DMA_CHANNEL_0;
  hdma_spi2_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
  hdma_spi2_tx.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_spi2_tx.Init.MemInc = DMA_MINC_ENABLE;
  hdma_spi2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_spi2_tx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_spi2_tx.Init.Mode = DMA_CIRCULAR;
  hdma_spi2_tx.Init.Priority = DMA_PRIORITY_HIGH;
  hdma_spi2_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  
  HAL_DMA_Init(&hdma_spi2_tx);
  HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
  
  __HAL_DMA_ENABLE_IT(&hdma_spi2_tx, DMA_IT_TC);
  __HAL_DMA_ENABLE_IT(&hdma_spi2_tx, DMA_IT_HT);
  
  SPI2->CR2 |= SPI_CR2_TXDMAEN;
  
  for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
    audioBuffer[i] = 0;
  }
  
  HAL_DMA_Start(&hdma_spi2_tx, (uint32_t)audioBuffer, (uint32_t)&(SPI2->DR), AUDIO_BUFFER_SIZE);
  SPI2->I2SCFGR |= (1 << 10);
  
  pinMode(PC13, OUTPUT);
}

void loop() {
  processMIDI();  // 5-pin MIDI (commented out inside)
  processESP32Commands();  // ESP32 #2 BLE MIDI on PA3
  processWebControl();  // ESP32 #1 Web Control on PA10
  
  static uint32_t lastHalf = 0;
  static uint32_t lastFull = 0;
  
  if (halfComplete != lastHalf) {
    if (halfComplete - lastHalf > 1) diagMissedBuffers += halfComplete - lastHalf - 1;
    uint32_t t0 = DWT->CYCCNT;
    renderAudio(audioBuffer, 0, AUDIO_BUFFER_SIZE / 2);
    uint32_t dt = DWT->CYCCNT - t0;
    if (dt > diagMaxRenderCycles) diagMaxRenderCycles = dt;
    uint32_t lat = DWT->CYCCNT - diagHalfStamp;
    if (lat > diagMaxLatencyCycles) diagMaxLatencyCycles = lat;
    if (lat > (uint32_t)((float)SystemCoreClock * (AUDIO_BUFFER_SIZE / 4) / SAMPLE_RATE)) diagLateBuffers++;
    lastHalf = halfComplete;
  }
  
  if (fullComplete != lastFull) {
    if (fullComplete - lastFull > 1) diagMissedBuffers += fullComplete - lastFull - 1;
    uint32_t t0 = DWT->CYCCNT;
    renderAudio(audioBuffer, AUDIO_BUFFER_SIZE / 2, AUDIO_BUFFER_SIZE / 2);
    uint32_t dt = DWT->CYCCNT - t0;
    if (dt > diagMaxRenderCycles) diagMaxRenderCycles = dt;
    uint32_t lat = DWT->CYCCNT - diagFullStamp;
    if (lat > diagMaxLatencyCycles) diagMaxLatencyCycles = lat;
    if (lat > (uint32_t)((float)SystemCoreClock * (AUDIO_BUFFER_SIZE / 4) / SAMPLE_RATE)) diagLateBuffers++;
    lastFull = fullComplete;
  }
  
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 1000) {
    lastBlink = millis();
    digitalWrite(PC13, !digitalRead(PC13));
    
    // DIAGNOSTICS (temporary): worst-case render time vs. half-buffer budget
    {
      float budgetUs = (AUDIO_BUFFER_SIZE / 4) * 1e6f / SAMPLE_RATE;  // 128 frames
      float maxUs = diagMaxRenderCycles * 1e6f / SystemCoreClock;
      int voicesOn = 0;
      for (int i = 0; i < MAX_VOICES; i++) if (voices[i].active) voicesOn++;
      Serial.print("Render max "); Serial.print(maxUs, 0);
      Serial.print(" us / budget "); Serial.print(budgetUs, 0);
      Serial.print(" us ("); Serial.print(100.0f * maxUs / budgetUs, 0);
      Serial.print("%)  deadline used max ");
      Serial.print(100.0f * (diagMaxLatencyCycles * 1e6f / SystemCoreClock) / budgetUs, 0);
      Serial.print("%  late="); Serial.print(diagLateBuffers);
      Serial.print("  missed="); Serial.print(diagMissedBuffers);
      Serial.print("  voices="); Serial.println(voicesOn);
      diagMaxRenderCycles = 0;
      diagMaxLatencyCycles = 0;
    }
    
    for (int i = 0; i < MAX_VOICES; i++) {
      if (voices[i].active && (millis() - voices[i].noteOnTime) > 30000) {
        voices[i].active = false;
        voices[i].env.stage = ENV_IDLE;
        voices[i].env.level = 0;
        Serial.print("Watchdog killed stuck note: ");
        Serial.println(voices[i].note);
      }
    }
  }
  
}

extern "C" void DMA1_Stream4_IRQHandler(void) {
  if (__HAL_DMA_GET_FLAG(&hdma_spi2_tx, DMA_FLAG_HTIF0_4)) {
    __HAL_DMA_CLEAR_FLAG(&hdma_spi2_tx, DMA_FLAG_HTIF0_4);
    diagHalfStamp = DWT->CYCCNT;
    halfComplete++;
  }
  
  if (__HAL_DMA_GET_FLAG(&hdma_spi2_tx, DMA_FLAG_TCIF0_4)) {
    __HAL_DMA_CLEAR_FLAG(&hdma_spi2_tx, DMA_FLAG_TCIF0_4);
    diagFullStamp = DWT->CYCCNT;
    fullComplete++;
  }
}
