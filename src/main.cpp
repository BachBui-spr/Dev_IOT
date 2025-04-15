

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

// Import supported OTA
#include <OTA_Firmware_Update.h>
#include <Espressif_Updater.h>

#define DEBUG 1

// Replace with your network credentials
const char *ssid = "BBSpr";
const char *password = "12042004";

/* OTA Firmware object ---------------------------------------------*/
// Firmware title and version used to compare with remote version, to check if an update is needed.
// Title needs to be the same and version needs to be different --> downgrading is possible
constexpr char CURRENT_FIRMWARE_TITLE[] = "Lab1_IOT";
constexpr char CURRENT_FIRMWARE_VERSION[] = "1.0.0";

// Maximum amount of retries we attempt to download each firmware chunck over MQTT
constexpr uint8_t FIRMWARE_FAILURE_RETRIES = 12U;
// Size of each firmware chunck downloaded over MQTT,
// increased packet size, might increase download speed
constexpr uint16_t FIRMWARE_PACKET_SIZE = 4096U;

// Statuses for updating
bool currentFWSent = false;
bool updateRequestSent = false;

/* Server object ---------------------------------------------*/
constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 512U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 256U;

// Initialize used apis
OTA_Firmware_Update<> ota;
const std::array<IAPI_Implementation *, 1U> apis = {
    &ota};

WiFiClient espClient;
Arduino_MQTT_Client mqttClient(espClient);
ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

Espressif_Updater<> updater;

// Set up the device properties on server
constexpr char DEVICE_TOKEN[] = "8ozyb1mwmix1x9e53skf";
constexpr char TEMPERATURE_KEY[] = "temperature";
constexpr char HUMIDITY_KEY[] = "humidity";

/* Sensor object ---------------------------------------------*/
#define DHTPIN 6
#define DHTTYPE DHT22
DHT_Unified dht(DHTPIN, DHTTYPE);
typedef struct
{
  float Temperature = 0.0;
  float Humidity = 0.0;
} DHT20_Data_t;

DHT20_Data_t DHT20_Data;

// Task object
TaskHandle_t WifiTask_handle;
TaskHandle_t SensorTask_handle;
TaskHandle_t PublishData_handle;
TaskHandle_t ServerTask_handle;

/* OTA Callback ----------------------------------------------*/

/// @brief Update starting callback method that will be called as soon as the shared attribute firmware keys have been received and processed
/// and the moment before we subscribe the necessary topics for the OTA firmware update.
/// Is meant to give a moment were any additional processes or communication with the cloud can be stopped to ensure the update process runs as smooth as possible.
/// To ensure that calling the ThingsBoardSized::Cleanup_Subscriptions() method can be used which stops any receiving of data over MQTT besides the one for the OTA firmware update,
/// if this method is used ensure to call all subscribe methods again so they can be resubscribed, in the method passed to the finished_callback if the update failed and we do not restart the device
void update_starting_callback()
{
  // Nothing to do
  Serial.println("Suspend All Task");
  vTaskSuspend(SensorTask_handle);
  vTaskSuspend(PublishData_handle);
}

/// @brief End callback method that will be called as soon as the OTA firmware update, either finished successfully or failed.
/// Is meant to allow to either restart the device if the udpate was successfull or to restart any stopped services before the update started in the subscribed update_starting_callback
/// @param success Either true (update successful) or false (update failed)
void finished_callback(const bool &success)
{
  if (success)
  {
    Serial.println("Done, Reboot now");

    esp_restart();
    return;
  }
  Serial.println("Downloading firmware failed");
}

/// @brief Progress callback method that will be called every time our current progress of downloading the complete firmware data changed,
/// meaning it will be called if the amount of already downloaded chunks increased.
/// Is meant to allow to display a progress bar or print the current progress of the update into the console with the currently already downloaded amount of chunks and the total amount of chunks
/// @param current Already received and processs amount of chunks
/// @param total Total amount of chunks we need to receive and process until the update has completed
void progress_callback(const size_t &current, const size_t &total)
{
  Serial.printf("Progress %.2f%%\n", static_cast<float>(current * 100U) / total);
}

