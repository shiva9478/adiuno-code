
/*
 * ESP32 WiFi Repeater with BLE Control and Power Saving
 * This code configures an ESP32-WROOM to act as a WiFi repeater/range extender
 * with Bluetooth Low Energy interface for configuration and power-saving features
 */

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "esp_bt_main.h"
#include "esp_bt_device.h"

// WiFi configuration
String primarySSID = "Shivam5G";
String primaryPassword = "";
String apSSID = "Shivam5G_Repeater";
String apPassword = "";
int apChannel = 7;
int maxClients = 8;

// Power saving config
bool powerSavingEnabled = true;
uint8_t powerSaveMode = WIFI_PS_MIN_MODEM; // WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM
uint16_t listenInterval = 3;  // Default beacon listen interval (more = power saving but higher latency)

// IP configuration for the access point
IPAddress apIP(192, 168, 4, 1);
IPAddress apNetmask(255, 255, 255, 0);

// Status tracking
bool isPrimaryConnected = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 30000;  // Try to reconnect every 30 seconds

// Bluetooth related
BLEServer* pServer = NULL;
BLECharacteristic* pConfigCharacteristic = NULL;
BLECharacteristic* pStatusCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// UUID for BLE service and characteristics (using standard base UUIDs)
#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CONFIG_CHAR_UUID    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // Write characteristic for configuration
#define STATUS_CHAR_UUID    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // Read/notify characteristic for status

// BLE callback classes
class ServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE Client connected");
    updateBLEStatus(); // Send status update when device connects
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE Client disconnected");
    // Start advertising again when disconnected
    pServer->getAdvertising()->start();
  }
};

class ConfigCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0) {
      Serial.println("Received configuration update:");
      String jsonStr = String(value.c_str());
      parseConfig(jsonStr);
    }
  }

  void parseConfig(String jsonString) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, jsonString);
    
    if (error) {
      Serial.print("Failed to parse JSON: ");
      Serial.println(error.c_str());
      return;
    }
    
    bool configChanged = false;
    bool wifiChanged = false;
    
    // Parse primary WiFi settings
    if (doc.containsKey("primarySSID") && doc.containsKey("primaryPass")) {
      String newSSID = doc["primarySSID"].as<String>();
      String newPass = doc["primaryPass"].as<String>();
      
      if (newSSID != primarySSID || newPass != primaryPassword) {
        primarySSID = newSSID;
        primaryPassword = newPass;
        wifiChanged = true;
        configChanged = true;
        Serial.println("Primary WiFi settings updated");
      }
    }
    
    // Parse repeater settings
    if (doc.containsKey("apSSID") && doc.containsKey("apPass")) {
      String newAPSSID = doc["apSSID"].as<String>();
      String newAPPass = doc["apPass"].as<String>();
      
      if (newAPSSID != apSSID || newAPPass != apPassword) {
        apSSID = newAPSSID;
        apPassword = newAPPass;
        configChanged = true;
        Serial.println("AP settings updated");
      }
    }
    
    // Parse AP channel
    if (doc.containsKey("channel")) {
      int newChannel = doc["channel"].as<int>();
      if (newChannel != apChannel && newChannel >= 1 && newChannel <= 13) {
        apChannel = newChannel;
        configChanged = true;
        Serial.println("AP channel updated");
      }
    }
    
    // Parse max clients
    if (doc.containsKey("maxClients")) {
      int newMaxClients = doc["maxClients"].as<int>();
      if (newMaxClients != maxClients && newMaxClients > 0 && newMaxClients <= 10) {
        maxClients = newMaxClients;
        configChanged = true;
        Serial.println("Max clients updated");
      }
    }
    
    // Parse power saving settings
    if (doc.containsKey("powerSaving")) {
      bool newPowerSaving = doc["powerSaving"].as<bool>();
      if (newPowerSaving != powerSavingEnabled) {
        powerSavingEnabled = newPowerSaving;
        configChanged = true;
        Serial.print("Power saving mode ");
        Serial.println(powerSavingEnabled ? "enabled" : "disabled");
      }
    }
    
    if (doc.containsKey("powerMode")) {
      int newPowerMode = doc["powerMode"].as<int>();
      if (newPowerMode >= 0 && newPowerMode <= 2) {
        switch (newPowerMode) {
          case 0: powerSaveMode = WIFI_PS_NONE; break;
          case 1: powerSaveMode = WIFI_PS_MIN_MODEM; break;
          case 2: powerSaveMode = WIFI_PS_MAX_MODEM; break;
        }
        configChanged = true;
        Serial.print("Power save mode set to: ");
        Serial.println(newPowerMode);
      }
    }
    
    if (doc.containsKey("listenInterval")) {
      int newInterval = doc["listenInterval"].as<int>();
      if (newInterval != listenInterval && newInterval >= 1 && newInterval <= 10) {
        listenInterval = newInterval;
        configChanged = true;
        Serial.print("Listen interval set to: ");
        Serial.println(listenInterval);
      }
    }
    
    // Apply changes if needed
    if (configChanged) {
      applySettings(wifiChanged);
    }
  }
};

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n\nESP32 WiFi Repeater with BLE Control Starting...");
  
  // Setup BLE
  setupBLE();
  
  // Set up both WiFi modes - ESP32 can operate as both station and access point
  WiFi.mode(WIFI_AP_STA);
  
  // Configure the access point
  setupAccessPoint();
  
  // Connect to the primary WiFi
  connectToPrimaryWiFi();
  
  // Apply power saving settings
  applyPowerSavingSettings();
}

