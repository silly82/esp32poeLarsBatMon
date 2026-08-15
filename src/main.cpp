// esp32poeLarsBatMon
//
// ESP32-S3 (Waveshare ESP32-S3-ETH) connects to the boat's battery BMS
// ("R-12100BNNH19-C01278", MAC c8:47:80:70:44:d6 — a Redodo Power 12.8V
// 100Ah LiFePO4 pack) over BLE GATT, decodes pack voltage/current/SoC/
// cell voltages/temperatures, and publishes them over PoE Ethernet via
// MQTT.
//
// Protocol: LiTime/Redodo/PowerQueen family (NOT JBD/Xiaoxiang DD-A5,
// despite sharing the same 0xFFE0/0xFFE1/0xFFE2 GATT layout - confirmed
// by first trying the JBD command set and getting no response at all).
// Verified against the working, real-world implementation in
// https://github.com/rubenmuehlhans/litime-ble-hacs (Home Assistant
// integration) and cross-referenced with
// https://github.com/va13ak/esp_redodo_bms (ESPHome).
//
// Command frame (8 bytes, written to 0xFFE2):
//   {0x00, 0x00, 0x04, 0x01, CMD, 0x55, 0xAA, CHECKSUM}
//   CHECKSUM = (0x04 + CMD) & 0xFF
// CMD_QUERY_STATUS = 0x13 requests the full status frame.
//
// Status response (notified on 0xFFE1, little-endian, may arrive
// fragmented across multiple ~20-byte BLE packets - a new frame starts
// whenever byte[2] == 0x65, subsequent fragments are appended until the
// buffer reaches MIN_RESPONSE_LENGTH):
//   12-15  total voltage      uint32  mV
//   16-47  16x cell voltage   uint16  mV each (0 = unpopulated)
//   48-51  current            int32   mA, negative = discharge
//   52-53  cell temperature   int16   deg C
//   54-55  MOSFET temperature int16   deg C
//   62-63  remaining capacity uint16  x0.01 Ah
//   64-65  full capacity      uint16  x0.01 Ah
//   68-71  heat state         uint32  bit 0x80 = discharge disabled
//   76-79  protection flags   uint32
//   80-83  failure flags      uint32
//   84-87  balancing state    uint32  non-zero = balancing
//   88-89  battery state      uint16  0=discharging 1=charging 4=charge disabled
//   90-91  state of charge    uint16  %
//   92-93  state of health    uint16  %
//   96-99  discharge cycles   uint32
//   100-103 total discharge   uint32  mAh

#include <Arduino.h>
#include <ETH.h>
#include <SPI.h>
#define MQTT_MAX_PACKET_SIZE 2048
#define MQTT_KEEPALIVE 60
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <cstring>

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
static const char *TOPIC_STATUS = "LiFePo01/status";
static const char *TOPIC_BATTERY = "LiFePo01/battery";
static const char *TOPIC_BATTERY_CELLS = "LiFePo01/battery/cells";

static const uint32_t MQTT_RECONNECT_INTERVAL_MS = 5000;

static bool ethConnected = false;
static NetworkClient netClient;
static PubSubClient mqttClient(netClient);

// --- BMS connection (LiTime/Redodo protocol over BLE) ---
static const char *BMS_MAC = "c8:47:80:70:44:d6";
static const uint16_t BMS_SERVICE_UUID = 0xFFE0;
static const uint16_t BMS_NOTIFY_CHAR_UUID = 0xFFE1;
static const uint16_t BMS_WRITE_CHAR_UUID = 0xFFE2;
static const uint32_t BMS_SCAN_TIME_S = 8;
static const uint32_t BMS_RESPONSE_TIMEOUT_MS = 5000;
static const uint32_t BMS_POST_SUBSCRIBE_DELAY_MS = 1000;

static const uint8_t CMD_QUERY_STATUS = 0x13;
static const size_t MIN_RESPONSE_LENGTH = 104;
static const size_t RESPONSE_MARKER_OFFSET = 2;
static const uint8_t RESPONSE_MARKER_VALUE = 0x65;
static const int MAX_CELLS = 16;

