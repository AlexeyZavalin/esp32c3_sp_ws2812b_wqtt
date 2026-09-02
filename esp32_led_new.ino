#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <FastLED.h>
#include <HTTPClient.h>

#define LAMP_DEBUG 0
#if LAMP_DEBUG
  #define LOG(x) Serial.print(x)
  #define LOGLN(x) Serial.println(x)
#else
  #define LOG(x)
  #define LOGLN(x)
#endif

#define LED_PIN     4
#define WIDTH       16
#define HEIGHT      16
#define NUM_LEDS    (WIDTH * HEIGHT)
#define BRIGHTNESS  120
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define DNS_PORT 53
#define WIFI_RETRY_TIME 20000
#define WIFI_RECONNECT_INTERVAL 5000
#define WIFI_RECONNECT_ATTEMPTS 4
#define MQTT_RECONNECT_INTERVAL 3000
#define BTN_PIN 10              // GPIO сенсорной кнопки (TTP223 и т.п.)
#define BTN_ACTIVE_HIGH 1      // 1 = HIGH при касании (TTP223), 0 = кнопка к GND
#define BTN_DEBOUNCE_MS 40
#define BTN_CLICK_MAX_MS 450   // отпускание быстрее = клик
#define BTN_DOUBLE_GAP_MS 350  // окно второго клика
#define BTN_LONG_MS 550        // порог регулировки яркости
#define BTN_BRIGHT_STEP_MS 35  // шаг яркости при удержании
#define BTN_BRIGHT_STEP 2      // шаг в процентах 0..100
#define RESET_HOLD_TIME 10000  // factory reset (дольше регулировки яркости)
#define MQTT_API_URL "dash.wqtt.ru"
#define LED_VOLTS 5
#define LED_MAX_MA 2000        // лимит тока матрицы (подстрой под БП)

CRGB leds[NUM_LEDS];
CRGB prevLeds[NUM_LEDS];

// ---------- touch / button state ----------
bool btnRaw = false;
bool btnStable = false;
bool btnPrevStable = false;
uint32_t btnDebounceAt = 0;
uint32_t btnPressStart = 0;
bool btnLongActive = false;
bool btnResetDone = false;
uint8_t btnClickCount = 0;
uint32_t btnLastClickAt = 0;
int8_t brightnessDir = 1;     // +1 вверх, -1 вниз; реверс после каждого long-press
uint32_t lastBrightStepAt = 0;
uint32_t lastBrightPublishAt = 0;

uint16_t t = 0;

// ================= DEVICE STATE =================
uint8_t currentR = 250;
uint8_t currentG = 170;
uint8_t currentB = 20;

uint8_t hue = 20;
uint8_t sat = 210;
uint8_t brightness = 120;
uint8_t lastBrightness = brightness;
uint8_t effectSpeed = 70;   // 10 = очень медленно, 100 = быстро
uint32_t lastFrame = 0;
uint8_t effect = 2;          // текущий эффект
uint8_t nextEffect = 2;      // эффект для плавного перехода
uint8_t effectBlend = 0;
uint8_t cmd = 255;

bool enabled = true;
bool powerOn = true;

// ================= WIFI / MQTT =================
Preferences prefs;
DNSServer dnsServer;
WebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);

String wifi_ssid;
String wifi_password;
String mqttClientId;

String mqtt_server;
String mqtt_username;
String mqtt_password;
uint16_t mqtt_port;
String mqtt_token;

// команды
String topicEffect;
String topicBrightness;
String topicColor;
String topicPower;
// state
String topicEffectState;
String topicBrightnessState;
String topicColorState;
String topicPowerState;

//device
String deviceName;
String roomName;
int deviceId;

bool apMode = false;
unsigned long wifiFailTimer = 0;
unsigned long lastWifiReconnect = 0;
uint8_t wifiReconnectAttempts = 0;
unsigned long lastMqttAttempt = 0;

