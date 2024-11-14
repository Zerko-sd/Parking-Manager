#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>  // Using PCF8574 I2C LCD library instead
#include <TimeLib.h>
#include <EEPROM.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>

// WiFi Configuration
const char* ssid = "1st Floor 2G";
const char* password = "23781702";

// Web Server
ESP8266WebServer server(80);

// Pin Definitions
const int IR_SENSOR_PIN = D0;     // IR sensor
const int LED_PIN = D4;           // Status LED
const int SS_PIN = D8;            // RFID SDA/SS pin
const int RST_PIN = D3;           // RFID RST pin

// LCD Configuration
LiquidCrystal_PCF8574 lcd(0x27);  // Set LCD address to 0x27

// Initialize RFID
MFRC522 rfid(SS_PIN, RST_PIN);

// Parking Parameters
const int RATE_PER_HOUR = 50;     
const int MIN_FEE = 20;           
const unsigned long DEBOUNCE_DELAY = 1000;

// Authorized RFID Cards
byte authorizedCards[][4] = {
  { 0x13, 0xDB, 0x26, 0xC5 },  // Example card 1
  { 0x87, 0x65, 0x43, 0x21 }   // Example card 2
};

// System States
enum ParkingState {
  AVAILABLE,
  WAITING_FOR_CARD,
  OCCUPIED,
  WAITING_FOR_EXIT_TAP,
  PROCESSING
};

// Global Variables
struct ParkingData {
  ParkingState state;
  time_t entryTime;
  time_t exitTime;
  unsigned long lastStateChange;
  int totalEarnings;
  int vehiclesParked;
  int currentDuration;
  int currentFee;
  byte lastCardUID[4];
} parkingData = {
  AVAILABLE,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  {0, 0, 0, 0}
};

// Calculate parking fee
int calculateParkingFee(time_t entry, time_t exit) {
  int durationMinutes = (exit - entry) / 60;
  int hours = (durationMinutes + 59) / 60;  // Round up to nearest hour
  return max(MIN_FEE, hours * RATE_PER_HOUR);
}

// Save statistics to EEPROM
void saveStats() {
  EEPROM.put(0, parkingData.totalEarnings);
  EEPROM.put(sizeof(int), parkingData.vehiclesParked);
  EEPROM.commit();
}

// Load statistics from EEPROM
void loadStats() {
  EEPROM.get(0, parkingData.totalEarnings);
  EEPROM.get(sizeof(int), parkingData.vehiclesParked);
}

// Update LCD display
void updateDisplay() {
  lcd.clear();
  lcd.setCursor(0, 0);

  switch (parkingData.state) {
    case AVAILABLE:
      lcd.print("Space: AVAILABLE");
      lcd.setCursor(0, 1);
      lcd.print("Cars Today: ");
      lcd.print(parkingData.vehiclesParked);
      break;

    case WAITING_FOR_CARD:
      lcd.print("Scan RFID Card");
      lcd.setCursor(0, 1);
      lcd.print("to Enter");
      break;

    case OCCUPIED:
      lcd.print("Space: OCCUPIED");
      lcd.setCursor(0, 1);
      lcd.print("Time:");
      lcd.print(parkingData.currentDuration);
      lcd.print("m Fee:");
      lcd.print(parkingData.currentFee); // Show fee directly without extra spaces
      break;

    case WAITING_FOR_EXIT_TAP:
      lcd.print("Tap RFID Card");
      lcd.setCursor(0, 1);
      lcd.print("to Exit");
      break;

    case PROCESSING:
      lcd.print("Processing...");
      lcd.setCursor(0, 1);
      lcd.print("Fee: ");
      lcd.print(parkingData.currentFee);
      lcd.print("$");
      break;
  }
}

// Handle JSON API responses
void sendJsonResponse() {
  StaticJsonDocument<200> doc;
  
  doc["state"] = parkingData.state;
  doc["totalEarnings"] = parkingData.totalEarnings;
  doc["vehiclesParked"] = parkingData.vehiclesParked;
  doc["currentDuration"] = parkingData.currentDuration;
  doc["currentFee"] = parkingData.currentFee;
  doc["isOccupied"] = (parkingData.state == OCCUPIED);
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  server.send(200, "application/json", jsonString);
}

