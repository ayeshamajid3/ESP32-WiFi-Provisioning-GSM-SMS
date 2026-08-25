#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <WiFi.h>
#include <Preferences.h> 

// GSM Serial Pins
#define RXD2 4
#define TXD2 5

// Physical Push Button Pin to switch modes
#define BUTTON_PIN 12

// 1 Unique Service UUID
#define SERVICE_UUID      "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"

// 4 Distinct Characteristic UUIDs (All 4 Tabs are back!)
#define SSID_CHAR_UUID    "11111111-1FB5-459E-8FCC-C5C9C331914B"
#define PASS_CHAR_UUID    "22222222-1FB5-459E-8FCC-C5C9C331914B"
#define PHONE_CHAR_UUID   "33333333-1FB5-459E-8FCC-C5C9C331914B"
#define AP_NAME_CHAR_UUID "44444444-1FB5-459E-8FCC-C5C9C331914B"

Preferences preferences;

// System States
enum SystemState { STATE_SERIAL_SMS_MODE, STATE_BLE_CONFIG };
SystemState currentState = STATE_SERIAL_SMS_MODE;

// Global Application Variables
String typed_SSID = "";
String typed_PASS = "";
String target_Phone = "";   
String custom_AP_Name = "";   

// BLE Pointers and Global Flag
BLEServer *pServer = NULL;
BLEService *pService = NULL;
bool bleInitialized = false;
volatile bool dataReceivedAutomatically = false; // Triggers if ANY tab receives data!

void sendSMS(String phoneNumber, String message);
void loadSavedCredentials();
void startBLEConfig();
void stopBLEConfig();

// BLE Callback Class: Handles all 4 tabs and auto-switches on ANY write
class CharactericDataHandler: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string val = pCharacteristic->getValue();
      String rxValue = "";
      for (int i = 0; i < val.length(); i++) rxValue += val[i];
      rxValue.trim();

      if (rxValue.length() == 0) return;

      BLEUUID currentUUID = pCharacteristic->getUUID();

      // TAB 1: SSID
      if (currentUUID.equals(BLEUUID(SSID_CHAR_UUID))) {
        typed_SSID = rxValue;
        preferences.begin("wifi-store", false);
        preferences.putString("ssid", typed_SSID); 
        preferences.end();
        Serial.print("\n[BLE AUTOMATIC DETECTION] Saved SSID to Flash: "); Serial.println(typed_SSID);
        dataReceivedAutomatically = true; 
      } 
      // TAB 2: Password
      else if (currentUUID.equals(BLEUUID(PASS_CHAR_UUID))) {
        typed_PASS = rxValue;
        preferences.begin("wifi-store", false);
        preferences.putString("pass", typed_PASS); 
        preferences.end();
        Serial.print("\n[BLE AUTOMATIC DETECTION] Saved Password to Flash: "); Serial.println(typed_PASS);
        dataReceivedAutomatically = true; 
      } 
      // TAB 3: Phone Number
      else if (currentUUID.equals(BLEUUID(PHONE_CHAR_UUID))) {
        target_Phone = rxValue;
        target_Phone.replace(" ", "");
        preferences.begin("wifi-store", false);
        preferences.putString("phone", target_Phone); 
        preferences.end();
        Serial.print("\n[BLE AUTOMATIC DETECTION] Saved Target Phone to Flash: "); Serial.println(target_Phone);
        dataReceivedAutomatically = true; 
      } 
      // TAB 4: Custom Access Point Name
      else if (currentUUID.equals(BLEUUID(AP_NAME_CHAR_UUID))) {
        custom_AP_Name = rxValue;
        preferences.begin("wifi-store", false);
        preferences.putString("apname", custom_AP_Name); 
        preferences.end();
        Serial.print("\n[BLE AUTOMATIC DETECTION] Saved Custom AP Name to Flash: "); Serial.println(custom_AP_Name);
        dataReceivedAutomatically = true; 
      }
    }
};

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.println("\n=== STARTUP: READING NON-VOLATILE FLASH MEMORY ===");
  loadSavedCredentials();
  
  Serial.println("===== FLASH CONTENTS =====");
  Serial.print("SAVED SSID: '"); Serial.print(typed_SSID); Serial.println("'");
  Serial.print("SAVED PASS: '"); Serial.print(typed_PASS); Serial.println("'");
  Serial.print("TARGET PHONE: '"); Serial.print(target_Phone); Serial.println("'");
  Serial.print("CUSTOM AP NAME: '"); Serial.print(custom_AP_Name); Serial.println("'");
  Serial.println("=========================");

  // --- FIXED DISPLAY MESSAGES ON POWER-ON ---
  if (typed_SSID.length() > 0) {
    Serial.print("[FLASH AUTO-BOOT] Attempting network connection to: "); Serial.println(typed_SSID);
    WiFi.begin(typed_SSID.c_str(), typed_PASS.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500); Serial.print("."); attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[FLASH AUTO-BOOT SUCCESS] Connected!"); // Directly says Connected! now
      Serial.print("[IP ADDRESS] Connected Local IP: "); Serial.println(WiFi.localIP());
    } else {
      Serial.println("\n[FLASH AUTO-BOOT TIMEOUT] Saved credentials failed to connect. Open BLE to update.");
    }
  } else {
    Serial.println("[FLASH EMPTY] No router credentials found in memory. Awaiting BLE entry.");
  }

  // Ensure Access Point starts up cleanly
  if (custom_AP_Name.length() == 0) {
    custom_AP_Name = "ESP32_SMS_AP"; 
  }
  WiFi.softAP(custom_AP_Name.c_str(), "12345678");
  Serial.print("[AP STATUS] Soft-Access Point Active! Broadcast Name: "); Serial.println(custom_AP_Name);

  Serial.println("\n=== SYSTEM RUNNING: DEFAULT SERIAL SMS MODE ENGAGED ===");
  Serial.println("-> Type messages directly into the Serial Monitor box to send.");
  Serial.println("-> PRESS the physical button once to open BLE and update credentials.");
}

