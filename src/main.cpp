#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <Wire.h>

namespace {

constexpr char DEVICE_NAME[] = "ESP32C3-BLE-MQTT";
constexpr char DEFAULT_DEVICE_ID[] = "OY9LRGbg";
constexpr char PREVIOUS_DEFAULT_DEVICE_ID[] = "oWvWIn0N";
constexpr char HID_SERVICE_UUID[] = "1812";

constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
constexpr uint8_t DISPLAY_I2C_SDA_PIN = 5;
constexpr uint8_t DISPLAY_I2C_SCL_PIN = 6;
constexpr uint8_t BOOT_BUTTON_PIN = 9;
constexpr uint8_t PRESS_LED_PIN = 8;

constexpr unsigned long WIFI_CONNECT_WAIT_MS = 15000;
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 5000;
constexpr unsigned long BLE_CONNECT_RETRY_MS = 2000;
constexpr unsigned long BLE_KEEPALIVE_MS = 30000;
constexpr unsigned long DISPLAY_REFRESH_MS = 300;
constexpr unsigned long STATUS_ROTATE_MS = 2500;
constexpr unsigned long HEARTBEAT_MS = 10000;
constexpr unsigned long BUTTON_HOLD_FOR_PORTAL_MS = 4000;
constexpr unsigned long PRESS_LED_PULSE_MS = 120;
constexpr unsigned long GPIO_BUTTON_DEBOUNCE_MS = 30;

constexpr char DEFAULT_MQTT_HOST[] = "broker.emqx.io";
constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
constexpr char DEFAULT_MQTT_USER[] = "";
constexpr char DEFAULT_MQTT_PASS[] = "";
constexpr char DEFAULT_BUTTON1_MAC[] = "";
constexpr char DEFAULT_BUTTON2_MAC[] = "";
constexpr char DEFAULT_BUTTON3_MAC[] = "";
constexpr uint32_t DEFAULT_COOLDOWN_MS = 400;

constexpr char NVS_NAMESPACE[] = "blemqtt";
constexpr char KEY_MQTT_HOST[] = "mqtt_host";
constexpr char KEY_MQTT_PORT[] = "mqtt_port";
constexpr char KEY_MQTT_USER[] = "mqtt_user";
constexpr char KEY_MQTT_PASS[] = "mqtt_pass";
constexpr char KEY_DEVICE_ID[] = "device_id";
constexpr char KEY_BUTTON1_MAC[] = "btn1_mac";
constexpr char KEY_BUTTON2_MAC[] = "btn2_mac";
constexpr char KEY_BUTTON3_MAC[] = "btn3_mac";
constexpr char KEY_COOLDOWN[] = "cooldown";

constexpr size_t BUTTON_COUNT = 3;
const char *BUTTON_LABELS[BUTTON_COUNT] = {"button1", "button2", "button3"};
const uint8_t GPIO_BUTTON_PINS[BUTTON_COUNT] = {0, 1, 2};

struct AppConfig {
  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;
  String deviceId;
  String buttonMacs[BUTTON_COUNT];
  uint32_t cooldownMs;
};

struct ButtonSlot {
  String label;
  String configuredMac;
  NimBLEAddress address = NimBLEAddress("");
  NimBLEClient *client = nullptr;
  bool targetSeen = false;
  bool connected = false;
  bool subscribed = false;
  bool connecting = false;
  bool pressLatched = false;
  bool addressKnown = false;
  unsigned long lastConnectAttemptMs = 0;
  unsigned long lastKeepAliveMs = 0;
  unsigned long lastPublishMs = 0;
  String lastReportHex = "";
};

struct GpioButtonState {
  bool rawPressed = false;
  bool stablePressed = false;
  bool pressLatched = false;
  unsigned long lastRawChangeMs = 0;
};

Preferences prefs;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
U8G2_SSD1306_72X40_ER_F_HW_I2C display(U8G2_R2, U8X8_PIN_NONE);
NimBLEScan *bleScan = nullptr;
AppConfig config;
ButtonSlot buttons[BUTTON_COUNT];
GpioButtonState gpioButtons[BUTTON_COUNT];

unsigned long lastMqttAttemptMs = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastStatusRotateMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long wifiAttemptStartedMs = 0;
unsigned long bootButtonDownMs = 0;
unsigned long pressLedOffAtMs = 0;
bool bootButtonWasPressed = false;
bool portalRequested = false;
bool mqttWasConnected = false;
bool wifiConnectInProgress = false;
bool bleScanStarted = false;
uint8_t statusPage = 0;
String lastSeenSummary = "No BLE target";
String lastPublishSummary = "Waiting";
String bleConnectionSummary = "BLE scan";

String trimCopy(String value) {
  value.trim();
  return value;
}

String normalizeUpperNoSpace(String value) {
  value.trim();
  value.toUpperCase();
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c != ' ' && c != ':' && c != '-') {
      out += c;
    }
  }
  return out;
}

String bytesToHexString(const uint8_t *data, size_t length) {
  static const char *HEX_DIGITS = "0123456789ABCDEF";
  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out += HEX_DIGITS[(data[i] >> 4) & 0x0F];
    out += HEX_DIGITS[data[i] & 0x0F];
  }
  return out;
}

