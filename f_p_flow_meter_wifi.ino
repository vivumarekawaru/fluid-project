// YF-S201 flow sensor test on ESP32 (signal on GPIO27)
// Using the onboard built-in LED (GPIO 2)

#include <WiFi.h>
#include <WebServer.h>

// --- ESP32 Hotspot (Access Point) Credentials ---
const char* ap_ssid = "ESP32_Flow";
const char* ap_password = "password123"; // At least 8 chars

WebServer server(80);
unsigned long latestPulses = 0;
float latestFlowRate = 0.0;

const int FLOW_PIN = 32;
const int LED_PIN = 2; // Standard onboard LED pin for most ESP32 dev boards

volatile unsigned long pulseCount = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

unsigned long lastTime = 0;
float totalLitres = 0;

// Interrupt: runs on every rising edge from the sensor
void IRAM_ATTR onPulse() {
  portENTER_CRITICAL_ISR(&mux);
  pulseCount++;
  portEXIT_CRITICAL_ISR(&mux);
}

// --- Web Server UI ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Flow Sensor</title>
  <style>
    body { font-family: system-ui, sans-serif; text-align: center; background: #121212; color: #fff; margin: 0; padding: 20px; }
    .card { background: #1e1e1e; padding: 24px; border-radius: 16px; max-width: 340px; margin: 30px auto; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
    .val { font-size: 3rem; font-weight: 700; color: #00e5ff; margin: 10px 0; }
    .sub { font-size: 1.2rem; color: #b0bec5; margin-bottom: 8px; }
    button { margin-top: 16px; width: 100%; padding: 14px; font-size: 1.05rem; font-weight: 700; border: none; border-radius: 10px; background: #2979ff; color: #fff; cursor: pointer; }
    button:active { opacity: 0.8; }
  </style>
</head>
<body>
  <div class="card">
    <h2 style="margin: 6px 0;">YF-S201 Flow Meter</h2>
    <div class="val"><span id="flow">0.00</span> L/min</div>
    <div class="sub">Total: <span id="total">0.000</span> L</div>
    <div class="sub">Pulses/s: <span id="pulses">0</span></div>
    <button onclick="fetch('/reset')">Reset Total Litres</button>
  </div>
  <script>
    setInterval(() => {
      fetch('/data').then(r => r.json()).then(d => {
        document.getElementById('flow').innerText = d.flow.toFixed(2);
        document.getElementById('total').innerText = d.total.toFixed(3);
        document.getElementById('pulses').innerText = d.pulses;
      }).catch(() => {});
    }, 500);
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // Start with onboard LED off
  
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onPulse, RISING);

  Serial.println("YF-S201 test started. Blow or pour water through the sensor.");

  // --- Start ESP32 Access Point (Hotspot) ---
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("AP Started! Connect to Wi-Fi: ");
  Serial.println(ap_ssid);
  Serial.print("Open in phone browser: http://");
  Serial.println(WiFi.softAPIP()); // Defaults to 192.168.4.1

  server.on("/", []() {
    server.send(200, "text/html", INDEX_HTML);
  });

  server.on("/data", []() {
    String json = "{\"pulses\":" + String(latestPulses) +
                  ",\"flow\":" + String(latestFlowRate, 2) +
                  ",\"total\":" + String(totalLitres, 3) + "}";
    server.send(200, "application/json", json);
  });

  server.on("/reset", []() {
    totalLitres = 0;
    server.send(200, "text/plain", "OK");
  });

  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastTime >= 1000) {
    portENTER_CRITICAL(&mux);
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    portEXIT_CRITICAL(&mux);

    lastTime = millis();

    float flowRate = pulses / 7.5;          // L/min  (YF-S201: F = 7.5 * Q)
    totalLitres += pulses / 450.0;          // ~450 pulses per litre

    // Update web variables
    latestPulses = pulses;
    latestFlowRate = flowRate;

    // Control the onboard built-in LED
    if (pulses > 0) {
      digitalWrite(LED_PIN, HIGH); // Turn on onboard LED when flow is detected
    } else {
      digitalWrite(LED_PIN, LOW);  // Turn off when flow stops
    }

    Serial.print("Pulses/s: ");
    Serial.print(pulses);
    Serial.print("  |  Flow: ");
    Serial.print(flowRate, 2);
    Serial.print(" L/min  |  Total: ");
    Serial.print(totalLitres, 3);
    Serial.println(" L");
  }
}