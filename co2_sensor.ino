#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SensirionI2cScd4x.h>
#include <time.h>
#include "credentials.h"

// ================== CONFIGURACIÓN ==================
#define SDA_PIN 4       // D2
#define SCL_PIN 5       // D1
#define SENSOR_PWR_PIN 12 // D6 - MOSFET que corta la alimentación del sensor
//SCD40 SDA → D2 (GPIO4)
//SCD40 SCL → D1 (GPIO5)
//SCD40 GND → Drain del MOSFET (Source del MOSFET a GND) → Gate en D6 (GPIO12)

const char* SERVER_PATH = "/api/co2";
const char* DEVICE_ID = "esp8266-co2-salon";

const uint32_t DAILY_REBOOT_MS = 24UL * 60UL * 60UL * 1000UL; // reinicio preventivo
const uint32_t STALL_REBOOT_MS = 5UL * 60UL * 1000UL;          // sin datos -> power-cycle del sensor

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
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > 30000UL) {
      Serial.println("\nWiFi no conectó en 30s, reiniciando...");
      ESP.restart();
    }
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
  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"device\":\"%s\",\"room\":\"%s\",\"ts\":%lu,\"co2\":%u,\"t\":%.2f,\"rh\":%.1f}",
           DEVICE_ID, selectedRoom.c_str(), (unsigned long)now, co2, t, rh);

  int code = http.POST((uint8_t*)payload, strlen(payload));
  if (code > 0) {
    Serial.printf("POST %s -> %d\n", url.c_str(), code);
  } else {
    Serial.printf("HTTP POST error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code >= 200 && code < 300;
}

// ---------------- Power-cycle físico del sensor -----------------
void powerCycleSensor() {
  digitalWrite(SENSOR_PWR_PIN, LOW);
  delay(200);   // asegura descarga de capacitores del sensor
  digitalWrite(SENSOR_PWR_PIN, HIGH);
  delay(1000);  // tiempo de arranque del SCD4x tras energizarlo
}

// ---------------- (Re)iniciar medición low-power -----------------
bool startLowPowerMode() {
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
    return false;
  }
  Serial.println("SCD41 en modo Low Power (30 s).");
  lastReadyMs = millis();
  return true;
}

// ---------------- Inicializar sensor con reintentos -----------------
bool initSensor() {
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("Iniciando sensor (intento %d/3)...\n", attempt);
    powerCycleSensor();
    Wire.begin(SDA_PIN, SCL_PIN);
    scd4x.begin(Wire, 0x62);   // librería v1.1.0
    if (startLowPowerMode()) return true;
  }
  return false;
}

// ---------------- SETUP -----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("Iniciando...");
  Serial.println("Cuarto configurado: " + selectedRoom);

  pinMode(SENSOR_PWR_PIN, OUTPUT);
  digitalWrite(SENSOR_PWR_PIN, LOW); // sensor apagado hasta initSensor()

  connectWiFiAndTime();

  if (!initSensor()) {
    Serial.println("No se pudo iniciar el sensor tras varios intentos, reiniciando ESP...");
    ESP.restart();
  }

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

    // Salvaguarda: si hace mucho que no hay datos, el sensor probablemente quedó
    // trabado en el bus I2C -> power-cycle físico (un reinicio del ESP no alcanza)
    if (millis() - lastReadyMs > STALL_REBOOT_MS) {
      Serial.println("⟳ Sin datos por >5 min, power-cycling sensor...");
      if (!initSensor()) {
        Serial.println("Sensor sigue sin responder, reinicio completo del dispositivo...");
        ESP.restart();
      }
    } else if (millis() - lastReadyMs > 45000UL) {
      Serial.println("⟳ No hay datos en >45s, reiniciando medición low-power...");
      startLowPowerMode();
    }

    // Reinicio preventivo diario
    if (millis() > DAILY_REBOOT_MS) {
      Serial.println("⟳ Reinicio diario programado...");
      ESP.restart();
    }
  }

  delay(5); // siesta corta para no quemar CPU
}
