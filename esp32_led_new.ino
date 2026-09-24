#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ================= НАСТРОЙКИ =================
#define DATA_PIN        4
#define MAX_LEDS        32
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define DNS_PORT        53
#define WIFI_RETRY_MS   20000
#define BTN_PIN         9            // BOOT на Super Mini
#define RESET_HOLD_MS   5000
#define MQTT_API_HOST   "dash.wqtt.ru"
#define MQTT_BUF_SIZE   256
#define MQTT_RECONNECT_MS 5000
#define WIFI_CONNECT_MS 15000

// ================= LED ========================
CRGB leds[MAX_LEDS];
uint16_t ledCount = 32;

// ================= DEVICE STATE =================
uint8_t currentR = 250, currentG = 170, currentB = 20;
uint8_t hue = 20, sat = 210;
uint8_t brightness = 200;
uint8_t lastBrightness = 200;
uint8_t effectSpeed = 70;
uint32_t lastFrame = 0;
uint8_t effect = 1;
uint8_t nextEffect = 1;
uint8_t effectBlend = 0;

bool enabled = true;
bool powerOn = true;
bool ledsDirty = true;               // перерисовать при выключении один раз

// ================= WIFI / MQTT =================
Preferences prefs;
DNSServer dnsServer;
WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Фиксированные буферы — меньше фрагментации кучи, чем String
char wifiSsid[33] = "";
char wifiPass[65] = "";
char mqttServer[64] = "";
char mqttUser[48] = "";
char mqttPass[48] = "";
char mqttToken[64] = "";
char mqttClientId[32] = "";
uint16_t mqttPort = 1883;

char topicEffect[40] = "";
char topicBrightness[40] = "";
char topicColor[40] = "";
char topicPower[40] = "";
char topicEffectState[48] = "";
char topicBrightnessState[48] = "";
char topicColorState[48] = "";
char topicPowerState[48] = "";

char deviceName[32] = "Лампа";
char roomName[32] = "Комната";
uint32_t deviceId = 0;

bool apMode = false;
uint32_t wifiFailTimer = 0;
uint32_t mqttLastAttempt = 0;

struct WifiNet {
  char ssid[33];
  int32_t rssi;
};
static WifiNet wifiNets[16];
static uint8_t wifiNetCount = 0;

bool btnPressed = false;
uint32_t btnPressTime = 0;

