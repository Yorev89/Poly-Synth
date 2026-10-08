/*
 * ESP32 #2 - BLE MIDI Receiver for the Poly-Synth
 * https://github.com/Yorev89/Poly-Synth
 *
 * Features:
 * - BLE MIDI client: connects to a BLE MIDI keyboard (preferred: SMK25Mini,
 *   falls back to any BLE MIDI device after 10 s)
 * - Spec-compliant BLE MIDI parsing (timestamps, running status, SysEx skipped)
 * - Forwards notes, CC and pitch bend to the STM32 as text commands
 *
 * Hardware connections:
 * - GPIO17 (TX) -> STM32 PA3 (USART2 RX)
 * - GPIO16 (RX) <- STM32 PA2 (USART2 TX)  (not used yet; reserved for replies)
 * - GND -> GND (required)
 *
 * UART: 115200 baud, one command per line:
 * - NOTE,<note>,<velocity> | NOTEOFF,<note> | PITCHBEND,<lsb>,<msb> | CC,<num>,<value>
 */

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>

// BLE MIDI Standard UUIDs
#define MIDI_SERVICE_UUID        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define MIDI_CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"

// Preferred keyboard - will try this first for fast connection
const char* PREFERRED_KEYBOARD = "SMK25Mini";

// Fallback mode - if preferred not found, accept any BLE MIDI device
bool useAnyKeyboard = false;
unsigned long scanStartTime = 0;
#define PREFERRED_SCAN_TIME 10000  // Try preferred keyboard for 10 seconds

// UART Configuration
#define UART_TX 17
#define UART_RX 16
#define UART_BAUD 115200

HardwareSerial STM32Serial(1);  // Use UART1

// BLE State
static BLEClient* pClient = nullptr;
static BLERemoteCharacteristic* pMidiCharacteristic = nullptr;
static bool bleConnected = false;
static volatile bool pendingAllNotesOff = false;  // set on disconnect, sent from loop()
static bool bleScanning = false;
static BLEAdvertisedDevice* targetDevice = nullptr;

// Statistics
unsigned long midiMessagesReceived = 0;
unsigned long commandsSent = 0;
unsigned long lastActivityTime = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("  ESP32 #2 - BLE MIDI → UART Bridge");
  Serial.println("  with Smart Reconnection");
  Serial.println("========================================");
  
  // Initialize UART for STM32 communication
  STM32Serial.begin(UART_BAUD, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("✓ UART initialized (GPIO17=TX, GPIO16=RX)");
  
  // Initialize BLE
  BLEDevice::init("ESP32_BLE_MIDI");
  Serial.println("✓ BLE initialized");
  
  Serial.print("Preferred keyboard: ");
  Serial.println(PREFERRED_KEYBOARD);
  Serial.println("(Will fallback to ANY BLE MIDI device if not found)");
  
  Serial.println("\n🔍 Starting smart scan...");
  Serial.println("========================================\n");
  
  delay(500);
  startBLEScan();
}

void loop() {
  // Keyboard disconnected: release all notes on the STM32 (CC 123)
  if (pendingAllNotesOff) {
    pendingAllNotesOff = false;
    sendMidiCommand(0xB0, 123, 0);
  }
  
  // Handle scanning timeout and fallback to any keyboard
  if (bleScanning && !useAnyKeyboard && millis() - scanStartTime > PREFERRED_SCAN_TIME) {
    Serial.println("⏱️  Preferred keyboard not found after 10 seconds");
    Serial.println("🔄 Switching to scan for ANY BLE MIDI device...");
    BLEDevice::getScan()->stop();
    bleScanning = false;
    useAnyKeyboard = true;
    delay(500);
    startBLEScan();
  }
  
  // If scanning finished and device found, try to connect
  static unsigned long lastConnectionAttempt = 0;
  if (!bleScanning && !bleConnected && targetDevice != nullptr && millis() - lastConnectionAttempt > 5000) {
    lastConnectionAttempt = millis();
    Serial.println("✓ Device found, connecting...");
    if (connectToServer()) {
      Serial.println("✓✓✓ Connected successfully!");
    } else {
      Serial.println("✗ Connection failed, will retry");
      // Clear target and re-scan
      delete targetDevice;
      targetDevice = nullptr;
    }
  }
  
  // Periodic re-scan if no device found yet (every 30 seconds)
  static unsigned long lastReScan = 0;
  if (!bleConnected && !bleScanning && targetDevice == nullptr && millis() - lastReScan > 30000) {
    lastReScan = millis();
    Serial.println("\n⟳ No device found, re-scanning...");
    // Reset to preferred keyboard mode for next scan
    useAnyKeyboard = false;
    startBLEScan();
  }
  
  // Auto-reconnect if disconnected (device was previously connected)
  static unsigned long lastReconnect = 0;
  if (!bleConnected && !bleScanning && targetDevice != nullptr && millis() - lastReconnect > 10000) {
    lastReconnect = millis();
    Serial.println("⟳ Attempting to reconnect...");
    connectToServer();
  }
  
  // Status update every 30 seconds
  static unsigned long lastStatusUpdate = 0;
  if (bleConnected && millis() - lastStatusUpdate > 30000) {
    lastStatusUpdate = millis();
    Serial.print("📊 BLE: ✓ | MIDI msgs: ");
    Serial.print(midiMessagesReceived);
    Serial.print(" | Commands sent: ");
    Serial.print(commandsSent);
    Serial.print(" | Last activity: ");
    Serial.print((millis() - lastActivityTime) / 1000);
    Serial.println("s ago");
  }
  
  delay(2);
}

