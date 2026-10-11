/*
 * ============================================================
 *  PENYIRAM TANAMAN OTOMATIS - FUZZY TSUKAMOTO
 *  Metode : Tsukamoto (MIN inference, monoton output,
 *           invers fungsi keanggotaan, Weighted Average)
 *  Input  : Kelembapan Tanah (%), Suhu Udara (°C)
 *  Output : Durasi Pompa (0 - 10 detik)
 *  Tanpa library fuzzy apapun
 *
 *  HARDWARE (ESP32):
 *  - Soil sensor  : GPIO 34 (ADC1_CH6, input-only)
 *  - DHT11        : GPIO 4
 *  - Relay pompa  : GPIO 26
 *  - LCD I2C      : SDA=21, SCL=22
 * ============================================================
 */

#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===================== PIN ESP32 ============================
#define SOIL_PIN    34      // Sensor kelembapan tanah (ADC1_CH6)
#define DHT_PIN     4       // DHT11
#define DHT_TYPE    DHT11   // ← DIUBAH dari DHT22 ke DHT11
#define RELAY_PIN   26      // Relay pompa

// LCD I2C
#define I2C_SDA     21
#define I2C_SCL     22

DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===================== KALIBRASI SENSOR TANAH ===============
// Nilai ini HARUS dikalibrasi ulang sesuai hardware Anda
const int SOIL_DRY = 850;   // ADC saat tanah kering (udara)
const int SOIL_WET = 350;   // ADC saat tanah basah (air)

// ===================== VARIABEL GLOBAL ======================
float soilPercent  = 0;
float temperature  = 0;
float humidity     = 0;
float pumpDuration = 0;

unsigned long lastRead = 0;
const unsigned long READ_INTERVAL = 2000;   // DHT11 minimal 1 detik

// ============================================================
//         FUNGSI KEANGGOTAAN INPUT (SEGITIGA & TRAPESIUM)
// ============================================================

float segitiga(float x, float a, float b, float c) {
  if (b <= a || c <= b) return (x >= a && x <= c) ? 1.0 : 0.0;
  if (x <= a || x >= c) return 0.0;
  if (x == b) return 1.0;
  if (x < b)  return (x - a) / (b - a);
  return (c - x) / (c - b);
}

float trapesium(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0;
  if (x >= b && x <= c) return 1.0;
  if (x < b) {
    if (b <= a) return 1.0;
    return (x - a) / (b - a);
  }
  if (d <= c) return 1.0;
  return (d - x) / (d - c);
}

// ---------- KELEMBAPAN TANAH ----------
float soilKering(float x) { return trapesium(x, 0,   0,  30, 45); }
float soilSedang(float x) { return segitiga (x, 30, 50, 70);       }
float soilBasah (float x) { return trapesium(x, 55, 70, 100, 100); }

// ---------- SUHU UDARA ----------
// Catatan: DHT11 range 0-50°C, jadi suhuDingin/Panas disesuaikan
float suhuDingin(float x) { return trapesium(x, 0,   0,  20, 27); }
float suhuNormal(float x) { return segitiga (x, 22, 29, 36);      }
float suhuPanas (float x) { return trapesium(x, 32, 38, 50, 50);  }

// ============================================================
//         FUNGSI KEANGGOTAAN OUTPUT (MONOTON - TSUKAMOTO)
// ============================================================

float outMati(float z) {
  if (z <= 0.0)  return 1.0;
  if (z >= 2.5)  return 0.0;
  return (2.5 - z) / 2.5;
}

float outSebentar(float z) {
  if (z <= 1.5) return 0.0;
  if (z >= 4.0) return 1.0;
  return (z - 1.5) / 2.5;
}

float outSedang(float z) {
  if (z <= 4.0) return 0.0;
  if (z >= 6.5) return 1.0;
  return (z - 4.0) / 2.5;
}

float outLama(float z) {
  if (z <= 6.0) return 0.0;
  if (z >= 8.5) return 1.0;
  return (z - 6.0) / 2.5;
}

float outSangatLama(float z) {
  if (z <= 8.0)  return 0.0;
  if (z >= 10.0) return 1.0;
  return (z - 8.0) / 2.0;
}

// ============================================================
//         FUNGSI INVERS OUTPUT (UNTUK TSUKAMOTO)
// ============================================================

float invMati(float alpha)       { return 2.5 - 2.5 * alpha; }
float invSebentar(float alpha)   { return 1.5 + 2.5 * alpha; }
float invSedang(float alpha)     { return 4.0 + 2.5 * alpha; }
float invLama(float alpha)       { return 6.0 + 2.5 * alpha; }
float invSangatLama(float alpha) { return 8.0 + 2.0 * alpha; }

// ============================================================
//   FUNGSI MIN
// ============================================================
float minimum(float a, float b) { return (a < b) ? a : b; }

// ============================================================
//               FUZZY TSUKAMOTO - INTI SISTEM
// ============================================================