// ================= HTML =======================
const char* setup_html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>WiFi и MQTT Setup</title>
</head>
<body>
<h2>Настройки WiFi и MQTT</h2>
<form action="/save" method="post">
  <h3>WiFi</h3>
  <div class="form-item"><label>SSID</label><input type="text" name="ssid" required></div>
  <div class="form-item"><label>Пароль</label><input type="password" name="password" required></div>
  <h3>MQTT</h3>
  <div class="form-item"><label>MQTT сервер</label><input type="text" name="mqtt_server" required></div>
  <div class="form-item"><label>MQTT порт</label><input type="number" name="mqtt_port" required></div>
  <div class="form-item"><label>MQTT логин</label><input type="text" name="mqtt_user" required></div>
  <div class="form-item"><label>MQTT пароль</label><input type="password" name="mqtt_pass" required></div>
  <div class="form-item"><label>MQTT token</label><input type="text" name="mqtt_token" required></div>
  <h3>Комната</h3>
  <div class="form-item"><label>Название комнаты</label><input type="text" name="room_name" value="Комната" required></div>
  <div class="form-item"><label>Название устройства</label><input type="text" name="device_name" value="Лампа" required></div>
  <button type="submit">Сохранить</button>
</form>
<style>
html{font-family:Arial;color:#dcdcdc;height:100vh;}
body{background:#1c1638;display:flex;flex-direction:column;align-items:center;justify-content:center;padding-top:40px;}
form{display:flex;flex-direction:column;align-items:center;}
label{display:block;margin-bottom:5px;}
.form-item{margin-bottom:25px;}
input{padding:8px;border-radius:5px;border:none;}
button{padding:15px 30px;border:none;border-radius:5px;background:#4f3cab;color:#dcdcdc;font-weight:bold;font-size:1.125rem;cursor:pointer;margin-top:20px;}
button:hover{background:#6043ea;}
</style>
</body>
</html>
)rawliteral";

static const char done_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><body style="font-family:sans-serif;background:#1c1638;color:#ddd;text-align:center;padding:40px">
<h3>Сохранено. Перезагрузка...</h3></body></html>
)rawliteral";

// ================= HELPER =====================
void rebuildStateTopics() {
  topicPowerState = topicPower.length() ? topicPower + "/state" : "";
  topicEffectState = topicEffect.length() ? topicEffect + "/state" : "";
  topicColorState = topicColor.length() ? topicColor + "/state" : "";
  topicBrightnessState = topicBrightness.length() ? topicBrightness + "/state" : "";
}

String escapeJson(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n' || c == '\r' || c == '\t') continue;
    out += c;
  }
  return out;
}

uint8_t brightnessToPercent() {
  return map(brightness, 0, 255, 0, 100);
}

void setPower(bool on) {
  powerOn = on;
  enabled = on;
  if (on && brightness == 0) {
    brightness = lastBrightness > 0 ? lastBrightness : 150;
  }
  if (!on) {
    FastLED.clear();
    FastLED.setBrightness(0);
    FastLED.show();
  }
}

void setBrightnessPercent(int pct) {
  pct = constrain(pct, 0, 100);
  brightness = map(pct, 0, 100, 0, 255);
  lastBrightness = brightness;
  if (pct == 0) {
    setPower(false);
  } else {
    powerOn = true;
    enabled = true;
  }
}

void requestEffect(uint8_t v) {
  if (v < 1 || v > 5) return;
  cmd = v;
  if (v != effect) {
    memcpy((void*)prevLeds, (const void*)leds, sizeof(leds));
    effectBlend = 0;
  }
  nextEffect = v;
  setPower(true);
}

void nextEffectFromButton() {
  uint8_t v = nextEffect + 1;
  if (v > 5) v = 1;
  requestEffect(v);
  publishEffectState();
  publishPowerState();
  LOG(F("BTN next effect: "));
  LOGLN(v);
}

void togglePowerFromButton() {
  setPower(!powerOn);
  publishPowerState();
  LOG(F("BTN power: "));
  LOGLN(powerOn ? "ON" : "OFF");
}

bool readButtonRaw() {
#if BTN_ACTIVE_HIGH
  return digitalRead(BTN_PIN) == HIGH;
#else
  return digitalRead(BTN_PIN) == LOW;
#endif
}

void handleButton() {
  bool raw = readButtonRaw();
  uint32_t now = millis();

  if (raw != btnRaw) {
    btnRaw = raw;
    btnDebounceAt = now;
  }
  if ((now - btnDebounceAt) >= BTN_DEBOUNCE_MS) {
    btnStable = btnRaw;
  }

  // press edge
  if (btnStable && !btnPrevStable) {
    btnPressStart = now;
    btnLongActive = false;
    btnResetDone = false;
    lastBrightStepAt = now;
  }

  // held
  if (btnStable) {
    uint32_t held = now - btnPressStart;

    if (!btnResetDone && held >= RESET_HOLD_TIME) {
      btnResetDone = true;
      factoryReset();
      return;
    }

    if (!btnLongActive && held >= BTN_LONG_MS) {
      btnLongActive = true;
      btnClickCount = 0;  // long отменяет клики
      if (!powerOn) setPower(true);
      lastBrightStepAt = now;
      LOG(F("BTN long brightness dir="));
      LOGLN(brightnessDir > 0 ? "+" : "-");
    }

    if (btnLongActive && (now - lastBrightStepAt) >= BTN_BRIGHT_STEP_MS) {
      lastBrightStepAt = now;
      int pct = (int)brightnessToPercent() + (int)brightnessDir * BTN_BRIGHT_STEP;
      pct = constrain(pct, 1, 100);  // при удержании не гасим в 0
      setBrightnessPercent(pct);
      if (now - lastBrightPublishAt >= 300) {
        lastBrightPublishAt = now;
        publishBrightnessState();
        publishPowerState();
      }
    }
  }

  // release edge
  if (!btnStable && btnPrevStable) {
    uint32_t held = now - btnPressStart;

    if (btnLongActive) {
      brightnessDir = (int8_t)(-brightnessDir);  // следующее удержание — в другую сторону
      publishBrightnessState();
      publishPowerState();
      LOG(F("BTN long end, next dir="));
      LOGLN(brightnessDir > 0 ? "+" : "-");
    } else if (held < BTN_LONG_MS) {
      btnClickCount++;
      btnLastClickAt = now;
    }
  }

  // double / single click resolve
  if (!btnStable && btnClickCount > 0 && (now - btnLastClickAt) >= BTN_DOUBLE_GAP_MS) {
    if (btnClickCount >= 2) {
      nextEffectFromButton();
    } else {
      togglePowerFromButton();
    }
    btnClickCount = 0;
  }

  btnPrevStable = btnStable;
}

// ================= LOAD CONFIG =================
void loadConfig() {
  prefs.begin("wifi", true);
  wifi_ssid = prefs.getString("ssid", "");
  wifi_password = prefs.getString("password", "");
  prefs.end();

  prefs.begin("device", true);
  deviceName = prefs.getString("device_name", "Эмби-лампа");
  roomName = prefs.getString("room_name", "Комната");
  prefs.end();
  prefs.begin("device_id", true);
  deviceId = prefs.getUInt("deviceId", 0);
  prefs.end();
  prefs.begin("mqtt", true);
  mqtt_server   = prefs.getString("mqtt_server", "");
  mqtt_port     = prefs.getUInt("mqtt_port", 1883);
  mqtt_username = prefs.getString("mqtt_user", "");
  mqtt_password = prefs.getString("mqtt_pass", "");
  mqtt_token = prefs.getString("mqtt_token", "");
  // команды
  topicEffect = prefs.getString("topicEffect", "");
  topicBrightness = prefs.getString("topicBrightness", "");
  topicColor = prefs.getString("topicColor", "");
  topicPower = prefs.getString("topicPower", "");
  prefs.end();

  rebuildStateTopics();
}

bool parseRgb(const char* payload, uint8_t &r, uint8_t &g, uint8_t &b) {
  int ri, gi, bi;
  if (sscanf(payload,"%d,%d,%d",&ri,&gi,&bi)!=3) return false;
  r = constrain(ri,0,255); g = constrain(gi,0,255); b = constrain(bi,0,255);
  return true;
}

void factoryReset() {
  LOGLN(F("FACTORY RESET START"));

  // ---------- СБРОС СОСТОЯНИЯ ----------
  brightness = 150;
  lastBrightness = 150;
  hue = 0;
  sat = 255;
  effect = 1;
  nextEffect = 1;
  effectBlend = 0;
  enabled = true;
  powerOn = true;

  if (deviceId > 0 && mqtt_token.length() > 0 && WiFi.status() == WL_CONNECTED) {
    String url = "https://" + String(MQTT_API_URL) + "/api/devices/" + String(deviceId);
    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Token " + mqtt_token);
    int code = http.sendRequest("DELETE");
    LOG(F("DELETE device response: "));
    LOGLN(code);
    http.end();
    delay(200);
  }

  WiFi.disconnect(true, true);
  delay(200);
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  prefs.begin("device", false);
  prefs.clear();
  prefs.end();

  prefs.begin("device_id", false);
  prefs.clear();
  prefs.end();

  prefs.begin("mqtt", false);
  prefs.clear();
  prefs.end();

  LOGLN(F("Preferences cleared"));
  
  delay(500);
  ESP.restart();
}

void showStatus(CRGB color){
  FastLED.clear();
  for(int i=0;i<min(3,(int)NUM_LEDS);i++) leds[i]=color;
  FastLED.show();
}

// ================= WIFI ========================
bool connectWiFi() {
  if(wifi_ssid=="") return false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

  unsigned long start=millis();
  uint16_t pos=0;
  while(WiFi.status()!=WL_CONNECTED && millis()-start<20000){
    FastLED.clear();
    leds[pos]=CRGB::Blue;
    FastLED.setBrightness(brightness);
    FastLED.show();
    pos=(pos+1)%NUM_LEDS;
    delay(120);
    LOG(".");
  }
  LOGLN();

  if(WiFi.status()!=WL_CONNECTED){
    LOG(F("WiFi failed, status="));
    LOGLN(WiFi.status());
    for(int i=0;i<3;i++){
      for(uint8_t b=0;b<180;b+=10){
        FastLED.clear();
        leds[pos]=CRGB(b,0,0);
        FastLED.show();
        delay(30);
      }
      for(int b=180;b>0;b-=10){
        FastLED.clear();
        leds[pos]=CRGB(b,0,0);
        FastLED.show();
        delay(30);
      }
    }
    return false;
  }

  for(int i=0;i<2;i++){
    FastLED.clear(); leds[pos]=CRGB::Green; FastLED.show();
    delay(150);
    FastLED.clear(); FastLED.show();
    delay(120);
  }
  return true;
}

void setupServer() {
  server.on("/",HTTP_GET,[]{server.send_P(200,"text/html",setup_html);});
  server.on("/save",HTTP_POST,[]{
    wifi_ssid = server.arg("ssid");
    wifi_password = server.arg("password");
    mqtt_server = server.arg("mqtt_server");
    mqtt_port = server.arg("mqtt_port").toInt();
    mqtt_username = server.arg("mqtt_user");
    mqtt_password = server.arg("mqtt_pass");
    mqtt_token = server.arg("mqtt_token");
    deviceName = server.arg("device_name");
    roomName = server.arg("room_name");
    uint64_t chipId = ESP.getEfuseMac();
    char buf[13];
    sprintf(buf, "%04X%08X", (uint16_t)(chipId >> 32), (uint32_t)chipId);
    String root = String(buf);
    topicPower = root + "/power";
    topicEffect = root + "/effect";
    topicColor = root + "/color";
    topicBrightness = root + "/brightness";

    topicPowerState = topicPower+"/state";
    topicEffectState = topicEffect+"/state";
    topicColorState = topicColor+"/state";
    topicBrightnessState = topicBrightness+"/state";

    rebuildStateTopics();

    LOGLN(F("=== CONFIG SAVED ==="));
    LOG(F("SSID: ")); LOGLN(wifi_ssid);
    LOG(F("MQTT: ")); LOGLN(mqtt_server);

    prefs.begin("wifi",false); 
    prefs.putString("ssid",wifi_ssid); 
    prefs.putString("password",wifi_password); 
    prefs.end();
    prefs.begin("device",false);
    prefs.putString("device_name",deviceName);
    prefs.putString("room_name",roomName);
    prefs.end();
    prefs.begin("mqtt",false);
    prefs.putString("mqtt_server",mqtt_server);
    prefs.putUInt("mqtt_port",mqtt_port);
    prefs.putString("mqtt_user",mqtt_username);
    prefs.putString("mqtt_pass",mqtt_password);
    prefs.putString("topicPower",topicPower);
    prefs.putString("topicEffect",topicEffect);
    prefs.putString("topicColor",topicColor);
    prefs.putString("topicBrightness",topicBrightness);
    prefs.putString("mqtt_token",mqtt_token);
    
    prefs.end();

    server.send_P(200,"text/html",done_html);
    delay(1500);
    ESP.restart();
  });
  server.onNotFound([](){server.sendHeader("Location","/");server.send(302,"text/plain","");});
  server.begin();
}

void startAP(){
  apMode=true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("lamp-setup","6G5lWYSI");
  IPAddress ip = WiFi.softAPIP();
  dnsServer.start(DNS_PORT,"*",ip);
  setupServer();
  showStatus(CRGB::Red);
}

int parseDeviceId(const String& response) {
  int key = response.indexOf("device_id");
  if (key < 0) return 0;
  int colon = response.indexOf(':', key);
  if (colon < 0) return 0;
  return response.substring(colon + 1).toInt();
}

void registerDevice() {

  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String url = "https://" + String(MQTT_API_URL) + "/api/devices";
  String refreshUrl = "https://" + String(MQTT_API_URL) + "/api/devices/refresh";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Token " + mqtt_token);
  String safeName = escapeJson(deviceName);
  String safeRoom = escapeJson(roomName);
  String json =
  "{"
    "\"name\":\"" + safeName + "\","
    "\"type\":25,"
    "\"room\":\"" + safeRoom + "\","

    "\"on_off\":[{"
        "\"topic_cmd\":\"" + topicPower + "\","
        "\"topic_state\":\"" + topicPowerState + "\","
        "\"cmd_on\":\"1\","
        "\"cmd_off\":\"0\""
    "}],"

    "\"range\":[{"
        "\"type\":0,"
        "\"topic_cmd\":\"" + topicBrightness + "\","
        "\"topic_state\":\"" + topicBrightnessState + "\","
        "\"max\":100.0,"
        "\"min\":0.0,"
        "\"precision\":1.0,"
        "\"multiplier\":1.0"
    "}],"

    "\"color\":[{"
        "\"type\":1,"
        "\"topic_cmd\":\"" + topicColor + "\","
        "\"topic_state\":\"" + topicColorState + "\","
        "\"options\":\"1500,9000\""
    "}],"

    "\"mode\":[{"
        "\"type\":6,"
        "\"topic_cmd\":\"" + topicEffect + "\","
        "\"topic_state\":\"" + topicEffectState + "\","
        "\"options\":\"one=1,two=2,three=3,four=4,five=5\""
    "}]"
  "}";

  int httpResponseCode = http.POST(json);
  LOGLN(json);
  LOG(F("POST response: "));
  LOGLN(httpResponseCode);

  if (httpResponseCode == 200 || httpResponseCode == 201) {
    String response = http.getString();
    LOGLN(response);
    http.end();

    int device_id = parseDeviceId(response);
    if (device_id <= 0) {
      LOGLN(F("device_id parse failed"));
      return;
    }

    prefs.begin("device_id", false);
    prefs.putUInt("deviceId", device_id);
    prefs.end();
    deviceId = device_id;
    delay(200);
    HTTPClient http2;
    http2.begin(refreshUrl);
    http2.addHeader("Authorization", "Token " + mqtt_token);
    http2.GET();
    http2.end();
  } else {
    http.end();
  }
}

// ================= MQTT ========================
// ================= MQTT HELPERS =================
void publishEffectState() {
  if (!client.connected() || topicEffectState.length() == 0) return;
  char buf[4];
  itoa(nextEffect, buf, 10);
  client.publish(topicEffectState.c_str(), buf, true);
  LOG(F("Published EFFECT state: ")); LOGLN(buf);
}

void publishBrightnessState() {
  if (!client.connected() || topicBrightnessState.length() == 0) return;
  char buf[4];
  itoa(brightnessToPercent(), buf, 10);
  client.publish(topicBrightnessState.c_str(), buf, true);
  LOG(F("Published BRIGHTNESS state: ")); LOGLN(buf);
}

void publishColorState() {
  if (!client.connected() || topicColorState.length() == 0) return;
  char buf[20];
  sprintf(buf, "%d,%d,%d", currentR, currentG, currentB);
  client.publish(topicColorState.c_str(), buf, true);
  LOG(F("Published COLOR state: ")); LOGLN(buf);
}

void publishPowerState() {
  if (!client.connected() || topicPowerState.length() == 0) return;
  client.publish(topicPowerState.c_str(), powerOn ? "1" : "0", true);
  LOG(F("Published POWER state: ")); LOGLN(powerOn ? "1" : "0");
}

void publishAllStates() {
  publishPowerState();
  publishEffectState();
  publishBrightnessState();
  publishColorState();
}

void mqttCallback(char* topic, byte* payload, unsigned int length){
  char msgBuf[64];
  if (length >= sizeof(msgBuf)) length = sizeof(msgBuf) - 1;
  memcpy(msgBuf, payload, length);
  msgBuf[length] = '\0';

  String msg = String(msgBuf);
  LOG(F("MQTT | "));
  LOG(topic);
  LOG(F(" | "));
  LOGLN(msg);

  // ---------- EFFECT ----------
  if (strcmp(topic, topicEffect.c_str()) == 0) {
    int v = atoi(msg.c_str());
    LOG(F("Received EFFECT: ")); LOGLN(v);
    if (v >= 1 && v <= 5) {
      requestEffect(v);
      setPower(true);
    }
    publishEffectState();
    publishPowerState();
    return;
  }

  // ---------- BRIGHTNESS ----------
  if (strcmp(topic, topicBrightness.c_str()) == 0) {
    int v = atoi(msg.c_str());
    setBrightnessPercent(v);
    LOG(F("Set BRIGHTNESS percent: ")); LOGLN(v);
    publishBrightnessState();
    publishPowerState();
    return;
  }

  // ---------- COLOR ----------
  if (strcmp(topic, topicColor.c_str()) == 0) {
    uint8_t r, g, b;
    if (parseRgb(msg.c_str(), r, g, b)) {
      CHSV hsv = rgb2hsv_approximate(CRGB(r, g, b));
      hue = hsv.h;
      sat = hsv.s;
      currentR = r;
      currentG = g;
      currentB = b;
      setPower(true);
      LOG(F("Set COLOR: "));
      LOG(r); LOG(","); LOG(g); LOG(","); LOGLN(b);
      publishColorState();
      publishPowerState();
    } else {
      LOGLN(F("Invalid color format"));
    }
    return;
  }

  // ---------- POWER ----------
  if (strcmp(topic, topicPower.c_str()) == 0) {
    int v = atoi(msg.c_str());
    if (v == 0) {
      setPower(false);
      LOGLN(F("POWER OFF"));
    } else if (v == 1) {
      setPower(true);
      LOGLN(F("POWER ON"));
    }
    publishPowerState();
    return;
  }

  LOGLN(F("MQTT topic not handled"));
}


void reconnect(){
  if (client.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt_server.length() == 0) return;
  if (millis() - lastMqttAttempt < MQTT_RECONNECT_INTERVAL) return;
  lastMqttAttempt = millis();

  LOGLN(F("=== MQTT RECONNECT ==="));
  mqttClientId = "ESP32-" + String((uint32_t)ESP.getEfuseMac());

  if (client.connect(mqttClientId.c_str(), mqtt_username.c_str(), mqtt_password.c_str())) {
    client.subscribe(topicEffect.c_str());
    client.subscribe(topicBrightness.c_str());
    client.subscribe(topicColor.c_str());
    client.subscribe(topicPower.c_str());
    LOGLN(F("Connected and subscribed"));
    publishAllStates();
  } else {
    LOG(F("FAILED, rc="));
    LOGLN(client.state());
  }
}

// ---------- XY MAPPING (змейкой) ----------
uint16_t XY(uint8_t x, uint8_t y) {

  y = HEIGHT - 1 - y;   // переворот по вертикали

  if (y % 2 == 0) {
    return y * WIDTH + x;
  } else {
    return y * WIDTH + (WIDTH - 1 - x);
  }
}

void color(){
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, 255));
}

