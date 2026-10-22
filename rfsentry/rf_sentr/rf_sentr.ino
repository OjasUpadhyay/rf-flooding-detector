#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

// ---------- Pins ----------
#define CE_PIN    4
#define CSN_PIN   5
#define SCK_PIN   18
#define MISO_PIN  19
#define MOSI_PIN  23
#define LED_PIN   2

RF24 radio(CE_PIN, CSN_PIN);

// ---------- Tuning ----------
const uint8_t NUM_CHANNELS       = 64;   // channels 0-63 (2.400-2.463 GHz)
const uint8_t SAMPLES            = 20;   // listen cycles per channel per sweep
const uint8_t HIT_PERCENT        = 25;   // channel is "busy" if carrier seen in >= this % of samples
const uint8_t ALARM_MARGIN       = 3;    // ALARM if busy channels > baseline + this
const uint8_t CALIBRATION_SWEEPS = 5;    // quiet-environment sweeps at startup
const uint8_t QUIET_AVG_PERCENT  = 2;    // below this average activity = "no devices"

// ---------- Display options ----------
const bool    SHOW_DETAILS       = true;   // second line with numbers + bar
const bool    SHOW_MAP           = true;   // 64-char channel map (for debugging)
const uint8_t JUMP_PERCENT       = 15;     // "suddenly changed" if avg moves this much between sweeps

uint8_t  hits[NUM_CHANNELS];
uint8_t  baselineBusy = 0;
uint8_t  prevAvg = 0;
uint32_t sweepNum = 0;

// One full sweep (channels interleaved so short bursts are not missed)
void sweep() {
  memset(hits, 0, sizeof(hits));
  for (uint8_t rep = 0; rep < SAMPLES; rep++) {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      radio.setChannel(ch);
      radio.startListening();
      delayMicroseconds(170);
      radio.stopListening();
      if (radio.testCarrier()) hits[ch]++;   // read AFTER stopListening
    }
  }
}

uint8_t countBusy() {
  uint8_t busy = 0;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if ((hits[ch] * 100) / SAMPLES >= HIT_PERCENT) busy++;
  }
  return busy;
}

uint8_t avgPercent() {
  uint32_t sum = 0;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) sum += hits[ch];
  return (sum * 100) / ((uint32_t)SAMPLES * NUM_CHANNELS);
}

void printBar(uint8_t pct) {
  uint8_t filled = (pct * 20) / 100;
  Serial.print('[');
  for (uint8_t i = 0; i < 20; i++) Serial.print(i < filled ? '#' : '-');
  Serial.print(']');
}

void printMap() {
  Serial.print("      MAP ch0 |");
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    uint8_t pct = (hits[ch] * 100) / SAMPLES;
    char c = ' ';
    if      (pct >= 90) c = '#';
    else if (pct >= 50) c = 'O';
    else if (pct >= 20) c = 'o';
    else if (pct >= 1)  c = '.';
    Serial.print(c);
  }
  Serial.println("| ch63");
}

// Blink the onboard LED (GPIO 2) a number of times
void blinkLed(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(onMs);
    digitalWrite(LED_PIN, LOW);  delay(offMs);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);   // give the Serial Monitor time to connect
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  blinkLed(3, 100, 100);   // 3 quick blinks = board has booted
  Serial.println("\n[BOOT] Sketch started. Serial is working.");
  Serial.println("[BOOT] Starting radio...");

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CSN_PIN);

  if (!radio.begin(&SPI)) {
    Serial.println("[ERROR] Radio not responding. Check wiring and 3.3V supply + 10uF cap.");
    while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(150); }
  }
  if (!radio.isChipConnected()) {
    Serial.println("[ERROR] nRF24L01 not detected on SPI bus (bad wiring or fake/damaged module).");
    while (1) { digitalWrite(LED_PIN, !digitalRead(LED_PIN)); delay(150); }
  }

  radio.setAutoAck(false);
  radio.disableCRC();
  radio.setDataRate(RF24_1MBPS);
  Serial.println("[BOOT] Radio OK.");

  Serial.println("\n=======================================================");
  Serial.println("   RF SENTINEL - 2.4 GHz device / jamming monitor");
  Serial.println("=======================================================");
  Serial.println("What the messages mean:");
  Serial.println("  CLEAR    = no devices detected, air is quiet");
  Serial.println("  NORMAL   = a few devices detected (usual Wi-Fi/Bluetooth)");
  Serial.println("  WARNING  = many devices detected, air is crowded");
  Serial.println("  ALARM    = TOO MANY devices / possible jamming or flooding");
  Serial.println("Note: this estimates how busy the air is, it cannot count exact devices.");
  Serial.println("LED: 1 blink = CLEAR, 2 = NORMAL, 3 = WARNING, fast strobe = ALARM");
  Serial.println("-------------------------------------------------------");
  Serial.println("Calibrating... keep things as normal, no jammer running.");

  for (uint8_t i = 0; i < CALIBRATION_SWEEPS; i++) {
    sweep();
    uint8_t busy = countBusy();
    if (busy > baselineBusy) baselineBusy = busy;
    Serial.printf("  calibration %d/%d ...\n", i + 1, CALIBRATION_SWEEPS);
    blinkLed(1, 100, 0);
  }
  prevAvg = avgPercent();

  Serial.printf("Calibration done. Normal level = %d busy channels.\n", baselineBusy);
  Serial.println("Monitoring started.\n");
}

void loop() {
  sweepNum++;
  sweep();

  uint8_t busy = countBusy();
  uint8_t avg  = avgPercent();
  int dAvg = (int)avg - (int)prevAvg;

  bool alarm = false;
  int ledBlinks = 2;   // NORMAL = 2 blinks (CLEAR = 1, WARNING = 3)

  if (busy > baselineBusy + ALARM_MARGIN) {
    alarm = true;
    Serial.printf("[#%lu] >>> ALARM: TOO MANY DEVICES! Possible jamming / flooding <<<\n", sweepNum);
  } else if (busy > baselineBusy + ALARM_MARGIN / 2) {
    ledBlinks = 3;
    Serial.printf("[#%lu] >>> WARNING: Many devices detected - air is crowded <<<\n", sweepNum);
  } else if (avg < QUIET_AVG_PERCENT && busy == 0) {
    ledBlinks = 1;
    Serial.printf("[#%lu] >>> CLEAR: No devices detected - air is quiet <<<\n", sweepNum);
  } else {
    Serial.printf("[#%lu] >>> NORMAL: A few devices detected <<<\n", sweepNum);
  }

  if (SHOW_DETAILS) {
    Serial.printf("      Activity %3d%% ", avg);
    printBar(avg);
    Serial.printf("  | Busy channels: %d (normal: %d)\n", busy, baselineBusy);
  }
  if (abs(dAvg) >= JUMP_PERCENT) {
    Serial.printf("      ! Activity suddenly %s by %d%% since last scan\n",
                  dAvg > 0 ? "INCREASED" : "DROPPED", abs(dAvg));
  }
  if (SHOW_MAP) printMap();

  // LED shows the status after every scan:
  //   CLEAR = 1 blink, NORMAL = 2 blinks, WARNING = 3 blinks, ALARM = fast strobe
  if (alarm) {
    blinkLed(15, 40, 40);
  } else {
    blinkLed(ledBlinks, 150, 150);
  }
  digitalWrite(LED_PIN, LOW);

  prevAvg = avg;
  Serial.println();
}