String bytesToHexString(const std::string &value) {
  return bytesToHexString(reinterpret_cast<const uint8_t *>(value.data()),
                          value.length());
}

bool isAllZeroPayload(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) {
      return false;
    }
  }
  return true;
}

String defaultTopic() {
  return String("jj/ble/button/") + config.deviceId;
}

String displayTopicSuffix() {
  return config.deviceId;
}

String displayTopicLine() {
  return String("Topic ") + displayTopicSuffix();
}

String makePressedPayload(const String &buttonLabel) {
  return String("{\"") + buttonLabel + "\":\"pressed\"}";
}

void pulsePressLed() {
  digitalWrite(PRESS_LED_PIN, HIGH);
  pressLedOffAtMs = millis() + PRESS_LED_PULSE_MS;
}

void updatePressLed() {
  if (pressLedOffAtMs && static_cast<long>(millis() - pressLedOffAtMs) < 0) {
    digitalWrite(PRESS_LED_PIN, HIGH);
    return;
  }

  pressLedOffAtMs = 0;
  digitalWrite(PRESS_LED_PIN, mqttClient.connected() ? LOW : HIGH);
}

void initGpioButtons() {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    pinMode(GPIO_BUTTON_PINS[i], INPUT_PULLUP);
    const bool pressed = digitalRead(GPIO_BUTTON_PINS[i]) == LOW;
    gpioButtons[i].rawPressed = pressed;
    gpioButtons[i].stablePressed = pressed;
    gpioButtons[i].pressLatched = pressed;
    gpioButtons[i].lastRawChangeMs = millis();
    Serial.printf("[%10lu] GPIO button init: %s pin=%u state=%s\n", millis(),
                  BUTTON_LABELS[i], GPIO_BUTTON_PINS[i],
                  pressed ? "PRESSED" : "released");
  }
}

void logLine(const String &message) {
  Serial.printf("[%10lu] %s\n", millis(), message.c_str());
}

void logBootBanner() {
  Serial.println();
  Serial.println("===== ESP32-C3 3x BLE HID -> MQTT =====");
  Serial.printf("Build date  : %s %s\n", __DATE__, __TIME__);
  Serial.printf("Chip model  : %s\n", ESP.getChipModel());
  Serial.printf("SDK version : %s\n", ESP.getSdkVersion());
  Serial.println("=======================================");
}

void initButtonSlots() {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    buttons[i].label = BUTTON_LABELS[i];
  }
}

void applyConfigToButtonSlots() {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    buttons[i].configuredMac = config.buttonMacs[i];
    buttons[i].targetSeen = false;
    buttons[i].addressKnown = buttons[i].client && buttons[i].client->isConnected();
    buttons[i].connected = buttons[i].client && buttons[i].client->isConnected();
    buttons[i].subscribed = false;
    buttons[i].connecting = false;
    buttons[i].pressLatched = false;
    buttons[i].lastKeepAliveMs = 0;
    buttons[i].lastReportHex = "";
  }
}

void logConfigSummary() {
  Serial.println("===== Active Config =====");
  Serial.printf("MQTT host   : %s:%u\n", config.mqttHost.c_str(), config.mqttPort);
  Serial.printf("MQTT topic  : %s\n", defaultTopic().c_str());
  Serial.printf("Cooldown ms : %lu\n", static_cast<unsigned long>(config.cooldownMs));
  Serial.printf("Device ID   : %s\n", config.deviceId.c_str());
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    Serial.printf("%s MAC   : %s\n", buttons[i].label.c_str(),
                  buttons[i].configuredMac.length() ? buttons[i].configuredMac.c_str()
                                                    : "(empty)");
  }
  Serial.println("=========================");
}

void loadConfig() {
  prefs.begin(NVS_NAMESPACE, true);
  config.mqttHost = prefs.getString(KEY_MQTT_HOST, DEFAULT_MQTT_HOST);
  config.mqttPort = prefs.getUShort(KEY_MQTT_PORT, DEFAULT_MQTT_PORT);
  config.mqttUser = prefs.getString(KEY_MQTT_USER, DEFAULT_MQTT_USER);
  config.mqttPass = prefs.getString(KEY_MQTT_PASS, DEFAULT_MQTT_PASS);
  config.deviceId = prefs.getString(KEY_DEVICE_ID, DEFAULT_DEVICE_ID);
  config.buttonMacs[0] = prefs.getString(KEY_BUTTON1_MAC, DEFAULT_BUTTON1_MAC);
  config.buttonMacs[1] = prefs.getString(KEY_BUTTON2_MAC, DEFAULT_BUTTON2_MAC);
  config.buttonMacs[2] = prefs.getString(KEY_BUTTON3_MAC, DEFAULT_BUTTON3_MAC);
  config.cooldownMs = prefs.getUInt(KEY_COOLDOWN, DEFAULT_COOLDOWN_MS);
  prefs.end();

  config.mqttHost = trimCopy(config.mqttHost);
  config.mqttUser = trimCopy(config.mqttUser);
  config.mqttPass = trimCopy(config.mqttPass);
  config.deviceId = trimCopy(config.deviceId);
  if (config.deviceId == PREVIOUS_DEFAULT_DEVICE_ID) {
    config.deviceId = DEFAULT_DEVICE_ID;
  }
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    config.buttonMacs[i] = normalizeUpperNoSpace(config.buttonMacs[i]);
  }

  if (!config.mqttPort) config.mqttPort = DEFAULT_MQTT_PORT;
  if (!config.mqttHost.length()) config.mqttHost = DEFAULT_MQTT_HOST;
  if (!config.deviceId.length()) config.deviceId = DEFAULT_DEVICE_ID;
  if (!config.cooldownMs) config.cooldownMs = DEFAULT_COOLDOWN_MS;
}