static BLEClient *bmsClient = nullptr;
static BLERemoteCharacteristic *bmsNotifyChar = nullptr;
static BLERemoteCharacteristic *bmsWriteChar = nullptr;
// Set by BmsFinderCallback during a scan, consumed right after connecting.
// Reconnects are rare (only after a lost BLE link), so we accept the small
// one-object leak on a failed connect rather than risk freeing something
// the client library may still reference internally.
static BLEAdvertisedDevice *foundBmsDevice = nullptr;

static const size_t BMS_BUF_CAPACITY = 160;
static uint8_t bmsBuf[BMS_BUF_CAPACITY];
static size_t bmsBufLen = 0;
static bool bmsFrameReady = false;

class BmsFinderCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    if (foundBmsDevice != nullptr) {
      return;  // already found this scan
    }
    if (strcmp(device.getAddress().toString().c_str(), BMS_MAC) == 0) {
      foundBmsDevice = new BLEAdvertisedDevice(device);
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
  doc["bms_connected"] = (bmsClient != nullptr && bmsClient->isConnected());
  String out;
  serializeJson(doc, out);
  mqttClient.publish(TOPIC_STATUS, out.c_str(), true);
}

// --- BMS frame handling ---

static inline uint16_t readU16LE(const uint8_t *d, size_t offset) {
  return (uint16_t)d[offset] | ((uint16_t)d[offset + 1] << 8);
}

static inline uint32_t readU32LE(const uint8_t *d, size_t offset) {
  return (uint32_t)d[offset] | ((uint32_t)d[offset + 1] << 8) | ((uint32_t)d[offset + 2] << 16) | ((uint32_t)d[offset + 3] << 24);
}

void onBmsNotify(BLERemoteCharacteristic *chr, uint8_t *data, size_t length, bool isNotify) {
  (void)chr;
  (void)isNotify;
  if (length == 0) {
    return;
  }
  bool isFrameStart = length > RESPONSE_MARKER_OFFSET && data[RESPONSE_MARKER_OFFSET] == RESPONSE_MARKER_VALUE;
  if (isFrameStart) {
    bmsBufLen = 0;
  } else if (bmsBufLen == 0) {
    return;  // continuation fragment with no frame in progress, ignore
  }
  if (bmsBufLen + length > BMS_BUF_CAPACITY) {
    bmsBufLen = 0;
    return;
  }
  memcpy(bmsBuf + bmsBufLen, data, length);
  bmsBufLen += length;
  if (bmsBufLen >= MIN_RESPONSE_LENGTH) {
    bmsFrameReady = true;
  }
}

bool sendBmsCommand(uint8_t cmd) {
  if (bmsWriteChar == nullptr) {
    return false;
  }
  uint8_t frame[8] = {0x00, 0x00, 0x04, 0x01, cmd, 0x55, 0xAA, (uint8_t)(0x04 + cmd)};
  bmsBufLen = 0;
  bmsFrameReady = false;
  bool useResponse = !bmsWriteChar->canWriteNoResponse();
  bool ok = bmsWriteChar->writeValue(frame, sizeof(frame), useResponse);
  if (!ok) {
    Serial.printf("BMS: failed to write command 0x%02X\n", cmd);
  }
  return ok;
}

bool waitForBmsFrame(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (bmsFrameReady) {
      return true;
    }
    delay(10);
  }
  return false;
}

void parseStatus(JsonDocument &doc) {
  uint8_t *d = bmsBuf;
  float totalVoltage = readU32LE(d, 12) / 1000.0f;
  float current = (int32_t)readU32LE(d, 48) / 1000.0f;

  doc["total_voltage_v"] = totalVoltage;
  doc["current_a"] = current;
  doc["power_w"] = totalVoltage * current;
  doc["cell_temperature_c"] = (int16_t)readU16LE(d, 52);
  doc["mosfet_temperature_c"] = (int16_t)readU16LE(d, 54);
  doc["remaining_capacity_ah"] = readU16LE(d, 62) / 100.0f;
  doc["full_charge_capacity_ah"] = readU16LE(d, 64) / 100.0f;

  uint32_t heatState = readU32LE(d, 68);
  doc["discharge_enabled"] = (heatState & 0x80) == 0;
  doc["protection_flags"] = readU32LE(d, 76);
  doc["failure_flags"] = readU32LE(d, 80);
  doc["balancing"] = readU32LE(d, 84) != 0;

  uint16_t batteryState = readU16LE(d, 88);
  doc["charging"] = batteryState == 0x0001;
  doc["discharging"] = batteryState == 0x0000 && current < 0;
  doc["charge_enabled"] = batteryState != 0x0004;

  doc["soc_percent"] = readU16LE(d, 90);
  doc["soh_percent"] = readU16LE(d, 92);
  doc["discharge_cycles"] = readU32LE(d, 96);
  doc["total_discharge_ah"] = readU32LE(d, 100) / 1000.0f;
}

