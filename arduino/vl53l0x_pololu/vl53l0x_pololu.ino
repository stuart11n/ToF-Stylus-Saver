/*
 * Pololu VL53L4CD on LOLIN S2 Mini — proximity latch + Pushover
 * Wiring: SDA=8 SCL=9 XSHUT=7 VIN=3V3 GND=GND
 * LED: GPIO15 (onboard)
 *
 * LED: immediate (<ENTER_MM on, else off) — no filter/debounce.
 * Filtered latch: Shelly OFF on OBSTACLE, ON on CLEAR (Pushover disabled).
 * Web UI: http://<board-ip>/ — view/change Shelly host (stored in NVS).
 * Serial: 'c' cal, 'z' clear offset, '1'/'0' Shelly on/off
 */

#include <Wire.h>
#include <VL53L4CD.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include "secrets.h"

static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const int PIN_XSHUT = 7;
static const int PIN_LED = 15;

static const uint16_t CAL_TRUE_MM = 40;
static const int CAL_SAMPLES = 25;

static const float ENTER_MM = 40.0f;
static const float EXIT_MM = 40.0f;

static const float EMA_ALPHA = 0.08f;
static const int ENTER_STREAK = 8;
static const int EXIT_STREAK = 10;
static const uint16_t SAMPLE_MS = 40;
static const uint16_t I2C_TIMEOUT_MS = 50;
static const uint16_t HTTP_TIMEOUT_MS = 800;
static const int RANGE_FAIL_RECOVER = 12;

VL53L4CD sensor;
Preferences prefs;
WebServer server(80);
int16_t range_offset_mm = 0;
char shelly_host[64];
bool web_started = false;
int range_fail_streak = 0;
uint32_t last_wifi_try_ms = 0;

float filt_mm = 1000.0f;
bool filt_init = false;
bool obstacle = false;
int below_streak = 0;
int above_streak = 0;

static void loadShellyHost() {
  String saved = prefs.getString("shelly", "");
  saved.trim();
  if (saved.length() == 0 || saved.length() >= (int)sizeof(shelly_host)) {
    strncpy(shelly_host, SHELLY_HOST, sizeof(shelly_host) - 1);
  } else {
    strncpy(shelly_host, saved.c_str(), sizeof(shelly_host) - 1);
  }
  shelly_host[sizeof(shelly_host) - 1] = '\0';
}

static bool saveShellyHost(const String &host) {
  String h = host;
  h.trim();
  if (h.length() < 1 || h.length() >= (int)sizeof(shelly_host)) {
    return false;
  }
  strncpy(shelly_host, h.c_str(), sizeof(shelly_host) - 1);
  shelly_host[sizeof(shelly_host) - 1] = '\0';
  prefs.putString("shelly", shelly_host);
  return true;
}

static void handleRoot() {
  String html;
  html.reserve(900);
  html += F("<!DOCTYPE html><html><head><meta charset=utf-8>"
            "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<title>S2 Mini Shelly</title>"
            "<style>"
            "body{font-family:system-ui,sans-serif;max-width:28rem;margin:2rem auto;padding:0 1rem}"
            "label{display:block;margin:.5rem 0 .25rem}"
            "input[type=text]{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem}"
            "button{margin-top:1rem;padding:.5rem 1rem;font-size:1rem}"
            ".meta{color:#555;font-size:.9rem;margin-top:1.5rem}"
            "</style></head><body>"
            "<h1>Shelly host</h1>"
            "<form method=POST action=/save>"
            "<label for=host>IP or hostname</label>"
            "<input id=host name=host type=text value=\"");
  html += shelly_host;
  html += F("\" required maxlength=63>"
            "<button type=submit>Save</button>"
            "</form><p class=meta>Board ");
  html += WiFi.localIP().toString();
  html += F("</p></body></html>");
  server.send(200, "text/html", html);
}

