/*
 * WiFi scan / join diagnostics for LOLIN S2 Mini.
 * Edit WIFI_SSID / WIFI_PASSWORD below for local testing.
 * Do not commit real credentials.
 */

#include <WiFi.h>

static const char *WIFI_SSID = "your-wifi-ssid";
static const char *WIFI_PASSWORD = "your-wifi-password";

void setup() {
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
  Serial.begin(115200);
  delay(2000);
  Serial.println("boot");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  digitalWrite(15, LOW);
}

void loop() {
  Serial.println("--- scan ---");
  int n = WiFi.scanNetworks(false, true);
  Serial.print("APs=");
  Serial.println(n);
  bool saw = false;
  for (int i = 0; i < n; i++) {
    Serial.print(WiFi.SSID(i));
    Serial.print(" RSSI=");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" ch=");
    Serial.println(WiFi.channel(i));
    if (WiFi.SSID(i) == WIFI_SSID) saw = true;
  }
  Serial.println(saw ? "target SSID: YES" : "target SSID: NO");
  WiFi.scanDelete();

  if (saw) {
    Serial.println("trying STA...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      Serial.print("st=");
      Serial.println((int)WiFi.status());
      delay(1000);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("STA OK ");
      Serial.println(WiFi.localIP());
      delay(10000);
      WiFi.disconnect(true, false);
    } else {
      Serial.print("STA FAIL st=");
      Serial.println((int)WiFi.status());
      WiFi.disconnect(true, false);
    }
  }

  delay(3000);
}