void loop() {
  // Check if we're connected to the primary WiFi
  if (WiFi.status() != WL_CONNECTED) {
    if (!isPrimaryConnected || (millis() - lastReconnectAttempt > reconnectInterval)) {
      isPrimaryConnected = false;
      Serial.println("Connection to primary WiFi lost. Attempting to reconnect...");
      connectToPrimaryWiFi();
    }
  } else if (!isPrimaryConnected) {
    // We just connected
    isPrimaryConnected = true;
    Serial.println("Connection to primary WiFi established!");
    printWiFiStatus();
    updateBLEStatus();
  }
  
  // Handle BLE connections
  if (deviceConnected) {
    // Update status every 5 seconds when BLE device is connected
    static unsigned long lastBLEStatusTime = 0;
    if (millis() - lastBLEStatusTime > 5000) {
      lastBLEStatusTime = millis();
      updateBLEStatus();
    }
  }
  
  // Handle connecting/disconnecting BLE devices
  if (deviceConnected != oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  // Print status every 60 seconds
  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > 60000) {
    lastStatusTime = millis();
    printStatus();
  }
  
  // Non-blocking delay
  delay(100);
}

void setupBLE() {
  // Create the BLE Device
  BLEDevice::init("ESP32_WiFi_Repeater");
  
  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Create BLE Characteristics
  pConfigCharacteristic = pService->createCharacteristic(
                            CONFIG_CHAR_UUID,
                            BLECharacteristic::PROPERTY_WRITE
                          );
  pConfigCharacteristic->setCallbacks(new ConfigCallbacks());
  
  pStatusCharacteristic = pService->createCharacteristic(
                            STATUS_CHAR_UUID,
                            BLECharacteristic::PROPERTY_READ |
                            BLECharacteristic::PROPERTY_NOTIFY
                          );
  pStatusCharacteristic->addDescriptor(new BLE2902());
  
  // Start the service
  pService->start();
  
  // Start advertising
  pServer->getAdvertising()->start();
  Serial.println("BLE server started, waiting for connections...");
  
  // Print the device address
  const uint8_t* point = esp_bt_dev_get_address();
  char bleAddress[18];
  sprintf(bleAddress, "%02X:%02X:%02X:%02X:%02X:%02X", point[0], point[1], point[2], point[3], point[4], point[5]);
  Serial.print("BLE MAC Address: ");
  Serial.println(bleAddress);
}

void setupAccessPoint() {
  Serial.println("Setting up Access Point...");
  
  // Configure the access point with a static IP
  WiFi.softAPConfig(apIP, apIP, apNetmask);
  
  // Start the access point
  if (WiFi.softAP(apSSID.c_str(), apPassword.c_str(), apChannel, 0, maxClients)) {
    Serial.print("Access Point established! SSID: ");
    Serial.println(apSSID);
    Serial.print("IP address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Failed to create Access Point!");
  }
}

void connectToPrimaryWiFi() {
  Serial.print("Connecting to primary WiFi network ");
  Serial.println(primarySSID);
  
  // Disconnect if previously connected
  WiFi.disconnect();
  delay(100);
  
  // Attempt to connect to the WiFi network
  WiFi.begin(primarySSID.c_str(), primaryPassword.c_str());
  
  // Wait up to 20 seconds for connection
  int connectionAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && connectionAttempts < 20) {
    delay(1000);
    Serial.print(".");
    connectionAttempts++;
  }
  
  // Check if connected
  if (WiFi.status() == WL_CONNECTED) {
    isPrimaryConnected = true;
    Serial.println("\nConnected to primary WiFi!");
    printWiFiStatus();
  } else {
    isPrimaryConnected = false;
    Serial.println("\nFailed to connect to primary WiFi. Will retry later.");
  }
  
  lastReconnectAttempt = millis();
}