void loop() {
  // Always proxy incoming GSM module messages to terminal
  if (Serial2.available()) {
    Serial.write(Serial2.read());
  }

  // --- 1. PHYSICAL BUTTON MANUALLY OPENS BLE CONFIG MODE ---
  if (currentState == STATE_SERIAL_SMS_MODE && digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // Debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("\n=== BUTTON PRESSED: OPENING BLE PROVISION CHANNEL ===");
      startBLEConfig();
      currentState = STATE_BLE_CONFIG;
      while(digitalRead(BUTTON_PIN) == LOW); // Wait until button is released
    }
  }

  // --- 2. THE AUTOMATIC SMART-SWITCH LOGIC ENGINE (ANY TAB TRIGGER) ---
  if (currentState == STATE_BLE_CONFIG && dataReceivedAutomatically == true) {
    Serial.println("\n=== AUTO-SWITCH TRIGGERED: CLOSING BLE & RE-ENGAGING ACCESS POINT ===");
    
    dataReceivedAutomatically = false; // Reset the automatic flag
    stopBLEConfig();                   // Shut down Bluetooth completely
    
    // Reload whatever was just saved to keep RAM synchronous
    loadSavedCredentials();
    
    // Attempt router auto-reconnect instantly if Wi-Fi tabs were what updated
    if (typed_SSID.length() > 0) {
      Serial.print("[AUTO-RECONNECT] Trying to connect to network: "); Serial.println(typed_SSID);
      WiFi.begin(typed_SSID.c_str(), typed_PASS.c_str());
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 6) { delay(500); Serial.print("."); attempts++; }
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[AUTO-RECONNECT RES] Connected!"); 
      } else {
        Serial.println("\n[AUTO-RECONNECT RES] Wi-Fi connection timed out. Moving forward.");
      }
    }

    // Refresh and bring up the Access Point immediately using the configuration name
    WiFi.softAP(custom_AP_Name.c_str(), "12345678");
    Serial.print("[AP ACTIVE] Soft-Access Point running as: "); Serial.println(custom_AP_Name);
    
    Serial.println("=== SERIAL SMS MODE RE-ENGAGED AUTOMATICALLY. READY FOR INPUT ===");
    currentState = STATE_SERIAL_SMS_MODE;
  }

  // --- 3. SERIAL SMS PROCESSING ENGINE ---
  if (currentState == STATE_SERIAL_SMS_MODE) {
    if (Serial.available() > 0) {
      String serialInput = Serial.readString();
      serialInput.trim();

      if (serialInput.length() > 0) {
        Serial.println("\n-----------------------------------------");
        Serial.print("[SERIAL INPUT] Message text payload: \""); Serial.print(serialInput); Serial.println("\"");
        
        if (target_Phone.length() == 0) {
          Serial.println("[ERROR] No target phone number on file. Press button to configure via BLE.");
        } else {
          Serial.print("[GSM ROUTER] Dispatched to target cell line: "); Serial.println(target_Phone);
          sendSMS(target_Phone, serialInput);
        }
        Serial.println("-----------------------------------------");
      }
    }
  }
}

void startBLEConfig() {
  if (!bleInitialized) {
    BLEDevice::init("ESP32_SMS_Controller");
    pServer = BLEDevice::createServer();
    pService = pServer->createService(SERVICE_UUID);
    CharactericDataHandler *dataHandler = new CharactericDataHandler();

    // Re-registering all 4 Tabs to allow flexible updates
    BLECharacteristic *pSSIDChar = pService->createCharacteristic(SSID_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pSSIDChar->setCallbacks(dataHandler);

    BLECharacteristic *pPassChar = pService->createCharacteristic(PASS_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pPassChar->setCallbacks(dataHandler);

    BLECharacteristic *pPhoneChar = pService->createCharacteristic(PHONE_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pPhoneChar->setCallbacks(dataHandler);

    BLECharacteristic *pAPNameChar = pService->createCharacteristic(AP_NAME_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pAPNameChar->setCallbacks(dataHandler);

    pService->start();
    bleInitialized = true;
  }
  
  pServer->getAdvertising()->start();
  Serial.println("[BLE CONFIG ACTIVE] All 4 Provision channels open. Update any tab inside nRF Connect now.");
}

void stopBLEConfig() {
  if (bleInitialized) {
    pServer->getAdvertising()->stop();
    Serial.println("[BLE CONFIG CLOSED] Bluetooth radio channel safely closed.");
  }
}

void loadSavedCredentials() {
  preferences.begin("wifi-store", true);
  typed_SSID     = preferences.getString("ssid", "");
  typed_PASS     = preferences.getString("pass", "");
  target_Phone   = preferences.getString("phone", "");
  custom_AP_Name = preferences.getString("apname", "ESP32_SMS_AP");
  preferences.end();
}

void sendSMS(String phoneNumber, String message) {
  Serial2.println("AT+CMGF=1");
  delay(200);
  Serial2.print("AT+CMGS=\"");
  Serial2.print(phoneNumber);
  Serial2.println("\"");
  delay(200);
  Serial2.print(message);
  delay(200);
  Serial2.write(26); 
  Serial.println("[GSM STATUS] Payload successfully dispatched to lines.");
}