// ================== 1. ОГОНЬ ==================

#define COOLING  55
#define SPARKING 100
#define SPARK_COUNT 10

struct Spark {
  int x;
  int y;
  uint8_t heat;
  bool active;
};

Spark sparks[SPARK_COUNT];
byte heat[WIDTH][HEIGHT];

void updateSparks() {

  for (int i = 0; i < SPARK_COUNT; i++) {

    if (sparks[i].active) {

      if (sparks[i].x >= 0 && sparks[i].x < WIDTH &&
          sparks[i].y >= 0 && sparks[i].y < HEIGHT) {
        leds[XY(sparks[i].x, sparks[i].y)] += HeatColor(sparks[i].heat);
      }

      sparks[i].y--;                 // искра поднимается
      sparks[i].heat = qsub8(sparks[i].heat, 20);

      if (sparks[i].y < 0 || sparks[i].heat < 20) {
        sparks[i].active = false;
      }
    }
  }

  // создаём новые искры
  if (random8() < 60) {

    for (int i = 0; i < SPARK_COUNT; i++) {

      if (!sparks[i].active) {

        sparks[i].active = true;
        sparks[i].x = random(WIDTH);
        sparks[i].y = 1;
        sparks[i].heat = random8(180, 255);

        break;
      }
    }
  }
}

