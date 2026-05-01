# Air Quality Monitor Firmware

**Platform:** ESP32 | **Sensor:** Sensirion SEN55 | **Communication:** SIM7600E GSM (Primary) + WiFi (Backup) | **Storage:** SD Card

Firmware for a distributed network of 20 air quality monitoring devices deployed across Addis Ababa, Ethiopia. Each device continuously reads particulate matter, gas, and environmental data and uploads it to a cloud API every 30 seconds over cellular (GSM) with WiFi as a fallback.

---

## Table of Contents

- [Hardware Requirements](#hardware-requirements)
- [Dependencies](#dependencies)
- [Configuration](#configuration)
- [System Architecture](#system-architecture)
  - [State Machine](#state-machine)
  - [GSM Integration](#gsm-integration)
  - [WiFi Integration](#wifi-integration)
  - [Upload Logic & Priority](#upload-logic--priority)
  - [SD Card Logging](#sd-card-logging)
  - [Time Synchronization](#time-synchronization)
  - [Sensor Pipeline](#sensor-pipeline)
  - [Reliability & Watchdog](#reliability--watchdog)
- [Function Reference](#function-reference)
- [Serial Debug Output](#serial-debug-output)
- [Deployment Notes](#deployment-notes)

---

## Hardware Requirements

| Component | Details |
|---|---|
| Microcontroller | ESP32 (dual-core, with UART2 available) |
| Air Quality Sensor | Sensirion SEN55 (I2C) |
| GSM Module | SIMCom SIM7600E |
| Storage | MicroSD card (FAT32 formatted) |
| SDA / SCL | GPIO 21 / GPIO 22 (I2C for SEN55) |
| GSM RX / TX | GPIO 16 / GPIO 17 (UART2) |
| SD CS Pin | GPIO 5 |

### Wiring Summary

```
ESP32 GPIO 21  →  SEN55 SDA
ESP32 GPIO 22  →  SEN55 SCL
ESP32 GPIO 16  →  SIM7600E TX
ESP32 GPIO 17  →  SIM7600E RX
ESP32 GPIO 5   →  SD Card CS
3.3V / GND     →  All peripherals (check SIM7600E — may require 5V supply)
```

---

## Dependencies

Install the following libraries via the Arduino Library Manager or PlatformIO:

| Library | Purpose |
|---|---|
| `SensirionI2CSen5x` | SEN55 sensor driver |
| `ArduinoJson` | JSON serialization for API payload |
| `WiFi` | ESP32 built-in WiFi stack |
| `HTTPClient` | HTTP/HTTPS client over WiFi |
| `WiFiClientSecure` | TLS/SSL support for HTTPS over WiFi |
| `SD` | SD card read/write |
| `Wire` | I2C communication |
| `esp_task_wdt` | ESP32 hardware task watchdog |
| `time.h` | POSIX time functions (NTP/RTC) |

---

## Configuration

All configuration is defined via `#define` macros at the top of the firmware file.

### WiFi
```cpp
#define WIFI_SSID     "your_ssid"
#define WIFI_PASSWORD "your_password"
```

### GSM
```cpp
#define GSM_RX_PIN    16
#define GSM_TX_PIN    17
#define GSM_APN       "internet"   // Use "ethionet" for Ethio Telecom
```

### Device ID
```cpp
#define DEVICE_ID     "5afd4455-93ab-4501-b12a-19feb37c607e"
#define API_KEY       "1NkPL7xXwa-JlPqPS_FmQDHH_qp3f-DOMpRhGX4kC04"
#define SERIAL_NUMBER "AQ-ET-AA-ETB-2026-01"
```
### API Endpoint
```cpp
#define API_URL "https://air-q-9f333037f389.herokuapp.com/api/v1/sensor-readings/device"
```

### Timing
```cpp
#define REALTIME_UPLOAD_INTERVAL  30000UL   // 30 seconds between uploads
#define WARMUP_TIME               180000UL  // 3-minute SEN55 warm-up
#define WIFI_CHECK_INTERVAL       10000UL   // WiFi reconnect check interval
#define WDT_TIMEOUT_MS            300000UL  // 5-minute watchdog timeout
```

### Time Validation
```cpp
#define MIN_VALID_EPOCH           1704067200UL  // 2024-01-01 — reject timestamps older than this
#define TIME_FUTURE_MARGIN        300            // Max allowed forward jump in seconds
#define TIME_MONOTONIC_MARGIN     10             // Max allowed backward drift in seconds
```

### GSM Tuning
```cpp
#define GSM_INIT_TIMEOUT_MS       60000UL   // Max wait for network registration
#define GSM_HTTP_TIMEOUT_MS       15000UL   // Max wait for +HTTPACTION server response
#define GSM_MAX_INIT_RETRIES      3         // GSM init attempts before WiFi fallback
```

---

## System Architecture

### State Machine

The firmware runs a non-blocking state machine in `loop()`. This avoids blocking `delay()` calls in the main flow and ensures the watchdog is always fed and the sensor warm-up runs in parallel with network initialization.

```

   STATE_INITIALIZING    (hardware init, sensor start, SD check)
         ↓
    STATE_GSM_INIT       GSM setup: AT commands, SIM, network, PDP, HTTP session
         │  GSM init fails GSM_MAX_INIT_RETRIES (3) times
         ↓
  STATE_CONNECTING_WIFI    WiFi fallback: WiFi.begin(), 30s timeout
         ↓
   STATE_SYNCING_TIME     NTP sync via WiFi, 60s window, up to 10 cycles
         ↓
  STATE_OPERATIONAL      Main loop: 30s upload cycle, SD logging

```

**Key design decision:** `STATE_CONNECTING_WIFI` is only entered if GSM fails all retries. WiFi is never started unless GSM is unavailable. This conserves power and eliminates unnecessary radio contention.

Each state transition is driven by time-based checks using `millis()` — no blocking calls exist in the state machine transitions.

---

### GSM Integration

The SIM7600E communicates with the ESP32 over **UART2** (`HardwareSerial sim7600(2)`) at 115200 baud using AT commands. The integration is modeled directly from a validated test implementation that successfully uploaded data to a remote HTTPS endpoint.

#### GSM Initialization — `gsm_init()`

Called during `STATE_GSM_INIT`. Full sequence:

```
AT                           Confirm module is responsive
ATE0                         Disable echo
AT+CPIN?                     Confirm SIM card is present and unlocked
AT+CREG?                     Poll until network registration confirmed (0,1 or 0,5)
AT+CGATT?                    Confirm GPRS attachment
AT+CGDCONT=1,"IP","<APN>"    Set APN for PDP context
AT+CGACT=1,1                 Activate PDP context (mobile data)
AT+HTTPTERM                  Terminate any stale HTTP session
AT+HTTPINIT                  Initialize new HTTP session
AT+HTTPPARA="URL","<API>"    Set target endpoint
AT+HTTPPARA="CONTENT","application/json"
AT+HTTPPARA="USERDATA","X-Device-ID: ...\r\nX-API-Key: ..."   Custom headers
```

Network registration is polled in a loop with a configurable timeout (`GSM_INIT_TIMEOUT_MS = 60s`). The watchdog is fed inside the polling loop to prevent a false reset during slow network attachment.

#### GSM HTTP Session — `gsm_init_http()` / `gsm_term_http()`

The HTTP session is persistent across upload cycles to avoid the overhead of re-running `AT+HTTPINIT` every 30 seconds. A boolean flag `gsmHttpInitialized` tracks session state. If an upload fails mid-transmission, the flag is cleared and the session is cleanly re-initialized on the next cycle via `gsm_term_http()` → `gsm_init_http()`.

#### GSM POST — `postReadingGSM()`

Called every 30 seconds during `STATE_OPERATIONAL`.

```
AT+HTTPDATA=<length>,10000  → Tell modem payload size; 10s window to send data
 wait for "DOWNLOAD" prompt
 send raw JSON payload
 wait for "OK"
AT+HTTPACTION=1             → Execute HTTP POST
 wait for "+HTTPACTION:" response, timeout = GSM_HTTP_TIMEOUT_MS
 parse response for HTTP 200 or 201 → successs
```

The SIM7600E handles TLS/SSL natively — no additional SSL configuration commands are required. HTTPS URLs are supported transparently through `AT+HTTPINIT`.

> **Note:** Some SIM7600E firmware versions may require `AT+HTTPSSL=1` before `AT+HTTPINIT`. If HTTPS POSTs fail, add this command in `gsm_init_http()`.

#### GSM AT Command Helpers

| Function | Purpose |
|---|---|
| `gsm_send_at(cmd, delayMs)` | Fire an AT command, drain response to Serial after `delayMs` |
| `gsm_wait_response(expected, timeoutMs)` | Block until expected substring appears in modem output or timeout |

`gsm_wait_response()` feeds the watchdog inside its polling loop, so long waits (e.g., server response) do not trigger a false watchdog reset.

---

### WiFi Integration

WiFi is used **only as a backup**. It is not initialized at boot unless GSM initialization fails all `GSM_MAX_INIT_RETRIES` attempts.

#### WiFi Startup (fallback path)
```cpp
WiFi.mode(WIFI_STA);
WiFi.setSleep(false);
WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
currentState = STATE_CONNECTING_WIFI;
```
A 30-second connection timeout is enforced. If WiFi does not connect within 30 seconds, the firmware transitions directly to `STATE_OPERATIONAL` and runs without any internet connectivity (SD logging continues).

#### WiFi Maintenance — `maintainWiFi()`
Called every `WIFI_CHECK_INTERVAL` (10s) during `STATE_OPERATIONAL` when GSM is not available. If `WiFi.status() != WL_CONNECTED`, it calls `WiFi.disconnect()` followed by `WiFi.begin()` to force a reconnect attempt.

#### NTP on WiFi Reconnect — `feedNtpOnWifiConnectEdge()`
Called every loop iteration. Detects the rising edge of WiFi connection (transition from disconnected → connected) and immediately triggers `configTime()` to resync NTP. This ensures time is always re-validated after a WiFi dropout and reconnect.

#### WiFi POST — `postReadingWiFi()`
Uses `WiFiClientSecure` with `setInsecure()` (skips certificate verification for compatibility) and `HTTPClient`. Sends the same JSON payload with the same API headers (`X-Device-ID`, `X-API-Key`) as the GSM path. Returns `true` on HTTP 201.

---

### Upload Logic & Priority

Upload is attempted inside `processRealtimeSensorFlow()`, triggered every `REALTIME_UPLOAD_INTERVAL` (30s).

```
          gsmReady == true?             
                                       
  YES → postReadingGSM()               
         ↓ success?                    
         YES → uploaded = true         
         NO  → gsmHttpInitialized=false
                (session reset for      
                 next cycle)            
         ↓ if !uploaded
      WiFi.status() == WL_CONNECTED?    
  YES → postReadingWiFi()              
          ↓ success?                   
          YES → uploaded = true         
          NO  → log HTTP code          
         ↓ regardless of uploaded
   Append JSON to SD card log file     
    (always — upload result irrelevant) 
```

**SD write is unconditional.** Data is never discarded due to a network failure.

---

### SD Card Logging

The SD card provides a persistent local backup of every sensor reading.

#### Log Rotation — `updateLogFileFromRTC()`

Log files are rotated on a **2-hour boundary**. A new file is created every 2 hours based on the current date and hour block from the RTC.

File naming convention:
```
/log_YYYY-MM-DD_HH-HH.txt
```
Example:
```
/log_2026-05-01_10-12.txt   ← readings from 10:00 to 11:59
/log_2026-05-01_12-14.txt   ← readings from 12:00 to 13:59
```

A corresponding pointer file is also created:
```
/ptr_2026-05-01_10-12.txt
```

Rotation state is tracked using static variables (`lastRotYear`, `lastRotMon`, `lastRotMday`, `lastRotPeriod`). Rotation only triggers when the period actually changes — not on every call — making it safe to call `updateLogFileFromRTC()` frequently.

A metadata file `/current_log.txt` is written on every rotation with the current active log and pointer file paths. This allows the firmware to recover the correct log file path after a reboot without scanning the SD card.

#### Log Format

Each line is a single JSON object:
```json
{"pm2_5":24.7,"pm10":25.1,"humidity":42.74,"temperature":22.985,"voc_index":106,"nox_index":1,"recorded_at":"2026-05-01T14:32:00+03:00","device_id":"5afd4455...","serial_number":"AQ-ET-AA-ETB-2026-01"}
```

---

### Time Synchronization

Accurate timestamps are critical for data integrity. The firmware uses a layered approach:

#### Time Sources (in priority order)

| Priority | Source | Method |
|---|---|---|
| 1 | GSM network clock | `AT+CCLK?` → `gsm_get_time_via_at()` |
| 2 | NTP over GSM data | `configTime()` while GSM data is active |
| 3 | NTP over WiFi | `configTime()` during `STATE_SYNCING_TIME` |
| 4 | No time | Timestamps rejected; SD logging continues without timestamp |

#### `gsm_get_time_via_at()`
Sends `AT+CCLK?` and parses the response format `"YY/MM/DD,HH:MM:SS±ZZ"`. Sets the ESP32 system time via `settimeofday()`. Timezone is forced to EAT (UTC+3) via `setenv("TZ", "EAT-3", 1)`.

#### Timestamp Validation — `sensor_get_validated_timestamp()`
Every timestamp is validated before use:

| Check | Condition | Action on fail |
|---|---|---|
| Minimum epoch | `now >= 2024-01-01` | Reject — RTC not yet synced |
| Forward jump | `now <= lastValid + 300s` | Reject — suspicious large jump |
| Backward drift | `now >= lastValid - 10s` | Reject — clock rolled back |

Output format: `"YYYY-MM-DDTHH:MM:SS+03:00"` (ISO 8601, EAT offset hardcoded).

---

### Sensor Pipeline

#### Warm-Up
The SEN55 requires a 3-minute warm-up after power-on for stable readings. The `sensorReady` flag is set to `true` once `millis() - warmupStartTime >= WARMUP_TIME`. The state machine and upload cycle run during warm-up but readings are only taken after `sensorReady == true`.

#### Read Flow — `sensor_read()`

```
waitForDataReadyAndRead()
    ↓
  sen5x.readDataReady()   ← polled every 100ms, 5s timeout
    ↓
  sen5x.readMeasuredValues()
    ↓
validateSensorData()
    ↓
  NaN check on all 8 values
  PM range: 0–1000 µg/m³
  Humidity: 0–100 %
  Temperature: -10–50 °C
  VOC Index: 1–500
  NOx Index: 1–500
```

#### Sensor Error Recovery — `recoverSEN55()`
If `readDataReady()` or `readMeasuredValues()` returns an error, or if the device status register reports a hardware fault (fan speed, laser, fan failure, gas sensor error, RHT error), the firmware attempts:
1. `sen5x.deviceReset()`
2. `sen5x.startMeasurement()`

Up to 3 recovery attempts are made before the reading cycle is skipped entirely.

---

### Reliability & Watchdog

#### Hardware Watchdog Timer — `system_init_watchdog()` / `system_feed_watchdog()`

Uses the ESP32 Task Watchdog Timer (TWDT) configured at **5 minutes** (`WDT_TIMEOUT_MS = 300000`). Both CPU cores are monitored (`idle_core_mask = core 0 | core 1`). A panic reset is triggered on timeout (`trigger_panic = true`).

`system_feed_watchdog()` (`esp_task_wdt_reset()`) is called:
- At the top of every `loop()` iteration
- Inside long polling loops (GSM network registration wait, `waitForDataReadyAndRead()`, `gsm_wait_response()`, NTP retry loops)

This prevents false watchdog resets during intentionally long operations while still catching genuine firmware hangs.

#### Scheduled Restart — `system_check_scheduled_restart()`

The device performs a clean `esp_restart()` twice per day at **00:00** and **12:00 EAT**. A `resetTriggeredThisMinute` flag prevents multiple restarts within the same minute. The flag resets when `tm_min != 0`.

This is a proactive measure to clear heap fragmentation and reset any slow-accumulating state after long unattended operation.

#### SD Card Boot Failure Guard
If `SD.begin()` fails at boot, the firmware logs the failure and calls `esp_restart()` after a 5-second delay. This handles SD card ejection or contact failure on boot.

---

## Function Reference

### Core Flow

| Function | Description |
|---|---|
| `setup()` | Hardware init, GSM UART start, SD init, state machine entry |
| `loop()` | State machine dispatcher, sensor warmup check, upload trigger |
| `processRealtimeSensorFlow()` | Full 30s cycle: read → timestamp → upload (GSM/WiFi) → SD log |

### GSM

| Function | Description |
|---|---|
| `gsm_init()` | Full GSM module initialization (AT handshake → network → PDP → HTTP) |
| `gsm_init_http()` | Open HTTP session, set URL and headers |
| `gsm_term_http()` | Tear down HTTP session |
| `postReadingGSM(doc)` | Serialize JSON and POST via GSM AT commands |
| `gsm_get_time_via_at(tm)` | Parse `AT+CCLK?` response and set system time |
| `gsm_send_at(cmd, delay)` | Send AT command, drain response after delay |
| `gsm_wait_response(expected, timeout)` | Wait for substring in modem output |

### WiFi

| Function | Description |
|---|---|
| `postReadingWiFi(doc, httpCode)` | POST JSON via WiFiClientSecure + HTTPClient |
| `maintainWiFi()` | Reconnect WiFi if disconnected (called every 10s) |
| `feedNtpOnWifiConnectEdge()` | Trigger NTP resync on WiFi connect edge |

### Sensor

| Function | Description |
|---|---|
| `sensor_read(reading)` | Read and validate SEN55 measurement |
| `sensor_get_validated_timestamp(epoch, buf, len)` | Get and validate current timestamp |
| `sensor_attempt_ntp_resync()` | Force NTP resync attempt (10 retries × 500ms) |
| `recoverSEN55()` | Attempt sensor reset on hardware fault |
| `waitForDataReadyAndRead(reading)` | Poll data-ready flag then read values |
| `validateSensorData(reading)` | Range and NaN checks on all sensor values |

### System

| Function | Description |
|---|---|
| `updateLogFileFromRTC()` | Rotate SD log file on 2-hour boundary |
| `system_check_scheduled_restart()` | Trigger `esp_restart()` at 00:00 and 12:00 EAT |
| `system_init_watchdog()` | Configure and start 5-minute TWDT |
| `system_feed_watchdog()` | Reset watchdog timer ("I'm alive" signal) |
| `applyConfigTime()` | Call `configTime()` with all three NTP servers |

---

## Serial Debug Output

Connect at **115200 baud** to monitor device state. Key prefixes:

| Prefix | Source |
|---|---|
| `[GSM >>]` | AT command sent to SIM7600E |
| `[GSM]` | GSM state/result messages |
| `[WiFi]` | WiFi connection events |
| `[NTP]` | Time sync events |
| `[STATE]` | State machine transitions |
| `[UPLOAD]` | Upload attempt results |
| `Rotated to new...` | SD log file rotation |
| `Backup logged to SD` | Successful SD write |
| `Watchdog enabled...` | WDT initialization confirmation |

---

## Deployment Notes

1. **Per-device configuration:** Update `DEVICE_ID` and `SERIAL_NUMBER` uniquely for each of the 20 units before flashing.

2. **APN setting:** Confirm with the SIM card provider whether `"internet"` or `"ethionet"` is the correct APN for the SIM cards installed.

3. **SIM7600E power supply:** The SIM7600E module may require up to 2A peak current during network registration. Ensure the power supply can handle this. Insufficient power is the most common cause of GSM init failure in the field.

4. **HTTPS compatibility:** If HTTPS POSTs fail during field testing, add `gsm_send_at("AT+HTTPSSL=1", 500)` before the `AT+HTTPINIT` call in `gsm_init_http()`. Some SIM7600E firmware versions require explicit SSL enable.

5. **SD card format:** Must be FAT32. Cards larger than 32GB may require manual FAT32 formatting as most OS formatters default to exFAT above that size.

6. **Scheduled restarts:** Devices reboot at 00:00 and 12:00 EAT daily. This is intentional. Ensure deployment timestamps account for the ~5 second reboot window.

7. **Sensor warm-up:** The SEN55 requires 3 minutes of warm-up after every power cycle before readings are taken. Readings during this period are intentionally suppressed by the firmware.