float hitungDurasiTsukamoto(float kelembapan, float suhu) {

  // ---------- 1. FUZZIFIKASI INPUT ----------
  float sKering = soilKering(kelembapan);
  float sSedang = soilSedang(kelembapan);
  float sBasah  = soilBasah (kelembapan);

  float tDingin = suhuDingin(suhu);
  float tNormal = suhuNormal(suhu);
  float tPanas  = suhuPanas (suhu);

  // ---------- 2. FIRING STRENGTH (α-predikat) tiap rule ----------
  float a1 = minimum(sKering, tDingin);  // Kering & Dingin  -> Lama
  float a2 = minimum(sKering, tNormal);  // Kering & Normal  -> Sangat Lama
  float a3 = minimum(sKering, tPanas );  // Kering & Panas   -> Sangat Lama

  float a4 = minimum(sSedang, tDingin);  // Sedang & Dingin  -> Sebentar
  float a5 = minimum(sSedang, tNormal);  // Sedang & Normal  -> Sedang
  float a6 = minimum(sSedang, tPanas );  // Sedang & Panas   -> Lama

  float a7 = minimum(sBasah,  tDingin);  // Basah & Dingin   -> Mati
  float a8 = minimum(sBasah,  tNormal);  // Basah & Normal   -> Mati
  float a9 = minimum(sBasah,  tPanas );  // Basah & Panas    -> Sebentar

  // ---------- 3. INVERS FUNGSI KEANGGOTAAN OUTPUT ----------
  float z1 = invLama(a1);
  float z2 = invSangatLama(a2);
  float z3 = invSangatLama(a3);

  float z4 = invSebentar(a4);
  float z5 = invSedang(a5);
  float z6 = invLama(a6);

  float z7 = invMati(a7);
  float z8 = invMati(a8);
  float z9 = invSebentar(a9);

  // ---------- 4. DEFUZZIFIKASI WEIGHTED AVERAGE ----------
  float num = a1*z1 + a2*z2 + a3*z3
            + a4*z4 + a5*z5 + a6*z6
            + a7*z7 + a8*z8 + a9*z9;

  float den = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9;

  if (den < 0.0001) return 0.0;
  return num / den;
}

// ============================================================
//               BACA SENSOR
// ============================================================
float bacaKelembapanTanah() {
  // Rata-rata 8 sampel untuk stabilkan ADC ESP32
  long sum = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) {
    sum += analogRead(SOIL_PIN);
    delayMicroseconds(200);
  }
  float adc = (float)sum / (float)N;

  // Mapping float: SOIL_DRY -> 0%, SOIL_WET -> 100%
  float persen = (SOIL_DRY - adc) * 100.0 / (float)(SOIL_DRY - SOIL_WET);
  if (persen < 0)   persen = 0;
  if (persen > 100) persen = 100;
  return persen;
}

// ============================================================
//               SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);   // Pompa OFF (active HIGH)

  dht.begin();

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Penyiram Fuzzy");
  lcd.setCursor(0, 1); lcd.print("Tsukamoto DHT11");
  delay(2000);
  lcd.clear();

  Serial.println("=== Fuzzy Tsukamoto Penyiram Tanaman (DHT11) ===");
  Serial.println("Pin: Soil=34, DHT=4, Relay=26");
}

// ============================================================
//               LOOP UTAMA
// ============================================================
void loop() {
  if (millis() - lastRead >= READ_INTERVAL) {
    lastRead = millis();

    // --- Baca sensor ---
    soilPercent = bacaKelembapanTanah();
    temperature = dht.readTemperature();
    humidity    = dht.readHumidity();

    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("Gagal baca DHT11!");
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("DHT11 Error!");
      lcd.setCursor(0, 1); lcd.print("Cek wiring...");
      return;
    }

    // --- Fuzzy Tsukamoto ---
    pumpDuration = hitungDurasiTsukamoto(soilPercent, temperature);

    // --- Serial Monitor ---
    Serial.print("Tanah="); Serial.print(soilPercent, 1); Serial.print("% | ");
    Serial.print("Suhu=");  Serial.print(temperature, 1); Serial.print("C | ");
    Serial.print("Hum=");   Serial.print(humidity, 1);    Serial.print("% | ");
    Serial.print("Durasi="); Serial.print(pumpDuration, 2); Serial.println(" s");

    // --- LCD ---
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("T:"); lcd.print(soilPercent, 0); lcd.print("% ");
    lcd.print("S:"); lcd.print(temperature, 1); lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print("Pompa:"); lcd.print(pumpDuration, 1); lcd.print("s ");

    // --- Aktuasi pompa ---
    if (pumpDuration > 0.5) {
      Serial.println(">> Pompa ON");
      lcd.setCursor(11, 1); lcd.print("ON ");
      digitalWrite(RELAY_PIN, HIGH);
      delay((unsigned long)(pumpDuration * 1000));
      digitalWrite(RELAY_PIN, LOW);
      Serial.println(">> Pompa OFF");
      lcd.setCursor(11, 1); lcd.print("OFF");
    }
  }
}