// ========== UART COMMAND SENDER ==========

void sendCommandToSTM32(const char* cmd) {
  STM32Serial.println(cmd);
  commandsSent++;
}

// ========== MIDI TO COMMAND CONVERSION ==========

void sendMidiCommand(uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t msgType = status & 0xF0;
  char cmd[32];
  
  if (msgType == 0x90) {  // Note On
    if (data2 > 0) {
      sprintf(cmd, "NOTE,%d,%d", data1, data2);
    } else {
      sprintf(cmd, "NOTEOFF,%d", data1);
    }
  }
  else if (msgType == 0x80) {  // Note Off
    sprintf(cmd, "NOTEOFF,%d", data1);
  }
  else if (msgType == 0xB0) {  // Control Change
    sprintf(cmd, "CC,%d,%d", data1, data2);
  }
  else if (msgType == 0xE0) {  // Pitch Bend
    sprintf(cmd, "PITCHBEND,%d,%d", data1, data2);
  }
  else {
    return; // Unsupported message type
  }
  
  sendCommandToSTM32(cmd);
  
  // Debug output
  Serial.print("→ STM32: ");
  Serial.println(cmd);
  
  midiMessagesReceived++;
  lastActivityTime = millis();
}

// ========== BLE MIDI PARSER ==========

// BLE MIDI packet: [header][timestamp][status][data...][timestamp][status or data...]...
// The header and every timestamp byte have bit 7 set, just like MIDI status
// bytes. Rule from the BLE-MIDI spec: a byte with bit 7 set is a TIMESTAMP,
// unless it directly follows a timestamp - then it is a STATUS byte.
// Running status: after a timestamp (or after a complete message) data bytes
// may follow without a status byte; they reuse the last status.
// (The old parser treated a timestamp followed by running-status data as a new
// status byte, turning chord notes into wrong messages: missing notes, stray
// pitch bends or CCs.)
static void dispatchMidi(uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t msgType = status & 0xF0;
  if (msgType == 0x90) {
    if (data2 == 0) {
      Serial.printf("BLE NoteOff: N%d\n", data1);
      sendMidiCommand(0x80, data1, 0);
    } else {
      Serial.printf("BLE NoteOn: N%d V%d\n", data1, data2);
      sendMidiCommand(status, data1, data2);
    }
  }
  else if (msgType == 0x80) {
    Serial.printf("BLE NoteOff: N%d\n", data1);
    sendMidiCommand(status, data1, data2);
  }
  else if (msgType == 0xB0) {
    Serial.printf("BLE CC: %d=%d\n", data1, data2);
    sendMidiCommand(status, data1, data2);
  }
  else if (msgType == 0xE0) {
    Serial.printf("BLE Bend: %d,%d\n", data1, data2);
    sendMidiCommand(status, data1, data2);
  }
  // 0xA0 aftertouch, 0xC0 program change, 0xD0 channel pressure: ignored
}