uint16_t fireNoiseTime = 0;

void fireEffect() {

  fireNoiseTime += 4;

  // 1. охлаждение
  for (int x = 0; x < WIDTH; x++) {
    for (int y = 0; y < HEIGHT; y++) {
      heat[x][y] = qsub8(
        heat[x][y],
        random8(0, ((COOLING * 10) / HEIGHT) + 1)
      );
    }
  }

  // 2. перенос тепла вверх с турбулентностью
  for (int x = 0; x < WIDTH; x++) {
    for (int y = HEIGHT - 1; y >= 1; y--) {

      int shift = map(
        inoise8(x * 50, y * 50, fireNoiseTime),
        0, 255,
        -1, 1
      );

      int srcX = constrain(x + shift, 0, WIDTH - 1);

      if (y >= 2) {
        heat[x][y] =
          (heat[srcX][y - 1] +
           heat[srcX][y - 2] +
           heat[x][y - 2]) / 3;
      } else {
        // y == 1: нет индекса y-2
        heat[x][y] = (heat[srcX][0] + heat[x][0]) / 2;
      }
    }
  }

  // 3. новые языки пламени
  for (int x = 0; x < WIDTH; x++) {

    if (random8() < SPARKING) {

      uint8_t boost = random8(140, 210);

      heat[x][0] = qadd8(heat[x][0], boost);
    }
  }

  // 4. отображение
  for (int x = 0; x < WIDTH; x++) {
    for (int y = 0; y < HEIGHT; y++) {

      uint8_t colorindex = scale8(heat[x][y], 200);

      leds[XY(x, HEIGHT - 1 - y)] = HeatColor(colorindex);
    }
  }

}

