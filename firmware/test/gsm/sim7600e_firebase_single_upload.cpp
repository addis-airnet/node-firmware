#include <Arduino.h>

#define RX_PIN 16 
#define TX_PIN 17 
HardwareSerial sim7600(2); 

const char* APN = "internet"; 
const char* FIREBASE_URL = "https://sen55-air-quality-monitor-default-rtdb.europe-west1.firebasedatabase.app/sensor/data.json"; 

String jsonData = "{\"Date\":\"test\",\"pm2_5\":25.7,\"pm10\":25.1,\"humidity\":42.74,\"temperature\":22.985,\"voc_index\":106,\"nox_index\":1}";

void sendAT(String cmd, int delayMs = 1000) {
    sim7600.println(cmd);
    Serial.println(">> " + cmd);
    delay(delayMs);
    while (sim7600.available()) {
        Serial.write(sim7600.read());
    }
}

void setup() {
    Serial.begin(115200);
    sim7600.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
    delay(3000);
    Serial.println("SIM7600 - Firebase HTTPS POST");

    // 1. Basic Module Setup
    sendAT("AT");
    sendAT("ATE0");
    sendAT("AT+CPIN?");
    sendAT("AT+CREG?");
    sendAT("AT+CGATT?");

    // 2. Network Configuration
    sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");
    sendAT("AT+CGACT=1,1");

    // 3. HTTPS Configuration
    sendAT("AT+HTTPTERM"); // Clear stuck sessions
    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"URL\",\"" + String(FIREBASE_URL) + "\"");
    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");

    // 4. Send Data Payload
    sim7600.print("AT+HTTPDATA=");
    sim7600.print(jsonData.length());
    sim7600.println(",10000");
    delay(1000);
    
    while (sim7600.available()) {
        Serial.write(sim7600.read());
    }

    sim7600.print(jsonData);
    delay(3000);
    
    // 5. Execute POST and Read Response
    sendAT("AT+HTTPACTION=1", 8000); 
    sendAT("AT+HTTPREAD", 3000);     
    sendAT("AT+HTTPTERM"); 
}
void loop() {}
