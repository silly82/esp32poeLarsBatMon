// esp32poeLarsBatMon
//
// ESP32-S3 (Waveshare ESP32-S3-ETH) scans nearby BLE devices to identify the
// boat's battery BMS, and publishes over PoE Ethernet via MQTT:
//   - <TOPIC_STATUS>: retained heartbeat (IP, uptime, free heap)
//   - <TOPIC_BLE_SCAN>: raw BLE scan hits (MAC/RSSI/name/manufacturer data)
//     as JSON, so the BMS can be identified from the MQTT log without a
//     serial connection.
// The BMS's own advertisement format is not decoded yet (Phase 2); once
// known, its values replace/augment the raw scan dump.

#include <Arduino.h>
#include <ETH.h>
#include <SPI.h>
#define MQTT_MAX_PACKET_SIZE 4096
#define MQTT_KEEPALIVE 60
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// Waveshare ESP32-S3-ETH: W5500 Ethernet over SPI.
#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR 1
#define ETH_PHY_CS   14
#define ETH_PHY_IRQ  10
#define ETH_PHY_RST  9
#define ETH_SPI_SCK  13
#define ETH_SPI_MISO 12
#define ETH_SPI_MOSI 11

static const char *MQTT_HOST = "192.168.24.213";
static const uint16_t MQTT_PORT = 1883;
static const char *MQTT_CLIENT_ID = "esp32poeLarsBatMon";
static const char *TOPIC_STATUS = "esp32poeLarsBatMon/status";
static const char *TOPIC_BLE_SCAN = "esp32poeLarsBatMon/ble/scan";

static const uint32_t SCAN_TIME_S = 10;
static const uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;
static const size_t MAX_REPORTED_DEVICES = 15;

static bool ethConnected = false;
static NetworkClient netClient;
static PubSubClient mqttClient(netClient);

static JsonDocument bleScanDoc;
static JsonArray bleDevices;

class AdvertisedDeviceCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    Serial.printf("MAC=%s RSSI=%d Name=\"%s\"",
                   device.getAddress().toString().c_str(),
                   device.getRSSI(),
                   device.haveName() ? device.getName().c_str() : "");

    String mfgHex;
    if (device.haveManufacturerData()) {
      String data = device.getManufacturerData();
      mfgHex.reserve(data.length() * 2);
      char byteBuf[3];
      for (size_t i = 0; i < data.length(); i++) {
        snprintf(byteBuf, sizeof(byteBuf), "%02X", static_cast<unsigned char>(data[i]));
        mfgHex += byteBuf;
      }
      Serial.printf(" MfgData=%s", mfgHex.c_str());
    }

    if (device.haveServiceUUID()) {
      Serial.printf(" ServiceUUID=%s", device.getServiceUUID().toString().c_str());
    }
    Serial.println();

    if (bleDevices.size() >= MAX_REPORTED_DEVICES) {
      return;
    }
    JsonObject o = bleDevices.add<JsonObject>();
    o["mac"] = device.getAddress().toString();
    o["rssi"] = device.getRSSI();
    if (device.haveName()) {
      o["name"] = device.getName();
    }
    if (mfgHex.length() > 0) {
      o["mfg"] = mfgHex;
    }
    if (device.haveServiceUUID()) {
      o["uuid"] = device.getServiceUUID().toString();
    }
  }
};

void onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname("esp32poeLarsBatMon");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethConnected = true;
      Serial.printf("ETH got IP: %s\n", ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_LOST_IP:
    case ARDUINO_EVENT_ETH_STOP:
      ethConnected = false;
      break;
    default:
      break;
  }
}

void ensureMqttConnected() {
  if (!ethConnected || mqttClient.connected()) {
    return;
  }
  static uint32_t lastAttemptMs = 0;
  if (millis() - lastAttemptMs < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }
  lastAttemptMs = millis();

  Serial.printf("MQTT connecting to %s:%u ... ", MQTT_HOST, MQTT_PORT);
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("connected");
    mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\"}", true);
  } else {
    Serial.printf("failed, rc=%d\n", mqttClient.state());
  }
}

void publishHeartbeat() {
  if (!mqttClient.connected()) {
    return;
  }
  JsonDocument doc;
  doc["status"] = "online";
  doc["ip"] = ETH.localIP().toString();
  doc["uptime_s"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  String out;
  serializeJson(doc, out);
  mqttClient.publish(TOPIC_STATUS, out.c_str(), true);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("esp32poeLarsBatMon starting");

  Network.onEvent(onEthEvent);
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, SPI);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  BLEDevice::init("esp32poeLarsBatMon");
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallback(), true);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  // Without this, a device that re-advertises very rapidly (seen on this
  // boat's dock: a MAC flooding Apple continuity-style ads many times per
  // second) starves the callback task and corrupts Serial output.
  scan->setDuplicateFilter(true);
}

void loop() {
  ensureMqttConnected();
  mqttClient.loop();

  bleScanDoc.clear();
  bleDevices = bleScanDoc.to<JsonArray>();

  Serial.println("--- Scan start ---");
  BLEScan *scan = BLEDevice::getScan();
  scan->start(SCAN_TIME_S, false);
  scan->clearResults();
  Serial.println("--- Scan end ---");

  mqttClient.loop();
  if (mqttClient.connected() && bleDevices.size() > 0) {
    String out;
    serializeJson(bleScanDoc, out);
    mqttClient.publish(TOPIC_BLE_SCAN, out.c_str());
  }

  publishHeartbeat();
  mqttClient.loop();

  delay(2000);
}