static void handleSave() {
  if (!server.hasArg("host") || !saveShellyHost(server.arg("host"))) {
    server.send(400, "text/plain", "bad host");
    return;
  }
  Serial.print("Shelly host -> ");
  Serial.println(shelly_host);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void startWebUi() {
  if (web_started) {
    return;
  }
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  web_started = true;
  Serial.print("Web UI http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

static void xshutReset() {
  pinMode(PIN_XSHUT, OUTPUT);
  digitalWrite(PIN_XSHUT, LOW);
  delay(10);
  digitalWrite(PIN_XSHUT, HIGH);
  delay(10);
}

static void i2cBegin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

// Unstick SDA-held-low (classic ToF/I2C hang), then re-init the bus.
static void i2cRecover() {
  Wire.end();
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, OUTPUT);
  digitalWrite(PIN_SCL, HIGH);
  delayMicroseconds(5);
  for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) {
    digitalWrite(PIN_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(PIN_SDA, OUTPUT);
  digitalWrite(PIN_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SDA, HIGH);
  delayMicroseconds(5);
  xshutReset();
  i2cBegin();
}

static bool sensorStart() {
  xshutReset();
  sensor.setTimeout(200);
  if (!sensor.init()) {
    return false;
  }
  sensor.setRangeTiming(20, 0);
  sensor.startContinuous();
  return true;
}

static void setLed(bool on) {
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

static void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (mac[i] < 16) Serial.print('0');
    Serial.print(mac[i], HEX);
  }
}

// New locally-administered MAC each boot — avoids mesh client lockout
// without restarting the mesh (our reconnect spam may have flagged the old MAC).
static void useFreshStaMac() {
  uint8_t mac[6];
  // 0x02 = locally administered, unicast
  mac[0] = 0x02;
  mac[1] = 0xF6;
  mac[2] = 0xEE;
  uint32_t r = esp_random();
  mac[3] = (r >> 16) & 0xFF;
  mac[4] = (r >> 8) & 0xFF;
  mac[5] = r & 0xFF;
  esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
  Serial.print("STA MAC ");
  printMac(mac);
  Serial.print(err == ESP_OK ? " OK" : " set_mac FAIL");
  Serial.println();
}

static bool tryJoinBssid(uint8_t ch, const uint8_t *bssid, uint32_t timeout_ms) {
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, ch, bssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
    delay(300);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void connectWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);  // allow WPA/WPA2 mesh APs
  esp_wifi_set_ps(WIFI_PS_NONE);
  useFreshStaMac();
  delay(100);

  Serial.print("WiFi scan ");
  Serial.println(WIFI_SSID);
  int n = WiFi.scanNetworks(false, true);

  // Collect s16n hits, sort by RSSI (simple selection each attempt)
  struct ApHit {
    int32_t rssi;
    uint8_t ch;
    uint8_t bssid[6];
  };
  ApHit hits[12];
  int hitCount = 0;
  for (int i = 0; i < n && hitCount < 12; i++) {
    if (WiFi.SSID(i) != WIFI_SSID) continue;
    hits[hitCount].rssi = WiFi.RSSI(i);
    hits[hitCount].ch = WiFi.channel(i);
    memcpy(hits[hitCount].bssid, WiFi.BSSID(i), 6);
    hitCount++;
  }
  WiFi.scanDelete();

  // Sort strongest first
  for (int i = 0; i < hitCount; i++) {
    for (int j = i + 1; j < hitCount; j++) {
      if (hits[j].rssi > hits[i].rssi) {
        ApHit tmp = hits[i];
        hits[i] = hits[j];
        hits[j] = tmp;
      }
    }
  }

  Serial.print("mesh nodes with SSID: ");
  Serial.println(hitCount);
  if (hitCount == 0) {
    Serial.println("SSID not found");
    return;
  }

  for (int i = 0; i < hitCount; i++) {
    Serial.print("try ");
    printMac(hits[i].bssid);
    Serial.print(" RSSI=");
    Serial.print(hits[i].rssi);
    Serial.print(" ch=");
    Serial.print(hits[i].ch);
    Serial.print(" ...");
    if (tryJoinBssid(hits[i].ch, hits[i].bssid, 12000)) {
      Serial.print(" OK ");
      Serial.println(WiFi.localIP());
      return;
    }
    Serial.print(" fail st=");
    Serial.println((int)WiFi.status());
  }

  Serial.println("WiFi FAILED all mesh BSSIDs");
}

// Do not run a full mesh scan from the ranging loop — that can stall for tens of seconds.
static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    startWebUi();
    return true;
  }
  uint32_t now = millis();
  if (now - last_wifi_try_ms < 5000) {
    return false;
  }
  last_wifi_try_ms = now;
  Serial.println("WiFi quick reconnect...");
  WiFi.reconnect();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 1500) {
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    startWebUi();
    return true;
  }
  return false;
}

