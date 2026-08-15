#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ================= НАСТРОЙКИ =================
#define DATA_PIN        4
#define MAX_LEDS        256          // 256×3=768B; при нехватке RAM снизь
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
uint16_t ledCount = 100;

// ================= DEVICE STATE =================
uint8_t currentR = 250, currentG = 170, currentB = 20;
uint8_t hue = 20, sat = 210;
uint8_t brightness = 200;
uint8_t lastBrightness = 200;
uint8_t effectSpeed = 70;
uint32_t lastFrame = 0;
uint8_t effect = 4;
uint8_t nextEffect = 4;
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

bool btnPressed = false;
uint32_t btnPressTime = 0;

// ================= HTML (во flash) =================
static const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>WiFi и MQTT Setup</title>
<style>
html{font-family:Arial;color:#dcdcdc;height:100vh}
body{background:#1c1638;display:flex;flex-direction:column;align-items:center;padding:40px 16px}
form{display:flex;flex-direction:column;align-items:center;width:100%;max-width:320px}
label{display:block;margin-bottom:5px}
.form-item{margin-bottom:18px;width:100%}
input{padding:8px;border-radius:5px;border:none;width:100%;box-sizing:border-box}
button{padding:15px 30px;border:none;border-radius:5px;background:#4f3cab;color:#dcdcdc;font-weight:bold;font-size:1.1rem;cursor:pointer;margin-top:12px}
</style>
</head>
<body>
<h2>Настройки WiFi и MQTT</h2>
<form action="/save" method="post">
  <h3>WiFi</h3>
  <div class="form-item"><label>SSID</label><input type="text" name="ssid" required maxlength="32"></div>
  <div class="form-item"><label>Пароль</label><input type="password" name="password" maxlength="64"></div>
  <h3>MQTT</h3>
  <div class="form-item"><label>MQTT сервер</label><input type="text" name="mqtt_server" required maxlength="63"></div>
  <div class="form-item"><label>MQTT порт</label><input type="number" name="mqtt_port" value="1883" required></div>
  <div class="form-item"><label>MQTT логин</label><input type="text" name="mqtt_user" required maxlength="47"></div>
  <div class="form-item"><label>MQTT пароль</label><input type="password" name="mqtt_pass" required maxlength="47"></div>
  <div class="form-item"><label>MQTT token</label><input type="text" name="mqtt_token" required maxlength="63"></div>
  <h3>Устройство</h3>
  <div class="form-item"><label>Количество диодов</label><input type="number" name="led_count" value="100" min="1" max="256" required></div>
  <h3>Комната</h3>
  <div class="form-item"><label>Название комнаты</label><input type="text" name="room_name" value="Комната" required maxlength="31"></div>
  <div class="form-item"><label>Название устройства</label><input type="text" name="device_name" value="Свет" required maxlength="31"></div>
  <button type="submit">Сохранить</button>
</form>
</body>
</html>
)rawliteral";

static const char DONE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Готово</title>
<style>html{font-family:Arial;color:#dcdcdc;height:100vh}body{background:#1c1638;display:flex;align-items:center;justify-content:center;height:100vh;margin:0}</style>
</head><body><h3>Сохранено. Перезагрузка...</h3>
<script>setTimeout(function(){window.close()},1000)</script>
</body></html>
)rawliteral";

// ================= HELPERS =================
static void safeCopy(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return;
  size_t n = src.length();
  if (n >= dstSize) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
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
  int ri, gi, bi;
  if (sscanf(payload, "%d,%d,%d", &ri, &gi, &bi) != 3) return false;
  r = constrain(ri, 0, 255);
  g = constrain(gi, 0, 255);
  b = constrain(bi, 0, 255);
  return true;
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
  ledCount = prefs.getUInt("leds", 100);
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
    server.send_P(200, "text/html", SETUP_HTML);
  });

  server.on("/save", HTTP_POST, []() {
    safeCopy(wifiSsid, sizeof(wifiSsid), server.arg("ssid"));
    safeCopy(wifiPass, sizeof(wifiPass), server.arg("password"));
    safeCopy(mqttServer, sizeof(mqttServer), server.arg("mqtt_server"));
    mqttPort = (uint16_t)server.arg("mqtt_port").toInt();
    safeCopy(mqttUser, sizeof(mqttUser), server.arg("mqtt_user"));
    safeCopy(mqttPass, sizeof(mqttPass), server.arg("mqtt_pass"));
    safeCopy(mqttToken, sizeof(mqttToken), server.arg("mqtt_token"));
    safeCopy(deviceName, sizeof(deviceName), server.arg("device_name"));
    safeCopy(roomName, sizeof(roomName), server.arg("room_name"));
    ledCount = (uint16_t)constrain(server.arg("led_count").toInt(), 1, MAX_LEDS);

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
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Lamp-Setup", "12345678");
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  setupServer();
  showStatus(CRGB::Red);
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

  StaticJsonDocument<1024> body;
  body["name"] = deviceName;
  body["type"] = 25;
  body["room"] = roomName;

  JsonArray onOffArr = body.createNestedArray("on_off");
  JsonObject oo = onOffArr.createNestedObject();
  oo["topic_cmd"] = topicPower;
  oo["topic_state"] = topicPowerState;
  oo["cmd_on"] = "1";
  oo["cmd_off"] = "0";

  JsonArray rangeArr = body.createNestedArray("range");
  JsonObject rg = rangeArr.createNestedObject();
  rg["type"] = 0;
  rg["topic_cmd"] = topicBrightness;
  rg["topic_state"] = topicBrightnessState;
  rg["max"] = 100.0;
  rg["min"] = 0.0;
  rg["precision"] = 1.0;
  rg["multiplier"] = 1.0;

  JsonArray colorArr = body.createNestedArray("color");
  JsonObject cl = colorArr.createNestedObject();
  cl["type"] = 1;
  cl["topic_cmd"] = topicColor;
  cl["topic_state"] = topicColorState;
  cl["options"] = "1500,9000";

  JsonArray modeArr = body.createNestedArray("mode");
  JsonObject md = modeArr.createNestedObject();
  md["type"] = 6;
  md["topic_cmd"] = topicEffect;
  md["topic_state"] = topicEffectState;
  md["options"] = "one=1,two=2,three=3,four=4,five=5,six=6,seven=7,eight=8,nine=9";

  char jsonBuf[900];
  size_t n = serializeJson(body, jsonBuf, sizeof(jsonBuf));
  if (n == 0 || n >= sizeof(jsonBuf) - 1) {
    http.end();
    Serial.println(F("JSON too large"));
    return;
  }

  int code = http.POST((uint8_t*)jsonBuf, n);
  Serial.printf("POST devices: %d\n", code);

  if (code == 200 || code == 201) {
    String response = http.getString();
    http.end();

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, response)) {
      Serial.println(F("JSON parse failed"));
      return;
    }

    deviceId = doc["detail"]["device_id"] | 0;
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
  fill_solid(leds, ledCount, CHSV(hue, sat, 255));
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
  fill_solid(leds, ledCount, CHSV(hue, sat, b));
}

void effectNebula() {
  static uint16_t t = 0;
  t++;
  for (uint16_t i = 0; i < ledCount; i++) {
    uint8_t n = inoise8(i * 40, t * 3);
    uint8_t h = hue + map(n, 0, 255, -30, 30);
    uint8_t v = map(n, 0, 255, brightness / 5, 255);
    leds[i] = CHSV(h, sat, v);
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
