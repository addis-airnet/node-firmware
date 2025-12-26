#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <SensirionI2CSen5x.h>
#include <SD.h>
#include <SPI.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <time.h>

#define NTP_SERVER "pool.ntp.org"
//#define GMT_OFFSET_SEC (3 * 3600)   
//#define DAYLIGHT_OFFSET_SEC 0


#define WIFI_SSID ""
#define WIFI_PASSWORD ""

#define API_KEY "AIzaSyAWCvZuaKVbtPYlu83KDlz1QkLDDY0pj-E"
#define DATABASE_URL "https://sen55-air-quality-monitor-default-rtdb.europe-west1.firebasedatabase.app/"

#define USER_EMAIL "ephraimwoldemichael@gmail.com"
#define USER_PASSWORD "itisme"


SensirionI2CSen5x sen5x;

unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 30000; 

bool sensorReady = false;         
unsigned long warmupStartTime = 0;
const unsigned long WARMUP_TIME = 60000; 

File myFile;
const int cs = 5;


FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long sendDataPrevMillis = 0;


void setup() {
  Serial.begin(9600);
  Wire.begin(21, 22); 

  sen5x.begin(Wire);

  
  uint8_t error;
  char errorMessage[256];
 error = sen5x.deviceReset();
  if (error) {
  Serial.print("Reset Error: ");
   Serial.println(errorMessage);
   return;
 }

  Serial.println("Starting SEN55 measurements...");
  sen5x.startMeasurement();

  
  warmupStartTime = millis();

  Serial.println("Initializing SD card...");
   if(!SD.begin(cs)){
   Serial.println("Initialization failed.");
   Serial.println("Things to check:");
   Serial.println("1. is a card inserted?");
   Serial.println("2. is your wiring correct?");
   Serial.println("3. did you change the chipSelect pin to match  your shield or module?");
  }
  else{
   Serial.println("SD card initialized successfully");
  }

  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();
 
  // Force East Africa Timezone (UTC+3)
setenv("TZ", "EAT", 1);
tzset();

// Sync NTP time (UTC, timezone handled by TZ)
configTime(0, 0, NTP_SERVER);

struct tm timeinfo;
if (!getLocalTime(&timeinfo)) {
  Serial.println("Failed to obtain time");
} else {
  Serial.print("Time synchronized: ");
  Serial.println(&timeinfo, "%d-%m-%Y %I:%M:%S %p");
}


  
  Serial.printf("Firebase Client v%s\n\n", FIREBASE_CLIENT_VERSION);
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.setDoubleDigits(5);

  Serial.println("Firebase initialized!");
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String("01-01-1970 12:00:00 AM");
  }

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d-%m-%Y %I:%M:%S %p", &timeinfo);
  return String(buffer);
}



void loop() {
  
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();  

    if (!sensorReady) {
      if (millis() - warmupStartTime >= WARMUP_TIME) {
        sensorReady = true;
        Serial.println("SEN55 warmed up and ready.");
      }
      return;
    }

    
    float pm1_0, pm2_5, pm4_0, pm10_0;
    float humidity, temperature;
    float vocIndex, noxIndex;

    uint16_t error = sen5x.readMeasuredValues(
        pm1_0,
        pm2_5,
        pm4_0,
        pm10_0,
        humidity,
        temperature,
        vocIndex,
        noxIndex
    );

    if (error) {
      Serial.print("Read Error: ");
      Serial.println(error);  
      return;
    }

    
    Serial.print("PM1.0: "); Serial.println(pm1_0);
    Serial.print("PM2.5: "); Serial.println(pm2_5);
    Serial.print("PM4.0: "); Serial.println(pm4_0);
    Serial.print("PM10.0: "); Serial.println(pm10_0);
    Serial.print("Humidity: "); Serial.println(humidity);
    Serial.print("Temperature: "); Serial.println(temperature);
    Serial.print("VOC Index: "); Serial.println(vocIndex);
    Serial.print("NOx Index: "); Serial.println(noxIndex);

    
    StaticJsonDocument<256> doc;
    doc["Date"] = getTimestamp();
    doc["pm2_5"] = pm2_5;
    doc["pm10"] = pm10_0;
    doc["humidity"] = humidity;
    doc["temperature"] = temperature;
    doc["voc_index"] = vocIndex;
    doc["nox_index"] = noxIndex;

    String jsonOutput;
    serializeJson(doc, jsonOutput);
    
    myFile = SD.open("/test.json", FILE_APPEND);
    if(myFile){
    Serial.println("Writing to test.json...");
    myFile.println(jsonOutput);
    Serial.println("Data recorded to SD!");
    delay(50); 
    myFile.close();
    }
    else {
    Serial.println("error opening test.json");
    }

    FirebaseJson json;
    json.setJsonData(jsonOutput);


    
    bool ok = Firebase.setJSON(fbdo, "/sensor/data", json);
    if (ok) {
      Serial.println("Data uploaded to Firebase!");
    } else {
      Serial.println("Failed to upload data.");
      Serial.println(fbdo.errorReason());  
    }
  }
}
