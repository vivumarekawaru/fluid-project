// ============================================================================
// ESP32 Water Filtration Monitor
// Features:
//   1. YF-S201 Flow Sensor (Interrupt-driven, Cumulative Litres, Carbon Life)
//   2. Dual Pressure Transducers (Upstream & Downstream Differential Pressure in PSI/bar/MPa)
//   3. Configurable Sediment Filter alert threshold (19.0 PSI)
//   4. Web-configurable Carbon Filter rated capacity with cartridge label suggestion
//   5. Retroactive Prior Volume Estimation:
//      - Asks user how many days ago the carbon filter was replaced (0 if brand new)
//      - Tracks progressive daily usage across the next 10 days
//      - Multiplies daily average by past days to estimate prior water treated
//      - Locks 10-day average permanently once 10 days have elapsed
//   6. Status LEDs:
//      - GPIO 19: Green LED (Solid ON when calibrated, configured, and no alerts)
//      - GPIO 18: Red LED (Solid ON during alerts; Blinks until calibrated & configured)
//   7. Non-Volatile Storage (NVS/Preferences) for power-loss survival
//   8. Wi-Fi Access Point Web Server with live streaming dashboard & phone time sync
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ============================================================================
// CONFIGURATION SECTION - ADJUST TO MATCH YOUR SETUP
// ============================================================================

// --- Wi-Fi Access Point (Hotspot) Settings ---
const char* AP_SSID          = "ESP32_Filter_Monitor"; // Wi-Fi network name for phone
const char* AP_PASSWORD      = "password123";          // Password (minimum 8 characters)

// --- GPIO Pin Assignments ---
// Status LEDs
const int PIN_LED_GREEN      = 19;  // Green LED: ON when everything is fine
const int PIN_LED_RED        = 18;  // Red LED: Solid ON during alerts; Blinks until calibrated & configured
const int PIN_LED_BOARD      = 2;   // Onboard LED (lights on flow activity)

// Flow Sensor (Must be an interrupt-capable GPIO)
const int PIN_FLOW           = 34;  // Flow sensor signal pin (GPIO 32, tested)

// Pressure Transducers (Must use ADC1 pins: GPIO 32-39. ADC2 is disabled by Wi-Fi!)
const int PIN_P_UPSTREAM     = 35;  // Upstream pressure transducer (GPIO 34, tested)
const int PIN_P_DOWNSTREAM   = 32;  // Downstream pressure transducer (GPIO 35, ADC1_CH7)

// Hardware Reset Button (Hold for 2 seconds to reset carbon filter volume)
const int PIN_RESET_BUTTON   = 0;   // Onboard BOOT button (active LOW)

// --- Filter Alert Thresholds & Capacity Defaults ---
const float SEDIMENT_ALERT_PSI              = 19.0;   // Alert when differential pressure (ΔP) reaches 19 PSI
const float DEFAULT_CARBON_CAPACITY_LITRES  = 2000.0; // Default capacity until configured on web page
const uint32_t TEN_DAYS_IN_SECONDS          = 864000; // 10 days * 24h * 3600s = 864,000s

// --- Flow Sensor Calibration (YF-S201) ---
const float FLOW_CAL_FACTOR  = 7.5;   // Pulse frequency per L/min: F = 7.5 * Q (tested)
const float PULSES_PER_LITRE = 450.0; // ~450 pulses per 1.0 Litre (tested)

// --- Upstream Pressure Sensor Calibration (1.2 MPa Transducer) ---
const float UP_DIVIDER_RATIO = 1.0;   // Resistor divider ratio (1.0 = direct)
const float UP_SPAN_V        = 4.0;   // Voltage span: 0.5 V to 4.5 V -> 4.0 V span
const float UP_P_MAX         = 1.2;   // Rated max pressure in MPa (1.2 MPa = 12 bar)
const float UP_NOISE_FLOOR_V = 0.040; // 40 mV deadband to ignore idle power adapter ripple

// --- Downstream Pressure Sensor Calibration (1.2 MPa Transducer) ---
const float DOWN_DIVIDER_RATIO = 1.0; // Resistor divider ratio (1.0 = direct)
const float DOWN_SPAN_V      = 4.0;   // Voltage span: 0.5 V to 4.5 V -> 4.0 V span
const float DOWN_P_MAX       = 1.2;   // Rated max pressure in MPa (1.2 MPa = 12 bar)
const float DOWN_NOISE_FLOOR_V = 0.040; // 40 mV deadband to ignore idle power adapter ripple

// --- Non-Blocking Timing Intervals (Milliseconds) ---
const unsigned long FLOW_INTERVAL_MS      = 1000;   // Flow rate & accumulation update (1s)
const unsigned long PRESSURE_INTERVAL_MS  = 500;    // Pressure sensor read update (500ms)
const unsigned long NVS_SAVE_INTERVAL_MS  = 60000;  // Periodic flash save every 60s (if changed)
const unsigned long LED_BLINK_INTERVAL_MS = 500;    // Red LED blink rate during unconfigured state (1 Hz)
const unsigned long BUTTON_HOLD_MS        = 2000;   // Hold reset button for 2000ms to trigger

// ============================================================================
// GLOBAL OBJECTS & STATE VARIABLES
// ============================================================================

WebServer server(80);
Preferences preferences;

// Interrupt state for flow meter
volatile unsigned long isrPulseCount = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Live flow & volume variables
unsigned long latestPulses = 0;
float latestFlowRate = 0.0;             // L/min
float measuredLitres = 0.0;             // Actual volume measured by flow sensor since installation
float lastSavedMeasuredLitres = 0.0;    // Value at last NVS flash write

// Retroactive 10-Day Averaging & Carbon Life Variables
float carbonFilterRatedLitres = DEFAULT_CARBON_CAPACITY_LITRES;
int   carbonFilterPastDays = 0;         // Days ago filter was replaced (entered by user, 0 if new)
bool  carbonPastDaysConfigured = false; // True when user inputs filter age
bool  carbonCapacityConfigured = false; // True when user inputs capacity
bool  pressureCalibratedByUser = false; // True when user calibrates pressure sensor

