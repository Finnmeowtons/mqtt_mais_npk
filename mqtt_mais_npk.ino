#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SoftwareSerial.h>

// === Device Setup ===
#define device_id 4  //TODO Change Device Number

// === WiFi & MQTT ===
const char* ssid = "tp-link";
const char* password = "09270734452";
const char* mqtt_server = "157.245.204.46";

WiFiClient espClient;
PubSubClient client(espClient);

// === Sensor Pins ===
#define DHTTYPE DHT11
#define DHTPin 0
#define SoilMoisturePin A0
#define DS18B20_PIN 4
#define BUZZER_PIN 15

// RS485 Modbus for NPK
#define DE 12
#define RE 14
SoftwareSerial mod(13, 5);  // RO=13 (D7), DI=5 (D1)

// Modbus RTU requests for reading NPK values
const byte nitro[] = {0x01,0x03, 0x00, 0x1e, 0x00, 0x01, 0xe4, 0x0c};
const byte phos[] = {0x01,0x03, 0x00, 0x1f, 0x00, 0x01, 0xb5, 0xcc};
const byte pota[] = {0x01,0x03, 0x00, 0x20, 0x00, 0x01, 0x85, 0xc0};

// A variable used to store NPK values
byte values[11];

DHT dht(DHTPin, DHTTYPE);
OneWire oneWire(DS18B20_PIN);
DallasTemperature DS18B20(&oneWire);

#define SENSOR_TOPIC "sensor/device4/data"   //TODO Change Device Number

unsigned long lastSensorSend = 0;
const unsigned long SENSOR_INTERVAL = 5000;

// === Setup ===
void setup() {
  Serial.begin(115200);
  dht.begin();
  DS18B20.begin();
  mod.begin(4800);

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  pinMode(DE, OUTPUT);
  pinMode(RE, OUTPUT);
  digitalWrite(DE, LOW);
  digitalWrite(RE, LOW);

  WiFiManager wifiManager;

  // Uncomment to reset saved credentials (for testing)
  wifiManager.resetSettings();

  if (!wifiManager.autoConnect("MAIS-DEVICE4")) { //TODO Change Device Number
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
    delay(1000);
  }

  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  reconnect();
}

// === Loop ===
void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long now = millis();
  if (now - lastSensorSend > SENSOR_INTERVAL) {
    lastSensorSend = now;
    sendSensorData();
  }
}

// === Soil Moisture ===
void soilMoistureData(float data[2]) {
  int rawValue = analogRead(SoilMoisturePin);
  float moisture_percentage = 100.00 - ((rawValue / 1023.00) * 100.00);
  data[0] = rawValue;
  data[1] = moisture_percentage;
}

// === Temperature & Humidity ===
bool temperatureData(float data[2]) {
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  if (isnan(humidity) || isnan(temperature)) return false;
  data[0] = temperature;
  data[1] = humidity;
  return true;
}

// === DS18B20 Soil Temp ===
float getSoilTemperature() {
  DS18B20.requestTemperatures();
  return DS18B20.getTempCByIndex(0);
}

byte nitrogen(){
  digitalWrite(DE, HIGH);
  digitalWrite(RE, HIGH);
  delay(10);
  if (mod.write(nitro, sizeof(nitro)) == 8) {
    digitalWrite(DE, LOW);
    digitalWrite(RE, LOW);
    delay(10); // give sensor a moment to respond

    byte i = 0;
    while (mod.available() && i < 7) {
      values[i++] = mod.read();
    }
  }
  return values[4];
}

byte phosphorous(){
  digitalWrite(DE, HIGH);
  digitalWrite(RE, HIGH);
  delay(10);
  if (mod.write(phos, sizeof(phos)) == 8) {
    digitalWrite(DE, LOW);
    digitalWrite(RE, LOW);
    delay(10);

    byte i = 0;
    while (mod.available() && i < 7) {
      values[i++] = mod.read();
    }
  }
  return values[4];
}

byte potassium(){
  digitalWrite(DE, HIGH);
  digitalWrite(RE, HIGH);
  delay(10);
  if (mod.write(pota, sizeof(pota)) == 8) {
    digitalWrite(DE, LOW);
    digitalWrite(RE, LOW);
    delay(10);

    byte i = 0;
    while (mod.available() && i < 7) {
      values[i++] = mod.read();
    }
  }
  return values[4];
}

// === Send Data to MQTT ===
void sendSensorData() {
  float soilData[2];
  float tempData[2];
  float soilTemperature = getSoilTemperature();
  soilMoistureData(soilData);
  bool validTemp = temperatureData(tempData);

  // Read NPK
  byte nitro, phos, pota;
  nitro = nitrogen();
  delay(100);
  phos = phosphorous();
  delay(100);
  pota = potassium();

  // JSON
  StaticJsonDocument<256> doc;
  doc["device_id"] = device_id;
  if (validTemp) {
    doc["temperature"] = tempData[0];
    doc["humidity"] = tempData[1];
  }
  doc["soil_moisture_raw"] = soilData[0];
  doc["soil_moisture_percentage"] = soilData[1];
  doc["soil_temperature"] = soilTemperature;
  doc["soil_ph"] = "N/A";
  doc["nitrogen"] = nitro;
  doc["phosphorus"] = phos;
  doc["potassium"] = pota;

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);
  Serial.print("📡 Sending JSON: ");
  Serial.println(jsonBuffer);
  client.publish(SENSOR_TOPIC, jsonBuffer);
}

// === MQTT Callback ===
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];
  Serial.print("MQTT Message Received: ");
  Serial.println(message);
  Serial.print("Topic: ");
  Serial.println(topic);

  if (strcmp(topic, "mais/animal") == 0) {
    StaticJsonDocument<64> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) return;

    bool hasAnimal = doc["has_animal"];
    if (hasAnimal) {
      tone(BUZZER_PIN, 1000);
      delay(5000);
      noTone(BUZZER_PIN);
    }
  }
}

// === MQTT Reconnect ===
void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    if (client.connect("Device4")) {  //TODO Change Device Number
      Serial.println("✅ Connected!");
      client.subscribe("mais/animal");
    } else {
      Serial.print("❌ Failed, rc=");
      Serial.print(client.state());
      delay(2000);
    }
  }
}