// Shelly Gen1 HTTP API; falls back to Gen2 RPC if needed
static bool setShellyRelay(bool on) {
  if (!ensureWifi()) {
    Serial.println("Shelly: no WiFi");
    return false;
  }

  HTTPClient http;
  String url = String("http://") + shelly_host + "/relay/0?turn=";
  url += on ? "on" : "off";

  if (!http.begin(url)) {
    Serial.println("Shelly: begin failed");
    return false;
  }
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  String resp = http.getString();
  http.end();

  if (code > 0) {
    Serial.print("Shelly ");
    Serial.print(on ? "ON" : "OFF");
    Serial.print(" HTTP ");
    Serial.print(code);
    Serial.print(" ");
    Serial.println(resp);
    return code == 200;
  }

  // Gen2 fallback: Switch.Set
  url = String("http://") + shelly_host + "/rpc/Switch.Set?id=0&on=";
  url += on ? "true" : "false";
  if (!http.begin(url)) {
    Serial.println("Shelly Gen2: begin failed");
    return false;
  }
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  code = http.GET();
  resp = http.getString();
  http.end();
  Serial.print("Shelly Gen2 ");
  Serial.print(on ? "ON" : "OFF");
  Serial.print(" HTTP ");
  Serial.print(code);
  Serial.print(" ");
  Serial.println(resp);
  return code == 200;
}

static bool sendPushover(const char *title, const char *message) {
  if (!ensureWifi()) {
    Serial.println("Pushover: no WiFi");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, "https://api.pushover.net/1/messages.json")) {
    Serial.println("Pushover: begin failed");
    return false;
  }
  https.setConnectTimeout(HTTP_TIMEOUT_MS);
  https.setTimeout(HTTP_TIMEOUT_MS);

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "token=";
  body += PUSHOVER_TOKEN;
  body += "&user=";
  body += PUSHOVER_USER;
  body += "&title=";
  body += title;
  body += "&message=";
  body += message;

  int code = https.POST(body);
  String resp = https.getString();
  https.end();

  Serial.print("Pushover HTTP ");
  Serial.print(code);
  Serial.print(" ");
  Serial.println(resp);
  return code == 200;
}

static bool last_range_ok = false;

static uint16_t readRawMm() {
  uint16_t mm = sensor.read();
  // range_status 0 = valid (UM2931)
  last_range_ok = !sensor.timeoutOccurred() && sensor.ranging_data.range_status == 0;
  if (!last_range_ok) {
    return 8190;
  }
  return mm;
}

static uint16_t readCorrectedMm() {
  uint16_t raw = readRawMm();
  if (!last_range_ok) {
    return 8190;
  }
  int v = (int)raw - (int)range_offset_mm;
  if (v < 0) v = 0;
  return (uint16_t)v;
}