uint32_t learningSecondsElapsed = 0;    // Total elapsed seconds in 10-day learning phase
float    learningMeasuredLitres = 0.0;  // Measured litres during the 10-day learning phase
bool     learningPhaseCompleted = false;// True when 10 full days have elapsed
float    lockedPastLitres = 0.0;        // Locked historical litres after 10-day learning completion
uint32_t startEpochTimestamp = 0;       // Synced calendar start time from phone browser

float estimatedPriorLitres = 0.0;       // Current calculated prior volume (L)
float totalTreatedLitres = 0.0;         // Effective total = Measured + Prior (L)
float carbonLifeUsedPct = 0.0;          // % life used (0 - 100%+)
float currentDailyAvgLitres = 0.0;      // Current measured daily average consumption (L/day)

// Replacement alert flags
bool replaceCarbonFilter = false;       // Flag when totalTreatedLitres >= carbonFilterRatedLitres
bool replaceSedimentFilter = false;     // Flag when differential pressure >= 19.0 PSI

// Live pressure variables
float vZeroUp = 0.5;                    // Measured at atmospheric zero calibration
float vZeroDown = 0.5;
float filteredVUp = 0.0;                // EMA filtered voltage
float filteredVDown = 0.0;
float latestPUpMPa = 0.0;               // Upstream pressure (MPa)
float latestPDownMPa = 0.0;             // Downstream pressure (MPa)
float latestPDiffMPa = 0.0;             // Differential pressure (Upstream - Downstream)
float latestPDiffPsi = 0.0;             // Differential pressure in PSI

// Non-blocking timers
unsigned long lastFlowTime = 0;
unsigned long lastPressureTime = 0;
unsigned long lastNvsSaveTime = 0;
unsigned long lastLedBlinkTime = 0;
unsigned long lastSecondTick = 0;
unsigned long buttonPressStartTime = 0;
bool redLedBlinkState = false;

// ============================================================================
// HARDWARE INTERRUPT SERVICE ROUTINE (FLOW SENSOR)
// ============================================================================
void IRAM_ATTR onFlowPulse() {
  portENTER_CRITICAL_ISR(&mux);
  isrPulseCount++;
  portEXIT_CRITICAL_ISR(&mux);
}

// ============================================================================
// SENSOR READING & FILTERING LOGIC
// ============================================================================

// Reads 32 samples, discards 8 lowest and 8 highest spikes, averages middle 16
float readAdcVoltage(int pin, float dividerRatio) {
  int samples[32];
  for (int i = 0; i < 32; i++) {
    samples[i] = analogReadMilliVolts(pin);
    delayMicroseconds(200); // Small spread without blocking Wi-Fi or WebServer
  }
  // Sort samples to eliminate outliers
  for (int i = 0; i < 31; i++) {
    for (int j = i + 1; j < 32; j++) {
      if (samples[i] > samples[j]) {
        int temp = samples[i];
        samples[i] = samples[j];
        samples[j] = temp;
      }
    }
  }
  long sum = 0;
  for (int i = 8; i < 24; i++) {
    sum += samples[i];
  }
  float adcVolts = (sum / 16.0) / 1000.0;
  return adcVolts / dividerRatio;
}

// Zero-offset calibration routine for both pressure sensors at atmospheric pressure
void calibratePressureZero() {
  Serial.println("Calibrating pressure sensors at atmospheric pressure...");
  float sumUp = 0;
  float sumDown = 0;
  for (int i = 0; i < 10; i++) {
    sumUp += readAdcVoltage(PIN_P_UPSTREAM, UP_DIVIDER_RATIO);
    sumDown += readAdcVoltage(PIN_P_DOWNSTREAM, DOWN_DIVIDER_RATIO);
    server.handleClient(); // Keep web server responsive
    delay(20);
  }
  vZeroUp = sumUp / 10.0;
  vZeroDown = sumDown / 10.0;
  filteredVUp = vZeroUp;
  filteredVDown = vZeroDown;

  Serial.print("Upstream Zero Voltage Locked:   "); Serial.print(vZeroUp, 3); Serial.println(" V");
  Serial.print("Downstream Zero Voltage Locked: "); Serial.print(vZeroDown, 3); Serial.println(" V");
}

// Computes pressure in MPa given filtered voltage and calibration values
float calculatePressureMPa(float filteredV, float vZero, float noiseFloor, float pMax, float spanV) {
  float deltaV = filteredV - vZero;
  if (deltaV < noiseFloor) {
    deltaV = 0.0; // Deadband to suppress idle power supply noise
  }
  return (deltaV * pMax) / spanV;
}

// ============================================================================
// RETROACTIVE VOLUME & CARBON FILTER LIFE CALCULATIONS
// ============================================================================

void recalculateCarbonFilterTotals() {
  // Case 1: Brand new filter installed (0 past days)
  if (carbonFilterPastDays <= 0) {
    estimatedPriorLitres = 0.0;
    currentDailyAvgLitres = 0.0;
    totalTreatedLitres = measuredLitres;
    learningPhaseCompleted = true;
  }
  // Case 2: 10-day learning phase is complete
  else if (learningPhaseCompleted) {
    estimatedPriorLitres = lockedPastLitres;
    currentDailyAvgLitres = (learningMeasuredLitres > 0) ? (learningMeasuredLitres / 10.0f) : 0.0f;
    totalTreatedLitres = measuredLitres + estimatedPriorLitres;
  }
  // Case 3: Actively in 10-day learning phase (Progressive Estimation)
  else {
    float daysElapsed = (float)learningSecondsElapsed / 86400.0f;
    // For the initial hour, use a minimal baseline to avoid division by zero
    if (daysElapsed < 0.02f) {
      currentDailyAvgLitres = 0.0;
      estimatedPriorLitres = 0.0;
    } else {
      currentDailyAvgLitres = learningMeasuredLitres / daysElapsed;
      estimatedPriorLitres = (float)carbonFilterPastDays * currentDailyAvgLitres;
    }
    totalTreatedLitres = measuredLitres + estimatedPriorLitres;
  }

  // Carbon filter percentage and replacement alert
  if (carbonFilterRatedLitres > 0) {
    carbonLifeUsedPct = (totalTreatedLitres / carbonFilterRatedLitres) * 100.0f;
    replaceCarbonFilter = (totalTreatedLitres >= carbonFilterRatedLitres);
  } else {
    carbonLifeUsedPct = 0.0;
    replaceCarbonFilter = false;
  }
}

