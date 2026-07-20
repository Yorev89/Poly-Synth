/*
 * ESP32 #1 - BLE MIDI Receiver with UART Output
 * For STM32 MIDI Synthesizer
 * 
 * Features:
 * - BLE MIDI client (connects to keyboards/controllers)
 * - UART output (sends MIDI commands to STM32)
 * - Auto-discovers and connects to BLE MIDI devices
 * 
 * Hardware Connections:
 * ESP32 #1 GPIO17 (TX) → STM32 PA3 (RX)
 * ESP32 #1 GPIO16 (RX) → STM32 PA2 (TX) (optional, for responses)
 * GND → GND (CRITICAL!)
 * 
 * UART Configuration:
 * - Speed: 115200 baud
 * - TX: GPIO17
 * - RX: GPIO16
 * 
 * Compatible with:
 * - BLE MIDI keyboards (Arturia, Korg, etc.)
 * - Most BLE MIDI devices
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
  Serial.println("  ESP32 #1 - BLE MIDI → UART Bridge");
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

void parseBLEMidiData(uint8_t* data, size_t length) {
  if (length < 3) return;
  
  uint8_t runningStatus = 0;
  size_t i = 1;  // Skip timestamp byte
  
  while (i < length) {
    uint8_t byte = data[i];
    
    // Status byte
    if (byte & 0x80) {
      if (byte >= 0x80 && byte <= 0xEF && i + 1 < length && !(data[i+1] & 0x80)) {
        runningStatus = byte;
        i++;
        continue;
      }
      i++;
      continue;
    }
    
    if (runningStatus == 0) {
      i++;
      continue;
    }
    
    uint8_t msgType = runningStatus & 0xF0;
    
    // 2-byte messages
    if (msgType == 0x80 || msgType == 0x90 || msgType == 0xB0 || msgType == 0xE0) {
      if (i + 1 >= length) break;
      
      uint8_t data1 = data[i];
      uint8_t data2 = data[i + 1];
      
      if ((data1 & 0x80) || (data2 & 0x80)) {
        i++;
        continue;
      }
      
      if (msgType == 0x90) {
        if (data2 == 0) {
          Serial.printf("BLE NoteOff: N%d\n", data1);
          sendMidiCommand(0x80, data1, 0);
        } else {
          Serial.printf("BLE NoteOn: N%d V%d\n", data1, data2);
          sendMidiCommand(runningStatus, data1, data2);
        }
      }
      else if (msgType == 0x80) {
        Serial.printf("BLE NoteOff: N%d\n", data1);
        sendMidiCommand(runningStatus, data1, data2);
      }
      else if (msgType == 0xB0) {
        Serial.printf("BLE CC: %d=%d\n", data1, data2);
        sendMidiCommand(runningStatus, data1, data2);
      }
      else if (msgType == 0xE0) {
        Serial.printf("BLE Bend: %d,%d\n", data1, data2);
        sendMidiCommand(runningStatus, data1, data2);
      }
      
      i += 2;
    } else {
      i++;
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