void applySettings(bool reconnectWifi) {
  // Apply power saving settings
  applyPowerSavingSettings();
  
  // Reconfigure AP if needed
  WiFi.softAPConfig(apIP, apIP, apNetmask);
  WiFi.softAP(apSSID.c_str(), apPassword.c_str(), apChannel, 0, maxClients);
  
  // Reconnect to primary WiFi if credentials changed
  if (reconnectWifi) {
    connectToPrimaryWiFi();
  }
  
  // Update BLE status
  updateBLEStatus();
}

void applyPowerSavingSettings() {
  if (powerSavingEnabled) {
    // Set power save mode
    esp_wifi_set_ps(powerSaveMode);
    
    // Set listen interval (advanced)
    wifi_config_t conf;
    esp_wifi_get_config(WIFI_IF_STA, &conf);
    conf.sta.listen_interval = listenInterval;
    esp_wifi_set_config(WIFI_IF_STA, &conf);
    
    Serial.println("Power saving mode applied");
  } else {
    // Disable power saving
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.println("Power saving disabled");
  }
}

void updateBLEStatus() {
  if (!deviceConnected) return;
  
  DynamicJsonDocument statusDoc(1024);
  
  // WiFi Status
  statusDoc["primaryConnected"] = (WiFi.status() == WL_CONNECTED);
  statusDoc["primarySSID"] = primarySSID;
  statusDoc["primaryIP"] = WiFi.localIP().toString();
  statusDoc["primaryRSSI"] = WiFi.RSSI();
  statusDoc["apSSID"] = apSSID;
  statusDoc["apIP"] = WiFi.softAPIP().toString();
  statusDoc["connectedClients"] = WiFi.softAPgetStationNum();
  
  // Power saving status
  statusDoc["powerSaving"] = powerSavingEnabled;
  switch (powerSaveMode) {
    case WIFI_PS_NONE: statusDoc["powerMode"] = 0; break;
    case WIFI_PS_MIN_MODEM: statusDoc["powerMode"] = 1; break;
    case WIFI_PS_MAX_MODEM: statusDoc["powerMode"] = 2; break;
  }
  statusDoc["listenInterval"] = listenInterval;
  
  // System info
  statusDoc["freeHeap"] = ESP.getFreeHeap();
  statusDoc["uptime"] = millis() / 1000;
  
  // Convert to JSON string
  String statusJson;
  serializeJson(statusDoc, statusJson);
  
  // Update characteristic
  pStatusCharacteristic->setValue(statusJson.c_str());
  pStatusCharacteristic->notify();
}

void printWiFiStatus() {
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());
  
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  Serial.print("Signal Strength (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
}

void printStatus() {
  Serial.println("\n--- Status Update ---");
  
  // Primary network status
  Serial.print("Primary WiFi connection: ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected");
    Serial.print("IP: ");
    Serial.print(WiFi.localIP());
    Serial.print(", RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("Disconnected");
  }
  
  // Access point status
  Serial.print("Access Point: ");
  Serial.print(apSSID);
  Serial.print(", Connected clients: ");
  Serial.println(WiFi.softAPgetStationNum());
  
  // Power saving status
  Serial.print("Power saving: ");
  Serial.println(powerSavingEnabled ? "Enabled" : "Disabled");
  Serial.print("Power save mode: ");
  switch (powerSaveMode) {
    case WIFI_PS_NONE: Serial.println("None"); break;
    case WIFI_PS_MIN_MODEM: Serial.println("Minimum"); break;
    case WIFI_PS_MAX_MODEM: Serial.println("Maximum"); break;
  }
  
  // BLE status
  Serial.print("BLE connection: ");
  Serial.println(deviceConnected ? "Connected" : "Disconnected");
  
  // System stats
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  
  // System uptime
  Serial.print("Uptime: ");
  unsigned long uptime = millis() / 1000;
  Serial.print(uptime / 3600); // hours
  Serial.print("h ");
  Serial.print((uptime % 3600) / 60); // minutes
  Serial.print("m ");
  Serial.print(uptime % 60); // seconds
  Serial.println("s");
  
  Serial.println("--------------------");
}