// ================== 3. ПЛАВАЮЩИЕ ПЯТНА ==================

uint16_t lavaTime = 0;

void blobsEffect() {

  lavaTime += 2;   // движение

  for (int x = 0; x < WIDTH; x++) {
    for (int y = 0; y < HEIGHT; y++) {

      uint8_t noise = inoise8(x * 40, y * 40, lavaTime);

      // усиливаем контраст
      uint8_t lava = scale8(noise, 200);

      uint8_t hue = map(lava, 0, 255, 0, 45);   // красный → оранжевый

      uint8_t brightness = lava;

      leds[XY(x, y)] = CHSV(hue, 255, brightness);
    }
  }

  // пузырьки лавы
  if (random8() < 60) {
    int bx = random(WIDTH);
    int by = random(HEIGHT);
    leds[XY(bx, by)] += CHSV(20, 255, 255);
  }
}
// ================== 4. ПОЛЯРНОЕ СИЯНИЕ ==================

uint16_t auroraTime = 0;

void auroraEffect() {

  auroraTime += 1;

  for (int x = 0; x < WIDTH; x++) {

    // центр ленты
    uint8_t band = inoise8(x * 40, auroraTime * 2);

    for (int y = 0; y < HEIGHT; y++) {

      uint8_t distance = abs(y * 16 - band);
      uint8_t brightness = qsub8(255, distance * 2);

      uint8_t colorNoise = inoise8(x * 30, y * 30, auroraTime);

      uint8_t hue = 60 + colorNoise / 6;  // зелёно-бирюзовая гамма

      leds[XY(x, y)] = CHSV(hue, 180, brightness);
    }
  }
}