void saveConfig() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(KEY_MQTT_HOST, config.mqttHost);
  prefs.putUShort(KEY_MQTT_PORT, config.mqttPort);
  prefs.putString(KEY_MQTT_USER, config.mqttUser);
  prefs.putString(KEY_MQTT_PASS, config.mqttPass);
  prefs.putString(KEY_DEVICE_ID, config.deviceId);
  prefs.putString(KEY_BUTTON1_MAC, config.buttonMacs[0]);
  prefs.putString(KEY_BUTTON2_MAC, config.buttonMacs[1]);
  prefs.putString(KEY_BUTTON3_MAC, config.buttonMacs[2]);
  prefs.putUInt(KEY_COOLDOWN, config.cooldownMs);
  prefs.end();
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  const long rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void drawWifiBars(int x, int baselineY, int bars) {
  for (int i = 0; i < 4; ++i) {
    const int barHeight = 2 + i * 2;
    const int barX = x + i * 4;
    const int barY = baselineY - barHeight;
    if (i < bars) {
      display.drawBox(barX, barY, 3, barHeight);
    } else {
      display.drawFrame(barX, barY, 3, barHeight);
    }
  }
}

void drawStatusBar() {
  drawWifiBars(0, 10, wifiBars());
  display.setFont(u8g2_font_4x6_tr);
  bool bleConnected = false;
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    bleConnected = bleConnected || buttons[i].connected;
  }
  display.drawStr(22, 8, bleConnected ? "BLE" : "BLE..");
  display.drawStr(48, 8, mqttClient.connected() ? "MQTT" : "MQ..");
  display.drawHLine(0, 12, 72);
}

String buttonSummaryLine() {
  String summary;
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    if (summary.length()) summary += " ";
    summary += buttons[i].connected ? "UP" : (buttons[i].configuredMac.length() ? ".." : "--");
  }
  return summary;
}

void refreshDisplay(bool force = false) {
  const unsigned long now = millis();
  if (!force && now - lastDisplayRefreshMs < DISPLAY_REFRESH_MS) return;
  lastDisplayRefreshMs = now;
  if (now - lastStatusRotateMs >= STATUS_ROTATE_MS) {
    lastStatusRotateMs = now;
    statusPage = (statusPage + 1) % 2;
  }

  display.clearBuffer();
  drawStatusBar();
  display.setFont(u8g2_font_5x8_tr);
  if (statusPage == 0) {
    display.drawUTF8(0, 28, displayTopicLine().c_str());
    display.drawUTF8(0, 40, buttonSummaryLine().c_str());
  } else {
    display.drawStr(0, 23, "Last");
    display.drawUTF8(0, 33, lastSeenSummary.c_str());
    display.drawUTF8(0, 40, lastPublishSummary.c_str());
  }
  display.sendBuffer();
}

void connectWifiBlocking() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  Serial.printf("[%10lu] WiFi begin: using saved credentials\n", millis());
  WiFi.begin();
  wifiConnectInProgress = true;
  wifiAttemptStartedMs = millis();
  lastWifiAttemptMs = wifiAttemptStartedMs;
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < WIFI_CONNECT_WAIT_MS) {
    refreshDisplay(true);
    delay(50);
  }
  wifiConnectInProgress = WiFi.status() != WL_CONNECTED;
  Serial.printf("[%10lu] WiFi wait done: status=%d connected=%s\n", millis(),
                static_cast<int>(WiFi.status()),
                WiFi.status() == WL_CONNECTED ? "YES" : "NO");
}

void serviceWifiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectInProgress = false;
    return;
  }

  const unsigned long now = millis();
  if (wifiConnectInProgress &&
      now - wifiAttemptStartedMs < WIFI_CONNECT_WAIT_MS) {
    return;
  }

  if (wifiConnectInProgress) {
    Serial.printf("[%10lu] WiFi attempt timed out, will retry\n", now);
    wifiConnectInProgress = false;
  }

  if (lastWifiAttemptMs && now - lastWifiAttemptMs < WIFI_RETRY_MS) return;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  Serial.printf("[%10lu] WiFi retry: using saved credentials\n", now);
  WiFi.begin();
  wifiConnectInProgress = true;
  wifiAttemptStartedMs = now;
  lastWifiAttemptMs = now;
  lastPublishSummary = "WiFi retry";
}