// ================= HTML (во flash) =================
static const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ru"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Setup</title>
<style>
html{font-family:Arial;color:#dcdcdc}body{background:#1c1638;display:flex;flex-direction:column;align-items:center;padding:40px 16px}
form{width:100%;max-width:320px}label{display:block;margin-bottom:5px}.form-item{margin-bottom:16px}
input,select{padding:8px;border-radius:5px;border:none;width:100%;box-sizing:border-box}
button{padding:15px 30px;border:none;border-radius:5px;background:#4f3cab;color:#dcdcdc;font-weight:bold;font-size:1.1rem;margin-top:12px}
.row{display:flex;justify-content:space-between;align-items:center;margin-bottom:5px}.row a{color:#bbb;font-size:.85rem}
</style></head><body>
<h2>Настройки WiFi и MQTT</h2>
<form action="/save" method="post">
<h3>WiFi</h3>
<div class="form-item"><div class="row"><label for="ssid">SSID</label><a href="/rescan">Обновить</a></div>
<select name="ssid" id="ssid">%%WIFI_OPTIONS%%</select></div>
<div class="form-item"><label>Или другая сеть</label><input name="ssid_custom" maxlength="32"></div>
<div class="form-item"><label>Пароль</label><input type="password" name="password" maxlength="64"></div>
<h3>MQTT</h3>
<div class="form-item"><label>MQTT сервер</label><input name="mqtt_server" required maxlength="63"></div>
<div class="form-item"><label>MQTT порт</label><input type="number" name="mqtt_port" value="1883" required></div>
<div class="form-item"><label>MQTT логин</label><input name="mqtt_user" required maxlength="47"></div>
<div class="form-item"><label>MQTT пароль</label><input type="password" name="mqtt_pass" required maxlength="47"></div>
<div class="form-item"><label>MQTT token</label><input name="mqtt_token" required maxlength="63"></div>
<h3>Комната</h3>
<div class="form-item"><label>Название комнаты</label><input name="room_name" value="Комната" required maxlength="31"></div>
<div class="form-item"><label>Название устройства</label><input name="device_name" value="Лампа" required maxlength="31"></div>
<button type="submit">Сохранить</button>
</form></body></html>
)rawliteral";

static const char DONE_HTML[] PROGMEM =
  "<html><body style='font-family:Arial;background:#1c1638;color:#dcdcdc;text-align:center;padding:40px'>"
  "<h3>Сохранено. Перезагрузка...</h3></body></html>";

// ================= HELPERS =================
static void safeCopy(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return;
  size_t n = src.length();
  if (n >= dstSize) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

static String htmlEscape(const char* s) {
  String out;
  if (!s) return out;
  out.reserve(strlen(s) + 8);
  for (; *s; s++) {
    switch (*s) {
      case '&':  out += F("&amp;"); break;
      case '<':  out += F("&lt;"); break;
      case '>':  out += F("&gt;"); break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default:   out += *s; break;
    }
  }
  return out;
}

void scanWifiNetworks() {
  wifiNetCount = 0;
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    WiFi.scanDelete();
    return;
  }

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    int32_t rssi = WiFi.RSSI(i);

    int dup = -1;
    for (uint8_t j = 0; j < wifiNetCount; j++) {
      if (ssid == wifiNets[j].ssid) { dup = j; break; }
    }
    if (dup >= 0) {
      if (rssi > wifiNets[dup].rssi) wifiNets[dup].rssi = rssi;
      continue;
    }

    if (wifiNetCount >= 16) {
      uint8_t weakest = 0;
      for (uint8_t j = 1; j < wifiNetCount; j++) {
        if (wifiNets[j].rssi < wifiNets[weakest].rssi) weakest = j;
      }
      if (rssi <= wifiNets[weakest].rssi) continue;
      safeCopy(wifiNets[weakest].ssid, sizeof(wifiNets[weakest].ssid), ssid);
      wifiNets[weakest].rssi = rssi;
      continue;
    }

    safeCopy(wifiNets[wifiNetCount].ssid, sizeof(wifiNets[wifiNetCount].ssid), ssid);
    wifiNets[wifiNetCount].rssi = rssi;
    wifiNetCount++;
  }
  WiFi.scanDelete();

  for (uint8_t i = 0; i < wifiNetCount; i++) {
    for (uint8_t j = i + 1; j < wifiNetCount; j++) {
      if (wifiNets[j].rssi > wifiNets[i].rssi) {
        WifiNet tmp = wifiNets[i];
        wifiNets[i] = wifiNets[j];
        wifiNets[j] = tmp;
      }
    }
  }
  Serial.printf("WiFi scan: %u nets\n", wifiNetCount);
}

static String buildWifiOptions() {
  String out;
  out.reserve(wifiNetCount * 80);
  for (uint8_t i = 0; i < wifiNetCount; i++) {
    out += F("<option value=\"");
    out += htmlEscape(wifiNets[i].ssid);
    out += '"';
    if (i == 0) out += F(" selected");
    out += '>';
    out += htmlEscape(wifiNets[i].ssid);
    out += F(" (");
    out += wifiNets[i].rssi;
    out += F(" dBm)</option>");
  }
  return out;
}

static void buildStateTopics() {
  snprintf(topicPowerState, sizeof(topicPowerState), "%s/state", topicPower);
  snprintf(topicEffectState, sizeof(topicEffectState), "%s/state", topicEffect);
  snprintf(topicColorState, sizeof(topicColorState), "%s/state", topicColor);
  snprintf(topicBrightnessState, sizeof(topicBrightnessState), "%s/state", topicBrightness);
}

static void buildMqttClientId() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttClientId, sizeof(mqttClientId), "lamp-%04X%08X",
           (uint16_t)(mac >> 32), (uint32_t)mac);
}

