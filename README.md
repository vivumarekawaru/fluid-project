# 💧 ESP32 Smart Water Filtration Monitor

An intelligent, non-blocking ESP32-based multi-stage water filtration monitor. It tracks real-time flow rate, cumulative carbon filter lifespan, and differential pressure ($\Delta P$) across sediment filter stages, streaming live data to any mobile browser over a local Wi-Fi Access Point.

---

## 🚀 Key Features

- **🌊 Real-Time Flow Metering (YF-S201)**:
  - Hardware interrupt pulse capture (`portENTER_CRITICAL_ISR`).
  - Measures instantaneous flow rate (L/min) and total volume.
  - Onboard built-in LED activates during flow detection.

- **🛡️ Carbon Filter Life Tracking & Retroactive 10-Day Averaging**:
  - Configurable rated capacity (e.g., 2,000 L).
  - **Retroactive Estimation**: If installed on an existing system, input how many days ago the cartridge was installed (`0` if brand new). The system tracks actual water usage over the next 10 days, calculates your progressive daily average consumption, and retroactively accounts for past treated water.
  - Automatically locks the 10-day baseline permanently into flash memory after 10 days.
  - **NVS Power-Loss Survival**: Uses native ESP32 `Preferences.h` with wear-leveling logic (saves periodically or on flow completion to safeguard flash lifespan).
  - Web alert: `⚠️ REPLACE CARBON FILTER NOW` when total volume reaches rated capacity.

- **⚖️ Dual-Sensor Differential Pressure ($\Delta P$) for Sediment Filter**:
  - Measures upstream (inlet) and downstream (outlet) pressure transducers independently.
  - 32-sample sorting trimmed-mean filter (discards highest/lowest spikes to suppress Wi-Fi ADC noise) + Exponential Moving Average (EMA).
  - Continuous differential calculation: $\Delta P = P_{\text{upstream}} - P_{\text{downstream}}$.
  - **Sediment Clog Alert**: Triggers a web warning when $\Delta P \ge 19.0\text{ PSI}$ ($1.31\text{ bar}$).

- **💡 Hardware Status LEDs**:
  - 🟢 **Green LED (`GPIO 19`)**: Solid ON when calibrated, configured, and operating normally.
  - 🔴 **Red LED (`GPIO 18`)**:
    - **Blinks (1 Hz)** on startup until atmospheric pressure is zeroed and filter parameters are saved.
    - **Solid ON** whenever an alert is active ($\Delta P \ge 19\text{ PSI}$ or carbon volume reached).
    - **OFF** during normal operation.

- **📱 Standalone Wi-Fi Access Point & Mobile Dashboard**:
  - ESP32 broadcasts its own Wi-Fi hotspot (`ESP32_Filter_Monitor`).
  - Zero external apps or internet required—open `http://192.168.4.1` on your phone browser.
  - Responsive dark-theme dashboard with real-time 500 ms JSON streaming.
  - Browser automatically syncs calendar time in the background (`/sync_time`).

---

## 📌 Hardware Pinout

| ESP32 Pin | Function | Device / Connection | Notes |
| :--- | :--- | :--- | :--- |
| **GPIO 32** | Digital Input | Flow Sensor Signal (YF-S201) | Internal pull-up, hardware interrupt |
| **GPIO 34** | Analog Input (ADC1_CH6) | Upstream Pressure Transducer | Wi-Fi safe ADC1 pin (0 – 3.3V) |
| **GPIO 35** | Analog Input (ADC1_CH7) | Downstream Pressure Transducer | Wi-Fi safe ADC1 pin (0 – 3.3V) |
| **GPIO 19** | Digital Output | **Green Status LED** | Normal operational indicator |
| **GPIO 18** | Digital Output | **Red Status LED** | Alert indicator / setup blinker |
| **GPIO 2**  | Digital Output | Onboard Built-in LED | Illuminates on active water flow |
| **GPIO 0**  | Digital Input | Onboard BOOT Button | Long-press (2s) resets carbon volume |

> **⚠️ Important Note on ESP32 ADCs**: Pressure sensors must be wired to **ADC1** pins (GPIO 32–39). **ADC2** pins cannot be used while the ESP32 Wi-Fi radio is active.

---

## 🗂️ Project Directory Structure

```text
├── filter_monitor/
│   └── filter_monitor.ino         # Primary sketch for Arduino IDE
├── filter_monitor.ino             # Mirrored root sketch
├── f_p_flow_meter_wifi/
│   └── f_p_flow_meter_wifi.ino    # Standalone flow sensor test sketch
├── f_P_Pressure_transducer_wifi/
│   └── f_P_Pressure_transducer_wifi.ino # Standalone pressure sensor test sketch
└── README.md                      # Project documentation
```

---

## 🛠️ Software Requirements

- **Arduino IDE** (v1.8.x or v2.x)
- **ESP32 Arduino Core** (Installed via Boards Manager)
- **Required Libraries** (All built into the ESP32 core—no third-party downloads required):
  - `<WiFi.h>`
  - `<WebServer.h>`
  - `<Preferences.h>`

---

## ⚡ Quick Start Guide

