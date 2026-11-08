// 1.2 MPa pressure transducer test on ESP32 (signal on GPIO34)
// Zero offset is measured at startup (keep sensor at atmospheric pressure)
// Built-in LED blinks when pressure > 0.010 MPa

#include <WiFi.h>
#include <WebServer.h>

// --- ESP32 Hotspot (Access Point) Credentials ---
const char* ap_ssid = "ESP32_Pressure";
const char* ap_password = "password123";

WebServer server(80);
float latestV = 0.0;
float latestMPa = 0.0;

const int SENSOR_PIN = 34;
const int LED_PIN = 2;       // Onboard built-in LED pin

const float DIVIDER_RATIO = 1.0;
const float SPAN_V = 4.0;    // Fixed span so low boot voltage doesn't amplify noise
const float P_MAX  = 1.2;    // MPa
const float NOISE_FLOOR_V = 0.040; // Ignore idle adapter ripple smaller than 40 mV

float V_ZERO = 0.5;          // measured at startup
float filteredV = 0.0;       // smoothed voltage
bool ledState = false;

// Reads 32 samples, discards the 8 lowest and 8 highest Wi-Fi spikes, and averages the middle 16
float readVoltage() {
  int samples[32];
  for (int i = 0; i < 32; i++) {
    samples[i] = analogReadMilliVolts(SENSOR_PIN);
    delay(2);
  }
  // Simple sort to isolate outliers
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
  return adcVolts / DIVIDER_RATIO;
}

// Averages over ~1 second for a rock-solid baseline
void calibrateZero() {
  float sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += readVoltage();
    server.handleClient();
  }
  V_ZERO = sum / 10.0;
  filteredV = V_ZERO;
  Serial.print("New V_ZERO locked at: ");
  Serial.print(V_ZERO, 3);
  Serial.println(" V");
}

// --- Web Server UI ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Pressure</title>
  <style>
    body { font-family: system-ui, sans-serif; text-align: center; background: #121212; color: #fff; margin: 0; padding: 20px; }
    .card { background: #1e1e1e; padding: 24px; border-radius: 16px; max-width: 340px; margin: 30px auto; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
    .val { font-size: 3rem; font-weight: 700; color: #00e676; margin: 10px 0; }
    .sub { font-size: 1.15rem; color: #b0bec5; margin-bottom: 8px; }
    button { margin-top: 14px; padding: 12px 20px; font-size: 1rem; font-weight: 600; border: none; border-radius: 8px; background: #2979ff; color: #fff; cursor: pointer; }
    button:active { opacity: 0.8; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Live Pressure</h2>
    <div class="val"><span id="mpa">0.000</span> MPa</div>
    <div class="sub"><span id="bar">0.00</span> bar</div>
    <div class="sub">Smoothed V: <span id="v">0.000</span> V</div>
    <div class="sub">Zero V: <span id="vzero">0.000</span> V</div>
    <button onclick="fetch('/zero')">Re-Zero at Atmospheric</button>
  </div>
  <script>
    setInterval(() => {
      fetch('/data').then(r => r.json()).then(d => {
        document.getElementById('mpa').innerText = d.mpa.toFixed(3);
        document.getElementById('bar').innerText = d.bar.toFixed(2);
        document.getElementById('v').innerText = d.v.toFixed(3);
        document.getElementById('vzero').innerText = d.vzero.toFixed(3);
      }).catch(() => {});
    }, 500);
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);  // full 0-3.3 V range

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("AP Started! Open -> http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", []() {
    server.send(200, "text/html", INDEX_HTML);
  });

  server.on("/data", []() {
    String json = "{\"v\":" + String(latestV, 3) +
                  ",\"vzero\":" + String(V_ZERO, 3) +
                  ",\"mpa\":" + String(latestMPa, 3) +
                  ",\"bar\":" + String(latestMPa * 10.0, 2) + "}";
    server.send(200, "application/json", json);
  });

  server.on("/zero", []() {
    calibrateZero();
    server.send(200, "text/plain", "OK");
  });

  server.begin();

  delay(1500);
  calibrateZero();
}

void loop() {
  float rawV = readVoltage();

  // Exponential Moving Average filter (75% previous + 25% new reading)
  filteredV = (0.75 * filteredV) + (0.25 * rawV);

  float deltaV = filteredV - V_ZERO;
  if (deltaV < NOISE_FLOOR_V) {
    deltaV = 0.0; // Deadband to suppress idle power-supply ripple
  }

  float pressureMPa = deltaV * P_MAX / SPAN_V;

  // Blink onboard LED if pressure exceeds 0.010 MPa
  if (pressureMPa > 0.010) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  } else {
    ledState = false;
    digitalWrite(LED_PIN, LOW);
  }

  Serial.print("Voltage: ");
  Serial.print(filteredV, 3);
  Serial.print(" V  |  Pressure: ");
  Serial.print(pressureMPa, 3);
  Serial.print(" MPa  (");
  Serial.print(pressureMPa * 10.0, 2);
  Serial.println(" bar)");

  latestV = filteredV;
  latestMPa = pressureMPa;
  unsigned long startWait = millis();
  while (millis() - startWait < 500) {
    server.handleClient();
    delay(2);
  }
}