static void buildTopicRoot(char* root, size_t rootSize) {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(root, rootSize, "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
}

bool parseRgb(const char* payload, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!payload) return false;
  const char* p2 = strchr(payload, ',');
  if (!p2) return false;
  const char* p3 = strchr(p2 + 1, ',');
  if (!p3) return false;
  r = (uint8_t)constrain(atoi(payload), 0, 255);
  g = (uint8_t)constrain(atoi(p2 + 1), 0, 255);
  b = (uint8_t)constrain(atoi(p3 + 1), 0, 255);
  return true;
}

static void jsonEscapeTo(const char* s, char* dst, size_t dstSize) {
  if (!dst || dstSize == 0) return;
  size_t j = 0;
  if (s) {
    for (; *s && j + 2 < dstSize; s++) {
      char c = *s;
      if (c == '"' || c == '\\') {
        if (j + 3 >= dstSize) break;
        dst[j++] = '\\';
      }
      if ((uint8_t)c >= 32) dst[j++] = c;
    }
  }
  dst[j] = '\0';
}

static uint32_t parseDeviceId(const String& response) {
  int key = response.indexOf("device_id");
  if (key < 0) return 0;
  int colon = response.indexOf(':', key);
  if (colon < 0) return 0;
  return (uint32_t)response.substring(colon + 1).toInt();
}

void showStatus(CRGB color) {
  FastLED.clear();
  uint16_t n = ledCount < 3 ? ledCount : 3;
  for (uint16_t i = 0; i < n; i++) leds[i] = color;
  FastLED.setBrightness(120);
  FastLED.show();
}

// ================= LOAD CONFIG =================
void loadConfig() {
  prefs.begin("wifi", true);
  safeCopy(wifiSsid, sizeof(wifiSsid), prefs.getString("ssid", ""));
  safeCopy(wifiPass, sizeof(wifiPass), prefs.getString("password", ""));
  prefs.end();

  prefs.begin("device", true);
  ledCount = prefs.getUInt("leds", 32);
  safeCopy(deviceName, sizeof(deviceName), prefs.getString("device_name", "Лампа"));
  safeCopy(roomName, sizeof(roomName), prefs.getString("room_name", "Комната"));
  prefs.end();

  prefs.begin("device_id", true);
  deviceId = prefs.getUInt("deviceId", 0);
  prefs.end();

  prefs.begin("mqtt", true);
  safeCopy(mqttServer, sizeof(mqttServer), prefs.getString("mqtt_server", ""));
  mqttPort = prefs.getUInt("mqtt_port", 1883);
  safeCopy(mqttUser, sizeof(mqttUser), prefs.getString("mqtt_user", ""));
  safeCopy(mqttPass, sizeof(mqttPass), prefs.getString("mqtt_pass", ""));
  safeCopy(mqttToken, sizeof(mqttToken), prefs.getString("mqtt_token", ""));
  safeCopy(topicEffect, sizeof(topicEffect), prefs.getString("topicEffect", ""));
  safeCopy(topicBrightness, sizeof(topicBrightness), prefs.getString("topicBrightness", ""));
  safeCopy(topicColor, sizeof(topicColor), prefs.getString("topicColor", ""));
  safeCopy(topicPower, sizeof(topicPower), prefs.getString("topicPower", ""));
  prefs.end();

  ledCount = constrain(ledCount, 1, MAX_LEDS);
  buildStateTopics();   // критично: после ребута state-топики были пустыми
  buildMqttClientId();
}

