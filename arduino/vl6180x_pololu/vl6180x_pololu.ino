/*
 * Pololu VL6180X on LOLIN S2 Mini
 * Library: https://github.com/pololu/vl6180x-arduino
 *
 * Wiring:
 *   SDA  -> GPIO8
 *   SCL  -> GPIO9
 *   SHDN -> GPIO7
 *   VIN  -> 3V3
 *   GND  -> GND
 *   2v8  -> leave open
 */

#include <Wire.h>
#include <VL6180X.h>

static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const int PIN_SHDN = 7;

VL6180X sensor;

static void shdnReset() {
  pinMode(PIN_SHDN, OUTPUT);
  digitalWrite(PIN_SHDN, LOW);
  delay(10);
  digitalWrite(PIN_SHDN, HIGH);
  delay(10);  // >= 5 ms before first I2C
}

static const char *statusName(uint8_t s) {
  switch (s) {
    case 0:  return "NONE";
    case 1:  return "SYSERR_1";
    case 5:  return "SYSERR_5";
    case 6:  return "ECEFAIL";
    case 7:  return "NOCONVERGE";
    case 8:  return "RANGEIGNORE";
    case 11: return "SNR";
    case 12: return "RAWUFLOW";
    case 13: return "RAWOFLOW";
    case 14: return "RANGEUFLOW";
    case 15: return "RANGEOFLOW";
    default: return "OTHER";
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("VL6180X Pololu C++ on S2 Mini");

  shdnReset();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  sensor.setTimeout(500);
  sensor.init();
  sensor.configureDefault();

  Serial.print("model=0x");
  Serial.println(sensor.readReg(VL6180X::IDENTIFICATION__MODEL_ID), HEX);
  Serial.print("scaler=");
  Serial.println(sensor.readReg16Bit(VL6180X::RANGE_SCALER));
  Serial.println("Live range (mm) + status");
}

void loop() {
  uint16_t mm = sensor.readRangeSingleMillimeters();
  uint8_t st = sensor.readRangeStatus();

  Serial.print(mm);
  Serial.print(" mm  [");
  Serial.print(statusName(st));
  Serial.print("]");
  if (sensor.timeoutOccurred()) {
    Serial.print(" TIMEOUT");
  }
  Serial.println();
  delay(50);
}