bool connectMqtt() {
  if (mqttClient.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  const unsigned long now = millis();
  if (now - lastMqttAttemptMs < MQTT_RETRY_MS) return false;
  lastMqttAttemptMs = now;

  const String clientId =
      String("esp32c3-ble-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[%10lu] MQTT connect try: host=%s port=%u clientId=%s\n", now,
                config.mqttHost.c_str(), config.mqttPort, clientId.c_str());

  bool connected = false;
  if (config.mqttUser.length()) {
    connected = mqttClient.connect(clientId.c_str(), config.mqttUser.c_str(),
                                   config.mqttPass.c_str());
  } else {
    connected = mqttClient.connect(clientId.c_str());
  }

  if (connected != mqttWasConnected) {
    mqttWasConnected = connected;
    Serial.printf("MQTT connected: %s\n", connected ? "YES" : "NO");
  }
  if (!connected) {
    Serial.printf("[%10lu] MQTT connect failed, state=%d\n", now, mqttClient.state());
  }
  return connected;
}

bool publishButtonPress(ButtonSlot &button, const String &sourceSummary) {
  if (WiFi.status() != WL_CONNECTED || !connectMqtt()) {
    lastPublishSummary = "MQTT offline";
    Serial.printf("[%10lu] MQTT publish skipped: WiFi/MQTT offline, source=%s\n",
                  millis(), sourceSummary.c_str());
    return false;
  }

  const unsigned long now = millis();
  if (now - button.lastPublishMs < config.cooldownMs) {
    lastPublishSummary = "Cooldown";
    Serial.printf("[%10lu] MQTT publish suppressed by cooldown: button=%s remain=%lu ms\n",
                  now, button.label.c_str(),
                  static_cast<unsigned long>(config.cooldownMs - (now - button.lastPublishMs)));
    return false;
  }

  const String topic = defaultTopic();
  const String payload = makePressedPayload(button.label);
  Serial.printf("[%10lu] MQTT publish start: topic=%s payload=%s source=%s\n", now,
                topic.c_str(), payload.c_str(), sourceSummary.c_str());
  const bool ok = mqttClient.publish(topic.c_str(), payload.c_str());
  button.lastPublishMs = now;
  lastSeenSummary = sourceSummary;
  lastPublishSummary = ok ? "MQTT sent" : "Publish fail";
  Serial.printf("MQTT trigger: %s -> %s | %s | result=%s\n", sourceSummary.c_str(),
                topic.c_str(), payload.c_str(), ok ? "OK" : "FAIL");
  return ok;
}

void pollGpioButtons() {
  const unsigned long now = millis();
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    GpioButtonState &state = gpioButtons[i];
    const bool rawPressed = digitalRead(GPIO_BUTTON_PINS[i]) == LOW;

    if (rawPressed != state.rawPressed) {
      state.rawPressed = rawPressed;
      state.lastRawChangeMs = now;
    }

    if (now - state.lastRawChangeMs < GPIO_BUTTON_DEBOUNCE_MS) continue;
    if (rawPressed == state.stablePressed) continue;

    state.stablePressed = rawPressed;
    ButtonSlot &button = buttons[i];

    if (!state.stablePressed) {
      state.pressLatched = false;
      lastSeenSummary = button.label + ":gpio up";
      lastPublishSummary = "GPIO up";
      Serial.printf("[%10lu] GPIO release detected: %s pin=%u\n", now,
                    button.label.c_str(), GPIO_BUTTON_PINS[i]);
      continue;
    }

    if (state.pressLatched) continue;

    state.pressLatched = true;
    pulsePressLed();
    const String sourceSummary = button.label + ":gpio";
    Serial.printf("[%10lu] GPIO press detected: %s pin=%u\n", now,
                  button.label.c_str(), GPIO_BUTTON_PINS[i]);
    publishButtonPress(button, sourceSummary);
  }
}