// ================= FACTORY RESET =================
void factoryReset() {
  Serial.println(F("FACTORY RESET"));

  if (WiFi.status() == WL_CONNECTED && deviceId && mqttToken[0]) {
    WiFiClientSecure secure;
    secure.setInsecure();
    HTTPClient http;
    char url[96];
    snprintf(url, sizeof(url), "https://%s/api/devices/%lu", MQTT_API_HOST, (unsigned long)deviceId);
    if (http.begin(secure, url)) {
      char auth[80];
      snprintf(auth, sizeof(auth), "Token %s", mqttToken);
      http.addHeader("Authorization", auth);
      http.setTimeout(4000);
      http.sendRequest("DELETE");
      http.end();
    }
  }

  WiFi.disconnect(true, true);
  delay(100);

  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  prefs.begin("device", false); prefs.clear(); prefs.end();
  prefs.begin("device_id", false); prefs.clear(); prefs.end();
  prefs.begin("mqtt", false); prefs.clear(); prefs.end();

  delay(200);
  ESP.restart();
}

// ================= WIFI ========================
bool connectWiFi() {
  if (!wifiSsid[0]) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // стабильнее MQTT на C3
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(wifiSsid, wifiPass);

  uint32_t start = millis();
  uint16_t pos = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_MS) {
    FastLED.clear();
    leds[pos % ledCount] = CRGB::Blue;
    FastLED.setBrightness(brightness);
    FastLED.show();
    pos++;
    delay(80);
    yield();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi failed"));
    showStatus(CRGB::Red);
    return false;
  }

  Serial.print(F("WiFi OK "));
  Serial.println(WiFi.localIP());
  showStatus(CRGB::Green);
  return true;
}