//////////////////////////////////////////////////
// 🟢 MATRIX RAIN
//////////////////////////////////////////////////

int matrixPos[WIDTH];

void matrixEffect() {

  fadeToBlackBy(leds, NUM_LEDS, 40);

  for (int x = 0; x < WIDTH; x++) {

    if (random8() < 20) {
      matrixPos[x]++;
    }

    if (matrixPos[x] >= HEIGHT) {
      matrixPos[x] = 0;
    }

    int y = matrixPos[x];

    leds[XY(x, y)] = CRGB(180,255,180);  // яркая голова
    if (y > 0) leds[XY(x, y-1)] += CRGB(0,150,0);
    if (y > 1) leds[XY(x, y-2)] += CRGB(0,80,0);
  }
}

void renderEffect(uint8_t e) {
  switch (e) {
    case 1: color(); break;
    case 2: fireEffect(); break;
    case 3: blobsEffect(); break;
    case 4: auroraEffect(); break;
    case 5: matrixEffect(); break;
    default: color(); break;
  }
}

// ===================== SETUP =======================
void setup(){
#if LAMP_DEBUG
  Serial.begin(115200);
#endif
#if BTN_ACTIVE_HIGH
  pinMode(BTN_PIN, INPUT_PULLDOWN);
#else
  pinMode(BTN_PIN, INPUT_PULLUP);
#endif
  loadConfig();
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(LED_VOLTS, LED_MAX_MA);
  FastLED.setBrightness(brightness);
  FastLED.clear(); FastLED.show();
  memset((void*)prevLeds, 0, sizeof(prevLeds));
  if(!connectWiFi()) startAP();
  if (!apMode && !deviceId) {
    registerDevice();
  }
  if (!apMode && mqtt_server.length() > 0) {
    client.setServer(mqtt_server.c_str(), mqtt_port);
    client.setCallback(mqttCallback);
    mqttClientId="lamp-ESP32-"+String((uint32_t)ESP.getEfuseMac());
  }
  showStatus(CRGB::Green);
}

