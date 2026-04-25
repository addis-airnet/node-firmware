#include <Arduino.h>

#define RX_PIN 16 
#define TX_PIN 17 
HardwareSerial sim7600(2); 

const char* APN = "internet";  // Change to "ethionet" if using Ethio Telecom
const char* FIREBASE_URL = "https://sen55-air-quality-monitor-default-rtdb.europe-west1.firebasedatabase.app/sensor/data.json"; 

String jsonData = "{\"Date\":\"test\",\"pm2_5\":24.7,\"pm10\":25.1,\"humidity\":42.74,\"temperature\":22.985,\"voc_index\":106,\"nox_index\":1}";

// Standard function to send basic AT commands
void sendAT(String cmd, int delayMs = 1000) {
    sim7600.println(cmd);
    Serial.println(">> " + cmd);
    delay(delayMs);
    while (sim7600.available()) {
        Serial.write(sim7600.read());
    }
}

// Function to wait for a specific response (like the HTTP status code)
bool waitResponse(String expected, unsigned long timeout = 10000) {
    unsigned long start = millis();
    String response = "";
    while (millis() - start < timeout) {
        while (sim7600.available()) {
            char c = sim7600.read();
            Serial.write(c);
            response += c;
        }
        if (response.indexOf(expected) != -1) {
            return true; 
        }
    }
    return false; // Timed out
}

void setup() {
    Serial.begin(115200);
    sim7600.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
    delay(3000);
    Serial.println("SIM7600 - Looping POST Setup");

    // 1. Basic Module & Network Setup (Executes ONCE)
    sendAT("AT");
    sendAT("ATE0");
    sendAT("AT+CPIN?");
    sendAT("AT+CREG?");
    sendAT("AT+CGATT?");
    sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");
    sendAT("AT+CGACT=1,1");
    
    // Clear any leftover HTTP sessions
    sendAT("AT+HTTPTERM", 500); 
    // 1. Initialize HTTP Session
    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"URL\",\"" + String(FIREBASE_URL) + "\"");
    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");

    Serial.println("--- Setup Complete. Starting Loop. ---");

}

void loop() {
    Serial.println("\n--- Starting New HTTP POST ---");

    

    // 2. Request to Send Data
    sim7600.print("AT+HTTPDATA=");
    sim7600.print(jsonData.length());
    sim7600.println(",10000");
    
    // Wait for the "DOWNLOAD" prompt before sending actual JSON
    waitResponse("DOWNLOAD", 5000); 

    // 3. Send JSON Payload
    sim7600.print(jsonData);
    waitResponse("OK", 5000); 
    
    // 4. Execute POST and Wait for Status Code
    sim7600.println("AT+HTTPACTION=1");
    Serial.println(">> AT+HTTPACTION=1");
    Serial.println("[Waiting for server response...]");
    
    // Actively wait for up to 15 seconds for the +HTTPACTION response
    bool success = waitResponse("+HTTPACTION:", 15000); 
    
    if (success) {
        Serial.println("\n--- POST Completed Successfully ---");
    } else {
        Serial.println("\n--- POST Failed or Timed Out ---");
    }

    // 6. Wait 1 Minute before the next transmission
    Serial.println("Waiting 60 seconds...\n");
    delay(60000); 
}
