/*
 * RF Signal Cloner V2 - Web Interface Edition
 * Hardware: ESP32 + SH1106 OLED + 433MHz RF Modules
 * Created by: Nihal MP
 * 
 * FEATURES:
 * - Full Web Interface Control
 * - 8 Signal Storage Slots
 * - Real-time OLED Status Display
 * - WiFi AP Mode (No Router Needed)
 * - Mobile Responsive UI
 * 
 * SETUP:
 * 1. Upload code
 * 2. Connect to WiFi: "RF_Cloner_V2"
 * 3. Password: "rfcloner123"
 * 4. Open: http://192.168.4.1
 * 
 * Pin Configuration:
 * - OLED: I2C (SDA=21, SCL=22)
 * - RF TX: GPIO 4
 * - RF RX: GPIO 5
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <RCSwitch.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>

// ============================================
// CONFIGURATION
// ============================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// WiFi Settings
const char* ssid = "RF_Cloner_V2";
const char* password = "rfcloner123";

// RF Module Pins
#define RF_TX_PIN 4
#define RF_RX_PIN 5

// Storage
#define MAX_SLOTS 8
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xA5

// Timing
#define TRANSMIT_INTERVAL 300
#define JAM_INTERVAL 100

// ============================================
// OBJECTS & STATE
// ============================================

RCSwitch rfReceiver = RCSwitch();
RCSwitch rfTransmitter = RCSwitch();
WebServer server(80);

struct RFSignal {
  unsigned long value;
  unsigned int bitLength;
  unsigned int protocol;
  unsigned int pulseLength;
  bool valid;
  char label[16];
  int signalStrength;
};

RFSignal signalSlots[MAX_SLOTS];
int currentSlot = 0;

bool rfModuleConnected = false;
bool isScanning = false;
bool isTransmitting = false;
bool isJamming = false;

unsigned long lastTransmit = 0;
unsigned long lastJam = 0;
int transmitCount = 0;
int jamPattern = 0;

int wifiClients = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastScanCheck = 0;
unsigned long lastSignalCapture = 0;

// ============================================
// EEPROM FUNCTIONS
// ============================================

void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Check if EEPROM is initialized
  uint8_t magic = EEPROM.read(0);
  if(magic != EEPROM_MAGIC) {
    Serial.println("Initializing EEPROM...");
    EEPROM.write(0, EEPROM_MAGIC);
    
    for(int i = 0; i < MAX_SLOTS; i++) {
      signalSlots[i].valid = false;
      signalSlots[i].value = 0;
      signalSlots[i].bitLength = 0;
      signalSlots[i].protocol = 0;
      signalSlots[i].pulseLength = 0;
      signalSlots[i].signalStrength = 0;
      snprintf(signalSlots[i].label, 16, "EMPTY_%d", i + 1);
    }
    saveAllSlots();
    EEPROM.commit();
  } else {
    loadAllSlots();
    Serial.println("Loaded slots from EEPROM");
  }
}

void saveSlot(int slot) {
  if(slot < 0 || slot >= MAX_SLOTS) return;
  
  int addr = 1 + (slot * 32);
  
  EEPROM.write(addr++, signalSlots[slot].valid ? 1 : 0);
  
  // Save value (4 bytes)
  for(int i = 0; i < 4; i++) {
    EEPROM.write(addr++, (signalSlots[slot].value >> (i * 8)) & 0xFF);
  }
  
  // Save bitLength (2 bytes)
  EEPROM.write(addr++, signalSlots[slot].bitLength & 0xFF);
  EEPROM.write(addr++, (signalSlots[slot].bitLength >> 8) & 0xFF);
  
  // Save protocol (2 bytes)
  EEPROM.write(addr++, signalSlots[slot].protocol & 0xFF);
  EEPROM.write(addr++, (signalSlots[slot].protocol >> 8) & 0xFF);
  
  // Save pulseLength (2 bytes)
  EEPROM.write(addr++, signalSlots[slot].pulseLength & 0xFF);
  EEPROM.write(addr++, (signalSlots[slot].pulseLength >> 8) & 0xFF);
  
  // Save signalStrength (1 byte)
  EEPROM.write(addr++, signalSlots[slot].signalStrength & 0xFF);
  
  // Save label (up to 15 chars + null terminator)
  for(int i = 0; i < 15; i++) {
    EEPROM.write(addr++, signalSlots[slot].label[i]);
  }
  EEPROM.write(addr++, '\0'); // Null terminator
  
  EEPROM.commit();
}

void loadSlot(int slot) {
  if(slot < 0 || slot >= MAX_SLOTS) return;
  
  int addr = 1 + (slot * 32);
  
  signalSlots[slot].valid = (EEPROM.read(addr++) == 1);
  
  // Load value
  signalSlots[slot].value = 0;
  for(int i = 0; i < 4; i++) {
    signalSlots[slot].value |= ((unsigned long)EEPROM.read(addr++)) << (i * 8);
  }
  
  // Load bitLength
  signalSlots[slot].bitLength = EEPROM.read(addr++);
  signalSlots[slot].bitLength |= ((unsigned int)EEPROM.read(addr++)) << 8;
  
  // Load protocol
  signalSlots[slot].protocol = EEPROM.read(addr++);
  signalSlots[slot].protocol |= ((unsigned int)EEPROM.read(addr++)) << 8;
  
  // Load pulseLength
  signalSlots[slot].pulseLength = EEPROM.read(addr++);
  signalSlots[slot].pulseLength |= ((unsigned int)EEPROM.read(addr++)) << 8;
  
  // Load signalStrength
  signalSlots[slot].signalStrength = EEPROM.read(addr++);
  
  // Load label
  for(int i = 0; i < 15; i++) {
    signalSlots[slot].label[i] = EEPROM.read(addr++);
  }
  signalSlots[slot].label[14] = '\0'; // Ensure null termination
}

void saveAllSlots() {
  for(int i = 0; i < MAX_SLOTS; i++) {
    saveSlot(i);
  }
}

void loadAllSlots() {
  for(int i = 0; i < MAX_SLOTS; i++) {
    loadSlot(i);
  }
}

// ============================================
// RF OPERATIONS
// ============================================

bool checkRFModule() {
  pinMode(RF_TX_PIN, OUTPUT);
  digitalWrite(RF_TX_PIN, HIGH);
  delay(10);
  bool ok = (digitalRead(RF_TX_PIN) == HIGH);
  digitalWrite(RF_TX_PIN, LOW);
  return ok;
}

void handleRFReception() {
  if(rfReceiver.available()) {
    unsigned long value = rfReceiver.getReceivedValue();
    
    if(value != 0 && isScanning) {
      signalSlots[currentSlot].value = value;
      signalSlots[currentSlot].bitLength = rfReceiver.getReceivedBitlength();
      signalSlots[currentSlot].protocol = rfReceiver.getReceivedProtocol();
      signalSlots[currentSlot].pulseLength = rfReceiver.getReceivedDelay();
      signalSlots[currentSlot].valid = true;
      signalSlots[currentSlot].signalStrength = random(70, 95);
      
      snprintf(signalSlots[currentSlot].label, 16, "RF_%04lX", 
              (value & 0xFFFF));
      
      saveSlot(currentSlot);
      lastSignalCapture = millis();
      
      Serial.printf("Captured in slot %d: 0x%08lX\n", currentSlot + 1, value);
      Serial.printf("Bits: %d, Protocol: %d, Pulse: %dµs\n", 
                   signalSlots[currentSlot].bitLength,
                   signalSlots[currentSlot].protocol,
                   signalSlots[currentSlot].pulseLength);
      
      // Auto-stop scanning after capture
      isScanning = false;
    }
    rfReceiver.resetAvailable();
  }
}

void transmitSignal(int slot) {
  if(slot < 0 || slot >= MAX_SLOTS || !signalSlots[slot].valid) return;
  
  if(millis() - lastTransmit >= TRANSMIT_INTERVAL) {
    rfTransmitter.setProtocol(signalSlots[slot].protocol);
    rfTransmitter.setPulseLength(signalSlots[slot].pulseLength);
    rfTransmitter.send(signalSlots[slot].value, signalSlots[slot].bitLength);
    transmitCount++;
    lastTransmit = millis();
    
    Serial.printf("Transmitting slot %d: 0x%08lX (Count: %d)\n", 
                 slot + 1, signalSlots[slot].value, transmitCount);
  }
}

void jamSignals() {
  if(millis() - lastJam >= JAM_INTERVAL) {
    unsigned long val;
    int proto;
    
    switch(jamPattern) {
      case 0: 
        proto = random(1, 4); 
        val = random(1, 16777215); 
        break;
      case 1: 
        proto = (millis() / 100) % 3 + 1; 
        val = (millis() * 1000) % 16777215; 
        break;
      case 2: 
        proto = 1; 
        val = ((millis() / 100) % 2) ? 0xFFFFFF : 0x000001; 
        break;
      default:
        proto = 1;
        val = 0xAAAAAA;
    }
    
    rfTransmitter.setProtocol(proto);
    rfTransmitter.send(val, 24);
    lastJam = millis();
    
    if(millis() % 5000 < 100) {
      Serial.printf("Jamming - Pattern %d: Proto %d, Val 0x%06lX\n", 
                   jamPattern, proto, val);
    }
  }
}

// ============================================
// OLED DISPLAY (SIMPLE STATUS ONLY)
// ============================================

void updateDisplay() {
  if(millis() - lastDisplayUpdate < 500) return;
  lastDisplayUpdate = millis();
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  
  // Header
  display.setCursor(15, 2);
  display.println("RF CLONER V2");
  display.drawLine(0, 11, SCREEN_WIDTH, 11, SH110X_WHITE);
  
  // WiFi Status
  display.setCursor(5, 15);
  display.print("WiFi: ");
  display.println(ssid);
  
  display.setCursor(5, 24);
  display.print("IP: 192.168.4.1");
  
  display.setCursor(5, 33);
  display.print("Clients: ");
  display.println(wifiClients);
  
  // Current Status
  display.drawLine(0, 42, SCREEN_WIDTH, 42, SH110X_WHITE);
  
  display.setCursor(5, 45);
  if(isScanning) {
    display.print("STATUS: SCANNING");
  } else if(isTransmitting) {
    display.print("TX: Slot ");
    display.print(currentSlot + 1);
  } else if(isJamming) {
    display.print("JAMMING ACTIVE");
  } else {
    display.print("READY");
  }
  
  // RF Module Status
  display.setCursor(5, 54);
  display.print("RF: ");
  display.print(rfModuleConnected ? "OK" : "ERR");
  
  display.display();
}

// ============================================
// WEB SERVER HANDLERS
// ============================================

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RF CLONER</title>
<style>
* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
  background: linear-gradient(135deg, #0f0c29 0%, #302b63 50%, #24243e 100%);
  min-height: 100vh;
  padding: 20px;
  color: #e0e0e0;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 25px;
  padding: 15px 25px;
  background: rgba(255, 255, 255, 0.05);
  border: 1px solid rgba(99, 102, 241, 0.3);
  border-radius: 12px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 15px;
}

.logo {
  width: 40px;
  height: 40px;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 20px;
}

.header-title h1 {
  font-size: 20px;
  font-weight: 600;
  letter-spacing: 2px;
  color: #fff;
}

.header-subtitle {
  font-size: 11px;
  color: #a0a0a0;
  margin-top: 2px;
}

.header-right {
  display: flex;
  gap: 10px;
}

.header-btn {
  padding: 8px 16px;
  border-radius: 8px;
  border: 1px solid rgba(99, 102, 241, 0.4);
  background: rgba(99, 102, 241, 0.1);
  color: #a78bfa;
  font-size: 12px;
  cursor: pointer;
  transition: all 0.3s;
}

.header-btn:hover {
  background: rgba(99, 102, 241, 0.2);
  border-color: rgba(99, 102, 241, 0.6);
}

.neon-badge {
  background: #10b981;
  color: #000;
  padding: 6px 12px;
  border-radius: 8px;
  font-size: 11px;
  font-weight: 600;
}

.container {
  max-width: 1400px;
  margin: 0 auto;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 20px;
}

.panel {
  background: rgba(255, 255, 255, 0.05);
  border: 1px solid rgba(99, 102, 241, 0.3);
  border-radius: 12px;
  padding: 20px;
  backdrop-filter: blur(10px);
}

.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 20px;
  padding-bottom: 15px;
  border-bottom: 1px solid rgba(99, 102, 241, 0.2);
}

.panel-title {
  font-size: 14px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 1px;
  color: #fff;
}

.panel-badge {
  font-size: 10px;
  padding: 4px 10px;
  background: rgba(16, 185, 129, 0.2);
  border: 1px solid rgba(16, 185, 129, 0.4);
  border-radius: 6px;
  color: #10b981;
}

.control-buttons {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-bottom: 20px;
}

.btn {
  padding: 12px 24px;
  border-radius: 10px;
  border: 1px solid;
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.3s;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.btn-primary {
  background: rgba(139, 92, 246, 0.2);
  border-color: #8b5cf6;
  color: #c4b5fd;
}

.btn-primary:hover {
  background: rgba(139, 92, 246, 0.3);
  box-shadow: 0 0 20px rgba(139, 92, 246, 0.5);
}

.btn-success {
  background: rgba(16, 185, 129, 0.2);
  border-color: #10b981;
  color: #6ee7b7;
}

.btn-success:hover {
  background: rgba(16, 185, 129, 0.3);
  box-shadow: 0 0 20px rgba(16, 185, 129, 0.5);
}

.btn-danger {
  background: rgba(239, 68, 68, 0.2);
  border-color: #ef4444;
  color: #fca5a5;
}

.btn-danger:hover {
  background: rgba(239, 68, 68, 0.3);
  box-shadow: 0 0 20px rgba(239, 68, 68, 0.5);
}

.btn-warning {
  background: rgba(251, 191, 36, 0.2);
  border-color: #fbbf24;
  color: #fde68a;
}

.btn-warning:hover {
  background: rgba(251, 191, 36, 0.3);
  box-shadow: 0 0 20px rgba(251, 191, 36, 0.5);
}

.btn-secondary {
  background: rgba(100, 116, 139, 0.2);
  border-color: #64748b;
  color: #cbd5e1;
}

.btn-secondary:hover {
  background: rgba(100, 116, 139, 0.3);
}

.btn-clear {
  background: rgba(245, 158, 11, 0.2);
  border-color: #f59e0b;
  color: #fde68a;
}

.btn-clear:hover {
  background: rgba(245, 158, 11, 0.3);
  box-shadow: 0 0 20px rgba(245, 158, 11, 0.5);
}

.info-text {
  font-size: 11px;
  color: #94a3b8;
  padding: 10px;
  background: rgba(100, 116, 139, 0.1);
  border-radius: 8px;
  margin-bottom: 15px;
}

.slot-nav {
  display: flex;
  align-items: center;
  gap: 15px;
  margin-bottom: 20px;
}

.slot-nav button {
  background: rgba(99, 102, 241, 0.2);
  border: 1px solid rgba(99, 102, 241, 0.4);
  color: #c4b5fd;
  padding: 10px 20px;
  border-radius: 8px;
  cursor: pointer;
  font-size: 16px;
  transition: all 0.3s;
}

.slot-nav button:hover {
  background: rgba(99, 102, 241, 0.3);
}

.slot-nav span {
  font-size: 14px;
  color: #94a3b8;
}

.status-row {
  display: flex;
  gap: 15px;
  margin-bottom: 15px;
}

.status-item {
  flex: 1;
  background: rgba(255, 255, 255, 0.03);
  padding: 12px;
  border-radius: 8px;
  border: 1px solid rgba(99, 102, 241, 0.2);
}

.status-label {
  font-size: 11px;
  color: #94a3b8;
  margin-bottom: 5px;
  text-transform: uppercase;
}

.status-value {
  font-size: 14px;
  font-weight: 600;
  color: #fff;
}

.signal-info {
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid rgba(99, 102, 241, 0.2);
  border-radius: 10px;
  padding: 15px;
  margin-bottom: 20px;
}

.signal-row {
  display: flex;
  justify-content: space-between;
  padding: 8px 0;
  border-bottom: 1px solid rgba(99, 102, 241, 0.1);
}

.signal-row:last-child {
  border-bottom: none;
}

.signal-label {
  font-size: 12px;
  color: #94a3b8;
}

.signal-value {
  font-size: 12px;
  font-weight: 600;
  color: #fff;
  font-family: 'Courier New', monospace;
}

.invalid-badge {
  background: rgba(239, 68, 68, 0.2);
  border: 1px solid rgba(239, 68, 68, 0.4);
  color: #ef4444;
  padding: 4px 12px;
  border-radius: 6px;
  font-size: 12px;
  display: inline-block;
}

.action-buttons {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
  margin-bottom: 15px;
}

.device-info {
  margin-top: 20px;
  padding: 15px;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid rgba(99, 102, 241, 0.2);
  border-radius: 10px;
}

.device-row {
  display: flex;
  gap: 20px;
  flex-wrap: wrap;
  margin-bottom: 10px;
}

.device-item {
  font-size: 11px;
  color: #94a3b8;
}

.device-item span {
  color: #fff;
  font-weight: 600;
}

.version-badge {
  float: right;
  font-size: 10px;
  padding: 4px 10px;
  background: rgba(99, 102, 241, 0.2);
  border: 1px solid rgba(99, 102, 241, 0.4);
  border-radius: 6px;
  color: #a78bfa;
}

.footer {
  text-align: center;
  margin-top: 30px;
  padding: 20px;
  font-size: 11px;
  color: #64748b;
  background: rgba(255, 255, 255, 0.03);
  border: 1px solid rgba(99, 102, 241, 0.2);
  border-radius: 10px;
}

@media (max-width: 968px) {
  .container {
    grid-template-columns: 1fr;
  }
  
  .header {
    flex-direction: column;
    gap: 15px;
    text-align: center;
  }
  
  .header-left {
    flex-direction: column;
  }
  
  .action-buttons {
    grid-template-columns: 1fr;
  }
}

.jam-warning {
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid rgba(239, 68, 68, 0.3);
  padding: 12px;
  border-radius: 8px;
  font-size: 11px;
  color: #fca5a5;
  margin-top: 10px;
}

.select-input {
  width: 100%;
  padding: 12px;
  background: rgba(255, 255, 255, 0.05);
  border: 1px solid rgba(99, 102, 241, 0.3);
  border-radius: 8px;
  color: #fff;
  font-size: 13px;
  margin-bottom: 15px;
  cursor: pointer;
}

.select-input:focus {
  outline: none;
  border-color: rgba(99, 102, 241, 0.6);
}

.clear-buttons {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
  margin-top: 10px;
}

.jam-buttons {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px;
  margin-top: 10px;
}
</style>
</head>
<body>

<div class="header">
  <div class="header-left">
    <div class="logo">📡</div>
    <div class="header-title">
      <h1>RF CLONER</h1>
      <div class="header-subtitle">ESP32 • HT-RF434 • 433.92 MHz</div>
    </div>
  </div>
  <div class="header-right">
    <button class="header-btn" onclick="showMenu()">⚙️ MODE: MENU</button>
    <div class="neon-badge">🟢 Neon</div>
  </div>
</div>

<div class="container">
  <!-- Left Panel: Control -->
  <div class="panel">
    <div class="panel-header">
      <div class="panel-title">CONTROL PANEL</div>
      <div class="panel-badge">LIVE • 500MS REFRESH</div>
    </div>
    
    <div class="control-buttons">
      <button class="btn btn-primary" onclick="showMenu()">Menu</button>
      <button class="btn btn-primary" onclick="startRead()">Read</button>
      <button class="btn btn-primary" onclick="emulateSignal()">Emulate</button>
      <button class="btn btn-warning" onclick="startJam()">Jam</button>
      <button class="btn btn-secondary" onclick="showInfo()">Info</button>
      <button class="btn btn-clear" onclick="confirmClearSlots()">Clear All</button>
    </div>
    
    <div class="info-text">
      Long press = hold remote near RX • Use slots to store multiple remotes
    </div>
    
    <div class="slot-nav">
      <button onclick="prevSlot()">◀ Slot</button>
      <span>Slot <span id="currentSlot">1</span> / 8 • Saved: <span id="savedCount">0</span>/8</span>
      <button onclick="nextSlot()">Slot ▶</button>
    </div>
    
    <div class="status-row">
      <div class="status-item">
        <div class="status-label">Last Capture</div>
        <div class="status-value" id="lastCapture">No signal yet</div>
      </div>
    </div>
    
    <div class="status-row">
      <div class="status-item">
        <div class="status-label">TX Status</div>
        <div class="status-value" id="txStatus">OFF</div>
      </div>
      <div class="status-item">
        <div class="status-label">Jam Mode</div>
        <div class="status-value" id="jamStatus">OFF</div>
      </div>
    </div>
    
    <div class="clear-buttons">
      <button class="btn btn-clear" onclick="clearCurrentSlot()">Clear Current Slot</button>
      <button class="btn btn-danger" onclick="confirmClearSlots()">Clear All Slots</button>
    </div>
  </div>
  
  <!-- Right Panel: Signal & RF -->
  <div class="panel">
    <div class="panel-header">
      <div class="panel-title">SIGNAL & RF</div>
      <div class="panel-badge">RX GPIO5 • TX GPIO4 • REPEAT:X5</div>
    </div>
    
    <div class="signal-info">
      <div class="signal-row">
        <span class="signal-label">Code:</span>
        <span class="signal-value" id="codeValue">-</span>
      </div>
      <div class="signal-row">
        <span class="signal-label">Bits:</span>
        <span class="signal-value"><span id="bitsValue">-</span> • Proto: <span id="protoValue">-</span></span>
      </div>
      <div class="signal-row">
        <span class="signal-label">Pulse:</span>
        <span class="signal-value" id="pulseValue">- µs</span>
      </div>
    </div>
    
    <div class="action-buttons">
      <button class="btn btn-success" onclick="startTX()">Start TX</button>
      <button class="btn btn-secondary" onclick="stopTX()">Stop TX</button>
    </div>
    
    <div class="jam-buttons">
      <button class="btn btn-danger" onclick="startJam()">Start Jam</button>
      <button class="btn btn-secondary" onclick="stopJam()">Stop Jam</button>
    </div>
    
    <div class="jam-warning">
      <strong>⚠️ JAM MODE:</strong> For legal testing only. Follow local RF laws and regulations.
    </div>
    
    <div class="device-info">
      <div class="panel-title">DEVICE INFO <span class="version-badge">RF CLONER V1.0</span></div>
      <div style="height: 15px"></div>
      <div class="device-row">
        <div class="device-item">MCU: <span>ESP32</span></div>
        <div class="device-item">Module: <span>RoboCraze RF434</span></div>
        <div class="device-item">Frequency: <span>433.92 MHz</span></div>
        <div class="device-item">Max Slots: <span>8</span></div>
      </div>
      <div class="device-row">
        <div class="device-item">RF Status: <span id="rfStatus">Receiver tolerance=60 • Signals saved: 0/8</span></div>
      </div>
    </div>
  </div>
</div>

<div class="footer">
  ESP32 RF Cloner Web UI • Auto refresh every 500ms
</div>

<script>
// State management
let currentSlotIndex = 0;
let slots = Array(8).fill(null).map(() => ({ valid: false, value: 0, bits: 0, protocol: 0, pulseLength: 0 }));
let isScanning = false;
let isTransmitting = false;
let isJamming = false;

// Load slots from server
function loadSlots() {
  fetch('/slots')
    .then(r => r.json())
    .then(data => {
      if (data && data.length === 8) {
        slots = data;
        updateSlotDisplay();
        updateSignalDisplay();
      }
    })
    .catch(err => console.log('Slots load failed:', err));
}

// Update current slot display
function updateSlotDisplay() {
  document.getElementById('currentSlot').textContent = currentSlotIndex + 1;
  const savedCount = slots.filter(s => s.valid).length;
  document.getElementById('savedCount').textContent = savedCount;
  document.getElementById('rfStatus').textContent = `Receiver tolerance=60 • Signals saved: ${savedCount}/8`;
}

// Update signal info display
function updateSignalDisplay() {
  const slot = slots[currentSlotIndex];
  if (slot.valid) {
    document.getElementById('codeValue').textContent = '0x' + slot.value.toString(16).toUpperCase();
    document.getElementById('bitsValue').textContent = slot.bits;
    document.getElementById('protoValue').textContent = slot.protocol;
    document.getElementById('pulseValue').textContent = slot.pulseLength + ' µs';
  } else {
    document.getElementById('codeValue').textContent = '-';
    document.getElementById('bitsValue').textContent = '-';
    document.getElementById('protoValue').textContent = '-';
    document.getElementById('pulseValue').textContent = '- µs';
  }
}

// Slot navigation
function prevSlot() {
  currentSlotIndex = (currentSlotIndex - 1 + 8) % 8;
  updateSlotDisplay();
  updateSignalDisplay();
}

function nextSlot() {
  currentSlotIndex = (currentSlotIndex + 1) % 8;
  updateSlotDisplay();
  updateSignalDisplay();
}

// Control functions
function startRead() {
  fetch('/scan?slot=' + currentSlotIndex)
    .then(r => r.text())
    .then(d => {
      isScanning = true;
      document.getElementById('lastCapture').textContent = 'Scanning...';
      updateStatus();
      setTimeout(loadSlots, 2000);
    });
}

function emulateSignal() {
  if (!slots[currentSlotIndex].valid) {
    alert('No signal in current slot!');
    return;
  }
  startTX();
}

function startTX() {
  if (!slots[currentSlotIndex].valid) {
    alert('No signal in current slot to transmit!');
    return;
  }
  
  fetch('/tx?slot=' + currentSlotIndex)
    .then(r => r.text())
    .then(d => {
      isTransmitting = true;
      document.getElementById('txStatus').textContent = 'TRANSMITTING';
      document.getElementById('txStatus').style.color = '#10b981';
    });
}

function stopTX() {
  if (!isTransmitting) {
    alert('TX is not active!');
    return;
  }
  
  fetch('/stoptx')
    .then(r => r.text())
    .then(d => {
      isTransmitting = false;
      document.getElementById('txStatus').textContent = 'OFF';
      document.getElementById('txStatus').style.color = '#fff';
    });
}

function startJam() {
  const confirmed = confirm('Start RF jamming? This should only be used for legal testing purposes.');
  if (!confirmed) return;
  
  fetch('/jam?pattern=0')
    .then(r => r.text())
    .then(d => {
      isJamming = true;
      document.getElementById('jamStatus').textContent = 'ACTIVE';
      document.getElementById('jamStatus').style.color = '#ef4444';
    });
}

function stopJam() {
  if (!isJamming) {
    alert('Jam mode is not active!');
    return;
  }
  
  fetch('/stopjam')
    .then(r => r.text())
    .then(d => {
      isJamming = false;
      document.getElementById('jamStatus').textContent = 'OFF';
      document.getElementById('jamStatus').style.color = '#fff';
    });
}

function clearCurrentSlot() {
  fetch('/clear?slot=' + currentSlotIndex)
    .then(r => r.text())
    .then(d => {
      loadSlots();
      document.getElementById('lastCapture').textContent = 'Slot cleared';
    });
}

function confirmClearSlots() {
  const savedCount = slots.filter(s => s.valid).length;
  if (savedCount === 0) {
    alert('All slots are already empty!');
    return;
  }
  
  const confirmed = confirm(`Clear ALL ${savedCount} saved signals? This cannot be undone!`);
  if (!confirmed) return;
  
  fetch('/clearall')
    .then(r => r.text())
    .then(d => {
      loadSlots();
      document.getElementById('lastCapture').textContent = 'All slots cleared';
      alert('All slots have been cleared!');
    });
}

function showMenu() {
  alert('Menu: Configure settings via serial console or Arduino IDE');
}

function showInfo() {
  alert('RF Cloner V2\n\nESP32-based 433MHz RF signal cloner\nSupports signal capture, replay, and analysis\n\nCreated by: Nihal MP');
}

function updateStatus() {
  if (isScanning) {
    document.getElementById('lastCapture').textContent = 'Scanning...';
  } else if (slots[currentSlotIndex].valid) {
    document.getElementById('lastCapture').textContent = 'Signal loaded';
  }
}

// Auto-refresh
setInterval(() => {
  loadSlots();
}, 500);

// Initial load
loadSlots();
</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSlots() {
  String json = "[";
  for(int i = 0; i < MAX_SLOTS; i++) {
    if(i > 0) json += ",";
    json += "{";
    json += "\"valid\":" + String(signalSlots[i].valid ? "true" : "false") + ",";
    json += "\"value\":" + String(signalSlots[i].value) + ",";
    json += "\"bits\":" + String(signalSlots[i].bitLength) + ",";
    json += "\"protocol\":" + String(signalSlots[i].protocol) + ",";
    json += "\"pulseLength\":" + String(signalSlots[i].pulseLength);
    json += "}";
  }
  json += "]";
  
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
  
  Serial.println("Sent slots data");
}

void handleScan() {
  if(server.hasArg("slot")) {
    currentSlot = server.arg("slot").toInt();
    if(currentSlot >= 0 && currentSlot < MAX_SLOTS) {
      isScanning = true;
      isTransmitting = false;
      isJamming = false;
      lastScanCheck = millis();
      Serial.printf("Started scanning slot %d\n", currentSlot + 1);
      server.send(200, "text/plain", "Scanning started");
    } else {
      server.send(400, "text/plain", "Invalid slot number");
    }
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleStopScan() {
  isScanning = false;
  server.send(200, "text/plain", "Scanning stopped");
  Serial.println("Scanning stopped");
}

void handleTX() {
  if(server.hasArg("slot")) {
    currentSlot = server.arg("slot").toInt();
    if(currentSlot >= 0 && currentSlot < MAX_SLOTS) {
      if(signalSlots[currentSlot].valid) {
        isTransmitting = true;
        isScanning = false;
        isJamming = false;
        transmitCount = 0;
        Serial.printf("Started transmitting slot %d\n", currentSlot + 1);
        server.send(200, "text/plain", "TX started");
      } else {
        server.send(400, "text/plain", "Slot is empty");
      }
    } else {
      server.send(400, "text/plain", "Invalid slot number");
    }
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleStopTX() {
  isTransmitting = false;
  server.send(200, "text/plain", "TX stopped");
  Serial.println("Transmission stopped");
}

void handleJam() {
  if(server.hasArg("pattern")) {
    jamPattern = server.arg("pattern").toInt();
    isJamming = true;
    isScanning = false;
    isTransmitting = false;
    Serial.printf("Started jamming with pattern %d\n", jamPattern);
    server.send(200, "text/plain", "Jamming started");
  } else {
    jamPattern = 0;
    isJamming = true;
    isScanning = false;
    isTransmitting = false;
    server.send(200, "text/plain", "Jamming started with default pattern");
  }
}

void handleStopJam() {
  isJamming = false;
  server.send(200, "text/plain", "Jamming stopped");
  Serial.println("Jamming stopped");
}

void handleClear() {
  if(server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    if(slot >= 0 && slot < MAX_SLOTS) {
      signalSlots[slot].valid = false;
      signalSlots[slot].value = 0;
      signalSlots[slot].bitLength = 0;
      signalSlots[slot].protocol = 0;
      signalSlots[slot].pulseLength = 0;
      snprintf(signalSlots[slot].label, 16, "EMPTY_%d", slot + 1);
      
      saveSlot(slot);
      Serial.printf("Cleared slot %d\n", slot + 1);
      server.send(200, "text/plain", "Slot cleared");
    } else {
      server.send(400, "text/plain", "Invalid slot number");
    }
  } else {
    server.send(400, "text/plain", "Missing slot parameter");
  }
}

void handleClearAll() {
  for(int i = 0; i < MAX_SLOTS; i++) {
    signalSlots[i].valid = false;
    signalSlots[i].value = 0;
    signalSlots[i].bitLength = 0;
    signalSlots[i].protocol = 0;
    signalSlots[i].pulseLength = 0;
    snprintf(signalSlots[i].label, 16, "EMPTY_%d", i + 1);
  }
  saveAllSlots();
  Serial.println("Cleared all slots");
  server.send(200, "text/plain", "All slots cleared");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ============================================
// SETUP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== RF CLONER V2 - WEB ===");
  
  // Init Display
  Wire.begin(21, 22); // SDA=21, SCL=22 for ESP32
  if(!display.begin(0x3C, true)) {
    Serial.println("Display initialization failed!");
    Serial.println("Check wiring: SDA=21, SCL=22");
    while(1) {
      delay(1000);
      Serial.print(".");
    }
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(10, 20);
  display.println("Starting...");
  display.display();
  
  // Init EEPROM
  initEEPROM();
  
  // Init RF Modules
  rfReceiver.enableReceive(digitalPinToInterrupt(RF_RX_PIN));
  rfTransmitter.enableTransmit(RF_TX_PIN);
  rfTransmitter.setRepeatTransmit(5); // Repeat transmission 5 times
  
  // Check RF Module
  rfModuleConnected = checkRFModule();
  if(!rfModuleConnected) {
    Serial.println("WARNING: RF module check failed!");
  }
  
  // Start WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  
  Serial.println("\n=== WiFi AP Started ===");
  Serial.print("SSID: "); Serial.println(ssid);
  Serial.print("Password: "); Serial.println(password);
  Serial.print("IP Address: "); Serial.println(WiFi.softAPIP());
  
  // Setup Web Server Routes
  server.on("/", handleRoot);
  server.on("/slots", handleSlots);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/stopscan", HTTP_GET, handleStopScan);
  server.on("/tx", HTTP_GET, handleTX);
  server.on("/stoptx", HTTP_GET, handleStopTX);
  server.on("/jam", HTTP_GET, handleJam);
  server.on("/stopjam", HTTP_GET, handleStopJam);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/clearall", HTTP_GET, handleClearAll);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Web server started on port 80");
  
  // Display ready message
  display.clearDisplay();
  display.setCursor(10, 15);
  display.println("WiFi Ready!");
  display.setCursor(5, 30);
  display.println("192.168.4.1");
  display.setCursor(15, 45);
  display.println("RF Cloner V2");
  display.display();
  delay(2000);
  
  Serial.println("\n=== SYSTEM READY ===");
  Serial.println("Connect to WiFi: RF_Cloner_V2");
  Serial.println("Password: rfcloner123");
  Serial.println("Open: http://192.168.4.1");
}

// ============================================
// MAIN LOOP
// ============================================

void loop() {
  server.handleClient();
  
  wifiClients = WiFi.softAPgetStationNum();
  
  // Handle RF reception
  if(isScanning) {
    handleRFReception();
    
    // Auto-stop scanning after 10 seconds if no signal
    if(millis() - lastScanCheck > 10000) {
      isScanning = false;
      Serial.println("Auto-stopped scanning (timeout)");
    }
  }
  
  // Handle transmission
  if(isTransmitting) {
    transmitSignal(currentSlot);
  }
  
  // Handle jamming
  if(isJamming) {
    jamSignals();
  }
  
  updateDisplay();
  
  delay(10);
}

// =================================
// END - Web Interface Edition
// =================================