#include <Arduino.h>
#include <SD.h>
#include <Wire.h>
#include <RtcDS3231.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <ModbusRTUClient.h>
#include "ConfigManager.h"
#include "GPS.h"

#define TASK_WDT_TIMEOUT  60
#define SIM_RXD           32
#define SIM_TXD           33
#define SIM_BAUD          115200
#define RTU_BAUD          9600

PubSubClient mqtt;
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
GPS gps(modem);
RtcDS3231<TwoWire> Rtc(Wire);
ConfigManager config;

static const BaseType_t pro_cpu = 0;
static const BaseType_t app_cpu = 1;

uint8_t mac[6];
char macAdr[18];
char dateTime[40];
char logString[400];
RtcDateTime run_time, err_time, compiled;
std::vector<ProbeData> probeData;
bool rtcErr();

// Task handles
static TaskHandle_t gpsTaskHandle = NULL;
static TaskHandle_t modbusTaskHandle = NULL;
static TaskHandle_t rtcTaskHandle = NULL;
static TaskHandle_t pushTaskHandle = NULL;
static TaskHandle_t logTaskHandle = NULL;

// Task delay times
const TickType_t gpsDelay = pdMS_TO_TICKS(5000);
const TickType_t modbusDelay = pdMS_TO_TICKS(5000);
const TickType_t rtcDelay = pdMS_TO_TICKS(5000);
const TickType_t logDelay = pdMS_TO_TICKS(5000);
const TickType_t pushDelay = pdMS_TO_TICKS(5000);

String getTime(RtcDateTime& now);
void errLog(String time, const char* msg);
void callback(char* topic, byte* message, unsigned int len);
void writeFile(fs::FS &fs, const char * path, const char * message);
void appendFile(fs::FS &fs, const char * path, const char * message);
// Task prototypes
void readGPS(void *pvParameters);
void readModbus(void *pvParameters);
void checkRTC(void *pvParameters);
void remotePush(void *pvParameters);
void localLog(void *pvParameters);