// Handle CORS
void handleCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// Setup web server routes
void setupServer() {
  server.on("/status", HTTP_GET, []() {
    handleCORS();
    sendJsonResponse();
  });
  
  server.on("/stats", HTTP_GET, []() {
    handleCORS();
    StaticJsonDocument<200> doc;
    doc["totalEarnings"] = parkingData.totalEarnings;
    doc["vehiclesParked"] = parkingData.vehiclesParked;
    
    String jsonString;
    serializeJson(doc, jsonString);
    server.send(200, "application/json", jsonString);
  });

  server.on("/status", HTTP_OPTIONS, []() {
    handleCORS();
    server.send(204);
  });

  server.on("/stats", HTTP_OPTIONS, []() {
    handleCORS();
    server.send(204);
  });

  server.begin();
}

bool isCardAuthorized(MFRC522::Uid uid) {
  for (int i = 0; i < sizeof(authorizedCards) / sizeof(authorizedCards[0]); i++) {
    if (memcmp(uid.uidByte, authorizedCards[i], 4) == 0) {
      return true;
    }
  }
  return false;
}

void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  memcpy(parkingData.lastCardUID, rfid.uid.uidByte, 4);
  
  if (isCardAuthorized(rfid.uid)) {
    if (parkingData.state == WAITING_FOR_CARD) {
      parkingData.state = OCCUPIED;
      parkingData.entryTime = now();
      digitalWrite(LED_PIN, HIGH);
      lcd.clear();
      lcd.print("Access Granted");
      delay(1000);
    } else if (parkingData.state == WAITING_FOR_EXIT_TAP) {
      if (memcmp(parkingData.lastCardUID, rfid.uid.uidByte, 4) == 0) {
        parkingData.state = PROCESSING;
        parkingData.exitTime = now();
        parkingData.currentFee = calculateParkingFee(parkingData.entryTime, parkingData.exitTime);
        parkingData.totalEarnings += parkingData.currentFee;
        parkingData.vehiclesParked++;
        digitalWrite(LED_PIN, LOW);
        saveStats();
        delay(3000);
      } else {
        lcd.clear();
        lcd.print("Wrong Card!");
        delay(1000);
      }
    }
  } else {
    lcd.clear();
    lcd.print("Access Denied!");
    delay(1000);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void updateParkingState() {
  int sensorState = digitalRead(IR_SENSOR_PIN);
  unsigned long currentTime = millis();
  
  if (currentTime - parkingData.lastStateChange < DEBOUNCE_DELAY) {
    return;
  }

  switch (parkingData.state) {
    case AVAILABLE:
      if (sensorState == LOW) {
        parkingData.state = WAITING_FOR_CARD;
        parkingData.lastStateChange = currentTime;
      }
      break;

    case WAITING_FOR_CARD:
      if (sensorState == HIGH) {
        parkingData.state = AVAILABLE;
        parkingData.lastStateChange = currentTime;
      }
      handleRFID();
      break;

    case OCCUPIED:
      if (sensorState == HIGH) {
        parkingData.state = WAITING_FOR_EXIT_TAP;
        parkingData.lastStateChange = currentTime;
      } else {
        parkingData.currentDuration = (now() - parkingData.entryTime) / 60;
        parkingData.currentFee = calculateParkingFee(parkingData.entryTime, now());
      }
      break;

    case WAITING_FOR_EXIT_TAP:
      handleRFID();
      if (sensorState == LOW) {
        parkingData.state = OCCUPIED;
        parkingData.lastStateChange = currentTime;
      }
      break;
      
    case PROCESSING:
      delay(3000);
      parkingData.state = AVAILABLE;
      parkingData.lastStateChange = currentTime;
      break;
  }
}

void initializeSystem() {
  Serial.begin(115200);
  Serial.println("\nInitializing Smart Parking System with RFID...");

  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize web server
  setupServer();

  // Initialize hardware
  pinMode(IR_SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  
  Wire.begin(D2, D1);  // SDA = D2, SCL = D1
  
  // Initialize LCD
  lcd.begin(16, 2);
  lcd.setBacklight(255);
  lcd.clear();
  lcd.print("Smart Parking");
  delay(2000);
  
  // Initialize RFID
  SPI.begin();
  rfid.PCD_Init();
  
  // Initialize EEPROM and load saved stats
  EEPROM.begin(512);
  loadStats();
  
  // Display IP address
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());
  delay(2000);
  lcd.clear();
}

void setup() {
  initializeSystem();
}

void loop() {
  server.handleClient();
  updateParkingState();
  updateDisplay();
  delay(100);
}