### 1. Uploading the Code
1. Open `filter_monitor/filter_monitor.ino` in the Arduino IDE.
2. Select your board: **Tools > Board > ESP32 Arduino > ESP32 Dev Module** (or **NodeMCU-32S**).
3. Connect your ESP32 via USB and select the corresponding COM port.
4. Click **Upload**.

### 2. Initial Calibration & Setup
1. On boot, the **Red LED (`GPIO 18`) will blink**. This indicates the monitor is waiting for initial calibration and configuration.
2. Connect your phone or laptop to the ESP32's Wi-Fi network:
   - **SSID**: `ESP32_Filter_Monitor`
   - **Password**: `password123`
3. Open any web browser and navigate to:
   ```text
   http://192.168.4.1
   ```
4. **Step A — Configure Filter**:
   - In the **Setup & Filter Details** card, enter:
     - **Rated Capacity (L)**: Found on your carbon cartridge label (e.g. `2000`).
     - **Days Ago Replaced**: Enter `0` if you just put in a new cartridge, or the number of days it has already been running (e.g., `15`).
   - Click **Save Filter Configuration**.
5. **Step B — Zero-Calibrate Pressure**:
   - Ensure the filtration system is at atmospheric pressure (zero gauge pressure).
   - Click **Calibrate Pressure at Atmospheric**.
6. **Result**:
   - The **Red LED will stop blinking**, and the **Green LED (`GPIO 19`) will turn Solid ON**!

---

## 📊 Live Dashboard & Rest API

The ESP32 serves a single-page responsive dashboard and provides a lightweight REST JSON API polled every 500 ms:

### Endpoint: `GET /data`
Returns a JSON payload with all live parameters:
```json
{
  "flow": 1.45,
  "pulses": 11,
  "measured_total": 45.20,
  "estimated_past": 678.00,
  "total_treated": 723.20,
  "rated": 2000.0,
  "carbon_pct": 36.16,
  "past_days": 15,
  "learn_days": 1.50,
  "daily_avg": 45.20,
  "learn_done": false,
  "replace_carbon": false,
  "replace_sediment": false,
  "any_alert": false,
  "p_up_mpa": 0.350,
  "p_up_bar": 3.50,
  "p_up_psi": 50.8,
  "p_down_mpa": 0.315,
  "p_down_bar": 3.15,
  "p_down_psi": 45.7,
  "p_diff_mpa": 0.035,
  "p_diff_bar": 0.35,
  "p_diff_psi": 5.1,
  "p_calibrated": true,
  "cap_set": true,
  "days_set": true
}
```

### Additional Endpoints:
- `GET /set_filter_setup?capacity=2000&past_days=15`: Updates rated capacity and cartridge age.
- `GET /zero_pressure`: Re-zeros pressure transducers at atmospheric pressure.
- `GET /reset_carbon`: Resets cumulative treated volume back to 0.0 L (used upon cartridge replacement).
- `GET /sync_time?now=<epoch>`: Silently syncs Unix epoch time from phone browser.

---

## 🔄 Filter Replacement Procedure

When either filter reaches its replacement threshold:
1. **Sediment Filter Replacement ($\Delta P \ge 19\text{ PSI}$)**:
   - Shut off water supply and depressurize.
   - Replace sediment cartridge.
   - Restore pressure—the differential pressure drops back below 19 PSI, the alert clears, and the Green LED turns back ON.
2. **Carbon Filter Replacement (Capacity Reached)**:
   - Replace the carbon cartridge.
   - Tap **Reset Total (New Filter Installed)** on the web interface or hold the ESP32 **BOOT button (`GPIO 0`) for 2 seconds**.
   - Treated volume resets to `0.0 L`, past days reset to `0`, and the counter restarts cleanly.

---

## 📌 Next Steps & Future Roadmap

1. **🧪 TDS Feature for RO Membrane**:
   - Integrate dual Total Dissolved Solids (TDS) sensors (feed water vs. RO permeate).
   - Compute real-time membrane rejection rate ($\% \text{ Rejection} = \frac{\text{Feed TDS} - \text{Product TDS}}{\text{Feed TDS}} \times 100$).
   - Trigger alert when membrane rejection drops below acceptable threshold (e.g., $< 90\%$).

2. **📦 Custom Water-Resistant Casing**:
   - Design and 3D-print a custom, splash-proof enclosure (IP65 rated or similar) to house the ESP32, wiring terminals, and status LEDs securely away from pipe condensation and water drips.

3. **⚠️ Food-Grade Flow Sensor Upgrade & Calibration**:
   - **Confirmation on YF-S201**: The standard black **YF-S201 is an industrial-grade sensor and is NOT certified food-grade** (it lacks NSF/ANSI 61 or FDA food-contact compliance, and the standard POM plastic/O-rings are not rated for long-term potable drinking water).
   - **Action**: Replace with a certified food-grade flow sensor (such as a 1/4" push-fit food-grade POM/PP Hall-effect flow sensor).
   - **Post-Replacement Testing**: Because different sensors have different rotor geometries and pulse frequencies ($K$-factor), perform a volume calibration test (e.g., using a 1-litre graduated container) after replacement and update `FLOW_CAL_FACTOR` and `PULSES_PER_LITRE` in the configuration section.