void setup() {
  Serial.begin(115200);

  // Retrieve MAC address
  esp_efuse_mac_get_default(mac);
  // Format the MAC address into the string
  sprintf(macAdr, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  // Print the MAC address
  Serial.printf("ESP32 MAC Address: %s\r\n", macAdr);

  // Initialize DS3231 communication
  Rtc.Begin();
  compiled = RtcDateTime(__DATE__, __TIME__);
  Serial.println();
  if (!Rtc.IsDateTimeValid()){
    if(!rtcErr()){
      Serial.println("RTC lost confidence in the DateTime!");
      Rtc.SetIsRunning(true);
    }
  }
  if (!Rtc.GetIsRunning()){
    if (!rtcErr()){
      Serial.println("RTC was not actively running, starting now");
      Rtc.SetIsRunning(true);
    }
  }
  // Compare RTC time with compile time
  if (!rtcErr()){
    if (run_time < compiled){
      Serial.println("RTC is older than compile time, updating DateTime");
      Rtc.SetDateTime(compiled);
    } else if (run_time > compiled) {
      Serial.println("RTC is newer than compile time, this is expected");
    } else if (run_time == compiled) {
      Serial.println("RTC is the same as compile time, while not expected all is still fine");
    }
  }
  // Disable unnecessary functions of the RTC DS3231
  Rtc.Enable32kHzPin(false);
  Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone);
  delay(1000);

  if(!SD.begin(5)){
    Serial.println("Card Mount Failed");
  }
  uint8_t cardType = SD.cardType();
  if(cardType == CARD_NONE){
    Serial.println("No SD card attached");
  }
  // Check SD card type
  Serial.print("SD Card Type: ");
  if(cardType == CARD_MMC){
    Serial.println("MMC");
  } else if(cardType == CARD_SD){
    Serial.println("SDSC");
  } else if(cardType == CARD_SDHC){
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }
  // Check SD card size
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);
  Serial.println("");
  
  if(!config.getNetwork()){
    Serial.println(config.err);
  }
  if(!config.getTank()){
    Serial.println(config.err);
  }
  delay(1000);

  gps.init();

  // Initialize the Modbus RTU client
  if (!ModbusRTUClient.begin(RTU_BAUD)) {
    Serial.println("Failed to start Modbus RTU Client!");
  }
  probeData.reserve(config.probeId.size());
  delay(1000);

  SerialAT.begin(SIM_BAUD, SERIAL_8N1, SIM_RXD, SIM_TXD);
  Serial.println("Initializing modem...");
  if(!modem.init()){
    modem.restart();
  }
  if (modem.getSimStatus() != 1) {
    Serial.println("SIM not ready, checking for PIN...");
    if (modem.getSimStatus() == 2){
      Serial.println("SIM PIN required.");
      // Send the PIN to the modem
      modem.sendAT("+CPIN=" + config.simPin);
      delay(1000);
      if (modem.getSimStatus() != 1) {
        Serial.println("Failed to unlock SIM.");
        return;
      }
      Serial.println("SIM unlocked successfully.");
    } else {
      Serial.println("SIM not detected or unsupported status");
      return;
    }
  }
  delay(1000);

  Serial.print("Connecting to APN: ");
  Serial.println(config.apn);
  if (!modem.gprsConnect(config.apn.c_str(), config.gprsUser.c_str(), config.gprsPass.c_str())) {
    Serial.println("GPRS connection failed");
    errLog(getTime(err_time),"GPRS connection failed");
  } else {
    Serial.println("GPRS connected");
  }
  delay(1000);

  mqtt.setServer(config.broker.c_str(), config.port);
  mqtt.setBufferSize(1024);
  mqtt.setCallback(callback);

  File file = SD.open("/log.csv");
  if(!file) {
    Serial.println("File doens't exist");
    Serial.println("Creating file...");
    writeFile(SD, "/log.csv", "Date,Time,Latitude,Longitude,Speed(km/h),Altitude(m)"
                              "Volume(l),Ullage(l),Temperature(°C)"
                              "Product level(mm),Water level(mm)\n");
  } else {
    Serial.println("File already exists");  
  }
  file.close();
  delay(500);
  file = SD.open("/error.csv");
    if(!file){
      Serial.println("File doens't exist");
      Serial.println("Creating file...");
      writeFile(SD, "/error.csv", "Date,Time,Error");
    } else {
      Serial.println("File already exists");  
    }
  file.close();

  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(readGPS, "Read GPS", 3072, NULL, 1, &gpsTaskHandle, pro_cpu);
  xTaskCreatePinnedToCore(readModbus, "Read Modbus", 3072, NULL, 1, &modbusTaskHandle, pro_cpu);
  xTaskCreatePinnedToCore(checkRTC, "Check RTC", 2048, NULL, 2, &rtcTaskHandle, pro_cpu);
  xTaskCreatePinnedToCore(localLog, "Log to SD", 8000, NULL, 1, &logTaskHandle, app_cpu);
  xTaskCreatePinnedToCore(remotePush, "Push to MQTT", 5012, NULL, 1, &pushTaskHandle, app_cpu);

  // Initialize the Task Watchdog Timer
  esp_task_wdt_init(TASK_WDT_TIMEOUT, true);
  
  vTaskDelete(NULL);
}

void loop() {
  
}

void readGPS(void *pvParameters){
  while(1){
    gps.update();
    errLog(getTime(err_time), gps.lastError());
    getTime(run_time);
    xTaskNotifyGive(pushTaskHandle);
    xTaskNotifyGive(logTaskHandle);
    vTaskDelay(gpsDelay);
  }
}

void readModbus(void *pvParameters) {
  while(1){
    for (size_t i = 0; i < config.probeId.size(); ++i) {
      float* dataPointers[] = {&probeData[i].volume, &probeData[i].ullage, 
                              &probeData[i].temperature, &probeData[i].product, 
                              &probeData[i].water};
      const char* labels[] = {"Volume", "Ullage", "Temperature", "Product", "Water"};
      
      for (size_t j = 0; j < 5; ++j) {
        if (!ModbusRTUClient.requestFrom(config.probeId[i], HOLDING_REGISTERS, config.modbusReg[j])) {
          Serial.printf("Failed to read %s for probe ID: %d\r\nError: ", labels[j], config.probeId[i]);
          Serial.println(ModbusRTUClient.lastError());
          errLog(getTime(err_time), ModbusRTUClient.lastError());
        } else {
          while (ModbusRTUClient.available()){
            *dataPointers[j] = ModbusRTUClient.holdingRegisterRead<float>(
                            config.probeId[i], config.modbusReg[j], BIGEND);
            Serial.printf("%s: %.2f\r\n", labels[j], *dataPointers[j]);
          }
        }
      }
    }
    xTaskNotifyGive(pushTaskHandle);
    xTaskNotifyGive(logTaskHandle);
    vTaskDelay(modbusDelay);
  }
}