int buttonIndexFromClient(NimBLEClient *client) {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    if (buttons[i].client == client) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int buttonIndexFromCharacteristic(NimBLERemoteCharacteristic *characteristic) {
  if (!characteristic) return -1;
  NimBLERemoteService *service = characteristic->getRemoteService();
  if (!service) return -1;
  NimBLEClient *client = service->getClient();
  return buttonIndexFromClient(client);
}

void onHidReport(NimBLERemoteCharacteristic *characteristic, uint8_t *data,
                 size_t length, bool isNotify) {
  const int index = buttonIndexFromCharacteristic(characteristic);
  if (index < 0) return;

  ButtonSlot &button = buttons[index];
  const String payloadHex = bytesToHexString(data, length);
  const String charUuid = String(characteristic->getUUID().toString().c_str());
  button.lastReportHex = payloadHex;

  Serial.printf("[%10lu] HID report %s button=%s uuid=%s len=%u data=%s\n", millis(),
                isNotify ? "notify" : "indicate", button.label.c_str(),
                charUuid.c_str(), static_cast<unsigned>(length), payloadHex.c_str());

  if (isAllZeroPayload(data, length)) {
    button.pressLatched = false;
    lastSeenSummary = button.label + ":release";
    lastPublishSummary = "Key up";
    Serial.printf("[%10lu] HID release detected for %s\n", millis(),
                  button.label.c_str());
    return;
  }

  if (button.pressLatched) {
    lastPublishSummary = "HID held";
    Serial.printf("[%10lu] HID report suppressed: press latch already set for %s\n",
                  millis(), button.label.c_str());
    return;
  }

  button.pressLatched = true;
  pulsePressLed();
  publishButtonPress(button, button.label + ":" + payloadHex);
}

class ClientCallbacks : public NimBLEClientCallbacks {
public:
  void onConnect(NimBLEClient *client) override {
    const int index = buttonIndexFromClient(client);
    if (index >= 0) {
      buttons[index].connected = true;
      buttons[index].connecting = false;
      buttons[index].addressKnown = true;
      buttons[index].lastKeepAliveMs = millis();
      bleConnectionSummary = buttons[index].label + ":UP";
      Serial.printf("[%10lu] BLE connected: %s peer=%s mtu=%u\n", millis(),
                    buttons[index].label.c_str(),
                    client->getPeerAddress().toString().c_str(), client->getMTU());
    }
  }

  void onDisconnect(NimBLEClient *client) override {
    const int index = buttonIndexFromClient(client);
    if (index >= 0) {
      buttons[index].connected = false;
      buttons[index].subscribed = false;
      buttons[index].connecting = false;
      buttons[index].pressLatched = false;
      bleConnectionSummary = buttons[index].label + ":scan";
      Serial.printf("[%10lu] BLE disconnected: %s peer=%s\n", millis(),
                    buttons[index].label.c_str(),
                    client->getPeerAddress().toString().c_str());
      if (bleScan && !bleScan->isScanning()) {
        bleScan->start(0, nullptr, false);
        bleScanStarted = true;
      }
    }
  }

  bool onConnParamsUpdateRequest(NimBLEClient *,
                                 const ble_gap_upd_params *) override {
    return true;
  }
};

ClientCallbacks clientCallbacks;

void logRemoteHidTopology(ButtonSlot &button) {
  if (!button.client) return;
  auto *services = button.client->getServices(true);
  if (!services) {
    Serial.printf("[%10lu] BLE service discovery returned null for %s\n", millis(),
                  button.label.c_str());
    return;
  }
  for (auto *service : *services) {
    const String serviceUuid = String(service->getUUID().toString().c_str());
    Serial.printf("[%10lu] BLE service %s: %s\n", millis(), button.label.c_str(),
                  serviceUuid.c_str());
    auto *chars = service->getCharacteristics(true);
    if (!chars) continue;
    for (auto *chr : *chars) {
      const String chrUuid = String(chr->getUUID().toString().c_str());
      Serial.printf(
          "           chr=%s notify=%s indicate=%s read=%s write=%s handle=%u\n",
          chrUuid.c_str(), chr->canNotify() ? "Y" : "N",
          chr->canIndicate() ? "Y" : "N", chr->canRead() ? "Y" : "N",
          chr->canWrite() ? "Y" : "N", chr->getHandle());
    }
  }
}

bool subscribeToHidReports(ButtonSlot &button) {
  if (!button.client) return false;
  NimBLERemoteService *hidService = button.client->getService(HID_SERVICE_UUID);
  if (!hidService) {
    Serial.printf("[%10lu] HID service 0x1812 not found for %s\n", millis(),
                  button.label.c_str());
    return false;
  }

  bool subscribedAny = false;
  auto *chars = hidService->getCharacteristics(true);
  if (!chars) return false;
  for (auto *chr : *chars) {
    if (!(chr->canNotify() || chr->canIndicate())) continue;
    const bool useNotify = chr->canNotify();
    const String chrUuid = String(chr->getUUID().toString().c_str());
    Serial.printf("[%10lu] HID subscribe try: button=%s chr=%s mode=%s\n", millis(),
                  button.label.c_str(), chrUuid.c_str(),
                  useNotify ? "notify" : "indicate");
    const bool ok = chr->subscribe(useNotify, onHidReport, false);
    Serial.printf("[%10lu] HID subscribe %s: button=%s chr=%s\n", millis(),
                  ok ? "OK" : "FAIL", button.label.c_str(), chrUuid.c_str());
    subscribedAny = subscribedAny || ok;
  }
  return subscribedAny;
}

void keepBleConnectionAlive(ButtonSlot &button) {
  if (!button.client || !button.client->isConnected()) return;

  const unsigned long now = millis();
  if (now - button.lastKeepAliveMs < BLE_KEEPALIVE_MS) return;
  button.lastKeepAliveMs = now;

  NimBLERemoteService *batteryService = button.client->getService("180F");
  if (batteryService) {
    NimBLERemoteCharacteristic *batteryLevel =
        batteryService->getCharacteristic("2A19");
    if (batteryLevel && batteryLevel->canRead()) {
      const std::string value = batteryLevel->readValue();
      Serial.printf("[%10lu] BLE keepalive battery: %s len=%u\n", now,
                    button.label.c_str(), static_cast<unsigned>(value.length()));
      return;
    }
  }

  NimBLERemoteService *hidService = button.client->getService(HID_SERVICE_UUID);
  if (!hidService) return;
  auto *chars = hidService->getCharacteristics(true);
  if (!chars) return;
  for (auto *chr : *chars) {
    if (!chr->canRead()) continue;
    const std::string value = chr->readValue();
    Serial.printf("[%10lu] BLE keepalive HID: %s len=%u\n", now,
                  button.label.c_str(), static_cast<unsigned>(value.length()));
    return;
  }
}

bool shouldWatchDevice(NimBLEAdvertisedDevice *device, const String &normalizedMac) {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    if (buttons[i].configuredMac.length() &&
        normalizedMac == buttons[i].configuredMac) {
      return true;
    }
  }
  return false;
}

