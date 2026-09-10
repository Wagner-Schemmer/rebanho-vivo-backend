// RebanhoVivo — firmware protótipo (Wemos D1 Mini + MPU6050 -> MQTT)
// Bibliotecas (Arduino IDE > Library Manager):
//   - PubSubClient (Nick O'Leary)
//   - ArduinoJson (Benoit Blanchon)
// Fiação MPU6050: VCC->3V3, GND->G, SDA->D2, SCL->D1, INT->D6 (opcional)

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>

// ---- Config (trocar) ----
const char* WIFI_SSID = "WIFI_DO_GALPAO";
const char* WIFI_PASS = "SENHA";
const char* MQTT_HOST = "IP_DO_GATEWAY";
const int   MQTT_PORT = 1883;
const char* DEVICE_ID = "COL-001";
const unsigned long SAMPLE_MS = 100;   // 10 Hz
const unsigned long SEND_MS   = 2000;  // pacote a cada 2 s (média da janela)

#define MPU_ADDR 0x68
#define PIN_INT 12 // D6

WiFiClient espClient;
PubSubClient mqtt(espClient);

int16_t AcX, AcY, AcZ, GyX, GyY, GyZ;
float sumMag = 0;
int nSamples = 0;
unsigned long lastSample = 0, lastSend = 0;

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission(true);
}

void mpuRead() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 14, true);
  AcX = Wire.read() << 8 | Wire.read();
  AcY = Wire.read() << 8 | Wire.read();
  AcZ = Wire.read() << 8 | Wire.read();
  Wire.read(); Wire.read(); // TEMP (ignorada: é do chip, não do animal)
  GyX = Wire.read() << 8 | Wire.read();
  GyY = Wire.read() << 8 | Wire.read();
  GyZ = Wire.read() << 8 | Wire.read();
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  mpuWrite(0x6B, 0x00); // acorda MPU6050
  mpuWrite(0x1C, 0x10); // +-8g
  mpuWrite(0x1B, 0x10); // +-1000 deg/s

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
}

void reconnect() {
  while (!mqtt.connected()) {
    if (mqtt.connect(DEVICE_ID)) break;
    delay(2000);
  }
}

void loop() {
  if (!mqtt.connected()) reconnect();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    mpuRead();
    // magnitude em "g" (16384 LSB/g em +-2g; aprox válida p/ 8g p/ protótipo)
    float ax = AcX / 16384.0, ay = AcY / 16384.0, az = AcZ / 16384.0;
    sumMag += sqrt(ax * ax + ay * ay + az * az);
    nSamples++;
  }

  if (now - lastSend >= SEND_MS && nSamples > 0) {
    lastSend = now;
    StaticJsonDocument<256> doc;
    doc["mag"] = sumMag / nSamples;
    doc["n"] = nSamples;
    doc["batt"] = analogRead(A0) / 1024.0 * 4.2; // aproximado, calibrar divisor
    char buf[256];
    serializeJson(doc, buf);
    char topic[64];
    snprintf(topic, sizeof(topic), "rebanho/%s/telemetry", DEVICE_ID);
    mqtt.publish(topic, buf);
    sumMag = 0;
    nSamples = 0;
  }
}