// Advance learning timer and check if 10 days reached
void updateLearningTimer() {
  if (carbonFilterPastDays > 0 && !learningPhaseCompleted) {
    learningSecondsElapsed++;
    if (learningSecondsElapsed >= TEN_DAYS_IN_SECONDS) {
      learningPhaseCompleted = true;
      float avg10Day = learningMeasuredLitres / 10.0f;
      lockedPastLitres = (float)carbonFilterPastDays * avg10Day;
      Serial.println("\n**************************************************");
      Serial.println(">>> 10-DAY LEARNING COMPLETED! <<<");
      Serial.printf("10-Day Daily Average: %.2f L/day\n", avg10Day);
      Serial.printf("Locked Prior Volume: %.2f L (%d days ago)\n", lockedPastLitres, carbonFilterPastDays);
      Serial.println("**************************************************\n");
      saveAllPreferences();
    }
  }
}

// ============================================================================
// NVS / FLASH MEMORY MANAGEMENT (PREFERENCES)
// ============================================================================

void loadPreferences() {
  preferences.begin("water_mon", false);
  measuredLitres = preferences.getFloat("m_litres", 0.0f);
  lastSavedMeasuredLitres = measuredLitres;

  carbonFilterRatedLitres   = preferences.getFloat("rated_litres", DEFAULT_CARBON_CAPACITY_LITRES);
  carbonCapacityConfigured  = preferences.getBool("cap_set", false);

  carbonFilterPastDays      = preferences.getInt("past_days", 0);
  carbonPastDaysConfigured  = preferences.getBool("days_set", false);

  learningSecondsElapsed    = preferences.getUInt("learn_sec", 0);
  learningMeasuredLitres    = preferences.getFloat("learn_vol", 0.0f);
  learningPhaseCompleted    = preferences.getBool("learn_done", false);
  lockedPastLitres          = preferences.getFloat("lock_past", 0.0f);
  startEpochTimestamp       = preferences.getUInt("start_epoch", 0);

  pressureCalibratedByUser  = preferences.getBool("p_calibrated", false);
  preferences.end();

  recalculateCarbonFilterTotals();

  Serial.println("\n--- Restored Settings from NVS ---");
  Serial.printf("Measured Litres: %.2f L\n", measuredLitres);
  Serial.printf("Carbon Capacity: %.1f L (Set: %s)\n", carbonFilterRatedLitres, carbonCapacityConfigured ? "YES" : "NO");
  Serial.printf("Filter Age: %d days ago (Set: %s)\n", carbonFilterPastDays, carbonPastDaysConfigured ? "YES" : "NO");
  Serial.printf("Learning Time: %u s (%.2f days) | 10-Day Done: %s\n", learningSecondsElapsed, (float)learningSecondsElapsed/86400.0f, learningPhaseCompleted ? "YES" : "NO");
  Serial.printf("Prior Estimated Litres: %.2f L | Total Treated: %.2f L\n", estimatedPriorLitres, totalTreatedLitres);
  Serial.printf("Pressure Calibrated: %s\n", pressureCalibratedByUser ? "YES" : "NO");
}

void saveAllPreferences() {
  preferences.begin("water_mon", false);
  preferences.putFloat("m_litres", measuredLitres);
  preferences.putFloat("rated_litres", carbonFilterRatedLitres);
  preferences.putBool("cap_set", carbonCapacityConfigured);
  preferences.putInt("past_days", carbonFilterPastDays);
  preferences.putBool("days_set", carbonPastDaysConfigured);
  preferences.putUInt("learn_sec", learningSecondsElapsed);
  preferences.putFloat("learn_vol", learningMeasuredLitres);
  preferences.putBool("learn_done", learningPhaseCompleted);
  preferences.putFloat("lock_past", lockedPastLitres);
  preferences.putUInt("start_epoch", startEpochTimestamp);
  preferences.putBool("p_calibrated", pressureCalibratedByUser);
  preferences.end();
  lastSavedMeasuredLitres = measuredLitres;
  Serial.printf("NVS All Saved: Measured=%.2f L, Total=%.2f L, LearnSec=%u\n", measuredLitres, totalTreatedLitres, learningSecondsElapsed);
}

// Periodic or flow-end flash save check to preserve write endurance
void checkAndSaveNVS(unsigned long now) {
  bool intervalPassed = (now - lastNvsSaveTime >= NVS_SAVE_INTERVAL_MS);
  bool flowJustStopped = (latestFlowRate == 0.0 && (measuredLitres - lastSavedMeasuredLitres >= 0.05));

  if (intervalPassed || flowJustStopped) {
    if ((measuredLitres != lastSavedMeasuredLitres) || (carbonFilterPastDays > 0 && !learningPhaseCompleted)) {
      saveAllPreferences();
      lastNvsSaveTime = now;
    }
  }
}

// Reset cumulative flow total after installing a new carbon filter cartridge
void resetCarbonFilterTotal() {
  measuredLitres = 0.0;
  lastSavedMeasuredLitres = 0.0;
  carbonFilterPastDays = 0;
  carbonPastDaysConfigured = true; // Confirmed 0 days for new filter
  learningSecondsElapsed = 0;
  learningMeasuredLitres = 0.0;
  learningPhaseCompleted = true;
  lockedPastLitres = 0.0;
  startEpochTimestamp = 0;
  recalculateCarbonFilterTotals();
  saveAllPreferences();
  Serial.println(">>> CARBON FILTER RESET: Fresh filter installed (0 days prior, 0.0 L) <<<");
}

// Check hardware button press with debounce
void checkHardwareResetButton() {
  if (digitalRead(PIN_RESET_BUTTON) == LOW) {
    if (buttonPressStartTime == 0) {
      buttonPressStartTime = millis();
    } else if (millis() - buttonPressStartTime >= BUTTON_HOLD_MS) {
      resetCarbonFilterTotal();
      buttonPressStartTime = 0;
      // Visual feedback: blink green LED 4 times
      for (int i = 0; i < 4; i++) {
        digitalWrite(PIN_LED_GREEN, HIGH); delay(80);
        digitalWrite(PIN_LED_GREEN, LOW); delay(80);
      }
    }
  } else {
    buttonPressStartTime = 0;
  }
}