void setupServer() {
  server.on("/", HTTP_GET, []() {
    String html = FPSTR(SETUP_HTML);
    html.replace("%%WIFI_OPTIONS%%", buildWifiOptions());
    server.send(200, "text/html", html);
  });

  server.on("/rescan", HTTP_GET, []() {
    scanWifiNetworks();
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  server.on("/save", HTTP_POST, []() {
    String ssidArg = server.arg("ssid_custom");
    ssidArg.trim();
    if (!ssidArg.length()) ssidArg = server.arg("ssid");
    safeCopy(wifiSsid, sizeof(wifiSsid), ssidArg);
    safeCopy(wifiPass, sizeof(wifiPass), server.arg("password"));
    safeCopy(mqttServer, sizeof(mqttServer), server.arg("mqtt_server"));
    mqttPort = (uint16_t)server.arg("mqtt_port").toInt();
    safeCopy(mqttUser, sizeof(mqttUser), server.arg("mqtt_user"));
    safeCopy(mqttPass, sizeof(mqttPass), server.arg("mqtt_pass"));
    safeCopy(mqttToken, sizeof(mqttToken), server.arg("mqtt_token"));
    safeCopy(deviceName, sizeof(deviceName), server.arg("device_name"));
    safeCopy(roomName, sizeof(roomName), server.arg("room_name"));

    char root[13];
    buildTopicRoot(root, sizeof(root));
    snprintf(topicPower, sizeof(topicPower), "%s/power", root);
    snprintf(topicEffect, sizeof(topicEffect), "%s/effect", root);
    snprintf(topicColor, sizeof(topicColor), "%s/color", root);
    snprintf(topicBrightness, sizeof(topicBrightness), "%s/brightness", root);
    buildStateTopics();

    prefs.begin("wifi", false);
    prefs.putString("ssid", wifiSsid);
    prefs.putString("password", wifiPass);
    prefs.end();

    prefs.begin("device", false);
    prefs.putUInt("leds", ledCount);
    prefs.putString("device_name", deviceName);
    prefs.putString("room_name", roomName);
    prefs.end();

    prefs.begin("mqtt", false);
    prefs.putString("mqtt_server", mqttServer);
    prefs.putUInt("mqtt_port", mqttPort);
    prefs.putString("mqtt_user", mqttUser);
    prefs.putString("mqtt_pass", mqttPass);
    prefs.putString("mqtt_token", mqttToken);
    prefs.putString("topicPower", topicPower);
    prefs.putString("topicEffect", topicEffect);
    prefs.putString("topicColor", topicColor);
    prefs.putString("topicBrightness", topicBrightness);
    prefs.end();

    server.send_P(200, "text/html", DONE_HTML);
    delay(800);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Lamp-Setup", "12345678");
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  showStatus(CRGB::Red);
  scanWifiNetworks();
  setupServer();
  Serial.println(F("AP mode: Lamp-Setup"));
}

// ================= DEVICE API =================
void registerDevice() {
  if (WiFi.status() != WL_CONNECTED || !mqttToken[0]) return;
  if (topicPower[0] == '\0') return;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient http;

  char url[64];
  snprintf(url, sizeof(url), "https://%s/api/devices", MQTT_API_HOST);
  if (!http.begin(secure, url)) return;

  char auth[80];
  snprintf(auth, sizeof(auth), "Token %s", mqttToken);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", auth);
  http.setTimeout(8000);

  char nameEsc[64], roomEsc[64];
  jsonEscapeTo(deviceName, nameEsc, sizeof(nameEsc));
  jsonEscapeTo(roomName, roomEsc, sizeof(roomEsc));

  char jsonBuf[900];
  int n = snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"name\":\"%s\",\"type\":25,\"room\":\"%s\","
    "\"on_off\":[{\"topic_cmd\":\"%s\",\"topic_state\":\"%s\",\"cmd_on\":\"1\",\"cmd_off\":\"0\"}],"
    "\"range\":[{\"type\":0,\"topic_cmd\":\"%s\",\"topic_state\":\"%s\",\"max\":100.0,\"min\":0.0,\"precision\":1.0,\"multiplier\":1.0}],"
    "\"color\":[{\"type\":1,\"topic_cmd\":\"%s\",\"topic_state\":\"%s\",\"options\":\"1500,9000\"}],"
    "\"mode\":[{\"type\":6,\"topic_cmd\":\"%s\",\"topic_state\":\"%s\",\"options\":\"one=1,two=2,three=3,four=4,five=5,six=6,seven=7,eight=8,nine=9\"}]}",
    nameEsc, roomEsc,
    topicPower, topicPowerState,
    topicBrightness, topicBrightnessState,
    topicColor, topicColorState,
    topicEffect, topicEffectState);
  if (n <= 0 || n >= (int)sizeof(jsonBuf) - 1) {
    http.end();
    Serial.println(F("JSON too large"));
    return;
  }

  int code = http.POST((uint8_t*)jsonBuf, n);
  Serial.printf("POST devices: %d\n", code);

  if (code == 200 || code == 201) {
    String response = http.getString();
    http.end();

    deviceId = parseDeviceId(response);
    if (deviceId) {
      prefs.begin("device_id", false);
      prefs.putUInt("deviceId", deviceId);
      prefs.end();
    }

    // refresh
    snprintf(url, sizeof(url), "https://%s/api/devices/refresh", MQTT_API_HOST);
    if (http.begin(secure, url)) {
      http.addHeader("Authorization", auth);
      http.setTimeout(4000);
      http.GET();
      http.end();
    }
  } else {
    http.end();
  }
}

// ================= MQTT ========================
void publishPowerState() {
  if (!mqtt.connected() || !topicPowerState[0]) return;
  mqtt.publish(topicPowerState, powerOn ? "1" : "0", true);
}

void publishEffectState() {
  if (!mqtt.connected() || !topicEffectState[0]) return;
  char buf[4];
  itoa(nextEffect, buf, 10);   // целевой эффект, не старый
  mqtt.publish(topicEffectState, buf, true);
}

void publishBrightnessState() {
  if (!mqtt.connected() || !topicBrightnessState[0]) return;
  // наружу 0..100, внутри 0..255
  char buf[4];
  itoa(map(brightness, 0, 255, 0, 100), buf, 10);
  mqtt.publish(topicBrightnessState, buf, true);
}

void publishColorState() {
  if (!mqtt.connected() || !topicColorState[0]) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%u,%u,%u", currentR, currentG, currentB);
  mqtt.publish(topicColorState, buf, true);
}

void publishAllStates() {
  publishPowerState();
  publishEffectState();
  publishBrightnessState();
  publishColorState();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';

  Serial.printf("MQTT %s = %s\n", topic, msg);

  if (strcmp(topic, topicEffect) == 0) {
    int v = atoi(msg);
    if (v >= 1 && v <= 9) {
      nextEffect = (uint8_t)v;
      effectBlend = 0;
      powerOn = true;
      enabled = true;
      ledsDirty = true;
    }
    publishEffectState();
    publishPowerState();
    return;
  }

  if (strcmp(topic, topicBrightness) == 0) {
    int v = constrain(atoi(msg), 0, 100);
    brightness = (uint8_t)map(v, 0, 100, 0, 255);
    if (brightness > 0) lastBrightness = brightness;
    enabled = brightness > 0;
    powerOn = enabled;
    ledsDirty = true;
    publishBrightnessState();
    return;
  }

  if (strcmp(topic, topicColor) == 0) {
    uint8_t r, g, b;
    if (parseRgb(msg, r, g, b)) {
      CHSV hsv = rgb2hsv_approximate(CRGB(r, g, b));
      hue = hsv.h;
      sat = hsv.s;
      currentR = r;
      currentG = g;
      currentB = b;
      enabled = true;
      powerOn = true;
      ledsDirty = true;
      publishColorState();
    }
    return;
  }

  if (strcmp(topic, topicPower) == 0) {
    int v = atoi(msg);
    if (v == 0) {
      enabled = false;
      powerOn = false;
      ledsDirty = true;
    } else if (v == 1) {
      enabled = true;
      powerOn = true;
      if (brightness == 0) brightness = lastBrightness ? lastBrightness : 150;
      ledsDirty = true;
    }
    publishPowerState();
  }
}

// Неблокирующий reconnect — иначе C3 зависает навечно
bool mqttTryConnect() {
  if (!mqttServer[0]) return false;
  if (mqtt.connected()) return true;

  uint32_t now = millis();
  if (now - mqttLastAttempt < MQTT_RECONNECT_MS) return false;
  mqttLastAttempt = now;

  Serial.println(F("MQTT connect..."));
  if (mqtt.connect(mqttClientId, mqttUser, mqttPass)) {
    mqtt.subscribe(topicEffect);
    mqtt.subscribe(topicBrightness);
    mqtt.subscribe(topicColor);
    mqtt.subscribe(topicPower);
    Serial.println(F("MQTT OK"));
    publishAllStates();
    return true;
  }

  Serial.printf("MQTT fail rc=%d\n", mqtt.state());
  return false;
}

// ================= ЭФФЕКТЫ =================
void effectColor() {
  fill_solid(leds, ledCount, CRGB(currentR, currentG, currentB));
}

void effectRainbowFlow() {
  static uint8_t h = 0;
  fill_solid(leds, ledCount, CHSV(h++, sat, 255));
}

void effectMovingRainbow() {
  static uint8_t off = 0;
  uint8_t delta = ledCount > 1 ? (255 / ledCount) : 1;
  fill_rainbow(leds, ledCount, off++, delta);
}

void effectFire() {
  static uint16_t t = 0;
  t++;
  fadeToBlackBy(leds, ledCount, 20);
  for (uint16_t i = 0; i < ledCount; i++) {
    uint8_t n = inoise8(i * 45, t * 15);
    uint8_t heat = scale8(n, 255);
    uint8_t verticalFade = map(i, 0, ledCount > 1 ? ledCount - 1 : 1, 255, 120);
    heat = scale8(heat, verticalFade);
    leds[i] += CHSV(2 + scale8(n, 20), 255, heat);
  }
}

void effectPulse() {
  static uint8_t phase = 0;
  phase += 2;
  uint8_t b = map(sin8(phase), 0, 255, 10, 255);
  fill_solid(leds, ledCount, CRGB(currentR, currentG, currentB).nscale8(b));
}

void effectNebula() {
  static uint16_t t = 0;
  t++;
  for (uint16_t i = 0; i < ledCount; i++) {
    uint8_t n = inoise8(i * 40, t * 3);
    uint8_t v = map(n, 0, 255, 51, 255);
    leds[i] = CRGB(currentR, currentG, currentB);
    leds[i].nscale8(v);
  }
}

void effectComets() {
  static int16_t pos = 0, dir = 1;
  static uint8_t h = 0;
  fadeToBlackBy(leds, ledCount, 60);
  if (pos < 0) pos = 0;
  if (pos >= (int16_t)ledCount) pos = ledCount - 1;
  leds[pos] = CHSV(h, 255, 255);
  pos += dir;
  if (pos <= 0 || pos >= (int16_t)ledCount - 1) {
    dir = -dir;
    h += random8(40, 80);
  }
}

void effectAurora() {
  static uint16_t t = 0;
  t++;
  fadeToBlackBy(leds, ledCount, 20);
  for (uint16_t i = 0; i < ledCount; i++) {
    uint16_t x = i * 50;
    uint8_t n1 = inoise8(x, t * 20);
    uint8_t n2 = inoise8(x + 5000, t * 8);
    uint8_t glow = n1;
    uint8_t redMix = qsub8(n2, 160);
    uint8_t hueGreen = 65 + scale8(n1, 25);
    uint8_t hueRed = scale8(redMix, 10);
    uint8_t h = lerp8by8(hueGreen, hueRed, redMix);
    leds[i] = CHSV(h, 200, glow);
  }
}

void effectMeteor() {
  static int16_t pos = 0;
  static uint8_t h = 0;
  fadeToBlackBy(leds, ledCount, 40);
  if (pos >= 0 && pos < (int16_t)ledCount) leds[pos] = CHSV(h, 255, 255);
  if (++pos >= (int16_t)ledCount) {
    pos = 0;
    h += random8(40, 100);
  }
}

typedef void (*EffectFn)();
static const EffectFn EFFECTS[9] = {
  effectColor, effectRainbowFlow, effectMovingRainbow,
  effectFire, effectPulse, effectNebula,
  effectComets, effectAurora, effectMeteor
};

static inline void runEffect(uint8_t id) {
  if (id >= 1 && id <= 9) EFFECTS[id - 1]();
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);    // критично для BOOT/GPIO9

  loadConfig();

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, ledCount);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);
  FastLED.clear(true);

  if (!connectWiFi()) {
    startAP();
    return;
  }

  if (!deviceId) registerDevice();

  mqtt.setServer(mqttServer, mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(MQTT_BUF_SIZE);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);

  mqttTryConnect();
  showStatus(CRGB::Green);
}