// ===================== LOOP =======================
void loop(){

  handleButton();

  if(apMode){
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  if(WiFi.status()!=WL_CONNECTED){
    if(!wifiFailTimer) wifiFailTimer=millis();

    if(millis() - lastWifiReconnect >= WIFI_RECONNECT_INTERVAL){
      lastWifiReconnect = millis();
      LOGLN(F("WiFi lost, reconnecting..."));
      WiFi.reconnect();
      wifiReconnectAttempts++;
    }

    if(wifiReconnectAttempts >= WIFI_RECONNECT_ATTEMPTS &&
       millis() - wifiFailTimer > WIFI_RETRY_TIME){
      startAP();
      wifiFailTimer = 0;
      wifiReconnectAttempts = 0;
      return;
    }
  } else {
    wifiFailTimer = 0;
    wifiReconnectAttempts = 0;
  }

  if(!client.connected()) reconnect();
  else client.loop();

  if(!enabled || !powerOn){
    FastLED.clear();
    FastLED.setBrightness(0);
    FastLED.show();
    delay(5);
    return;
  }

  // ---------- EFFECTS ----------
  uint16_t frameDelay = map(effectSpeed, 0, 100, 120, 10);

  if(millis() - lastFrame >= frameDelay){
    lastFrame = millis();

    if(nextEffect != effect){
      renderEffect(nextEffect);
      for (uint16_t i = 0; i < NUM_LEDS; i++) {
        leds[i] = blend(prevLeds[i], leds[i], effectBlend);
      }
      effectBlend = qadd8(effectBlend, 8);
      if(effectBlend >= 250){
        effect = nextEffect;
        effectBlend = 0;
        memcpy((void*)prevLeds, (const void*)leds, sizeof(leds));
      }
    }
    else{
      renderEffect(effect);
      memcpy((void*)prevLeds, (const void*)leds, sizeof(leds));
    }

    FastLED.setBrightness(brightness);
    FastLED.show();
    yield();
  }
}