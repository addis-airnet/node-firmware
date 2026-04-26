
```markdown
# SIM7600 GSM Module Firebase Communication Test (Version 1)

## 1. Introduction
This code represents Version 1 (V1) of the communication system for our project. Its primary purpose is to test and validate the functionality of the SIM7600 GSM module by performing an HTTPS POST request to a Firebase Realtime Database.

At this stage, the system is not yet integrated with sensors or backend logic. Instead, it focuses on verifying three critical capabilities:
* GSM module responsiveness (AT command communication)
* Cellular network connectivity (GPRS/LTE data session)
* Secure HTTP communication with a cloud service (Firebase)

This version acts as a baseline validation step before integrating the GSM module into the full system 
## 2. Objectives
The main objectives of this test code are:
* Ensure the SIM7600 module responds correctly to AT commands
* Establish a mobile data connection using an APN
* Send a JSON payload to Firebase via HTTP POST
* Receive and display server responses for debugging
* Validate readiness for integration into the main system

## 3. Communication Flow
1. ESP32 sends AT commands via UART
2. SIM7600 executes commands (network + HTTP stack)
3. Data is sent over cellular network
4. Firebase receives and stores the JSON payload

## 4. Code Breakdown

### Hardware Serial Initialization
```cpp
HardwareSerial sim7600(2);
```
* Creates a second UART interface (UART2) on ESP32
* Allows independent communication with the GSM module

### Network and Server Configuration
```cpp
const char* APN = "internet";
```
* APN (Access Point Name) is required for mobile data connection. Must match the SIM card provider’s configuration.

```cpp
const char* FIREBASE_URL = ".../sensor/data.json";
```
* Firebase REST endpoint. `.json` is required for Firebase Realtime Database API.

### JSON Payload
```cpp
String jsonData = "{\"Date\":\"test\", ... }";
```
* Defines test sensor data in JSON format.
* Contains: Date, PM2.5 and PM10, Humidity, Temperature, VOC index, NOx index.
* Currently static (hardcoded), but will later be replaced with real sensor values.

### AT Command Function
```cpp
void sendAT(String cmd, int delayMs = 1000)
```
This function abstracts communication with the SIM7600 module. Inside the function:
* `sim7600.println(cmd);` - Sends AT command to GSM module
* `Serial.println(">> " + cmd);` - Prints command to serial monitor for debugging
* `delay(delayMs);` - Waits for module to process command
* `while (sim7600.available()) { Serial.write(sim7600.read()); }` - Reads response from module and prints it. Useful for debugging and verifying success.

## 5. Setup Function Execution Flow

### Serial Initialization
```cpp
Serial.begin(115200);
sim7600.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
```
* Initializes: Debug serial (PC) and UART communication with SIM7600.

### Basic Module Check
* `sendAT("AT");` - Checks if module is alive → should return OK
* `sendAT("ATE0");` - Disables echo (cleaner responses)
* `sendAT("AT+CPIN?");` - Checks SIM card status (should be READY)
* `sendAT("AT+CREG?");` - Verifies network registration
* `sendAT("AT+CGATT?");` - Confirms GPRS attachment

### Network Setup
* `sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");` - Defines PDP context (internet session)
* `sendAT("AT+CGACT=1,1");` - Activates the data connection

### HTTP Initialization
* `sendAT("AT+HTTPTERM");` - Clears previous sessions (important for stability)
* `sendAT("AT+HTTPINIT");` - Initializes HTTP service inside SIM7600
* `sendAT("AT+HTTPPARA=\"URL\",\"...\"");` - Sets target URL (Firebase endpoint)
* `sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");` - Specifies content type

### Sending Data
```cpp
sim7600.print("AT+HTTPDATA=");
sim7600.print(jsonData.length());
sim7600.println(",10000");
```
* Prepares module to receive data: Data length, Timeout (10 seconds)
* `sim7600.print(jsonData);` - Sends actual JSON payload

### HTTP POST Request
* `sendAT("AT+HTTPACTION=1", 8000);` - Executes HTTP POST (1 = POST method). Delay increased for network latency.

### Cleanup
* `sendAT("AT+HTTPTERM");` - Terminates HTTP session. Prevents memory/resource issues.

## 6. Results and Expected Output
If successful, the serial monitor should show:
* `OK` responses for AT commands
* Network registration success
* HTTP response code: `200` (Success)
* Firebase should store the JSON data

## 7. Conclusion
Version 1 successfully demonstrates that:
* The SIM7600 module is functional
* UART communication is correctly configured
* Cellular network connectivity is established
* HTTP POST to Firebase is achievable

This confirms that the GSM module is ready to be integrated into the full IoT system, where it will serve as the primary communication interface between the embedded device and the cloud platform.
```