static bool calibrateOffset(uint16_t true_mm) {
  Serial.print("Offset cal: HOLD at ");
  Serial.print(true_mm);
  Serial.println(" mm");
  delay(1000);

  long sum = 0;
  int good = 0;
  for (int i = 0; i < CAL_SAMPLES; i++) {
    uint16_t mm = readRawMm();
    if (last_range_ok && mm >= 1 && mm <= 1300) {
      sum += mm;
      good++;
      Serial.print("  ");
      Serial.println(mm);
    }
    delay(70);
  }
  if (good < 10) {
    Serial.println("cal FAILED");
    return false;
  }
  float avg = (float)sum / good;
  range_offset_mm = (int16_t)lroundf(avg - (float)true_mm);
  prefs.putShort("offset", range_offset_mm);
  Serial.print("offset=");
  Serial.println(range_offset_mm);

  filt_init = false;
  below_streak = 0;
  above_streak = 0;
  return true;
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  setLed(false);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // USB CDC: don't stall TX if host opens late
  delay(800);
  Serial.println();
  Serial.println("VL53L4CD proximity latch");

  prefs.begin("vl53", false);
  range_offset_mm = prefs.getShort("offset", 0);
  loadShellyHost();
  Serial.print("offset=");
  Serial.println(range_offset_mm);
  Serial.print("Shelly host=");
  Serial.println(shelly_host);

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    startWebUi();
  }

  i2cBegin();
  bool inited = false;
  for (int i = 0; i < 5 && !inited; i++) {
    inited = sensorStart();
    if (!inited) {
      Serial.println("INIT FAIL, I2C recover");
      i2cRecover();
    }
  }
  if (!inited) {
    Serial.println("INIT FAIL — will retry in loop");
  }

  Serial.print("Ready — LED live <");
  Serial.print(ENTER_MM, 0);
  Serial.println("mm; Shelly latch on filtered OBSTACLE/CLEAR");
}

void loop() {
  server.handleClient();

  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == 'c' || ch == 'C') {
      calibrateOffset(CAL_TRUE_MM);
    } else if (ch == 'z' || ch == 'Z') {
      range_offset_mm = 0;
      prefs.putShort("offset", 0);
      Serial.println("offset cleared");
    } else if (ch == '1') {
      setShellyRelay(true);
    } else if (ch == '0') {
      setShellyRelay(false);
    }
  }

  float mm = (float)readCorrectedMm();

  if (last_range_ok) {
    range_fail_streak = 0;
  } else {
    range_fail_streak++;
    if (range_fail_streak >= RANGE_FAIL_RECOVER) {
      Serial.println("range fail streak — I2C recover");
      i2cRecover();
      sensorStart();
      range_fail_streak = 0;
    }
  }

  Serial.print("mm=");
  Serial.print(mm, 0);
  if (!last_range_ok) {
    Serial.print(" st=");
    Serial.print(sensor.ranging_data.range_status);
  }
  Serial.println();

  // LED: raw (unfiltered) threshold
  setLed(mm < ENTER_MM);

  if (!filt_init) {
    filt_mm = mm;
    filt_init = true;
  } else {
    filt_mm += EMA_ALPHA * (mm - filt_mm);
  }

  if (!obstacle) {
    if (filt_mm < ENTER_MM) {
      below_streak++;
      above_streak = 0;
      if (below_streak >= ENTER_STREAK) {
        obstacle = true;
        below_streak = 0;
        Serial.print("OBSTACLE  filt=");
        Serial.print(filt_mm, 1);
        Serial.println(" mm");
        setShellyRelay(false);
      }
    } else {
      below_streak = 0;
    }
  } else {
    if (filt_mm > EXIT_MM) {
      above_streak++;
      below_streak = 0;
      if (above_streak >= EXIT_STREAK) {
        obstacle = false;
        above_streak = 0;
        Serial.print("CLEAR  filt=");
        Serial.print(filt_mm, 1);
        Serial.println(" mm");
        setShellyRelay(true);
      }
    } else {
      above_streak = 0;
    }
  }

  delay(SAMPLE_MS);
}
