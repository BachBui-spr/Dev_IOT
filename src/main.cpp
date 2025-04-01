#define LED_PIN 48
#define SDA_PIN GPIO_NUM_11
#define SCL_PIN GPIO_NUM_12

#define MQ2_PIN 34

#define DHT_PIN  GPIO_NUM_6  
#define DHT_TYPE DHT11 


#include <WiFi.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include "DHT20.h"
#include "Wire.h"
#include <ArduinoOTA.h>
#include <DHT.h>

DHT dht(DHT_PIN, DHT_TYPE); 
constexpr char WIFI_SSID[] = "BBSpr";
constexpr char WIFI_PASSWORD[] = "12042004";

constexpr char TOKEN[] = "sQpFV6Cro2XvIGQSJCEs";

constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

constexpr uint32_t MAX_MESSAGE_SIZE = 1024U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;

constexpr char BLINKING_INTERVAL_ATTR[] = "blinkingInterval";
constexpr char LED_MODE_ATTR[] = "ledMode";
constexpr char LED_STATE_ATTR[] = "ledState";

volatile bool attributesChanged = false;
volatile int ledMode = 0;
volatile bool ledState = false;

constexpr uint16_t BLINKING_INTERVAL_MS_MIN = 10U;
constexpr uint16_t BLINKING_INTERVAL_MS_MAX = 60000U;
volatile uint16_t blinkingInterval = 1000U;

uint32_t previousStateChange;

constexpr int16_t telemetrySendInterval = 10000U;
uint32_t previousDataSend;

constexpr std::array<const char *, 2U> SHARED_ATTRIBUTES_LIST = {
  LED_STATE_ATTR,
  BLINKING_INTERVAL_ATTR
};

WiFiClient wifiClient;
Arduino_MQTT_Client mqttClient(wifiClient);
ThingsBoard tb(mqttClient, MAX_MESSAGE_SIZE);

DHT20 dht20;

RPC_Response setLedSwitchState(const RPC_Data &data) {
    Serial.println("Received Switch state");
    bool newState = data;
    Serial.print("Switch state change: ");
    Serial.println(newState);
    digitalWrite(LED_PIN, newState);
    attributesChanged = true;
    return RPC_Response("setLedSwitchValue", newState);
}

const std::array<RPC_Callback, 1U> callbacks = {
  RPC_Callback{ "setLedSwitchValue", setLedSwitchState }
};

void processSharedAttributes(const Shared_Attribute_Data &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    if (strcmp(it->key().c_str(), BLINKING_INTERVAL_ATTR) == 0) {
      const uint16_t new_interval = it->value().as<uint16_t>();
      if (new_interval >= BLINKING_INTERVAL_MS_MIN && new_interval <= BLINKING_INTERVAL_MS_MAX) {
        blinkingInterval = new_interval;
        Serial.print("Blinking interval is set to: ");
        Serial.println(new_interval);
      }
    } else if (strcmp(it->key().c_str(), LED_STATE_ATTR) == 0) {
      ledState = it->value().as<bool>();
      digitalWrite(LED_PIN, ledState);
      Serial.print("LED state is set to: ");
      Serial.println(ledState);
    }
  }
  attributesChanged = true;
}

const Shared_Attribute_Callback attributes_callback(&processSharedAttributes, SHARED_ATTRIBUTES_LIST.cbegin(), SHARED_ATTRIBUTES_LIST.cend());
const Attribute_Request_Callback attribute_shared_request_callback(&processSharedAttributes, SHARED_ATTRIBUTES_LIST.cbegin(), SHARED_ATTRIBUTES_LIST.cend());

void InitWiFi() {
  Serial.println("Connecting to AP ...");
  // Attempting to establish a connection to the given WiFi network
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    // Delay 500ms until a connection has been successfully established
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to AP");
}

const bool reconnect() {
  // Check to ensure we aren't connected yet
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return true;
  }
  // If we aren't establish a new connection to the given WiFi network
  InitWiFi();
  return true;
}

void SensorTask(void *pvParameters) {
  dht.begin(); 
  while(1) {
      float temperature = dht.readTemperature(); 
      float humidity = dht.readHumidity();         

      if (isnan(temperature) || isnan(humidity)) {
          Serial.println("Failed to read from DHT sensor!");
      } else {
          Serial.print("Temp: ");
          Serial.print(temperature);
          Serial.print(" *C ");
          Serial.print("Humidity: ");
          Serial.print(humidity);
          Serial.println(" %");
      }
      vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void LEDTask(void *pvParameters) {
  pinMode(LED_PIN, OUTPUT);
  while (true) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

//uint32_t previousDataSend = 0;

void WiFiTask(void *pvParameters) {
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Connecting to WiFi...");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            int retry = 0;
            while (WiFi.status() != WL_CONNECTED && retry < 20) {
                delay(500);
                retry++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("WiFi Connected!");
            } else {
                Serial.println("WiFi Connection Failed!");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000)); // Kiểm tra lại mỗi 30 giây
    }
}

void MQTTTask(void *pvParameters) {
  while (true) {
      if (!tb.connected()) {
          Serial.println("Connecting to ThingsBoard...");
          if (tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
              Serial.println("Connected to ThingsBoard!");
              tb.sendAttributeData("macAddress", WiFi.macAddress().c_str());

              // Đăng ký RPC và nhận thuộc tính chia sẻ
              if (!tb.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
                  Serial.println("Failed to subscribe for RPC");
              }
              if (!tb.Shared_Attributes_Subscribe(attributes_callback)) {
                  Serial.println("Failed to subscribe for shared attribute updates");
              }
          } else {
              Serial.println("Failed to connect to ThingsBoard. Retrying...");
              vTaskDelay(pdMS_TO_TICKS(5000));
              continue;
          }
      }

      // Đọc dữ liệu từ cảm biến
      float temperature = dht.readTemperature();
      float humidity = dht.readHumidity();

      if (!isnan(temperature) && !isnan(humidity)) {
          Serial.print("Sending Temperature: ");
          Serial.print(temperature);
          Serial.print(" °C, Humidity: ");
          Serial.print(humidity);
          Serial.println(" %");

          tb.sendTelemetryData("temperature", temperature);
          tb.sendTelemetryData("humidity", humidity);
      } else {
          Serial.println("Failed to read from DHT sensor!");
      }

      // Gửi dữ liệu WiFi
      tb.sendAttributeData("rssi", WiFi.RSSI());
      tb.sendAttributeData("channel", WiFi.channel());
      tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
      tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
      tb.sendAttributeData("ssid", WiFi.SSID().c_str());

      tb.loop();
      vTaskDelay(pdMS_TO_TICKS(10000));  // Gửi dữ liệu mỗi 10 giây
  }
}



void setup() {
  Serial.begin(115200);
  xTaskCreate(WiFiTask, "WiFiTask", 4096, NULL, 1, NULL);
  xTaskCreate(MQTTTask, "MQTTTask", 4096, NULL, 1, NULL);
  xTaskCreate(SensorTask, "SensorTask", 4096, NULL, 1, NULL);
  xTaskCreate(LEDTask, "LEDTask", 2048, NULL, 1, NULL);
  //xTaskCreate(MQ2Task, "MQ2Task", 2048, NULL, 1, NULL);
}

void loop() {

}