void remotePush(void *pvParameters){
  while(1){
    if (!modem.isGprsConnected()){
      Serial.println("GPRS connection failed");
      errLog(getTime(err_time),"GPRS connection failed");
    } else {
      StaticJsonDocument<1024> data;
      data["Device"] = macAdr;
      data["Date/Time"] = dateTime;

      JsonObject gpsData = data.createNestedObject("Gps");
      gpsData["Latitude"] = gps.location.latitude;
      gpsData["Longitude"] = gps.location.longitude;
      gpsData["Altitude"] = gps.location.altitude;
      gpsData["Speed"] = gps.location.speed;

      JsonArray measures = data.createNestedArray("Measure");
      for(size_t i = 0; i < config.probeId.size(); i++){
        JsonObject measure = measures.createNestedObject();
        measure["Id"] = config.probeId[i];
        measure["Volume"] = probeData[i].volume;
        measure["Ullage"] = probeData[i].ullage;
        measure["Temperature"] = probeData[i].temperature;
        measure["ProductLevel"] = probeData[i].product;
        measure["WaterLevel"] = probeData[i].water;
      }
      // Serialize JSON and publish
      char buffer[1024];
      size_t n = serializeJson(data, buffer);
      mqtt.publish(config.topic.c_str(), buffer, n);
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pushDelay);
  }
}

void localLog(void *pvParameters){
  while(1){
    for(size_t i = 0; i < config.probeId.size(); i++){
      char fileName[10];
      snprintf(fileName, sizeof(fileName), "/log%d.csv", i + 1);

      snprintf(logString, sizeof(logString), "%s;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f\n",
              dateTime, gps.location.latitude, gps.location.longitude, gps.location.speed,
              gps.location.altitude, probeData[i].volume, probeData[i].ullage,
              probeData[i].temperature, probeData[i].product, probeData[i].water);
      appendFile(SD, fileName, logString);
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(logDelay);
  }
}

void checkRTC(void *pvParameters){
  while(1){
    if (!Rtc.IsDateTimeValid()){
      if(!rtcErr()){
        Serial.println("RTC lost confidence in the DateTime!");
        Rtc.SetIsRunning(true);
      }
      snprintf(dateTime, sizeof(dateTime), "%s", getTime(run_time));
    }
    xTaskNotifyGive(pushTaskHandle);
    xTaskNotifyGive(logTaskHandle);
    vTaskDelay(rtcDelay);
  }
}

String getTime(RtcDateTime& now){
  now = Rtc.GetDateTime();
  char time[40];
  snprintf_P(time,
            countof(time),
            PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
            now.Month(),
            now.Day(),
            now.Year(),
            now.Hour(),
            now.Minute(),
            now.Second());
  return String(time);
}

bool rtcErr(){
  uint8_t error = Rtc.LastError();
  if (error != 0){
    Serial.print("WIRE communications error: ");
    switch (error)
    {
    case Rtc_Wire_Error_None:
      Serial.println("(none?!)");
      break;
    case Rtc_Wire_Error_TxBufferOverflow:
      Serial.println("transmit buffer overflow");
      break;
    case Rtc_Wire_Error_NoAddressableDevice:
      Serial.println("no device responded");
      break;
    case Rtc_Wire_Error_UnsupportedRequest:
      Serial.println("device doesn't support request");
      break;
    case Rtc_Wire_Error_Unspecific:
      Serial.println("unspecified error");
      break;
    case Rtc_Wire_Error_CommunicationTimeout:
      Serial.println("communications timed out");
      break;
    }
    return true;
  }
  return false;
}

void errLog(String time, const char* msg){
  String errMsg = String(time) + "," + String(msg);
  File file = SD.open("/error.csv");
  appendFile(SD, "/error.csv", errMsg.c_str());
}

void callback(char* topic, byte* message, unsigned int len){
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < len; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();
}

void writeFile(fs::FS &fs, const char * path, const char * message) {
  Serial.printf("Writing file: %s", path);

  File file = fs.open(path, FILE_WRITE);
  if(!file) {
    Serial.print("Failed to open file for writing\n");
    return;
  }
  if(file.print(message)) {
    Serial.print("File written\n");
  } else {
    Serial.print("Write failed\n");
  }
  file.close();
}

void appendFile(fs::FS &fs, const char* path, const char* message){
  Serial.printf("Appending file: %s", path);
  
  File file = fs.open(path, FILE_APPEND);
  if(!file){
    Serial.print("File does not exist, creating file...\n");
    file = fs.open(path, FILE_WRITE);  // Create the file
    if(!file){
      Serial.print("Failed to create file\n");
      return;
    }
  }
  if(file.print(message)){
    Serial.print("Message appended\n");
  } else {
    Serial.print("Append failed\n");
  }
  file.close();
}