void loop() {
  // ---------- BUTTON ----------
  bool pressed = digitalRead(BTN_PIN) == LOW;
  if (pressed && !btnPressed) {
    btnPressed = true;
    btnPressTime = millis();
  }
  if (!pressed && btnPressed) btnPressed = false;
  if (btnPressed && millis() - btnPressTime > RESET_HOLD_MS) {
    factoryReset();
  }

  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  // ---------- WIFI ----------
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiFailTimer) wifiFailTimer = millis();
    if (millis() - wifiFailTimer > WIFI_RETRY_MS) {
      startAP();
      wifiFailTimer = 0;
      return;
    }
  } else {
    wifiFailTimer = 0;
  }

  // ---------- MQTT (non-blocking) ----------
  if (!mqtt.connected()) {
    mqttTryConnect();
  } else {
    mqtt.loop();
  }

  // ---------- POWER OFF: один clear, без спама show() ----------
  if (!enabled || !powerOn) {
    if (ledsDirty) {
      FastLED.clear(true);
      ledsDirty = false;
    }
    delay(2);
    return;
  }

  uint16_t frameDelay = map(effectSpeed, 0, 100, 120, 10);
  uint32_t now = millis();
  if (now - lastFrame < frameDelay) return;
  lastFrame = now;

  if (nextEffect != effect) {
    runEffect(nextEffect);
    effectBlend = qadd8(effectBlend, 8);
    if (effectBlend >= 250) {
      effect = nextEffect;
      effectBlend = 0;
      publishEffectState();
    }
  } else {
    runEffect(effect);
  }

  FastLED.setBrightness(brightness);
  FastLED.show();
}