void parseCellVoltages(JsonArray &cells) {
  uint8_t *d = bmsBuf;
  for (int i = 0; i < MAX_CELLS; i++) {
    uint16_t raw = readU16LE(d, 16 + i * 2);
    if (raw == 0) {
      continue;  // unpopulated cell slot
    }
    cells.add(raw / 1000.0f);
  }
}

void pollAndPublishStatus() {
  if (!sendBmsCommand(CMD_QUERY_STATUS)) {
    return;
  }
  if (!waitForBmsFrame(BMS_RESPONSE_TIMEOUT_MS)) {
    Serial.println("BMS: status response timeout");
    return;
  }

  JsonDocument doc;
  parseStatus(doc);
  String out;
  serializeJson(doc, out);
  Serial.printf("BMS status: %s\n", out.c_str());
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_BATTERY, out.c_str(), true);
  }

  JsonDocument cellsDoc;
  JsonArray cells = cellsDoc.to<JsonArray>();
  parseCellVoltages(cells);
  String cellsOut;
  serializeJson(cellsDoc, cellsOut);
  Serial.printf("BMS cell voltages: %s\n", cellsOut.c_str());
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_BATTERY_CELLS, cellsOut.c_str(), true);
  }
}

bool bmsIsConnected() {
  return bmsClient != nullptr && bmsClient->isConnected() && bmsNotifyChar != nullptr && bmsWriteChar != nullptr;
}

bool findAndConnectBms() {
  Serial.println("BMS not connected, scanning...");
  foundBmsDevice = nullptr;
  bmsNotifyChar = nullptr;
  bmsWriteChar = nullptr;

  BLEScan *scan = BLEDevice::getScan();
  scan->start(BMS_SCAN_TIME_S, false);
  scan->clearResults();

  if (foundBmsDevice == nullptr) {
    Serial.println("BMS: not found in scan");
    return false;
  }

  if (bmsClient == nullptr) {
    bmsClient = BLEDevice::createClient();
  }
  if (!bmsClient->connect(foundBmsDevice)) {
    Serial.println("BMS: connect failed");
    return false;
  }

  BLERemoteService *svc = bmsClient->getService(BLEUUID(BMS_SERVICE_UUID));
  if (svc == nullptr) {
    Serial.println("BMS: service 0xFFE0 not found");
    bmsClient->disconnect();
    return false;
  }
  bmsNotifyChar = svc->getCharacteristic(BLEUUID(BMS_NOTIFY_CHAR_UUID));
  bmsWriteChar = svc->getCharacteristic(BLEUUID(BMS_WRITE_CHAR_UUID));
  if (bmsNotifyChar == nullptr || bmsWriteChar == nullptr) {
    Serial.println("BMS: characteristic 0xFFE1/0xFFE2 not found");
    bmsClient->disconnect();
    return false;
  }
  if (bmsNotifyChar->canNotify()) {
    bmsNotifyChar->registerForNotify(onBmsNotify);
  }

  // The BMS needs a moment to settle after the notify subscription before
  // it will act on commands (matches the working litime-ble-hacs/ESPHome
  // implementations).
  delay(BMS_POST_SUBSCRIBE_DELAY_MS);
  Serial.println("BMS: connected");
  return true;
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
  scan->setAdvertisedDeviceCallbacks(new BmsFinderCallback(), true);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setDuplicateFilter(true);
}

void loop() {
  ensureMqttConnected();
  mqttClient.loop();

  if (!bmsIsConnected()) {
    findAndConnectBms();
  }

  if (bmsIsConnected()) {
    pollAndPublishStatus();
    mqttClient.loop();
  }

  publishHeartbeat();
  mqttClient.loop();

  delay(2000);
}
