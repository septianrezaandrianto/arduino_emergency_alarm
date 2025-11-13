#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

//==================== KONFIG WIFI ====================
const char* WIFI_SSID = "wifi_name";
const char* WIFI_PASS = "12345678";
const uint16_t HTTP_PORT = 80;

const char* AP_SSID = "ALARM-SETUP";
const char* AP_PASS = "12345678";

//==================== BACKEND EXPRESS ====================
const char* BACKEND_BASE = "https://tes.com";
const char* DEVICE_ID    = "elora-emergency-alarm";

//==================== PIN & LCD ======================
#define BUTTON_PIN  D7
#define BUZZER_PIN  D5
// I2C LCD: SDA=D2, SCL=D1
LiquidCrystal_I2C lcd(0x27, 16, 2);

//==================== STATE ==========================

bool alarmActive = false;

// Blink LCD
bool backlightOn = false;
unsigned long lastBlinkMs = 0;
const unsigned long BLINK_INTERVAL_MS_ALARM = 300;
const unsigned long BLINK_INTERVAL_MS_OFF   = 250;
int offBlinkPairsRemaining = 0;

//==================== SIRENE BARU (WAILING EMERGENCY) ====================
// Pola: frekuensi naik dari TONE_MIN ke TONE_MAX, turun lagi,
// beberapa kali, lalu jeda singkat (diam), diulang.
unsigned long lastToneStepMs = 0;
const unsigned long TONE_STEP_MS = 10;     // langkah lebih cepat untuk rasa "urgent"
int toneFreq  = 900;
int toneStep  = 40;
const int TONE_MIN = 800;
const int TONE_MAX = 2300;

// jeda singkat antar "wail"
bool sirenSilent = false;
unsigned long sirenSilenceStartMs = 0;
const unsigned long SIREN_SILENCE_MS = 150;

// berapa kali menyentuh batas MIN/MAX sebelum jeda
int sirenCycles = 0;
const int SIREN_CYCLES_BEFORE_SILENCE = 4;

// Address terakhir untuk ditampilkan saat ON
String lastAddress = "";

// Polling command dari backend
unsigned long lastCmdCheckMs = 0;
const unsigned long CMD_POLL_INTERVAL_MS = 1000;  // 1 detik

//==================== HTTP SERVER LOKAL ====================
ESP8266WebServer server(HTTP_PORT);

//==================== UTIL ===========================
void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCORSHeaders();
  server.send(204);
}

void sendJSON(int code, const String& body) {
  sendCORSHeaders();
  server.send(code, "application/json", body);
}

String currentIP() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();
}

//==================== LCD HELPERS ====================
void displayBlank() {
  lcd.clear();
  lcd.noBacklight();
  backlightOn = false;
}

void displayHelpAddress(const String& addr) {
  String a = addr;
  a.trim();
  if (a.length() > 16) a = a.substring(0, 16); // muat 16 kolom

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Butuh Bantuan");
  lcd.setCursor(0, 1);
  if (a.length() == 0) lcd.print("-");
  else                 lcd.print(a);
  lcd.backlight();
  backlightOn = true;
}

// Idle / OFF → LCD blank
void displaySafeIdle() {
  displayBlank();
}

void displaySafeDone() {
  displayBlank();
}

//==================== PARSE ADDRESS DARI JSON ====================
void updateAddressFromPayload(const String& payload) {
  int idx = payload.indexOf("\"address\":\"");
  if (idx < 0) {
    Serial.println("[BACKEND] address not found in payload");
    return;
  }
  idx += 11; // panjang string "\"address\":\""

  int end = payload.indexOf("\"", idx);
  if (end < 0) {
    Serial.println("[BACKEND] address end quote not found");
    return;
  }

  String addr = payload.substring(idx, end);
  addr.trim();
  lastAddress = addr;

  Serial.print("[BACKEND] Parsed address: ");
  Serial.println(lastAddress);
}

//==================== BACKEND CLIENT (HTTPS) ====================
void postAlarmEventToBackend(bool isActive, const char* eventName) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BACKEND] WiFi not connected, skip POST");
    return;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // demo: abaikan verifikasi sertifikat

  HTTPClient https;

  String url = String(BACKEND_BASE) + "/iot/event";
  Serial.print("[BACKEND] POST "); Serial.println(url);

  if (!https.begin(*client, url)) {
    Serial.println("[BACKEND] https.begin failed");
    return;
  }

  https.addHeader("Content-Type", "application/json");

  String body = String("{\"deviceId\":\"") + DEVICE_ID + "\"," +
                "\"alarmActive\":" + (isActive ? "true" : "false") + "," +
                "\"event\":\"" + eventName + "\"," +
                "\"ip\":\"" + currentIP() + "\"," +
                "\"address\":\"" + lastAddress + "\"}";

  Serial.print("[BACKEND] body: "); Serial.println(body);

  int httpCode = https.POST(body);
  Serial.printf("[BACKEND] Response code: %d\n", httpCode);

  if (httpCode > 0) {
    String payload = https.getString();
    Serial.print("[BACKEND] Response: "); Serial.println(payload);
  }

  https.end();
}