// ============================================================================
// LED CONTROL STATE MACHINE (NON-BLOCKING)
// ============================================================================
void updateStatusLeds(unsigned long now) {
  bool setupPending = (!pressureCalibratedByUser || !carbonCapacityConfigured || !carbonPastDaysConfigured);
  bool anyAlertActive = (replaceCarbonFilter || replaceSedimentFilter);

  // Condition 1: Setup pending -> RED BLINKS until user calibrates & configures settings
  if (setupPending) {
    if (now - lastLedBlinkTime >= LED_BLINK_INTERVAL_MS) {
      lastLedBlinkTime = now;
      redLedBlinkState = !redLedBlinkState;
    }
    digitalWrite(PIN_LED_RED, redLedBlinkState ? HIGH : LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
  }
  // Condition 2: Any Alert active -> RED SOLID ON, GREEN OFF
  else if (anyAlertActive) {
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_GREEN, LOW);
  }
  // Condition 3: Everything fine -> GREEN ALWAYS ON, RED OFF
  else {
    digitalWrite(PIN_LED_GREEN, HIGH);
    digitalWrite(PIN_LED_RED, LOW);
  }

  // Onboard LED lights when water is actively flowing
  if (latestPulses > 0) {
    digitalWrite(PIN_LED_BOARD, HIGH);
  } else {
    digitalWrite(PIN_LED_BOARD, LOW);
  }
}