void logDetailedAdvertisement(NimBLEAdvertisedDevice *device, const char *reason) {
  const String mac = normalizeUpperNoSpace(String(device->getAddress().toString().c_str()));
  const String name = device->haveName() ? String(device->getName().c_str()) : "";
  const String mfg = device->haveManufacturerData()
                         ? bytesToHexString(device->getManufacturerData())
                         : "";
  String uuids;
  for (int i = 0; i < device->getServiceUUIDCount(); ++i) {
    if (uuids.length()) uuids += ",";
    uuids += String(device->getServiceUUID(i).toString().c_str());
  }

  Serial.printf("[%10lu] BLE detail (%s)\n", millis(), reason);
  Serial.printf("           mac=%s name=%s rssi=%d\n", mac.c_str(),
                name.length() ? name.c_str() : "(none)", device->getRSSI());
  Serial.printf("           appearance=%u addrType=%d mfg=%s uuids=%s\n",
                device->getAppearance(), device->getAddressType(),
                mfg.length() ? mfg.c_str() : "(none)",
                uuids.length() ? uuids.c_str() : "(none)");
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
  void onResult(NimBLEAdvertisedDevice *device) override {
    static unsigned long lastDetailedLogMs = 0;
    const unsigned long now = millis();

    const String normalizedMac =
        normalizeUpperNoSpace(String(device->getAddress().toString().c_str()));
    if (!shouldWatchDevice(device, normalizedMac)) {
      return;
    }

    if (now - lastDetailedLogMs >= 700) {
      lastDetailedLogMs = now;
      logDetailedAdvertisement(device, "target-watch");
    }

    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
      ButtonSlot &button = buttons[i];
      if (!button.configuredMac.length()) continue;
      if (normalizedMac != button.configuredMac) continue;
      button.address = device->getAddress();
      button.addressKnown = true;
      button.targetSeen = true;
      lastSeenSummary = button.label + ":seen";
      Serial.printf("[%10lu] BLE target seen: %s mac=%s\n", millis(),
                    button.label.c_str(), normalizedMac.c_str());
    }
  }
};

void startBleScan() {
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  bleScan = NimBLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  bleScan->setInterval(80);
  bleScan->setWindow(45);
  bleScan->setActiveScan(true);
  bleScan->start(0, nullptr, false);
  bleScanStarted = true;
  bleConnectionSummary = "BLE scan";
  Serial.printf("[%10lu] BLE scan started: interval=%d window=%d active=%s\n",
                millis(), 80, 45, "YES");
}

void ensureButtonConnection(ButtonSlot &button) {
  if (!button.configuredMac.length()) return;
  if (!button.targetSeen && !button.addressKnown) return;
  if (button.connecting) return;
  if (button.client && button.client->isConnected()) return;

  const unsigned long now = millis();
  if (now - button.lastConnectAttemptMs < BLE_CONNECT_RETRY_MS) return;
  button.lastConnectAttemptMs = now;
  button.connecting = true;
  bleConnectionSummary = button.label + ":conn";

  if (bleScan && bleScan->isScanning()) {
    bleScan->stop();
  }

  if (!button.client) {
    button.client = NimBLEDevice::createClient(button.address);
    button.client->setClientCallbacks(&clientCallbacks, false);
    button.client->setConnectTimeout(10);
  } else {
    button.client->setPeerAddress(button.address);
  }

  Serial.printf("[%10lu] BLE connect try: %s mac=%s\n", now,
                button.label.c_str(), button.address.toString().c_str());
  const bool ok = button.client->connect(button.address, true);
  button.connecting = false;

  if (!ok) {
    button.connected = false;
    button.subscribed = false;
    bleConnectionSummary = button.label + ":fail";
    Serial.printf("[%10lu] BLE connect failed: %s err=%d\n", millis(),
                  button.label.c_str(), button.client->getLastError());
    if (bleScan && !bleScan->isScanning()) {
      bleScan->start(0, nullptr, false);
      bleScanStarted = true;
    }
    return;
  }

  button.client->updateConnParams(12, 24, 0, 400);
  logRemoteHidTopology(button);
  button.subscribed = subscribeToHidReports(button);
  button.connected = true;
  button.addressKnown = true;
  button.lastKeepAliveMs = millis();
  bleConnectionSummary = button.subscribed ? button.label + ":HID"
                                           : button.label + ":noN";

  if (bleScan && !bleScan->isScanning()) {
    bleScan->start(0, nullptr, false);
    bleScanStarted = true;
  }
}