void startAlarm();              // forward declare
void stopAlarmStart3Blinks();   // forward declare

void checkCommandFromBackend() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastCmdCheckMs < CMD_POLL_INTERVAL_MS) return;
  lastCmdCheckMs = now;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;

  String url = String(BACKEND_BASE) + "/iot/command?deviceId=" + DEVICE_ID;
  Serial.print("[BACKEND] GET "); Serial.println(url);

  if (!https.begin(*client, url)) {
    Serial.println("[BACKEND] https.begin failed (command)");
    return;
  }

  int httpCode = https.GET();
  Serial.printf("[BACKEND] command httpCode: %d\n", httpCode);

  if (httpCode == 200) {
    String payload = https.getString();
    Serial.print("[BACKEND] command payload: "); Serial.println(payload);

    if (payload.indexOf("ALARM_ON") >= 0) {
      Serial.println("[BACKEND] Command: ALARM_ON");
      updateAddressFromPayload(payload);
      startAlarm();
      postAlarmEventToBackend(true, "command_alarm_on");
    } else if (payload.indexOf("ALARM_OFF") >= 0) {
      Serial.println("[BACKEND] Command: ALARM_OFF");
      stopAlarmStart3Blinks();
      postAlarmEventToBackend(false, "command_alarm_off");
    } else {
      // command: "NONE"
    }
  }

  https.end();
}

//==================== ALARM ==========================
void startAlarm() {
  if (alarmActive) return;   // sudah ON, jangan diulang

  alarmActive = true;

  // Tampilkan "Butuh Bantuan" + address
  displayHelpAddress(lastAddress);

  // Reset sirene baru
  toneFreq = TONE_MIN;
  toneStep = 40;
  sirenCycles = 0;
  sirenSilent = false;
  lastToneStepMs = millis();
  tone(BUZZER_PIN, toneFreq);

  // Blink backlight (terus selama ON)
  backlightOn = true;
  offBlinkPairsRemaining = 0;
  lastBlinkMs = millis();

  Serial.println("[ALARM] ON");

  postAlarmEventToBackend(true, "alarm_on");
}

void stopAlarmStart3Blinks() {
  if (!alarmActive) return;  // kalau sudah OFF, jangan ngapa-ngapain

  alarmActive = false;
  noTone(BUZZER_PIN);

  // reset state sirene
  sirenSilent = false;
  sirenCycles = 0;

  // kosongkan LCD (tanpa tulisan)
  displaySafeDone();

  // Blink 3x sebagai feedback OFF (hanya backlight, teks kosong)
  offBlinkPairsRemaining = 3;
  backlightOn = true;
  lcd.backlight();
  lastBlinkMs = millis();

  Serial.println("[ALARM] OFF");

  postAlarmEventToBackend(false, "alarm_off");
}

void handleBlink() {
  unsigned long now = millis();

  if (alarmActive) {
    // blink backlight saat alarm ON
    if (now - lastBlinkMs >= BLINK_INTERVAL_MS_ALARM) {
      lastBlinkMs = now;
      backlightOn = !backlightOn;
      if (backlightOn) lcd.backlight();
      else             lcd.noBacklight();
    }
  } else if (offBlinkPairsRemaining > 0) {
    // blink 3x setelah dimatikan
    if (now - lastBlinkMs >= BLINK_INTERVAL_MS_OFF) {
      lastBlinkMs = now;
      backlightOn = !backlightOn;
      if (backlightOn) {
        lcd.backlight();
        offBlinkPairsRemaining--;
      } else {
        lcd.noBacklight();
      }
    }
  } else {
    // idle: LCD blank dan backlight OFF
    if (backlightOn) {
      lcd.noBacklight();
      backlightOn = false;
    }
  }
}

