/* Power Supply 
Option 1: Via USB(5V) for testing
option 2: 5V DC, 2A from DC source
*/
#include <Arduino.h>

#define RX_PIN 16 // esp32@pin 16
#define TX_PIN 17 // esp32@pin 17
HardwareSerial sim7600(2); // UART object


const char* APN = "internet";  
const char* FIREBASE_URL = "https://sen55-air-quality-monitor-default-rtdb.europe-west1.firebasedatabase.app/sensor/data.json"; 

// dummy data
String jsonData = "{\"Date\":\"test\",\"pm2_5\":24.7,\"pm10\":25.1,\"humidity\":42.74,\"temperature\":22.985,\"voc_index\":106,\"nox_index\":1}";

void setup() {
    Serial.begin(115200);
    sim7600.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN); SERIAAL_8N1: 8 data bit, No parity, 1 stop bit
    delay(3000);
    Serial.println("SIM7600 HTTPS TEST");

//*****************************************************************************************************
//! cmd "AT" checks if
//! 1. gsm responds/powered on
//! 2. rx, tx wired and work
//! 3. baud rate correct
//! cmd "ATE0": default echo off
//*****************************************************************************************************
    sendAT("AT");
    sendAT("ATE0");
    //sendAT("AT&F"); // factory reset not required for a brand new module as it's on default already


//*****************************************************************************************************
//! Network setup & Configuration
//*****************************************************************************************************
    sendAT("AT+CPIN?"); // read sim insertion and enter PIN if required 
    /*no need for the ff cmds if they are set by default
    sendAT("AT+CFUN=1") Set functionality mode 1(network enabled), no need for this command if enabled by default
    sendAT("AT+CNMP=2")   // nework mode: auto(LTE,3G,GSM) 
    */
    sendAT("AT+CREG?"); //Newtowrk regsitration
    sendAT("AT+CGATT?"); // +CGATT: 1=>data ready(attached to the mobile data network (GPRS/LTE)), +CGATT: 0 =>no data connection

//*****************************************************************************************************
//! Data/internet Configuration
//***************************************************************************************************** 
    sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");
    //sendAT("AT+CGATT=1"); //Enable internet
    sendAT("AT+CGACT=1, 1");
    sendAT("AT+CGPADDR=1");

//******************************************************************************************************
//!https config
//****************************************************************************************************** 
    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"CID\",1");
    sendAT("AT+HTTPSSL=1"); //Enable https
    sendAT("AT+HTTPPARA=\"URL\",\"" + String(FIREBASE_URL) + "\"");
    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");

//*******************************************************************************************************
//! send data
//******************************************************************************************************* 
    int len = jsonData.length();

    sim7600.print("AT+HTTPDATA=");
    sim7600.print(len);
    sim7600.println(",10000");
    delay(1000);

    while (sim7600.available()) {
        Serial.write(sim7600.read());
    }
    sim7600.print(jsonData);
    delay(3000);
    sendAT("AT+HTTPACTION=1", 8000);//post
    sendAT("AT+HTTPREAD", 3000);// Read response
    sendAT("AT+HTTPTERM"); //end https
}

void loop() {}

void sendAT(String cmd, int delayMs = 1000) {
    sim7600.println(cmd);
    Serial.println(">> " + cmd);
    delay(delayMs);

    while (sim7600.available()) {
        Serial.write(sim7600.read());
    }
}