void ensureBleConnections() {
  bool shouldScan = false;
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    shouldScan = shouldScan ||
                 (buttons[i].configuredMac.length() && !buttons[i].connected);
    ensureButtonConnection(buttons[i]);
    keepBleConnectionAlive(buttons[i]);
  }
  if (shouldScan && bleScan && !bleScan->isScanning()) {
    bleScan->start(0, nullptr, false);
    bleScanStarted = true;
    bleConnectionSummary = "BLE scan";
  }
}

void runConfigPortal() {
  Serial.println("Starting portal");
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tr);
  display.drawStr(0, 12, "Portal mode");
  display.drawStr(0, 24, "Connect AP:");
  display.drawStr(0, 36, DEVICE_NAME);
  display.sendBuffer();

  mqttClient.disconnect();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  delay(200);

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(300);

  char mqttHost[64];
  char mqttPort[8];
  char mqttUser[32];
  char mqttPass[32];
  char deviceId[24];
  char button1Mac[32];
  char button2Mac[32];
  char button3Mac[32];
  char cooldown[12];

  config.mqttHost.toCharArray(mqttHost, sizeof(mqttHost));
  snprintf(mqttPort, sizeof(mqttPort), "%u", config.mqttPort);
  config.mqttUser.toCharArray(mqttUser, sizeof(mqttUser));
  config.mqttPass.toCharArray(mqttPass, sizeof(mqttPass));
  config.deviceId.toCharArray(deviceId, sizeof(deviceId));
  config.buttonMacs[0].toCharArray(button1Mac, sizeof(button1Mac));
  config.buttonMacs[1].toCharArray(button2Mac, sizeof(button2Mac));
  config.buttonMacs[2].toCharArray(button3Mac, sizeof(button3Mac));
  snprintf(cooldown, sizeof(cooldown), "%lu",
           static_cast<unsigned long>(config.cooldownMs));

  WiFiManagerParameter pMqttHost("mqtt_host", "MQTT Host", mqttHost,
                                 sizeof(mqttHost) - 1);
  WiFiManagerParameter pMqttPort("mqtt_port", "MQTT Port", mqttPort,
                                 sizeof(mqttPort) - 1);
  WiFiManagerParameter pMqttUser("mqtt_user", "MQTT User", mqttUser,
                                 sizeof(mqttUser) - 1);
  WiFiManagerParameter pMqttPass("mqtt_pass", "MQTT Pass", mqttPass,
                                 sizeof(mqttPass) - 1);
  WiFiManagerParameter pDeviceId("device_id", "Device ID", deviceId,
                                 sizeof(deviceId) - 1);
  WiFiManagerParameter pButton1Mac("btn1_mac", "button1 MAC", button1Mac,
                                   sizeof(button1Mac) - 1);
  WiFiManagerParameter pButton2Mac("btn2_mac", "button2 MAC", button2Mac,
                                   sizeof(button2Mac) - 1);
  WiFiManagerParameter pButton3Mac("btn3_mac", "button3 MAC", button3Mac,
                                   sizeof(button3Mac) - 1);
  WiFiManagerParameter pCooldown("cooldown", "Cooldown ms", cooldown,
                                 sizeof(cooldown) - 1);

  wm.addParameter(&pMqttHost);
  wm.addParameter(&pMqttPort);
  wm.addParameter(&pMqttUser);
  wm.addParameter(&pMqttPass);
  wm.addParameter(&pDeviceId);
  wm.addParameter(&pButton1Mac);
  wm.addParameter(&pButton2Mac);
  wm.addParameter(&pButton3Mac);
  wm.addParameter(&pCooldown);

  const bool connected = wm.startConfigPortal(DEVICE_NAME);
  Serial.printf("Portal WiFi result: %s\n", connected ? "saved/connected" : "timeout/exit");

  config.mqttHost = trimCopy(String(pMqttHost.getValue()));
  config.mqttPort = static_cast<uint16_t>(atoi(pMqttPort.getValue()));
  config.mqttUser = trimCopy(String(pMqttUser.getValue()));
  config.mqttPass = trimCopy(String(pMqttPass.getValue()));
  config.deviceId = trimCopy(String(pDeviceId.getValue()));
  config.buttonMacs[0] = normalizeUpperNoSpace(String(pButton1Mac.getValue()));
  config.buttonMacs[1] = normalizeUpperNoSpace(String(pButton2Mac.getValue()));
  config.buttonMacs[2] = normalizeUpperNoSpace(String(pButton3Mac.getValue()));
  config.cooldownMs =
      static_cast<uint32_t>(strtoul(pCooldown.getValue(), nullptr, 10));

  if (!config.mqttPort) config.mqttPort = DEFAULT_MQTT_PORT;
  if (!config.mqttHost.length()) config.mqttHost = DEFAULT_MQTT_HOST;
  if (!config.deviceId.length()) config.deviceId = DEFAULT_DEVICE_ID;
  if (!config.cooldownMs) config.cooldownMs = DEFAULT_COOLDOWN_MS;

  saveConfig();
  mqttClient.setServer(config.mqttHost.c_str(), config.mqttPort);
  applyConfigToButtonSlots();
  logConfigSummary();
  lastPublishSummary = connected ? "Portal saved" : "Portal timeout";

  if (connected) {
    display.clearBuffer();
    display.setFont(u8g2_font_5x8_tr);
    display.drawStr(0, 12, "Portal saved");
    display.drawStr(0, 24, "Restarting...");
    display.sendBuffer();
    delay(1200);
    ESP.restart();
  }

  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    if (buttons[i].client && buttons[i].client->isConnected()) {
      buttons[i].client->disconnect();
    }
  }
  bleConnectionSummary = "BLE scan";
  if (bleScan && !bleScan->isScanning()) {
    bleScan->start(0, nullptr, false);
    bleScanStarted = true;
  }
  refreshDisplay(true);
}