// ============================================================================
// RESPONSIVE WEB DASHBOARD (HTML / CSS / JS)
// ============================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Filter Monitor</title>
  <style>
    :root {
      --bg: #121212;
      --card-bg: #1e1e1e;
      --card-border: #2a2a2a;
      --text: #ffffff;
      --subtext: #9e9e9e;
      --accent-flow: #00e5ff;
      --accent-pressure: #00e676;
      --accent-diff: #ffb300;
      --accent-learn: #7c4dff;
      --danger: #ff1744;
      --btn-bg: #2979ff;
    }
    body {
      font-family: system-ui, -apple-system, sans-serif;
      text-align: center;
      background: var(--bg);
      color: var(--text);
      margin: 0;
      padding: 16px;
    }
    h1 { font-size: 1.4rem; margin: 10px 0 16px; font-weight: 700; }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      padding: 18px;
      border-radius: 16px;
      max-width: 380px;
      margin: 14px auto;
      box-shadow: 0 4px 14px rgba(0,0,0,0.5);
      text-align: left;
      box-sizing: border-box;
    }
    .card-title { font-size: 0.95rem; font-weight: 700; text-transform: uppercase; letter-spacing: 1px; color: var(--subtext); margin-bottom: 10px; }
    .val-large { font-size: 2.3rem; font-weight: 800; margin: 6px 0; }
    .val-flow { color: var(--accent-flow); }
    .val-diff { color: var(--accent-diff); }
    .val-learn { color: var(--accent-learn); }
    .val-sub { font-size: 0.95rem; color: #eceff1; margin: 4px 0; }
    .row { display: flex; justify-content: space-between; align-items: baseline; margin: 6px 0; }
    .label { color: var(--subtext); font-size: 0.88rem; }
    .badge {
      display: inline-block;
      padding: 4px 10px;
      border-radius: 8px;
      font-size: 0.8rem;
      font-weight: 700;
      text-align: center;
    }
    .badge-ok { background: rgba(0,230,118,0.15); color: #00e676; }
    .badge-warn { background: rgba(255,23,68,0.25); color: #ff1744; }
    .badge-blink { background: rgba(255,179,0,0.25); color: #ffb300; animation: blink 1s infinite alternate; }
    .badge-learn { background: rgba(124,77,255,0.25); color: #b388ff; }
    @keyframes blink { from { opacity: 0.5; } to { opacity: 1; } }

    /* Alert Banners */
    .alert-banner {
      display: none;
      background: #b71c1c;
      color: #fff;
      font-weight: 800;
      font-size: 1.05rem;
      padding: 14px;
      border-radius: 12px;
      max-width: 380px;
      margin: 10px auto;
      box-shadow: 0 0 16px rgba(255,23,68,0.6);
      text-align: center;
    }

    /* Progress Bar */
    .progress-bar-bg {
      background: #2c2c2c;
      border-radius: 10px;
      height: 12px;
      overflow: hidden;
      margin: 10px 0 8px;
    }
    .progress-bar-fill {
      background: linear-gradient(90deg, #00e676, #ffb300, #ff1744);
      height: 100%;
      width: 0%;
      transition: width 0.4s ease;
    }

    /* Form Controls */
    input[type=number] {
      width: 100%;
      padding: 11px;
      border-radius: 8px;
      border: 1px solid #444;
      background: #121212;
      color: #fff;
      font-size: 1rem;
      box-sizing: border-box;
      margin-top: 5px;
    }
    input[type=number]:focus {
      outline: none;
      border-color: #2979ff;
    }
    .hint { font-size: 0.8rem; color: #b0bec5; margin-top: 4px; line-height: 1.35; }

    button {
      margin-top: 12px;
      width: 100%;
      padding: 12px;
      font-size: 0.95rem;
      font-weight: 700;
      border: none;
      border-radius: 10px;
      background: var(--btn-bg);
      color: #fff;
      cursor: pointer;
      box-sizing: border-box;
      transition: opacity 0.2s;
    }
    button:active { opacity: 0.8; }
    button.btn-sec { background: #37474f; }
    button.btn-cal { background: #4527a0; }
  </style>
</head>
<body>
  <h1>Water Filtration Monitor</h1>

  <!-- Alert Banners -->
  <div id="sedimentAlertBanner" class="alert-banner">
    ⚠️ REPLACE SEDIMENT FILTER NOW<br>
    <small style="font-size:0.85rem; font-weight:normal;">Differential pressure reached <span id="alertDiffPsi">0</span> PSI (Limit: 19 PSI)</small>
  </div>
  <div id="carbonAlertBanner" class="alert-banner">
    ⚠️ REPLACE CARBON FILTER NOW<br>
    <small style="font-size:0.85rem; font-weight:normal;">Total treated volume reached rated capacity</small>
  </div>

  <!-- Card 1: Setup & Filter Details -->
  <div class="card" style="border-left: 4px solid #ffb300;">
    <div class="row">
      <span class="card-title">Setup & Filter Details</span>
      <span id="setupBadge" class="badge badge-blink">SETUP REQUIRED</span>
    </div>
    <div class="hint" id="setupHint">Red LED blinks until calibration, capacity, and filter age are saved.</div>
    
    <div style="margin-top: 10px;">
      <label class="label"><b>1. Carbon Filter Capacity (Litres)</b></label>
      <input type="number" id="inputCapacity" placeholder="e.g. 2000" min="1" step="1">
      <div class="hint">Check rated capacity printed on your cartridge label.</div>
    </div>

    <div style="margin-top: 12px;">
      <label class="label"><b>2. Days Ago Filter Was Replaced</b></label>
      <input type="number" id="inputPastDays" placeholder="0 if brand new, e.g. 15" min="0" step="1">
      <div class="hint">If already in use, enter approximate days. The monitor will track the next 10 days to average your daily usage and retroactively add the prior water treated!</div>
    </div>

    <button onclick="submitFilterSetup()">Save Filter Configuration</button>

    <hr style="border: 0; border-top: 1px solid #2d2d2d; margin: 16px 0 12px;">
    <div class="row">
      <span class="label">Pressure Zero Status:</span>
      <span id="calibStatus" class="val-sub" style="font-size:0.9rem; font-weight:bold;">Pending</span>
    </div>
    <button class="btn-cal" onclick="reZeroPressure()">Calibrate Pressure at Atmospheric</button>
  </div>

  <!-- Card 2: 10-Day Usage Learning Status -->
  <div class="card" id="learnCard">
    <div class="row">
      <span class="card-title">10-Day Consumption Learning</span>
      <span id="learnBadge" class="badge badge-learn">DAY 0 / 10</span>
    </div>
    <div class="val-large val-learn"><span id="dailyAvg">0.0</span> <small style="font-size:1.2rem;">L/day</small></div>
    <div class="row">
      <span class="label">Prior Water Added:</span>
      <span class="val-sub"><b id="estPrior">0.0</b> L (<span id="dispPastDays">0</span> days &times; daily avg)</span>
    </div>
    <div class="row">
      <span class="label">Sensor Measured:</span>
      <span class="val-sub"><span id="measuredLitres">0.0</span> L</span>
    </div>
    <div class="hint" id="learnNote">Calculating progressive 10-day usage average to accurately estimate prior consumption.</div>
  </div>

  <!-- Card 3: Carbon Filter Cumulative Life -->
  <div class="card">
    <div class="row">
      <span class="card-title">Carbon Filter Total Life</span>
      <span id="carbonBadge" class="badge badge-ok">HEALTHY</span>
    </div>
    <div class="val-large" id="carbonPct">0.0%</div>
    <div class="progress-bar-bg">
      <div id="carbonBar" class="progress-bar-fill"></div>
    </div>
    <div class="row">
      <span class="label">Total Treated (Prior + New):</span>
      <span class="val-sub"><b id="totalTreated">0.00</b> / <span id="ratedLitres">--</span> L</span>
    </div>
    <button class="btn-sec" onclick="confirmResetCarbon()">Reset Total (New Filter Installed)</button>
  </div>

  <!-- Card 4: Sediment Filter & Differential Pressure -->
  <div class="card">
    <div class="row">
      <span class="card-title">Sediment Filter (Diff Pressure)</span>
      <span id="sedimentBadge" class="badge badge-ok">NORMAL</span>
    </div>
    <div class="val-large val-diff"><span id="pDiffPsi">0.0</span> <small style="font-size:1.2rem;">PSI (&Delta;P)</small></div>
    <div class="row">
      <span class="label">Differential (&Delta;P):</span>
      <span class="val-sub"><span id="pDiffBar">0.00</span> bar (<span id="pDiffMPa">0.000</span> MPa)</span>
    </div>
    <div class="hint">Alert threshold: <b>19.0 PSI</b> (Replace filter when reached)</div>
    <hr style="border: 0; border-top: 1px solid #2d2d2d; margin: 12px 0;">
    <div class="row">
      <span class="label">Upstream (Inlet):</span>
      <span class="val-sub"><b id="pUpPsi">0.0</b> PSI (<span id="pUpBar">0.00</span> bar)</span>
    </div>
    <div class="row">
      <span class="label">Downstream (Outlet):</span>
      <span class="val-sub"><b id="pDownPsi">0.0</b> PSI (<span id="pDownBar">0.00</span> bar)</span>
    </div>
  </div>

  <!-- Card 5: Live Flow Sensor -->
  <div class="card">
    <div class="card-title">Live Flow Sensor</div>
    <div class="val-large val-flow"><span id="flowRate">0.00</span> <small style="font-size:1.2rem;">L/min</small></div>
    <div class="row">
      <span class="label">Sensor Pulse Frequency:</span>
      <span class="val-sub"><span id="pulses">0</span> pulses/s</span>
    </div>
  </div>

  <script>
    let inputFocused = false;
    ['inputCapacity', 'inputPastDays'].forEach(id => {
      document.getElementById(id).addEventListener('focus', () => { inputFocused = true; });
      document.getElementById(id).addEventListener('blur', () => { inputFocused = false; });
    });

    // Auto-sync real phone time silently
    function syncPhoneTime() {
      let epochSec = Math.floor(Date.now() / 1000);
      fetch('/sync_time?now=' + epochSec).catch(() => {});
    }
    syncPhoneTime();

    function updateData() {
      fetch('/data')
        .then(r => r.json())
        .then(d => {
          // Flow
          document.getElementById('flowRate').innerText = d.flow.toFixed(2);
          document.getElementById('pulses').innerText = d.pulses;

          // Carbon Filter Totals
          document.getElementById('totalTreated').innerText = d.total_treated.toFixed(1);
          document.getElementById('ratedLitres').innerText = d.rated.toFixed(0);
          document.getElementById('carbonPct').innerText = d.carbon_pct.toFixed(1) + '%';
          document.getElementById('measuredLitres').innerText = d.measured_total.toFixed(1);
          document.getElementById('estPrior').innerText = d.estimated_past.toFixed(1);
          document.getElementById('dispPastDays').innerText = d.past_days;
          document.getElementById('dailyAvg').innerText = d.daily_avg.toFixed(1);

          if (!inputFocused) {
            if (!document.getElementById('inputCapacity').value) document.getElementById('inputCapacity').value = d.rated.toFixed(0);
            if (!document.getElementById('inputPastDays').value) document.getElementById('inputPastDays').value = d.past_days;
          }

          let fillPct = Math.min(d.carbon_pct, 100);
          document.getElementById('carbonBar').style.width = fillPct + '%';

          // Carbon Alert Banner
          let carbonAlert = document.getElementById('carbonAlertBanner');
          let carbonBadge = document.getElementById('carbonBadge');
          if (d.replace_carbon) {
            carbonAlert.style.display = 'block';
            carbonBadge.innerText = 'REPLACE';
            carbonBadge.className = 'badge badge-warn';
          } else {
            carbonAlert.style.display = 'none';
            carbonBadge.innerText = 'HEALTHY';
            carbonBadge.className = 'badge badge-ok';
          }

          // 10-Day Learning Card Badge & Text
          let learnBadge = document.getElementById('learnBadge');
          let learnNote = document.getElementById('learnNote');
          if (d.past_days === 0) {
            learnBadge.innerText = 'NEW FILTER (0 PRIOR DAYS)';
            learnBadge.className = 'badge badge-ok';
            learnNote.innerText = 'Brand new filter installed: Live volume directly tracks cartridge lifespan.';
          } else if (d.learn_done) {
            learnBadge.innerText = '10-DAY AVERAGE LOCKED';
            learnBadge.className = 'badge badge-ok';
            learnNote.innerText = '10-day calibration completed! Historical treated volume is permanently locked.';
          } else {
            let dayNum = Math.min(Math.floor(d.learn_days) + 1, 10);
            learnBadge.innerText = 'DAY ' + dayNum + ' OF 10 (LEARNING)';
            learnBadge.className = 'badge badge-learn';
            learnNote.innerText = 'Progressively learning average daily usage. Updates continually as days pass.';
          }

          // Sediment / Differential Pressure
          document.getElementById('pDiffPsi').innerText = d.p_diff_psi.toFixed(1);
          document.getElementById('pDiffBar').innerText = d.p_diff_bar.toFixed(2);
          document.getElementById('pDiffMPa').innerText = d.p_diff_mpa.toFixed(3);
          document.getElementById('pUpPsi').innerText = d.p_up_psi.toFixed(1);
          document.getElementById('pUpBar').innerText = d.p_up_bar.toFixed(2);
          document.getElementById('pDownPsi').innerText = d.p_down_psi.toFixed(1);
          document.getElementById('pDownBar').innerText = d.p_down_bar.toFixed(2);

          // Sediment Alert Banner (19 PSI)
          let sedimentAlert = document.getElementById('sedimentAlertBanner');
          let sedimentBadge = document.getElementById('sedimentBadge');
          if (d.replace_sediment) {
            sedimentAlert.style.display = 'block';
            document.getElementById('alertDiffPsi').innerText = d.p_diff_psi.toFixed(1);
            sedimentBadge.innerText = 'REPLACE (>=19 PSI)';
            sedimentBadge.className = 'badge badge-warn';
          } else {
            sedimentAlert.style.display = 'none';
            sedimentBadge.innerText = 'NORMAL';
            sedimentBadge.className = 'badge badge-ok';
          }

          // Setup / Calibration Status
          let setupBadge = document.getElementById('setupBadge');
          let setupHint = document.getElementById('setupHint');
          let calibStatus = document.getElementById('calibStatus');

          if (d.p_calibrated) {
            calibStatus.innerText = 'Calibrated OK';
            calibStatus.style.color = '#00e676';
          } else {
            calibStatus.innerText = 'Uncalibrated (Action Required)';
            calibStatus.style.color = '#ffb300';
          }

          if (d.p_calibrated && d.cap_set && d.days_set) {
            setupBadge.innerText = 'READY (GREEN LED)';
            setupBadge.className = 'badge badge-ok';
            setupHint.innerText = 'System configured. Green LED is ON.';
          } else {
            setupBadge.innerText = 'SETUP PENDING';
            setupBadge.className = 'badge badge-blink';
            setupHint.innerText = 'Red LED is blinking: Please calibrate pressure & save filter configuration.';
          }
        })
        .catch(() => {});
    }

    function submitFilterSetup() {
      let cap = parseFloat(document.getElementById('inputCapacity').value);
      let days = parseInt(document.getElementById('inputPastDays').value);

      if (isNaN(cap) || cap <= 0) {
        alert("Please enter a valid rated capacity in Litres (e.g., 2000).");
        return;
      }
      if (isNaN(days) || days < 0) {
        alert("Please enter a valid number of days ago the filter was replaced (0 if new).");
        return;
      }

      fetch('/set_filter_setup?capacity=' + cap + '&past_days=' + days)
        .then(r => r.text())
        .then(msg => {
          alert("Filter configuration saved!\nRated Capacity: " + cap + " L\nFilter Age: " + days + " days ago");
          updateData();
        })
        .catch(() => alert("Failed to save. Check Wi-Fi connection."));
    }

    function reZeroPressure() {
      if (confirm("Ensure both pressure sensors are at atmospheric pressure (0 gauge pressure) before calibrating.")) {
        fetch('/zero_pressure')
          .then(r => r.text())
          .then(msg => {
            alert("Pressure sensors calibrated to atmospheric baseline!");
            updateData();
          })
          .catch(() => alert("Calibration failed. Check Wi-Fi connection."));
      }
    }

    function confirmResetCarbon() {
      if (confirm("Reset carbon filter life count to 0 Litres?\nOnly do this after installing a brand new carbon filter cartridge.")) {
        fetch('/reset_carbon').then(() => updateData());
      }
    }

    setInterval(updateData, 500);
    updateData();
  </script>
</body>
</html>
)rawliteral";

// ============================================================================
// WEB SERVER ROUTE HANDLERS
// ============================================================================

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  float pUpBar    = latestPUpMPa * 10.0;
  float pDownBar  = latestPDownMPa * 10.0;
  float pDiffBar  = latestPDiffMPa * 10.0;

  float pUpPsi    = latestPUpMPa * 145.0377f;
  float pDownPsi  = latestPDownMPa * 145.0377f;

  bool anyAlert   = replaceCarbonFilter || replaceSedimentFilter;
  float learnDays = (float)learningSecondsElapsed / 86400.0f;

  String json = "{";
  json += "\"flow\":"             + String(latestFlowRate, 2) + ",";
  json += "\"pulses\":"           + String(latestPulses) + ",";
  json += "\"measured_total\":"   + String(measuredLitres, 2) + ",";
  json += "\"estimated_past\":"   + String(estimatedPriorLitres, 2) + ",";
  json += "\"total_treated\":"    + String(totalTreatedLitres, 2) + ",";
  json += "\"rated\":"            + String(carbonFilterRatedLitres, 1) + ",";
  json += "\"carbon_pct\":"       + String(carbonLifeUsedPct, 2) + ",";
  json += "\"past_days\":"        + String(carbonFilterPastDays) + ",";
  json += "\"learn_days\":"       + String(learnDays, 2) + ",";
  json += "\"daily_avg\":"        + String(currentDailyAvgLitres, 2) + ",";
  json += "\"learn_done\":"       + String(learningPhaseCompleted ? "true" : "false") + ",";
  json += "\"replace_carbon\":"   + String(replaceCarbonFilter ? "true" : "false") + ",";
  json += "\"replace_sediment\":" + String(replaceSedimentFilter ? "true" : "false") + ",";
  json += "\"any_alert\":"        + String(anyAlert ? "true" : "false") + ",";
  json += "\"p_up_mpa\":"         + String(latestPUpMPa, 3) + ",";
  json += "\"p_up_bar\":"         + String(pUpBar, 2) + ",";
  json += "\"p_up_psi\":"         + String(pUpPsi, 1) + ",";
  json += "\"p_down_mpa\":"       + String(latestPDownMPa, 3) + ",";
  json += "\"p_down_bar\":"       + String(pDownBar, 2) + ",";
  json += "\"p_down_psi\":"       + String(pDownPsi, 1) + ",";
  json += "\"p_diff_mpa\":"       + String(latestPDiffMPa, 3) + ",";
  json += "\"p_diff_bar\":"       + String(pDiffBar, 2) + ",";
  json += "\"p_diff_psi\":"       + String(latestPDiffPsi, 1) + ",";
  json += "\"p_calibrated\":"     + String(pressureCalibratedByUser ? "true" : "false") + ",";
  json += "\"cap_set\":"          + String(carbonCapacityConfigured ? "true" : "false") + ",";
  json += "\"days_set\":"         + String(carbonPastDaysConfigured ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
}

void handleSetFilterSetup() {
  bool updated = false;
  if (server.hasArg("capacity")) {
    float cap = server.arg("capacity").toFloat();
    if (cap > 0.0) {
      carbonFilterRatedLitres = cap;
      carbonCapacityConfigured = true;
      updated = true;
    }
  }
  if (server.hasArg("past_days")) {
    int days = server.arg("past_days").toInt();
    if (days >= 0) {
      // If days changed or newly set
      if (days != carbonFilterPastDays || !carbonPastDaysConfigured) {
        carbonFilterPastDays = days;
        carbonPastDaysConfigured = true;
        // If 0, mark complete immediately; if >0 and not yet done, reset learning cycle
        if (days == 0) {
          learningPhaseCompleted = true;
          learningSecondsElapsed = 0;
          learningMeasuredLitres = 0.0;
          lockedPastLitres = 0.0;
        } else if (!learningPhaseCompleted) {
          learningSecondsElapsed = 0;
          learningMeasuredLitres = 0.0;
        }
        updated = true;
      }
    }
  }

  if (updated) {
    recalculateCarbonFilterTotals();
    saveAllPreferences();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Invalid parameters");
  }
}

void handleSyncTime() {
  if (server.hasArg("now")) {
    uint32_t phoneEpoch = (uint32_t)strtoul(server.arg("now").c_str(), NULL, 10);
    if (phoneEpoch > 1700000000) { // Valid Unix epoch timestamp
      if (startEpochTimestamp == 0) {
        startEpochTimestamp = phoneEpoch;
        saveAllPreferences();
      } else if (phoneEpoch >= startEpochTimestamp) {
        // If phone was disconnected or ESP32 rebooted, adjust elapsed calendar seconds
        uint32_t realElapsed = phoneEpoch - startEpochTimestamp;
        if (realElapsed > learningSecondsElapsed && realElapsed < TEN_DAYS_IN_SECONDS) {
          learningSecondsElapsed = realElapsed;
          recalculateCarbonFilterTotals();
        } else if (realElapsed >= TEN_DAYS_IN_SECONDS && !learningPhaseCompleted && carbonFilterPastDays > 0) {
          learningSecondsElapsed = TEN_DAYS_IN_SECONDS;
          learningPhaseCompleted = true;
          float avg10Day = learningMeasuredLitres / 10.0f;
          lockedPastLitres = (float)carbonFilterPastDays * avg10Day;
          recalculateCarbonFilterTotals();
          saveAllPreferences();
        }
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleZeroPressure() {
  calibratePressureZero();
  pressureCalibratedByUser = true;
  saveAllPreferences();
  server.send(200, "text/plain", "OK");
}

void handleResetCarbon() {
  resetCarbonFilterTotal();
  server.send(200, "text/plain", "OK");
}

// ============================================================================
// ARDUINO SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- ESP32 Water Filtration Monitor Starting ---");

  // Hardware pin configuration
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BOARD, OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_BOARD, LOW);

  pinMode(PIN_RESET_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FLOW, INPUT_PULLUP);

  // ADC configuration for pressure transducers (0 - 3.3V range)
  analogSetPinAttenuation(PIN_P_UPSTREAM, ADC_11db);
  analogSetPinAttenuation(PIN_P_DOWNSTREAM, ADC_11db);

  // Flow sensor interrupt setup
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlowPulse, RISING);

  // Restore stored parameters from NVS flash memory
  loadPreferences();

  // Initial zero calibration for pressure transducers
  delay(1000); // Allow sensor power supplies to stabilize
  calibratePressureZero();

  // Start Wi-Fi Access Point (Hotspot)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Access Point started: ");
  Serial.println(AP_SSID);
  Serial.print("Connect phone and open: http://");
  Serial.println(WiFi.softAPIP());

  // Web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/set_filter_setup", HTTP_GET, handleSetFilterSetup);
  server.on("/sync_time", HTTP_GET, handleSyncTime);
  server.on("/zero_pressure", HTTP_GET, handleZeroPressure);
  server.on("/reset_carbon", HTTP_GET, handleResetCarbon);
  server.begin();
  Serial.println("HTTP Server active on port 80.");

  lastFlowTime = millis();
  lastPressureTime = millis();
  lastNvsSaveTime = millis();
  lastLedBlinkTime = millis();
  lastSecondTick = millis();
}

// ============================================================================
// ARDUINO MAIN LOOP (COMPLETELY NON-BLOCKING)
// ============================================================================
void loop() {
  unsigned long currentMillis = millis();

  // 1. Service active Wi-Fi HTTP requests
  server.handleClient();

  // 2. Check physical hardware reset button
  checkHardwareResetButton();

  // 3. Update LED state machine (Non-blocking)
  updateStatusLeds(currentMillis);

  // 4. Update 1-Second Timer for 10-day learning phase
  if (currentMillis - lastSecondTick >= 1000) {
    lastSecondTick = currentMillis;
    updateLearningTimer();
  }

  // 5. Flow Sensor Sampling & Carbon Filter Tracking (Every 1000 ms)
  if (currentMillis - lastFlowTime >= FLOW_INTERVAL_MS) {
    lastFlowTime = currentMillis;

    // Safely harvest pulses from interrupt
    portENTER_CRITICAL(&mux);
    unsigned long pulses = isrPulseCount;
    isrPulseCount = 0;
    portEXIT_CRITICAL(&mux);

    // Compute live flow rate and volume increment
    float flowRate = pulses / FLOW_CAL_FACTOR;    // L/min
    float litresAdded = pulses / PULSES_PER_LITRE; // Litres
    measuredLitres += litresAdded;

    // If still in learning phase, accumulate volume
    if (carbonFilterPastDays > 0 && !learningPhaseCompleted) {
      learningMeasuredLitres += litresAdded;
    }

    // Update retroactively calculated totals & carbon life %
    recalculateCarbonFilterTotals();

    // Update variables for web streaming
    latestPulses = pulses;
    latestFlowRate = flowRate;

    // Serial monitor debug output
    Serial.print("[Flow] Pulses/s: "); Serial.print(pulses);
    Serial.print(" | Flow: "); Serial.print(flowRate, 2); Serial.print(" L/min");
    Serial.print(" | Measured: "); Serial.print(measuredLitres, 2); Serial.print(" L");
    Serial.print(" | Prior: +"); Serial.print(estimatedPriorLitres, 1); Serial.print(" L");
    Serial.print(" | Total: "); Serial.print(totalTreatedLitres, 1); Serial.print(" / ");
    Serial.print(carbonFilterRatedLitres, 0); Serial.print(" L (");
    Serial.print(carbonLifeUsedPct, 1); Serial.print("%)");
    if (replaceCarbonFilter) {
      Serial.print(" [ALERT: REPLACE CARBON FILTER!]");
    }
    Serial.println();
  }

  // 6. Pressure Transducers & Differential Pressure Sampling (Every 500 ms)
  if (currentMillis - lastPressureTime >= PRESSURE_INTERVAL_MS) {
    lastPressureTime = currentMillis;

    // Read voltages using trimmed-mean outlier rejection
    float rawVUp = readAdcVoltage(PIN_P_UPSTREAM, UP_DIVIDER_RATIO);
    float rawVDown = readAdcVoltage(PIN_P_DOWNSTREAM, DOWN_DIVIDER_RATIO);

    // Exponential Moving Average filter (75% previous + 25% new)
    filteredVUp   = (0.75 * filteredVUp)   + (0.25 * rawVUp);
    filteredVDown = (0.75 * filteredVDown) + (0.25 * rawVDown);

    // Calculate individual pressures in MPa
    latestPUpMPa   = calculatePressureMPa(filteredVUp, vZeroUp, UP_NOISE_FLOOR_V, UP_P_MAX, UP_SPAN_V);
    latestPDownMPa = calculatePressureMPa(filteredVDown, vZeroDown, DOWN_NOISE_FLOOR_V, DOWN_P_MAX, DOWN_SPAN_V);

    // Differential pressure (Upstream - Downstream) across sediment filter
    latestPDiffMPa = latestPUpMPa - latestPDownMPa;
    latestPDiffPsi = latestPDiffMPa * 145.0377f; // Convert MPa to PSI

    // Sediment filter replacement alert check (19.0 PSI)
    replaceSedimentFilter = (latestPDiffPsi >= SEDIMENT_ALERT_PSI);

    // Serial monitor debug output
    Serial.print("[Pressure] Up: "); Serial.print(latestPUpMPa * 145.0377f, 1); Serial.print(" PSI");
    Serial.print(" | Down: "); Serial.print(latestPDownMPa * 145.0377f, 1); Serial.print(" PSI");
    Serial.print(" | Diff (ΔP): "); Serial.print(latestPDiffPsi, 1); Serial.print(" PSI");
    if (replaceSedimentFilter) {
      Serial.print(" [ALERT: REPLACE SEDIMENT FILTER!]");
    }
    Serial.println();
  }

  // 7. Non-Volatile Storage (Flash) Wear-Safe Auto-Save Check
  checkAndSaveNVS(currentMillis);
}
