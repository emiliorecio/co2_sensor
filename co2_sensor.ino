#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SensirionI2cScd4x.h>
#include <time.h>
#include "credentials.h"

// ================== CONFIGURACIÓN ==================
#define SDA_PIN 4   // D2
#define SCL_PIN 5   // D1
//SCD40 SDA → D2 (GPIO4)
//SCD40 SCL → D1 (GPIO5)

const char* SERVER_PATH = "/api/co2";
const char* DEVICE_ID = "esp8266-co2-salon";

// Cuarto por defecto (fijo)
String selectedRoom = "Oficina"; //Living / Dormitorio_Grande / Cocina / Oficina / Dormitorio_Ana

// ===================================================
SensirionI2cScd4x scd4x;

// Timing/polling
uint32_t lastPollMs = 0;
uint32_t lastReadyMs = 0;

// ---------------- WiFi + NTP -----------------
void connectWiFiAndTime() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  // NTP
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("Sincronizando hora");
  time_t now = 0;
  int tries = 0;
  while (now < 1700000000 && tries < 40) { // espera hasta tener epoch razonable
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    tries++;
  }
  Serial.println();
  Serial.print("Conectado a ");
  Serial.println(WiFi.SSID());
  Serial.print("IP asignada: ");
  Serial.println(WiFi.localIP());

}

// ---------------- HTTP POST -----------------
bool postJSON(uint16_t co2, float t, float rh) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFiAndTime();
  }

  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + SERVER_PATH;
  if (!http.begin(client, url)) {
    Serial.println("HTTP begin falló");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  time_t now = time(nullptr);
  String payload = "{";
  payload += "\"device\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"room\":\"" + selectedRoom + "\",";
  payload += "\"ts\":" + String((unsigned long)now) + ",";
  payload += "\"co2\":" + String(co2) + ",";
  payload += "\"t\":" + String(t, 2) + ",";
  payload += "\"rh\":" + String(rh, 1);
  payload += "}";

  int code = http.POST(payload);
  if (code > 0) {
    Serial.printf("POST %s -> %d\n", url.c_str(), code);
  } else {
    Serial.printf("HTTP POST error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code >= 200 && code < 300;
}

// ---------------- (Re)iniciar medición low-power -----------------
void startLowPowerMode() {
  // Secuencia robusta de inicio
  scd4x.stopPeriodicMeasurement();
  delay(500);
  scd4x.setSensorAltitude(650);              // ajustá según tu altitud si querés
  scd4x.setTemperatureOffset(0);             // calibración opcional
  scd4x.setAutomaticSelfCalibrationTarget(400);
  // Inicia modo de baja potencia -> 1 lectura cada ~30 s
  int16_t err = scd4x.startLowPowerPeriodicMeasurement();
  if (err != 0) {
    Serial.printf("**ERROR startLowPowerPeriodicMeasurement: %d\n", err);
  } else {
    Serial.println("SCD41 en modo Low Power (30 s).");
  }
  lastReadyMs = millis();
}

// ---------------- SETUP -----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("Iniciando...");
  Serial.println("Cuarto configurado: " + selectedRoom);

  connectWiFiAndTime();

  Wire.begin(SDA_PIN, SCL_PIN);
  scd4x.begin(Wire, 0x62);   // librería v1.1.0
  startLowPowerMode();

  Serial.println("Warm-up inicial...");
  // Sensirion recomienda descartar mediciones iniciales; con low power
  // esperamos al primer "ready" (≈30 s) antes de confiar plenamente.
}

// ---------------- LOOP -----------------
void loop() {
  // Poll del estado cada ~1 s
  if (millis() - lastPollMs >= 1000) {
    lastPollMs = millis();

    bool dataReady = false;
    int16_t err = scd4x.getDataReadyStatus(dataReady);
    if (err != 0) {
      Serial.printf("**ERROR getDataReadyStatus: %d\n", err);
    } else if (dataReady) {
      uint16_t co2 = 0;
      float t = 0, rh = 0;
      err = scd4x.readMeasurement(co2, t, rh);
      if (err != 0) {
        Serial.printf("**ERROR readMeasurement: %d\n", err);
      } else if (co2 != 0) {
        // Log local
        Serial.printf("[LP-30s] CO2=%u ppm  T=%.2f°C  RH=%.1f%%\n", co2, t, rh);
        // Enviar esta muestra directamente
        postJSON(co2, t, rh);
        lastReadyMs = millis();
      }
    }

    // Salvaguarda: si no vimos una muestra "ready" en >45 s, reiniciar medición
    if (millis() - lastReadyMs > 45000UL) {
      Serial.println("⟳ No hay datos en >45s, reiniciando medición low-power...");
      startLowPowerMode();
    }
  }

  delay(5); // siesta corta para no quemar CPU
}