//==================== SIREN HANDLER BARU ====================
void handleSiren() {
  // jika alarm mati, pastikan buzzer off dan state sirene direset
  if (!alarmActive) {
    noTone(BUZZER_PIN);
    sirenSilent = false;
    sirenCycles = 0;
    return;
  }

  unsigned long now = millis();

  // fase jeda (diam sebentar antara wail)
  if (sirenSilent) {
    if (now - sirenSilenceStartMs >= SIREN_SILENCE_MS) {
      sirenSilent = false;
      toneFreq = TONE_MIN;
      toneStep = abs(toneStep);   // mulai naik lagi dari bawah
      lastToneStepMs = now;
      tone(BUZZER_PIN, toneFreq);
    }
    return;
  }

  // update frekuensi wail
  if (now - lastToneStepMs < TONE_STEP_MS) return;
  lastToneStepMs = now;

  toneFreq += toneStep;

  // kalau menyentuh batas atas/bawah, balik arah dan hitung siklus
  if (toneFreq >= TONE_MAX) {
    toneFreq = TONE_MAX;
    toneStep = -toneStep;
    sirenCycles++;
  } else if (toneFreq <= TONE_MIN) {
    toneFreq = TONE_MIN;
    toneStep = -toneStep;
    sirenCycles++;
  }

  // setelah beberapa kali naik–turun, jeda dulu sebentar
  if (sirenCycles >= SIREN_CYCLES_BEFORE_SILENCE) {
    noTone(BUZZER_PIN);
    sirenSilent = true;
    sirenSilenceStartMs = now;
    sirenCycles = 0;
  } else {
    tone(BUZZER_PIN, toneFreq);
  }
}

//==================== HTTP HANDLERS LOKAL ==================
void handleAlarmOn() {
  if (server.method() == HTTP_OPTIONS) return handleOptions();

  String addr = server.hasArg("address") ? server.arg("address") : "";
  if (addr == "" && server.hasArg("plain")) {
    String body = server.arg("plain"); body.trim();
    int p = body.indexOf("address=");
    if (p >= 0) addr = body.substring(p + 8);
  }
  addr.trim();
  if (addr.length() > 0) {
    lastAddress = addr;
  }

  startAlarm();
  sendJSON(200, "{\"ok\":true,\"alarmActive\":true}");
}

void handleAlarmOff() {
  if (server.method() == HTTP_OPTIONS) return handleOptions();
  stopAlarmStart3Blinks();
  sendJSON(200, "{\"ok\":true,\"alarmActive\":false}");
}

void handleStatus() {
  if (server.method() == HTTP_OPTIONS) return handleOptions();
  String ip   = currentIP();
  String ssid = (WiFi.status()==WL_CONNECTED) ? WiFi.SSID() : String(AP_SSID);
  String json = String("{\"alarmActive\":") + (alarmActive ? "true" : "false") +
                ",\"ip\":\"" + ip + "\",\"ssid\":\"" + ssid + "\"}";
  sendJSON(200, json);
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) return handleOptions();
  sendJSON(404, "{\"ok\":false,\"error\":\"Not found\"}");
}

//==================== WIFI & MDNS ====================
void startHTTPRoutes() {
  server.on("/alarm/on",  HTTP_OPTIONS, handleOptions);
  server.on("/alarm/on",  HTTP_POST,    handleAlarmOn);

  server.on("/alarm/off", HTTP_OPTIONS, handleOptions);
  server.on("/alarm/off", HTTP_POST,    handleAlarmOff);

  server.on("/status",    HTTP_OPTIONS, handleOptions);
  server.on("/status",    HTTP_GET,     handleStatus);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void connectWiFiSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("WiFi connect...");
  lcd.setCursor(0,1); lcd.print(WIFI_SSID);

  Serial.printf("Connecting to %s\n", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  lcd.clear();
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.print("WiFi OK, IP: "); Serial.println(ip);
    lcd.setCursor(0,0); lcd.print("WiFi OK:");
    lcd.setCursor(0,1); lcd.print(ip);
    delay(1200);

    if (MDNS.begin("alarm")) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      Serial.println("mDNS siap: http://alarm.local");
    } else {
      Serial.println("mDNS gagal start");
    }
  } else {
    Serial.println("WiFi FAIL (STA)");
  }

  // setelah info WiFi sebentar, kosongkan LCD
  displaySafeIdle();
}

void startSoftAPFallbackIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  IPAddress ip = WiFi.softAPIP();

  Serial.printf("AP mode %s, IP: %s\n", ok ? "ON" : "FAIL", ip.toString().c_str());
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("AP:");
  lcd.setCursor(4,0); lcd.print(AP_SSID);
  lcd.setCursor(0,1); lcd.print(ip.toString());
  delay(1500);

  displaySafeIdle(); // setelah itu, kosongkan lagi
}

//==================== ARDUINO SETUP/LOOP =============
void setup() {
  Serial.begin(9600);  // pastikan Serial Monitor juga 9600

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  Wire.begin(D2, D1);
  lcd.begin();
  lcd.backlight();  // sementara ON untuk pesan WiFi

  connectWiFiSTA();
  startSoftAPFallbackIfNeeded();

  // setelah WiFi & AP info, LCD kosong
  displaySafeIdle();
  startHTTPRoutes();
}

void loop() {
  server.handleClient();

  // Push button: kapan pun ditekan (LOW), matikan alarm
  if (digitalRead(BUTTON_PIN) == LOW) {
    stopAlarmStart3Blinks();
  }

  handleBlink();
  handleSiren();
  checkCommandFromBackend();
  MDNS.update();
}
