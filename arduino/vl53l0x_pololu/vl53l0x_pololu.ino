/*
 * Pololu VL53L0X on LOLIN S2 Mini — proximity latch + Pushover
 * Wiring: SDA=8 SCL=9 XSHUT=7 VIN=3V3 GND=GND
 * LED: GPIO15 (onboard)
 *
 * LED: immediate (<40mm on, else off) — no filter/debounce.
 * Filtered latch: Shelly OFF on OBSTACLE, ON on CLEAR (Pushover disabled).
 * Serial: 'c' cal, 'z' clear offset, '1'/'0' Shelly on/off
 */

#include <Wire.h>
#include <VL53L0X.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include "secrets.h"

static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const int PIN_XSHUT = 7;
static const int PIN_LED = 15;

static const uint16_t CAL_TRUE_MM = 40;
static const int CAL_SAMPLES = 25;

static const float ENTER_MM = 40.0f;
static const float EXIT_MM = 50.0f;

static const float EMA_ALPHA = 0.08f;
static const int ENTER_STREAK = 8;
static const int EXIT_STREAK = 10;
static const uint16_t SAMPLE_MS = 40;

VL53L0X sensor;
Preferences prefs;
int16_t range_offset_mm = 0;

float filt_mm = 1000.0f;
bool filt_init = false;
bool obstacle = false;
int below_streak = 0;
int above_streak = 0;

static void xshutReset() {
  pinMode(PIN_XSHUT, OUTPUT);
  digitalWrite(PIN_XSHUT, LOW);
  delay(10);
  digitalWrite(PIN_XSHUT, HIGH);
  delay(10);
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

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  Serial.println("WiFi reconnect...");
  connectWifi();
  return WiFi.status() == WL_CONNECTED;
}

// Shelly Gen1 HTTP API; falls back to Gen2 RPC if needed
static bool setShellyRelay(bool on) {
  if (!ensureWifi()) {
    Serial.println("Shelly: no WiFi");
    return false;
  }

  HTTPClient http;
  String url = String("http://") + SHELLY_HOST + "/relay/0?turn=";
  url += on ? "on" : "off";

  if (!http.begin(url)) {
    Serial.println("Shelly: begin failed");
    return false;
  }
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
  url = String("http://") + SHELLY_HOST + "/rpc/Switch.Set?id=0&on=";
  url += on ? "true" : "false";
  if (!http.begin(url)) {
    Serial.println("Shelly Gen2: begin failed");
    return false;
  }
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

static uint16_t readRawMm() {
  return sensor.readRangeSingleMillimeters();
}

static uint16_t readCorrectedMm() {
  uint16_t raw = readRawMm();
  if (sensor.timeoutOccurred()) {
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
    if (!sensor.timeoutOccurred() && mm >= 15 && mm <= 2000) {
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
  Serial.println("VL53L0X proximity latch");

  connectWifi();

  xshutReset();
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  sensor.setTimeout(500);
  if (!sensor.init()) {
    Serial.println("INIT FAIL");
    while (1) delay(1000);
  }
  sensor.setMeasurementTimingBudget(33000);

  prefs.begin("vl53", false);
  range_offset_mm = prefs.getShort("offset", 0);
  Serial.print("offset=");
  Serial.println(range_offset_mm);
  Serial.println("Ready — LED live <40mm; Shelly latch on filtered OBSTACLE/CLEAR");
}

void loop() {
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

  Serial.print("mm=");
  Serial.println(mm, 0);

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