void pollPortalButton() {
  const bool pressed = digitalRead(BOOT_BUTTON_PIN) == LOW;
  if (pressed && !bootButtonWasPressed) {
    bootButtonDownMs = millis();
    Serial.printf("[%10lu] BOOT button pressed\n", bootButtonDownMs);
  }
  if (!pressed && bootButtonWasPressed) {
    Serial.printf("[%10lu] BOOT button released\n", millis());
    bootButtonDownMs = 0;
  }
  if (pressed && bootButtonDownMs &&
      millis() - bootButtonDownMs >= BUTTON_HOLD_FOR_PORTAL_MS) {
    portalRequested = true;
    Serial.printf("[%10lu] BOOT button long press detected -> portal\n", millis());
    bootButtonDownMs = 0;
  }
  bootButtonWasPressed = pressed;
}

bool bootPortalHoldConfirmed() {
  if (digitalRead(BOOT_BUTTON_PIN) != LOW) return false;

  const unsigned long holdStartMs = millis();
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tr);
  display.drawStr(0, 12, "Hold BOOT");
  display.drawStr(0, 24, "4s Portal");
  display.sendBuffer();

  while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (millis() - holdStartMs >= BUTTON_HOLD_FOR_PORTAL_MS) {
      return true;
    }
    delay(20);
  }

  return false;
}

void reportWifi() {
  static bool wifiWasConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected == wifiWasConnected) return;
  wifiWasConnected = connected;
  if (connected) {
    Serial.printf("WiFi connected: %s RSSI=%ld SSID=%s\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.SSID().c_str());
  } else {
    Serial.println("WiFi disconnected");
  }
}

void heartbeatLog() {
  const unsigned long now = millis();
  if (now - lastHeartbeatMs < HEARTBEAT_MS) return;
  lastHeartbeatMs = now;
  Serial.printf(
      "[%10lu] HEARTBEAT wifi=%s mqtt=%s ble=%s ip=%s rssi=%ld lastSeen=%s lastPub=%s heap=%u\n",
      now, WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
      mqttClient.connected() ? "UP" : "DOWN", buttonSummaryLine().c_str(),
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "0.0.0.0",
      WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, lastSeenSummary.c_str(),
      lastPublishSummary.c_str(), ESP.getFreeHeap());
}

} // namespace

void setup() {
  Serial.begin(115200);
  const unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 3000) {
    delay(10);
  }
  delay(200);
  logBootBanner();

  initButtonSlots();

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(PRESS_LED_PIN, OUTPUT);
  digitalWrite(PRESS_LED_PIN, HIGH);
  initGpioButtons();
  Wire.begin(DISPLAY_I2C_SDA_PIN, DISPLAY_I2C_SCL_PIN);
  display.setI2CAddress(OLED_I2C_ADDRESS << 1);
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tr);
  display.drawStr(0, 12, "3x BLE HID->MQTT");
  display.drawStr(0, 24, "Booting...");
  display.sendBuffer();

  loadConfig();
  applyConfigToButtonSlots();
  mqttClient.setServer(config.mqttHost.c_str(), config.mqttPort);
  logConfigSummary();
  Serial.printf("[%10lu] Setup: BOOT pin state=%s\n", millis(),
                digitalRead(BOOT_BUTTON_PIN) == LOW ? "LOW" : "HIGH");

  if (bootPortalHoldConfirmed()) {
    logLine("BOOT button held for 4 seconds at startup, entering portal");
    runConfigPortal();
  } else {
    connectWifiBlocking();
    if (WiFi.status() != WL_CONNECTED) {
      logLine("WiFi auto connect failed, staying in work mode");
      lastPublishSummary = "WiFi retry";
    }
  }

  startBleScan();
  logLine("BLE scanner ready");
  refreshDisplay(true);
  Serial.println("ESP32-C3 3x BLE HID MQTT trigger ready");
}

void loop() {
  updatePressLed();
  pollPortalButton();
  if (portalRequested) {
    portalRequested = false;
    Serial.printf("[%10lu] Entering portal from loop\n", millis());
    runConfigPortal();
  }

  reportWifi();
  if (WiFi.status() != WL_CONNECTED) {
    serviceWifiConnection();
  }

  if (WiFi.status() == WL_CONNECTED) {
    connectMqtt();
  }
  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  pollGpioButtons();
  ensureBleConnections();
  heartbeatLog();
  refreshDisplay();
  delay(20);
}