void parseBLEMidiData(uint8_t* data, size_t length) {
  if (length < 3) return;
  if (!(data[0] & 0x80)) return;        // not a valid header byte
  
  uint8_t runningStatus = 0;
  uint8_t msgData[2];
  uint8_t dataCount = 0;
  bool prevWasTimestamp = false;
  bool inSysEx = false;
  
  for (size_t i = 1; i < length; i++) {   // data[0] is the header
    uint8_t b = data[i];
    
    if (b & 0x80) {
      if (!prevWasTimestamp) {            // timestamp byte
        prevWasTimestamp = true;
        continue;
      }
      prevWasTimestamp = false;           // status byte (follows a timestamp)
      if (inSysEx) {
        if (b == 0xF7) inSysEx = false;   // end of SysEx
        continue;
      }
      if (b == 0xF0) { inSysEx = true; runningStatus = 0; continue; }
      if (b >= 0xF8) continue;            // real-time (clock etc.): no effect on running status
      if (b >= 0xF0) { runningStatus = 0; continue; }  // other system common: not used
      runningStatus = b;                  // channel message
      dataCount = 0;
      continue;
    }
    
    // Data byte
    prevWasTimestamp = false;
    if (inSysEx || runningStatus == 0) continue;
    
    msgData[dataCount++] = b;
    uint8_t msgType = runningStatus & 0xF0;
    uint8_t needed = (msgType == 0xC0 || msgType == 0xD0) ? 1 : 2;
    if (dataCount >= needed) {
      dispatchMidi(runningStatus, msgData[0], needed == 2 ? msgData[1] : 0);
      dataCount = 0;                      // running status: next data starts a new message
    }
  }
}

// ========== BLE CALLBACKS ==========

static void notifyCallback(BLERemoteCharacteristic* pCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  parseBLEMidiData(pData, length);
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    bleConnected = true;
    Serial.println("✓ BLE Connected");
  }
  
  void onDisconnect(BLEClient* pclient) {
    bleConnected = false;
    pMidiCharacteristic = nullptr;
    pendingAllNotesOff = true;  // keys held at disconnect would never get a note-off
    Serial.println("✗ BLE Disconnected");
  }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // Only check devices with MIDI service
    if (!advertisedDevice.haveServiceUUID()) return;
    
    BLEUUID midiService(MIDI_SERVICE_UUID);
    if (!advertisedDevice.isAdvertisingService(midiService)) return;
    
    // This is a BLE MIDI device!
    String deviceName = advertisedDevice.haveName() ? advertisedDevice.getName().c_str() : "(No name)";
    
    Serial.print("✓ Found BLE MIDI device: ");
    Serial.println(deviceName);
    
    // Check if this is our preferred keyboard
    if (!useAnyKeyboard && deviceName == PREFERRED_KEYBOARD) {
      Serial.println("✓✓✓ This is our preferred keyboard!");
      BLEDevice::getScan()->stop();
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
      bleScanning = false;
      return;
    }
    
    // In fallback mode, accept any BLE MIDI device
    if (useAnyKeyboard) {
      Serial.print("✓ Connecting to: ");
      Serial.println(deviceName);
      BLEDevice::getScan()->stop();
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
      bleScanning = false;
    }
  }
};

// ========== BLE CONNECTION ==========

bool connectToServer() {
  if (targetDevice == nullptr) return false;
  
  Serial.println("Attempting to connect to BLE MIDI device...");
  
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());
  
  if (!pClient->connect(targetDevice)) {
    Serial.println("✗ Failed to connect to device");
    return false;
  }
  
  Serial.println("✓ Connected to device, looking for MIDI service...");
  
  BLERemoteService* pRemoteService = pClient->getService(MIDI_SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.println("✗ MIDI service not found");
    pClient->disconnect();
    return false;
  }
  
  Serial.println("✓ Found MIDI service, looking for characteristic...");
  
  pMidiCharacteristic = pRemoteService->getCharacteristic(MIDI_CHARACTERISTIC_UUID);
  if (pMidiCharacteristic == nullptr) {
    Serial.println("✗ MIDI characteristic not found");
    pClient->disconnect();
    return false;
  }
  
  Serial.println("✓ Found MIDI characteristic, subscribing to notifications...");
  
  if (pMidiCharacteristic->canNotify()) {
    pMidiCharacteristic->registerForNotify(notifyCallback);
  }
  
  bleConnected = true;
  Serial.println("✓✓✓ BLE MIDI ready! Play your keyboard!");
  return true;
}

void startBLEScan() {
  if (!useAnyKeyboard) {
    Serial.print("🔍 Scanning for preferred keyboard: ");
    Serial.println(PREFERRED_KEYBOARD);
    Serial.println("(Will scan for ANY BLE MIDI device if not found after 10 seconds)");
  } else {
    Serial.println("🔍 Scanning for ANY BLE MIDI device...");
  }
  Serial.println("(Make sure keyboard is ON and in pairing mode)");
  
  bleScanning = true;
  scanStartTime = millis();
  
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  
  pBLEScan->start(0, false);  // Continuous scan, will be stopped by callback or timeout
}
