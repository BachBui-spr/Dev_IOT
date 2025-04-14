

// Import required libraries
#include "WiFi.h"
#include "ThingsBoard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Import supported libraries
#include <Arduino_MQTT_Client.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>

#include <Shared_Attribute_Update.h>


#define DEBUG 1

// Replace with your network credentials
const char* ssid = "BBSpr";
const char* password = "12042004";


/* Server object ---------------------------------------------*/
constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 512U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 256U;

// and should be the same as the amount of variables in the passed array. If it is less not all variables will be requested or subscribed
constexpr size_t MAX_ATTRIBUTES = 1U;

WiFiClient espClient;
Arduino_MQTT_Client mqttClient(espClient);
// Initialize used apis
Shared_Attribute_Update<1U, MAX_ATTRIBUTES> shared_update;
const std::array<IAPI_Implementation*, 1U> apis = {
    &shared_update
};
ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

// Set up the device properties on server
constexpr char DEVICE_TOKEN[] = "8ozyb1mwmix1x9e53skf";
constexpr char TEMPERATURE_KEY[] = "temperature";
constexpr char HUMIDITY_KEY[] = "humidity";
constexpr char SHARED_ATTRIBUTE_KEY[] = "measurement_status";

// Statuses for subscribing to shared attributes
bool subscribed = false;

/* Sensor object ---------------------------------------------*/
#define DHTPIN 6
#define DHTTYPE    DHT22 
DHT_Unified dht(DHTPIN, DHTTYPE);
typedef struct {
  float Temperature = 0.0;
  float Humidity = 0.0;
} DHT20_Data_t;

DHT20_Data_t DHT20_Data;

/* Task object ---------------------------------------------*/
TaskHandle_t WifiTask_handle;
TaskHandle_t SensorTask_handle;
TaskHandle_t PublishData_handle;
TaskHandle_t ServerTask_handle;


/* Synchronous object ---------------------------------------------*/
EventGroupHandle_t eventGroup;
const int ABORT_MEASURE_BIT = (1 << 0);
SemaphoreHandle_t mutex;


/// @brief Update callback that will be called as soon as one of the provided shared attributes changes value,
/// if none are provided we subscribe to any shared attribute change instead
/// @param data Data containing the shared attributes that were changed and their current value
void processSharedAttributeUpdate(const JsonObjectConst &data) {
  Serial.println("Process Shared Attribute");
  
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (strcmp(it->key().c_str(), "measurement_status") == 0) {  // So sánh chuỗi đúng cách
      bool status = it->value().as<bool>();  // Đọc giá trị dưới dạng boolean
      
      if (status) {
        Serial.println("Turn On");
        xEventGroupSetBits(eventGroup, ABORT_MEASURE_BIT); 
      } else {
        Serial.println("Turn Off");
        xEventGroupClearBits(eventGroup, ABORT_MEASURE_BIT); 
      }      
    }    
  }

  // In ra toàn bộ JSON để debug
  const size_t jsonSize = Helper::Measure_Json(data);
  char buffer[jsonSize];
  serializeJson(data, buffer, jsonSize);
  Serial.println(buffer);  
}



// Task to handle Wi-Fi connection
void wifiTask(void *pvParameters) 
{
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);    
#ifdef DEBUG
    Serial.println("Connecting to WiFi..");
#endif
  }

  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());
  vTaskResume(ServerTask_handle);  
  vTaskSuspend(NULL);  // Delete the task when done
}

