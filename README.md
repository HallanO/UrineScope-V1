# UrineScope 
**Smart Passive Urine Diagnostics Platform**

*An IoT-Enabled Multi-Sensor Health Monitoring System*

Version 1.0 | Arduino Innovation Contest Submission | Kampala, Uganda

---

## Abstract

**UrineScope** is a fully autonomous health screening platform that performs real-time multi-parameter urinalysis at the point of urination without requiring any conscious action from the user. 

By integrating sensor hardware directly into a standard urinal drain via a passive overflow sample chamber, UrineScope transforms a routine biological act into a zero-friction health screening event. It uses a clinically-referenced multi-parameter scoring algorithm to detect hydration status, metabolic markers (ketones), and pathological indicators (ammonia/UTI, hematuria), displaying results on a TFT screen within seconds and syncing data to a cloud dashboard.

---

## Hardware Architecture

UrineScope is built around the **Arduino Mega 2560**, selected for its robust I/O capabilities, capable of driving all sensors, displays, and communication modules without external multiplexing.

### Sensor Array
| Sensor | Measurement | Clinical Target | Interface |
| :--- | :--- | :--- | :--- |
| **Gravity TDS** | Total Dissolved Solids (ppm) | Hydration status, urine concentration | Analog (A12) |
| **TCS34725** | RGBC → CIE L\*a\*b\* | Hematuria, hydration color scale (b* axis) | I2C |
| **MQ-135** | Ammonia VOC (Rs/R0) | UTI, kidney disease, liver failure | Analog (A9) |
| **MQ-3** | Ketone VOC (Rs/R0) | Diabetic ketoacidosis, ketosis | Analog (A8) |

### Components
* **Microcontroller:** Arduino Mega 2560
* **Display:** 240x280 ST7789 TFT (SPI)
* **Connectivity:** SIM800L GSM/GPRS module (NTP + WeatherAPI + Firebase)
* **Alerts:** Active Piezo Buzzer (D2)
* **Chamber:** Custom passive overflow weir gravity drain

---

## Diagnostic Engine & Scoring

The scoring engine is a four-stage weighted rule-based system that operates entirely on the Arduino. It errs toward recommending medical consultation.

1. **Time-of-Day Correction (TCF):** Normalizes TDS against circadian rhythms (e.g., concentrated morning voids).
2. **Weather Correction (WCF):** Adjusts expected concentration based on ambient temperature and humidity (Heat Index via WeatherAPI).
3. **Override Flags:** Critical findings immediately trigger high-priority alerts:
    * `HEMATURIA` (Blood detected)
    * `HIGH AMMONIA` (Possible UTI)
    * `HIGH KETONES` (DKA risk)
    * `SEVERE DEHYDRATION` or `OVERHYDRATION`
4. **Composite Score:** If no flags are triggered, a weighted score (0-100) is generated based on TDS (40%), Color b* (30%), MQ-135 (17%), and MQ-3 (13%).

---

## Event-Driven State Machine

The system follows a strict event-driven pipeline to eliminate false readings:

1. `STANDBY` - Continuous TDS polling and environmental display.
2. `SETTLING` - Triggered by sustained TDS > 1500 ppm. Wait 2.5s for turbulence to dissipate.
3. `SAMPLING` - Multi-sensor burst (15 samples at 5 Hz).
4. `DISPLAYING` - Results and score displayed on the TFT screen for 30s. Firebase POST executed.
5. `FLUSHING` - Instructs user to flush. **System holds here until TDS drops < 1500 ppm** and 20s elapse.
6. `READY` - 10-second green screen reset period. Returns to STANDBY.

---

##  Cloud Integration & Dashboard

All sessions are fully anonymized to protect user privacy.

* **Firebase Realtime Database:** The SIM800L module POSTs JSON telemetry directly to Firebase via HTTPS. 
* **GitHub Pages Dashboard:** A live web dashboard connects to Firebase to show real-time unit status, current environmental context, recent session composite scores, and a historical reading log for population health analytics.

---

## Setup & Installation

### Dependencies
Ensure the following libraries are installed in your Arduino IDE:
* `Adafruit GFX Library`
* `Adafruit ST7789 Library`
* `Adafruit TCS34725`
* `SoftwareSerial` (Built-in)

### Configuration
Update the following credentials in `UrineScope_Final_fixed.ino`:
```cpp
#define FB_HOST   "your-project.firebaseio.com"
#define FB_SECRET "your-database-secret"
const char* WX_KEY  = "your-weatherapi-key";
const char* WX_CITY = "Kampala";
```

### Deployment
1. Assemble the passive overflow chamber and position the TDS sensor just below the overflow line.
2. Ensure MQ sensors are mounted in the sealed headspace above the liquid level.
3. Flash the Arduino Mega 2560.
4. Allow 20s for the MQ sensors to warm up and establish the clean-air baseline (`R0`).

---

##  Roadmap (v1.1)

* **pH Sensor Integration:** To identify acidosis and alkalosis (UTI risk).
* **Turbidity Sensor (SEN0189):** To measure light scatter (pyuria/bacteriuria).
* **Featherless AI Integration:** Supabase Edge functions linking Firebase to LLMs (Mistral/Llama 3) for secondary analysis notes.
* **Solenoid-Controlled Cleaning:** Active flushing cycle for high-volume automated deployments.
