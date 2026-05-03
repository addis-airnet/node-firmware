
# Air Quality Monitor Firmware
### ESP32 + SEN55 + SIM7600E — Addis Ababa deployment

This firmware runs on an ESP32 connected to a Sensirion SEN55 particulate and gas sensor and a SIM7600E GSM modem. Every minute it takes a sensor reading and appends it as a JSON line to an SD card log file. Every ten minutes it opens a GPRS data session and POSTs the ten most recent unuploaded readings to a Firebase Realtime Database endpoint.


## Hardware

- Board: ESP32 (dual-core, 240 MHz)
- Sensor: Sensirion SEN55 — PM1.0, PM2.5, PM4.0, PM10, temperature, relative humidity, VOC index, NOx index, connected over I2C on GPIO 21 (SDA) and GPIO 22 (SCL)
- Modem: SIM7600E on UART2, RX = GPIO 16, TX = GPIO 17, baud 115200
- Storage: microSD card over SPI, chip select GPIO 5
- SIM card APN: `internet`


## Dependencies

Install all four from the Arduino Library Manager before compiling.

- ArduinoJson (Benoit Blanchon)
- SensirionI2CSen5x (Sensirion)
- SD (built-in with ESP32 Arduino core)
- esp_task_wdt (built-in with ESP32 Arduino core)


## How it works

On every power-on the firmware goes through a fixed startup sequence: the watchdog is armed for five minutes, the SEN55 starts its three-minute thermal warm-up, the SIM7600E is initialised and brought up on GPRS, and NTP time is fetched from pool.ntp.org (falling back to time.google.com and time.windows.com). The ESP32 system clock is set to UTC and the timezone is locked to East Africa Time (UTC+3) for the duration of the session.

Once the warm-up period expires, the main loop runs two independent tasks on a shared schedule.

The sensor task fires every 60 seconds. It reads the SEN55, validates all eight measurement channels against hard-coded sanity bounds (PM values must be 0–1000 µg/m³, temperature –10 to 50 °C, VOC/NOx index 1–500, and so on), then serialises the reading as a single JSON line and appends it to the active hourly log file on the SD card. If the system clock has drifted backwards or jumped to an obviously wrong value the reading is discarded and a forced NTP resync is triggered after five consecutive failures.

The upload task fires every ten minutes. It reads up to ten JSON lines from the SD card and POSTs each one individually to the Firebase endpoint over HTTPS. The HTTP session (including the TLS handshake) is opened once at the start of the batch and closed when the last reading in the batch is done, so the modem does not renegotiate TLS for every individual POST.

The firmware also checks on every loop iteration whether the current EAT time is 00:00 or 12:00. If it is, a clean scheduled restart is performed via `esp_restart()`. This keeps heap fragmentation from accumulating over multi-day runs.


## Log file layout and rotation

Log files are hourly. The active file is named `/log_YYYY-MM-DD_HH.txt` and its upload pointer lives in `/ptr_YYYY-MM-DD_HH.txt`. When the hour changes both paths are rotated and the new names are written to `/current_log.txt` so the firmware can recover the correct file after a power cut without scanning the SD card directory.

Each line in a log file is a self-contained JSON object:

```json
{"pm2_5":12.4,"pm10":18.1,"humidity":61.2,"temperature":24.5,"voc_index":103.0,"nox_index":1.0,"recorded_at":"2026-05-03T12:49:41+03:00","device_id":"5afd4455-93ab-4501-b12a-19feb37c607e","serial_number":"AQ-ET-AA-ETB-2026-01"}
```


## Upload architecture and pointer design

The upload logic is split into two phases that have distinct failure properties.

Phase one is a read-only scan. Starting from the saved pointer offset, the firmware makes a single forward pass through the unread portion of the log file. A rolling circular buffer of ten `size_t` values (40 bytes) tracks the byte position at the start of each non-blank line. After the scan the buffer contains the file positions of the ten most recently written lines and `lineCount` holds the total number of unread lines. No SD writes happen during this phase, so the device can lose power at any point without leaving the card in an inconsistent state.

Phase two is the upload. If more than ten unread lines were found the firmware writes the skip-advance pointer to the SD card once — advancing it past the old lines so only the ten most recent readings will be uploaded. This single write is the only irreversible commitment before the HTTP session opens. If the device loses power after this write but before any POST completes, the next startup re-scans from the new pointer offset and finds the same ten lines, so nothing is permanently lost. The HTTP session then opens and the firmware POSTs one reading at a time. After each confirmed 200 or 201 response the pointer is immediately written to reflect the new position. If the device loses power between a successful POST and the pointer write, the worst outcome is that one reading is re-posted on the next cycle. The server is expected to deduplicate by the combination of `recorded_at` and `device_id`.

If a POST fails the batch stops immediately, the pointer stays where it is, and the same ten readings are retried on the next ten-minute cycle.

Corrupted JSON lines (which can result from a power cut mid-write) are detected by `deserializeJson`. When a parse error is found the pointer is advanced past that line and the loop continues to the next one, so a single corrupt line never blocks the rest of the batch.


## Pointer file integrity

The pointer file holds a single decimal integer — the byte offset into the log file of the first byte that has not yet been uploaded. On startup, if the pointer file is missing or zero the firmware starts from the beginning of the log file. If the stored value is negative or larger than the current file size it is treated as corrupted and reset to zero. In both fallback cases the worst outcome is that some already-uploaded readings are re-posted rather than any data being silently skipped.


## Watchdog

A hardware watchdog is armed at startup with a five-minute timeout. `esp_task_wdt_reset()` is called inside every blocking loop — the NTP wait, the AT command response waits, the scan phase, and the upload loop — so a modem hang or SD stall that lasts longer than five minutes triggers a clean hardware reset rather than leaving the device in a silent frozen state.


## Time validation

A timestamp is only accepted if it is later than 2024-01-01 00:00:00 UTC, not more than five minutes in the future relative to the system clock, and not more than ten seconds behind the previous valid timestamp. Any reading whose timestamp fails these checks is discarded. After five consecutive failures the firmware attempts an NTP resync before continuing.


## Firebase endpoint

```
https://sen55-air-quality-monitor-default-rtdb.europe-west1.firebasedatabase.app/sensor/data.json
```

Every POST carries two custom HTTP headers: `X-Device-ID` set to the device UUID and `X-API-Key` set to the Firebase API key. Both are injected via `AT+HTTPPARA="USERDATA"` before the session opens.


## Configuration constants

All deployment-specific values live at the top of the file under the CONFIG block. Change these before flashing a new unit.

| Constant | Default | Purpose |
|---|---|---|
| `DEVICE_ID` | `5afd4455-...` | UUID identifying this physical unit |
| `SERIAL_NUMBER` | `AQ-ET-AA-ETB-2026-01` | Human-readable serial number |
| `API_KEY` | `AIzaSy...` | Firebase API key |
| `APN` | `internet` | Mobile network APN |
| `SENSOR_READ_INTERVAL` | 60 000 ms | How often sensor readings are taken |
| `UPLOAD_INTERVAL` | 600 000 ms | How often the upload batch runs |
| `WARMUP_TIME` | 180 000 ms | SEN55 thermal warm-up before first reading |
| `MAX_LINES_PER_UPLOAD` | 10 | Maximum readings per upload cycle |
| `WDT_TIMEOUT_MS` | 300 000 ms | Watchdog timeout before hardware reset |