// Task to read value from DHT20
void sensorTask(void* pvParameters) 
{  
  dht.begin();    // Start sensor
  sensors_event_t event;

  while(1){
    xEventGroupWaitBits(eventGroup, ABORT_MEASURE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    for(;;){
      if(xSemaphoreTake(mutex, portMAX_DELAY)){
        dht.temperature().getEvent(&event);
      // get temperature value    
      DHT20_Data.Temperature = event.temperature;
      vTaskDelay(5);
      dht.humidity().getEvent(&event);    
      // get humidity value
      DHT20_Data.Humidity = event.relative_humidity;
  #ifdef DEBUG
      Serial.printf("Temperature: %.3f | Humidity: %.3f \n", DHT20_Data.Temperature, DHT20_Data.Humidity);
  #endif
        xSemaphoreGive(mutex);
      }
      if (!(xEventGroupGetBits(eventGroup) & ABORT_MEASURE_BIT)) {
        break;  // Stop measuring
      }

      vTaskDelay(1000 / portTICK_PERIOD_MS);       
    }
    
      
    // get sensor data periodly    
  }
}


// Task to publish data to coreiot server
void publishdataTask(void* pvParameters)
{

  while(1){
    xEventGroupWaitBits(eventGroup, ABORT_MEASURE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    for(;;){
      if(xSemaphoreTake(mutex, portMAX_DELAY)){
        Serial.println("Sended Data");
        tb.sendTelemetryData(TEMPERATURE_KEY, DHT20_Data.Temperature);
        tb.sendTelemetryData(HUMIDITY_KEY, DHT20_Data.Humidity);
        xSemaphoreGive(mutex);
      }
      
      if (!(xEventGroupGetBits(eventGroup) & ABORT_MEASURE_BIT)) {
        break;  // Stop measuring
      }      
      // publish data periodly      
      vTaskDelay(5000 / portTICK_PERIOD_MS); 
    }
    
  }

}

void ServerTask(void* pvParameters){

  while(1){
    if (!tb.connected())
    {
#ifdef  DEBUG    
        Serial.printf("Connecting to: (%s) with token (%s)\n", THINGSBOARD_SERVER, DEVICE_TOKEN);
#endif
        if (!tb.connect(THINGSBOARD_SERVER, DEVICE_TOKEN, THINGSBOARD_PORT))
        {          
#ifdef  DEBUG        
          Serial.println("Failed to connect");
#endif          
        }
        else
        {        
          vTaskResume(PublishData_handle);  
#ifdef  DEBUG                  
          Serial.println("Connected");
#endif          
        }                
    }

    if (!subscribed) {
      Serial.println("Subscribing for shared attribute updates...");
      // Shared attributes we want to request from the server
      constexpr std::array<const char*, MAX_ATTRIBUTES> SUBSCRIBED_SHARED_ATTRIBUTES = {SHARED_ATTRIBUTE_KEY};
      const Shared_Attribute_Callback<MAX_ATTRIBUTES> callback(&processSharedAttributeUpdate, SUBSCRIBED_SHARED_ATTRIBUTES);
      if (!shared_update.Shared_Attributes_Subscribe(callback)) {
        Serial.println("Failed to subscribe for shared attribute updates");
        return;
      }
  
      Serial.println("Subscribe done");
      subscribed = true;
    }
  
    Attribute attribute[MAX_ATTRIBUTES] = {{SHARED_ATTRIBUTE_KEY, true}}; 



    tb.loop();
    vTaskDelay(1000);
  }

}


void setup(){  
  mutex = xSemaphoreCreateMutex();
  if (mutex != NULL) {
    xSemaphoreGive(mutex); 
  }

  eventGroup = xEventGroupCreate();

  // Create tasks for Wi-Fi and server
  xTaskCreate(sensorTask, "SensorTask", 1024 * 4, NULL, 3, &SensorTask_handle);    
  xTaskCreate(publishdataTask, "PublishDataTask", 1024 * 4, NULL, 2, &PublishData_handle);
  vTaskSuspend(PublishData_handle);  
  xTaskCreate(ServerTask, "ServerTask", 1024 * 4, NULL, 1, &ServerTask_handle);    
  vTaskSuspend(ServerTask_handle);

  delay(1000);
  xTaskCreate(wifiTask, "WiFiTask", 1024 * 4, NULL, 1, &WifiTask_handle);    
  
}
 
void loop(){
  // Nothing to do here, FreeRTOS tasks handle the work
  // Push the main loop to the idle task to save the energy
  vTaskDelay(portMAX_DELAY);
}