// Task to handle Wi-Fi connection
void wifiTask(void *pvParameters)
{
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
#ifdef DEBUG
    Serial.println("Connecting to WiFi..");
#endif
  }

  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());
  vTaskResume(ServerTask_handle);  
  vTaskSuspend(NULL); // Delete the task when done
}

// Task to read value from DHT20
void sensorTask(void *pvParameters)
{
  dht.begin(); // Start sensor
  sensors_event_t event;

  while (1)
  {
    dht.temperature().getEvent(&event);
    // get temperature value
    DHT20_Data.Temperature = event.temperature;
    vTaskDelay(5);
    dht.humidity().getEvent(&event);
    // get humidity value
    DHT20_Data.Humidity = event.relative_humidity;
#ifdef DEBUG
    Serial.printf("Firmware version: %s | temperature: %.3f | Humidity: %.3f \n", CURRENT_FIRMWARE_VERSION, DHT20_Data.Temperature, DHT20_Data.Humidity);
#endif

    // get sensor data periodly
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// Task to publish data to coreiot server
void publishdataTask(void *pvParameters)
{

  while (1)
  {
    tb.sendTelemetryData(TEMPERATURE_KEY, DHT20_Data.Temperature);
    tb.sendTelemetryData(HUMIDITY_KEY, DHT20_Data.Humidity);
    // publish data periodly
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void ServerTask(void *pvParameters)
{
  uint8_t first = 1;
  while (1)
  {
    if (!tb.connected())
    {
#ifdef DEBUG
      Serial.printf("Connecting to: (%s) with token (%s)\n", THINGSBOARD_SERVER, DEVICE_TOKEN);
#endif
      if (!tb.connect(THINGSBOARD_SERVER, DEVICE_TOKEN, THINGSBOARD_PORT))
      {
#ifdef DEBUG
        Serial.println("Failed to connect");
#endif
        if(!first){
          vTaskSuspend(PublishData_handle);
        }
      }
      else
      {                
        first = 0;
        vTaskResume(PublishData_handle);
#ifdef DEBUG
        Serial.println("Connected");
#endif
      }
    }

    if (!currentFWSent)
    {
      currentFWSent = ota.Firmware_Send_Info(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION);
    }

    if (!updateRequestSent)
    {
      Serial.println("Firwmare Update Subscription...");
      const OTA_Update_Callback callback(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION, &updater, &finished_callback, &progress_callback, &update_starting_callback, FIRMWARE_FAILURE_RETRIES, FIRMWARE_PACKET_SIZE);
      // See https://thingsboard.io/docs/user-guide/ota-updates/
      // to understand how to create a new OTA pacakge and assign it to a device so it can download it.
      // Sending the request again after a successfull update will automatically send the UPDATED firmware state,
      // because the assigned firmware title and version on the cloud and the firmware version and title we booted into are the same.
      updateRequestSent = ota.Subscribe_Firmware_Update(callback);
      if (!updateRequestSent)
      {
        Serial.println("Firwmare Update Subscription Success");
      }
      else
      {
        Serial.println("Firwmare Update Subscription Fail");
      }
    }

    tb.loop();
  }
}

void setup()
{

  // Create tasks for Wi-Fi and server
  xTaskCreate(sensorTask, "SensorTask", 1024 * 4, NULL, 3, &SensorTask_handle);  
  xTaskCreate(publishdataTask, "PublishDataTask", 1024 * 4, NULL, 2, &PublishData_handle);
  vTaskSuspend(PublishData_handle);
  xTaskCreate(ServerTask, "ServerTask", 1024 * 4, NULL, 1, &ServerTask_handle);
  xTaskCreate(wifiTask, "WiFiTask", 1024 * 4, NULL, 1, &WifiTask_handle);
}

void loop()
{
  // Nothing to do here, FreeRTOS tasks handle the work
  // Push the main loop to the idle task to save the energy
  vTaskDelay(portMAX_DELAY);
}