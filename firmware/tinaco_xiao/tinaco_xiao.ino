// SPDX-License-Identifier: GPL-3.0-or-later
// =================== QUICK SETTINGS ===================
// WhatsApp alerts (Whatabot). Each fires ONCE when the level crosses
// the threshold, then re-arms after the level moves back by ALERT_HYST.
const int ALERT_LOW_PCT  = 20;    // below this  -> "Tinaco casi vacio"
const int ALERT_HIGH_PCT = 97;    // above this  -> "Tinaco lleno"
const int ALERT_HYST     = 5;     // re-arm margin, in %

// Tank calibration: sensor face to water surface, in cm.
const float TANK_EMPTY_CM = 32.0; // reads this when empty ->   0 %
const float TANK_FULL_CM  = 21.0; // reads this when full  -> 100 %
// Anything closer than FULL reads as 100 %, farther than EMPTY as 0 %.
// ======================================================

/*
  REXTRONIX. — Tinaco water-level monitor, XIAO port
  ---------------------------------------------------
  Board : any Seeed XIAO ESP32 (C3 / S3 / C6). Pin names D6/D7 are
          defined per-variant by the board package, so the same code
          builds for all three. Needs Arduino-ESP32 core 3.x.
  Sensor: AJ-SR04M / JSN-SR04T in Mode 5 (UART, ASCII "Gap=1234mm")
  Cloud : ThingSpeak  field1 = level (%)      <- what Alexa reads
  Alerts: Whatabot WhatsApp, checked every 2 s (not tied to ThingSpeak)
  Blind zone: closer than ~22 cm the sensor can't measure; that reads
          as 100 % (the tank is full) instead of being thrown away.

  RXT. TNK-1 — https://github.com/EnriqueMP05/RXT-TNK1
  Firmware license: GPL-3.0-or-later

  Wiring (sensor in UART mode, the silkscreen labels lie):
    Sensor "Echo" (its TX) -> XIAO D7 (RX)
    Sensor "Trig" (its RX) <- XIAO D6 (TX)
    Sensor VCC -> 3V3, GND -> GND
    HLK-PM03 +3.3V -> XIAO 3V3 pin (don't plug USB in at the same time)

  Arduino IDE: Tools > USB CDC On Boot > Enabled  (so Serial goes to USB)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

// ---------- config ----------
// Credentials live in secrets.h (not in the repo).
// Copy secrets.example.h -> secrets.h and fill it in.
#include "secrets.h"

const char* WIFI_SSID   = SECRET_WIFI_SSID;   // 2.4 GHz only
const char* WIFI_PASS   = SECRET_WIFI_PASS;
const char* TS_API_KEY  = SECRET_TS_API_KEY;  // ThingSpeak channel Write API Key

// Whatabot (WhatsApp). Message text is URL-encoded ('+' = space).
const char* WA_URL_FULL  =
  "https://api.whatabot.net/whatsapp/sendMessage?text=RXT.+TNK-1+-+Tinaco+lleno"
  "&apikey=" SECRET_WA_APIKEY "&phone=" SECRET_WA_PHONE;
const char* WA_URL_EMPTY =
  "https://api.whatabot.net/whatsapp/sendMessage?text=RXT.+TNK-1+-+Tinaco+casi+vacio"
  "&apikey=" SECRET_WA_APIKEY "&phone=" SECRET_WA_PHONE;
const uint32_t ALERT_CHECK_MS = 2000;

const uint32_t POST_INTERVAL_MS = 20000;      // ThingSpeak free tier: >= 15 s
const uint32_t WDT_TIMEOUT_MS   = 30000;      // must be > HTTP timeout + WiFi connect
const uint16_t HTTP_TIMEOUT_MS  = 10000;

// Sensor sanity: only 0 (no echo) and absurd values are thrown away.
// Short readings (blind zone, water too close) are KEPT and cap at 100 %.
const int MIN_VALID_MM = 1;
const int MAX_VALID_MM = 6000;

// Blind zone: the sensor can't measure closer than ~20 cm. Inside it,
// it answers 0 or nothing at all, so treat that as full.
const int BLIND_ZONE_MM  = 220;   // readings at or below this -> 100 %
const int BLIND_GUARD_MM = 400;   // a 0 or a silent sensor only counts as "blind zone"
                                  // if the last real reading was closer than this
const uint8_t MISSED_REPLIES_BLIND = 3;  // unanswered triggers in a row -> blind zone

#define SENSOR_RX D7   // XIAO pin that receives from sensor
#define SENSOR_TX D6   // XIAO pin that sends to sensor
#define SENSOR_BAUD 9600
#define SENSOR_TRIGGER_BYTE 0x01     // AJ-SR04M. A JSN-SR04T wants 0x55
const uint32_t TRIGGER_INTERVAL_MS = 500;

// Status LED: S3 and C6 have an onboard user LED (active LOW).
// The C3 has none, so there it drives an external LED on D10
// (D10 -> 330R -> LED -> GND, active HIGH).
#if defined(LED_BUILTIN)
  #define STATUS_LED LED_BUILTIN
  #define LED_ON  LOW
  #define LED_OFF HIGH
#else
  #define STATUS_LED D10
  #define LED_ON  HIGH
  #define LED_OFF LOW
#endif

// 1 blink = sent OK, 3 quick blinks = send failed
void blink(uint8_t times, uint16_t ms) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(STATUS_LED, LED_ON);
    delay(ms);
    digitalWrite(STATUS_LED, LED_OFF);
    if (i + 1 < times) delay(ms);
  }
}

// ---------- median filter (kills ripple / odd echoes) ----------
const uint8_t N_SAMPLES = 7;
int samples[N_SAMPLES];
uint8_t sampleIdx = 0, sampleCount = 0;
uint32_t lastValidMs = 0;
int lastRealMM = 100000;      // last genuine distance seen (starts "far")
uint8_t missedReplies = 0;    // triggers sent without an answer

void pushSample(int mm) {
  samples[sampleIdx] = mm;
  sampleIdx = (sampleIdx + 1) % N_SAMPLES;
  if (sampleCount < N_SAMPLES) sampleCount++;
  lastValidMs = millis();
}

int medianMM() {
  if (sampleCount == 0) return -1;
  int tmp[N_SAMPLES];
  for (uint8_t i = 0; i < sampleCount; i++) tmp[i] = samples[i];
  for (uint8_t i = 1; i < sampleCount; i++) {          // insertion sort
    int v = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = v;
  }
  return tmp[sampleCount / 2];
}

// ---------- sensor parsing (Mode 5: "Gap=1234mm") ----------
void parseLine(const char* line) {
  String s(line);
  int eq = s.indexOf('=');
  int mm = s.indexOf("mm");
  if (eq < 0 || mm <= eq) return;
  int d = s.substring(eq + 1, mm).toInt();
  missedReplies = 0;
  if (d > MAX_VALID_MM) return;                  // absurd, drop it
  if (d >= MIN_VALID_MM && d > BLIND_ZONE_MM) {  // normal reading
    lastRealMM = d;
    pushSample(d);
    return;
  }
  // d == 0 (no echo) or inside the blind zone
  if (d >= MIN_VALID_MM) lastRealMM = d;         // real but very close
  if (lastRealMM <= BLIND_GUARD_MM) pushSample(0);  // 0 mm clamps to 100 %
  // a 0 after a far reading = nothing in range, ignore it like before
}

void pollSensor() {
  static char buf[32];
  static uint8_t len = 0;

  // Mode 5 sleeps until it gets a byte on its RX ("Trig") pin, then
  // fires one ping and answers "Gap=XXXXmm". So we have to ask.
  static uint32_t lastTrigger = 0;
  if (millis() - lastTrigger >= TRIGGER_INTERVAL_MS) {
    lastTrigger = millis();
    // Some units just stay quiet when the target is inside the blind zone.
    if (missedReplies < 255) missedReplies++;
    if (missedReplies > MISSED_REPLIES_BLIND && lastRealMM <= BLIND_GUARD_MM) pushSample(0);
    Serial1.write(SENSOR_TRIGGER_BYTE);
  }

  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = '\0'; parseLine(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;  // garbage overflow, resync
    }
  }
}

// ---------- WiFi ----------
void selectExternalAntenna() {
#if defined(CONFIG_IDF_TARGET_ESP32C6)
  // XIAO C6 has an RF switch: GPIO3 LOW = enable switch, GPIO14 HIGH = U.FL.
  // C3 and S3 are U.FL-only, nothing to do there.
  pinMode(3, OUTPUT);  digitalWrite(3, LOW);
  delay(100);
  pinMode(14, OUTPUT); digitalWrite(14, HIGH);
#endif
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[wifi] connecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    esp_task_wdt_reset();
    pollSensor();                       // keep draining UART meanwhile
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("[wifi] %s  RSSI=%d dBm\n", ok ? "up" : "FAILED", ok ? WiFi.RSSI() : 0);
  return ok;
}

// ---------- level ----------
// Linear map EMPTY -> 0 %, FULL -> 100 %, clamped at both ends.
int levelPercent(int mm) {
  float cm  = mm / 10.0f;
  float pct = 100.0f * (TANK_EMPTY_CM - cm) / (TANK_EMPTY_CM - TANK_FULL_CM);
  return (int)roundf(constrain(pct, 0.0f, 100.0f));
}

// ---------- WhatsApp alerts (Whatabot) ----------
bool httpGet(const char* url) {
  WiFiClientSecure client;
  client.setInsecure();                 // no cert check, fine for a demo
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  int code = http.GET();
  http.end();
  Serial.printf("[wa] HTTP %d\n", code);
  return code >= 200 && code < 300;
}

bool fullArmed  = true;
bool emptyArmed = true;

void checkAlerts(int pct) {
  if (fullArmed && pct > ALERT_HIGH_PCT) {
    Serial.printf("[wa] level %d %% -> sending 'lleno'\n", pct);
    if (ensureWiFi() && httpGet(WA_URL_FULL)) { fullArmed = false; blink(2, 150); }
  } else if (!fullArmed && pct < ALERT_HIGH_PCT - ALERT_HYST) {
    fullArmed = true;
  }

  if (emptyArmed && pct < ALERT_LOW_PCT) {
    Serial.printf("[wa] level %d %% -> sending 'casi vacio'\n", pct);
    if (ensureWiFi() && httpGet(WA_URL_EMPTY)) { emptyArmed = false; blink(2, 150); }
  } else if (!emptyArmed && pct > ALERT_LOW_PCT + ALERT_HYST) {
    emptyArmed = true;
  }
}

// ---------- ThingSpeak ----------
void postReading(int mm) {
  int pct = levelPercent(mm);

  String url = "http://api.thingspeak.com/update?api_key=";
  url += TS_API_KEY;
  url += "&field1=" + String(pct);

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(client, url);              // core 3.x form
  int code = http.GET();
  String body = (code > 0) ? http.getString() : "";
  http.end();

  // ThingSpeak returns the entry id, or "0" if rate-limited / bad key
  Serial.printf("[ts] HTTP %d  entry=%s  level=%d %%  dist=%d mm\n",
                code, body.c_str(), pct, mm);

  bool sent = (code == 200) && body.toInt() > 0;
  if (sent) blink(1, 150);
  else      blink(3, 80);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nREXTRONIX. tinaco monitor — XIAO");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LED_OFF);

  // Watchdog: core 3.x already starts the TWDT, so reconfigure it;
  // fall back to init if it isn't running.
  esp_task_wdt_config_t wdt = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,      // only watch loop(), not idle tasks
    .trigger_panic = true     // reboot on timeout
  };
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(NULL);

  Serial1.begin(SENSOR_BAUD, SERIAL_8N1, SENSOR_RX, SENSOR_TX);

  selectExternalAntenna();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);       // mains powered, no need to save power
  ensureWiFi();
}

void loop() {
  esp_task_wdt_reset();
  pollSensor();

  bool fresh = sampleCount > 0 && (millis() - lastValidMs) < 10000;

  static uint32_t lastAlertCheck = 0;
  if (fresh && millis() - lastAlertCheck >= ALERT_CHECK_MS) {
    lastAlertCheck = millis();
    checkAlerts(levelPercent(medianMM()));
  }

  static uint32_t lastPost = 0;
  if (millis() - lastPost >= POST_INTERVAL_MS) {
    lastPost = millis();

    int mm = medianMM();

    if (!fresh) {
      Serial.println("[sensor] no valid reading in 10 s, skipping post");
    } else if (ensureWiFi()) {
      postReading(mm);
    }
  }

  delay(10);
}
