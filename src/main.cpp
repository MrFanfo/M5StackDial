#include <M5Unified.h>
#include <M5Dial.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <vector>




// ===== WiFi + MQTT Config =====
const char* WIFI_SSID     = "Casa Cuccioli";
const char* WIFI_PASS     = "Cuccioli1725";
const char* MQTT_HOST     = "192.168.1.27";   // HA broker
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "mqttuser";
const char* MQTT_PASSWD   = "Fanfo1122";
const char* MQTT_CLIENTID = "m5dial_menu";
bool enableClockOnIdle = true; // <<< NEW: toggleable setting
int displayBrightness = 180; // current screen brightness (0–255)
String publicIP = "-";
String otaAddress = "-";
int hue = 0;   // 0..359, rainbow angle


//web server
WebServer server(80);
// Log buffer
const int WEB_LOG_SIZE = 50;
String webLog[WEB_LOG_SIZE];
int webLogIndex = 0;
//
void addWebLog(const String &msg) {
  webLog[webLogIndex] = msg;
  webLogIndex = (webLogIndex + 1) % WEB_LOG_SIZE;
  Serial.println(msg); // still goes to Serial too
}
// Simple printf-style logging
void logMessage(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  addWebLog(String(buf));  // store + Serial print
}
void setupWebServer() {
  server.on("/", []() {
    String page = "<html><head><meta http-equiv='refresh' content='2'><title>M5Dial Log</title></head><body><h2>M5Dial Logs</h2><pre>";
    for (int i = 0; i < WEB_LOG_SIZE; i++) {
      if (webLog[i].length() > 0) {
        page += webLog[i] + "\n";
      }
    }
    page += "</pre></body></html>";
    server.send(200, "text/html", page);
  });

  server.begin();
  logMessage("WebServer started at http://%s", WiFi.localIP().toString().c_str());
}

unsigned long lastIPCheck = 0;
const unsigned long IP_CHECK_INTERVAL = 60000; // check every 60s


// ===== Weather Config =====
#include <HTTPClient.h>
#include <ArduinoJson.h>
const char* WEATHER_API_KEY = "a0c24e1cb91c5e7f30254f2fb0d6aab0";
const char* WEATHER_CITY = "Moncalieri"; // Change to your city
String weatherDesc = "";
String weatherTemp = "";
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 20 * 1000; // 20 seconds (3 calls per minute)
// Weather debug log
const int WEATHER_LOG_SIZE = 10;
String weatherLog[WEATHER_LOG_SIZE];
int weatherLogIndex = 0;
int weatherLogViewIndex = 0;
int mqttLogScrollOffset = 0;
int weatherLogScrollOffset = 0;

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://api.openweathermap.org/data/2.5/weather?q=") + WEATHER_CITY + "&appid=" + WEATHER_API_KEY + "&units=metric";
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      float temp = doc["main"]["temp"];
      weatherTemp = String((int)temp) + "°C";
      weatherDesc = doc["weather"][0]["main"].as<String>();
    }
    // Save raw JSON for debug
    weatherLog[weatherLogIndex] = payload;
    weatherLogIndex = (weatherLogIndex + 1) % WEATHER_LOG_SIZE;
  }
  http.end();
}
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;       // adjust for your timezone (Italy = UTC+1)
const int   daylightOffset_sec = 3600;  // summer time

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ===== Device model =====
enum DeviceType { LIGHT, SWITCH, BLIND };
// Extended Device struct with RGB for lights
struct Device {
  const char* name;
  DeviceType type;
  int brightness;  // 0..255 for lights
  bool state;
  int r, g, b;
  int hue;
  int color_temp;  // <<< NEW: color temperature (153 warm → 500 cold)

  Device(const char* n, DeviceType t, int br, bool st,
         int rr = 255, int gg = 255, int bb = 255, int hh = 0, int ct = 300)
    : name(n), type(t), brightness(br), state(st),
      r(rr), g(gg), b(bb), hue(hh), color_temp(ct) {}
};


// ---- Lights ----
Device lights[] = {
  {"Ender3",        LIGHT, 128, false},
  {"Soggiorno",     LIGHT, 128, false},
  {"Droid",         LIGHT, 128, false},
  {"Coca Cola",     LIGHT, 128, false},
  {"Studio Light",  LIGHT, 128, false},
  {"Skadis",        LIGHT, 128, false},
  {"The One Ring",  LIGHT, 128, false},
  {"Kitchen",       LIGHT, 128, false},
  {"Espositore",    LIGHT, 128, false},
  {"Candele",       LIGHT, 128, false}
};
int numLights = sizeof(lights) / sizeof(Device);

// ---- Switches ----
Device switches[] = {
  {"A1 Mini",           SWITCH, 0, false},
  {"Ender 3 Switch",    SWITCH, 0, false},
  {"The 100",           SWITCH, 0, false},
  {"Luci Fuori",        SWITCH, 0, false},
  {"Stronzetta Switch", SWITCH, 0, false}
};
int numSwitches = sizeof(switches) / sizeof(Device);

// ---- Blinds ----
Device blinds[] = {
  {"Bathroom Blind",       BLIND, 0, false},
  {"Bedroom Blind",        BLIND, 0, false},
  {"Kitchen Blind",        BLIND, 0, false},
  {"Laundry Blind",        BLIND, 0, false},
  {"Living Room Left",     BLIND, 0, false},
  {"Living Room Right",    BLIND, 0, false},
  {"Small Bathroom Blind", BLIND, 0, false},
  {"Studio Blind",         BLIND, 0, false},
  {"Studio Main Blind",    BLIND, 0, false}
};
int numBlinds = sizeof(blinds) / sizeof(Device);

// ===== UI / Menu State =====
enum ScreenMode {
  MENU_CATEGORIES, MENU_LIGHTS, MENU_SWITCHES, MENU_BLINDS,
  MENU_SETTINGS, SETTINGS_WIFI, SETTINGS_MQTT, SETTINGS_WEATHER_DEBUG,
  SETTINGS_DISPLAY, SETTINGS_BRIGHTNESS,
  CONTROL_SCREEN, COLOR_SCREEN, WHITE_SCREEN
};




ScreenMode currentMode = MENU_CATEGORIES;

int   menuIndex      = 0;
long  oldDetentPos   = -999;
Device* selectedDevice = nullptr;

// ===== Screen helpers =====
int  SCR_W, SCR_H, CX, CY;
const int SAFE_MARGIN = 32;
const int ROW_H       = 28;

// ===== UI Theme =====
inline uint16_t uiBg()        { return M5Dial.Display.color565(18, 22, 30); }
inline uint16_t uiPanel()     { return M5Dial.Display.color565(28, 34, 46); }
inline uint16_t uiPanelSoft() { return M5Dial.Display.color565(38, 46, 62); }
inline uint16_t uiText()      { return M5Dial.Display.color565(230, 236, 247); }
inline uint16_t uiTextMuted() { return M5Dial.Display.color565(150, 162, 182); }
inline uint16_t uiAccent()    { return M5Dial.Display.color565(92, 169, 255); }
inline uint16_t uiSuccess()   { return M5Dial.Display.color565(58, 212, 152); }
inline uint16_t uiDanger()    { return M5Dial.Display.color565(255, 107, 129); }
inline uint16_t uiGlow()      { return M5Dial.Display.color565(120, 98, 255); }

void drawBackgroundGradient(uint16_t top, uint16_t bottom) {
  int w = M5Dial.Display.width();
  int h = M5Dial.Display.height();
  uint8_t tr = ((top >> 11) & 0x1F) << 3;
  uint8_t tg = ((top >> 5) & 0x3F) << 2;
  uint8_t tb = (top & 0x1F) << 3;
  uint8_t br = ((bottom >> 11) & 0x1F) << 3;
  uint8_t bg = ((bottom >> 5) & 0x3F) << 2;
  uint8_t bb = (bottom & 0x1F) << 3;

  for (int y = 0; y < h; y++) {
    float t = (h <= 1) ? 0 : (float)y / (h - 1);
    uint8_t r = tr + (br - tr) * t;
    uint8_t g = tg + (bg - tg) * t;
    uint8_t b = tb + (bb - tb) * t;
    M5Dial.Display.drawFastHLine(0, y, w, M5Dial.Display.color565(r, g, b));
  }
}

int menuVisibleSlots(int count) {
  int usableTop = SAFE_MARGIN + 24;
  int usableHeight = SCR_H - usableTop - SAFE_MARGIN;
  int dynamicRows = usableHeight / ROW_H;
  int capped = constrain(dynamicRows, 3, 6);
  return min(capped, count);
}

String fitLabel(const String& value, int maxWidth) {
  if (M5Dial.Display.textWidth(value) <= maxWidth) return value;
  String out = value;
  while (out.length() > 1 && M5Dial.Display.textWidth(out + "…") > maxWidth) {
    out.remove(out.length() - 1);
  }
  return out + "…";
}

String brightnessPercentLabel(int value255) {
  int pct = map(constrain(value255, 0, 255), 0, 255, 0, 100);
  return String(pct) + "%";
}

void drawMenuHint(const char* text) {
  int w = SCR_W - SAFE_MARGIN * 2;
  int x = SAFE_MARGIN;
  int y = SCR_H - SAFE_MARGIN - 2;
  M5Dial.Display.fillRoundRect(x, y, w, 18, 8, uiPanel());
  M5Dial.Display.drawRoundRect(x, y, w, 18, 8, uiPanelSoft());
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextSize(1);
  M5Dial.Display.setTextColor(uiTextMuted(), uiPanel());
  M5Dial.Display.drawString(text, CX, y + 9);
}

void drawMenuCardRow(int y, bool selected, const String& label, const String& rightText = "", uint16_t rightColor = 0) {
  int rowH = 28;
  int rowW = SCR_W - SAFE_MARGIN * 2;
  int rowX = SAFE_MARGIN;
  uint16_t bg = selected ? uiPanelSoft() : uiPanel();
  uint16_t border = selected ? uiAccent() : uiPanelSoft();

  if (selected) {
    M5Dial.Display.fillRoundRect(rowX - 2, y - rowH / 2 - 2, rowW + 4, rowH + 4, 10, M5Dial.Display.color565(22, 64, 104));
  }
  M5Dial.Display.fillRoundRect(rowX, y - rowH / 2, rowW, rowH, 9, bg);
  M5Dial.Display.drawRoundRect(rowX, y - rowH / 2, rowW, rowH, 9, border);

  M5Dial.Display.setTextDatum(middle_left);
  M5Dial.Display.setTextSize(1);
  M5Dial.Display.setTextColor(selected ? uiText() : uiTextMuted(), bg);
  M5Dial.Display.drawString(label, rowX + 7, y);

  if (rightText.length() > 0) {
    M5Dial.Display.setTextDatum(middle_right);
    M5Dial.Display.setTextColor(rightColor == 0 ? uiAccent() : rightColor, bg);
    M5Dial.Display.drawString(rightText, rowX + rowW - 7, y);
  }
}

// ===== MQTT log buffer =====
const int MQTT_LOG_SIZE = 30;
String mqttLog[MQTT_LOG_SIZE];
int mqttLogIndex     = 0;
int mqttLogViewIndex = 0;
// Add a new line to the MQTT log
void addMqttLog(const String& line) {
  mqttLog[mqttLogIndex] = line;
  mqttLogIndex = (mqttLogIndex + 1) % MQTT_LOG_SIZE;
  
}
// ===== Color conversion helper =====
void hsvToRgb(int h, int s, int v, int &r, int &g, int &b) {
  float hf = h / 60.0;
  int i = floor(hf);
  float f = hf - i;
  int p = v * (255 - s) / 255;
  int q = v * (255 - s * f) / 255;
  int t = v * (255 - s * (1 - f)) / 255;

  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
  }
}


// ===== MQTT publish helpers =====
void mqttPublishDevice(Device* dev, const char* action, int value = -1) {
  if (!mqtt.connected()) return;

  String payload = "{";
  payload += "\"device\":\""; payload += dev->name; payload += "\",";
  payload += "\"type\":\"";
  if (dev->type == LIGHT) payload += "light";
  else if (dev->type == SWITCH) payload += "switch";
  else if (dev->type == BLIND)  payload += "blind";
  payload += "\",";
  payload += "\"action\":\""; payload += action; payload += "\"";

  if (value >= 0) {
    payload += ",\"value\":"; payload += value;
  }

  // Handle color updates
  if (strcmp(action, "color") == 0) {
    payload += ",\"r\":"; payload += dev->r;
    payload += ",\"g\":"; payload += dev->g;
    payload += ",\"b\":"; payload += dev->b;
    payload += ",\"hue\":"; payload += dev->hue;
  }

  // Handle white mode (color_temp)
  if (strcmp(action, "color_temp") == 0) {
    payload += ",\"color_temp\":"; payload += dev->color_temp;
  }

  payload += "}";

  mqtt.publish("m5dial/devices", payload.c_str(), true);
  addMqttLog("PUB → " + payload);
  logMessage("PUB → %s", payload.c_str());
}

// Request full state update from broker
void mqttRequestState() {
  if (!mqtt.connected()) return;
  mqtt.publish("m5dial/request/state", "all");
  addMqttLog("PUB → request state");
  logMessage("PUB → request state");

}
// ===== Round-safe UI helpers =====
void drawHeader(const char* title) {
  int barW = SCR_W - SAFE_MARGIN*2;
  int barX = SAFE_MARGIN;
  int barY = SAFE_MARGIN - 10;
  M5Dial.Display.fillRoundRect(barX, barY, barW, 24, 8, uiPanelSoft());
  M5Dial.Display.drawRoundRect(barX, barY, barW, 24, 8, uiAccent());
  uint16_t wifiDot = WiFi.status() == WL_CONNECTED ? uiSuccess() : uiDanger();
  uint16_t mqttDot = mqtt.connected() ? uiSuccess() : uiDanger();
  M5Dial.Display.fillCircle(barX + 10, barY + 12, 3, wifiDot);
  M5Dial.Display.fillCircle(barX + 20, barY + 12, 3, mqttDot);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextSize(1);
  M5Dial.Display.setTextColor(uiText(), uiPanelSoft());
  M5Dial.Display.drawString(title, CX, barY + 12);
  M5Dial.Display.setTextSize(2);
}
// ===== Color Screen =====
void drawColorScreen(Device* dev) {
  drawBackgroundGradient(M5Dial.Display.color565(7, 10, 16), M5Dial.Display.color565(23, 16, 42));
  drawHeader((String(dev->name) + " Color").c_str());

  int r, g, b;
  hsvToRgb(dev->hue, 255, 255, r, g, b);
  dev->r = r; dev->g = g; dev->b = b;

  // Rainbow border
  int radius = min(CX, CY) - 10;
  for (int h = 0; h < 360; h+=2) {
    int rr, gg, bb;
    hsvToRgb(h, 255, 255, rr, gg, bb);
    uint16_t col = M5Dial.Display.color565(rr,gg,bb);
    float rad = h * PI / 180.0;
    int x0 = CX + cos(rad) * (radius-6);
    int y0 = CY + sin(rad) * (radius-6);
    int x1 = CX + cos(rad) * (radius);
    int y1 = CY + sin(rad) * (radius);
    M5Dial.Display.drawLine(x0, y0, x1, y1, col);
  }

  // Glass center
  M5Dial.Display.fillCircle(CX, CY, radius - 24, M5Dial.Display.color565(8, 12, 20));
  M5Dial.Display.drawCircle(CX, CY, radius - 24, uiPanelSoft());

  // Pointer arrow showing current hue
  float rad = dev->hue * PI / 180.0;
  int pointerLen = radius - 12;   // arrow length
  int x0 = CX;
  int y0 = CY;
  int x1 = CX + cos(rad) * pointerLen;
  int y1 = CY + sin(rad) * pointerLen;

  // Draw arrow line
  M5Dial.Display.drawLine(x0, y0, x1, y1, M5Dial.Display.color565(255,255,255));
  // Dot at the tip
  M5Dial.Display.fillCircle(x1, y1, 4, M5Dial.Display.color565(255,255,255));

  // Show RGB values in the center
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextSize(2);
  M5Dial.Display.setTextColor(uiText(), M5Dial.Display.color565(8, 12, 20));
  char buf[32];
  snprintf(buf, sizeof(buf), "R:%d G:%d B:%d", r, g, b);
  M5Dial.Display.drawString(buf, CX, CY);
  drawMenuHint("Rotate hue • Double click for white");
}

// ===== Scrollable Menus =====
void drawScrollableMenu(Device* list, int count, int selected, const char* title, bool showBrightness) {
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(12, 16, 24));
  drawHeader(title);

  if (count <= 0) {
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(uiTextMuted());
    M5Dial.Display.drawString("(none)", CX, CY);
    return;
  }

  int visibleCount = menuVisibleSlots(count);
  int half = visibleCount / 2;

  int start = selected - half;
  if (start < 0) start = 0;
  if (start > count - visibleCount) start = count - visibleCount;

  int totalHeight = visibleCount * ROW_H;
  int startY = CY - totalHeight / 2 + ROW_H/2;

  for (int i = 0; i < visibleCount; i++) {
    int idx = start + i;
    int y = startY + i * ROW_H;

    uint16_t stateColor = list[idx].state ? uiSuccess() : uiDanger();
    String stateText = list[idx].state ? "ON" : "OFF";

    if (list[idx].type == LIGHT && showBrightness) {
      if (list[idx].state) stateText += " " + brightnessPercentLabel(list[idx].brightness);
    } else if (list[idx].type == BLIND) {
      stateText = list[idx].state ? "OPEN" : "CLOSED";
    }

    int rightZone = 74;
    int labelWidth = SCR_W - SAFE_MARGIN * 2 - rightZone;
    drawMenuCardRow(y, idx == selected, fitLabel(list[idx].name, labelWidth), stateText, stateColor);
  }
  drawMenuHint("Rotate • Press to open");
}
// ===== Menus =====
void drawCategories() {
  const char* items[] = {"Lights", "Switches", "Blinds", "Settings"};
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(14, 20, 28));
  drawHeader("Categories");

  int visibleCount = 4;
  int totalHeight = visibleCount * ROW_H;
  int startY = CY - totalHeight / 2 + ROW_H/2;

  for (int i = 0; i < visibleCount; i++) {
    int y = startY + i * ROW_H;
    drawMenuCardRow(y, i == menuIndex, items[i]);
  }
  drawMenuHint("Rotate • Press to enter");
}
/// Note: lights show brightness
void drawLightsMenu()   { drawScrollableMenu(lights, numLights, menuIndex, "Lights", true); }



/// Note: lights show brightness
void drawSwitchesMenu() {
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(11, 18, 26));
  drawHeader("Switches");

  int count = numSwitches;
  if (count <= 0) {
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(M5Dial.Display.color565(100,100,100));
    M5Dial.Display.drawString("(none)", CX, CY);
    return;
  }

  int visibleCount = menuVisibleSlots(count);
  int half = visibleCount / 2;

  int start = menuIndex - half;
  if (start < 0) start = 0;
  if (start > count - visibleCount) start = count - visibleCount;

  // spacing between rows
  int spacing = 6;  
  int itemHeight = ROW_H - spacing;
  int totalHeight = visibleCount * (itemHeight + spacing);
  int startY = CY - totalHeight / 2 + itemHeight / 2;

  for (int i = 0; i < visibleCount; i++) {
    int idx = start + i;
    int y = startY + i * (itemHeight + spacing);

    bool isSelected = (idx == menuIndex);
    bool state = switches[idx].state;

    int rightZone = 60;
    int labelWidth = SCR_W - SAFE_MARGIN * 2 - rightZone;
    drawMenuCardRow(
      y,
      isSelected,
      fitLabel(switches[idx].name, labelWidth),
      state ? "ON" : "OFF",
      state ? uiSuccess() : uiDanger()
    );
  }
  drawMenuHint("Rotate • Press to toggle");
}


/// Note: blinds do not show brightness
void drawBlindsMenu()   { drawScrollableMenu(blinds, numBlinds, menuIndex, "Blinds", false); }
/// ===== Settings Menu =====
void drawSettingsMenu() {
  const char* items[] = {"WiFi Info", "MQTT Info", "Weather Debug", "Display Options"};

  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(16, 20, 30));
  drawHeader("Settings");

  int totalHeight = 4 * ROW_H;
  int startY = CY - totalHeight / 2 + ROW_H/2;
  for (int i=0; i<4; i++) {
    int y = startY + i * ROW_H;
    drawMenuCardRow(y, i == menuIndex, items[i]);
  }
  drawMenuHint("Rotate • Press to open");
}
// ===== Settings: WiFi Info Screen =====
void drawSettingsWiFi() {
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(8, 16, 28));
  drawHeader("WiFi Status");

  String ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "(offline)";
  int rssi    = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  String ip   = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "-";
  String mqttStatus = mqtt.connected() ? "Connected" : "Disconnected";

  // Uptime
  unsigned long secs = millis() / 1000;
  int hh = (secs / 3600) % 24;
  int mm = (secs / 60) % 60;
  int ss = secs % 60;
  char uptimeBuf[32];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%02d:%02d:%02d", hh, mm, ss);

  const String labels[] = {"SSID", "RSSI", "Local IP", "Public IP", "MQTT", "Uptime", "OTA"};
  const String values[] = {ssid, String(rssi) + " dBm", ip, publicIP, mqttStatus, String(uptimeBuf), otaAddress};

  int rowH = 24;
  int topY = CY - 76;
  int rowW = SCR_W - SAFE_MARGIN * 2;
  for (int i = 0; i < 7; i++) {
    int y = topY + i * rowH;
    M5Dial.Display.fillRoundRect(SAFE_MARGIN, y - 10, rowW, 20, 7, uiPanel());
    M5Dial.Display.drawRoundRect(SAFE_MARGIN, y - 10, rowW, 20, 7, uiPanelSoft());
    M5Dial.Display.setTextDatum(middle_left);
    M5Dial.Display.setTextColor(uiTextMuted(), uiPanel());
    M5Dial.Display.drawString(labels[i], SAFE_MARGIN + 8, y);
    M5Dial.Display.setTextDatum(middle_right);
    uint16_t valueColor = (labels[i] == "MQTT") ? (mqtt.connected() ? uiSuccess() : uiDanger()) : uiText();
    M5Dial.Display.setTextColor(valueColor, uiPanel());
    M5Dial.Display.drawString(fitLabel(values[i], rowW - 70), SAFE_MARGIN + rowW - 8, y);
  }
  drawMenuHint("Long press to go home");
}

void drawWhiteScreen(Device* dev) {
  drawBackgroundGradient(M5Dial.Display.color565(22, 18, 14), M5Dial.Display.color565(44, 32, 20));
  drawHeader((String(dev->name) + " White").c_str());

  // Bar from warm (153) to cold (500)
  int barX = SAFE_MARGIN;
  int barY = CY;
  int barW = SCR_W - SAFE_MARGIN*2;
  int barH = 20;

  for (int i=0; i<barW; i++) {
    int ct = map(i, 0, barW, 153, 500);
    int r = map(ct, 153, 500, 255, 200);
    int g = map(ct, 153, 500, 180, 220);
    int b = map(ct, 153, 500, 120, 255);
    M5Dial.Display.drawFastVLine(barX+i, barY, barH, M5Dial.Display.color565(r,g,b));
  }

  // Draw indicator for current CT
  int pos = map(dev->color_temp, 500, 153, 0, barW);
  M5Dial.Display.drawLine(barX+pos, barY-5, barX+pos, barY+barH+5, M5Dial.Display.color565(255,255,255));

  // Show CT value
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(uiText(), M5Dial.Display.color565(40, 32, 22));
  char buf[32];
  snprintf(buf, sizeof(buf), "CT: %d", dev->color_temp);
  M5Dial.Display.drawString(buf, CX, barY+40);
  drawMenuHint("Rotate warmth • Double click to return");
}

// ===== Smooth MQTT Log Screen =====
void drawSettingsMQTT() {
  static LGFX_Sprite spr(&M5Dial.Display);  
  const int zoneTop = SAFE_MARGIN + 28;
  const int zoneHeight = SCR_H - SAFE_MARGIN - zoneTop;

  if (spr.width() != SCR_W || spr.height() != zoneHeight) {
    spr.deleteSprite();
    spr.createSprite(SCR_W, zoneHeight);
  }

  static int lastMsgIndex = -1;
  int currentMsgIndex = mqttLogViewIndex;
  static int scrollOffset = 0;
  static unsigned long lastScroll = 0;

  if (currentMsgIndex != lastMsgIndex) {
    scrollOffset = 0;
    lastScroll = millis();
    lastMsgIndex = currentMsgIndex;
  }

  spr.fillSprite(uiBg());

  // Draw message number
  char numBuf[32];
  snprintf(numBuf, sizeof(numBuf), "Msg %d/%d", currentMsgIndex + 1, MQTT_LOG_SIZE);
  spr.setTextDatum(middle_center);
  spr.setTextSize(1);
  spr.setTextColor(uiTextMuted(), uiBg());
  spr.drawString(numBuf, SCR_W/2, 8);

  // Scroll zone
  int scrollTop = 24;
  int scrollHeight = zoneHeight - 24;

  int total = MQTT_LOG_SIZE;
  int idx = mqttLogIndex - 1 - currentMsgIndex;
  if (idx < 0) idx += total;
  String msg = mqttLog[idx];
  if (msg.length() == 0) msg = "(empty)";

  // Word wrap
  int maxWidth = SCR_W - SAFE_MARGIN*2;
  std::vector<String> lines;
  int start = 0;
  while (start < msg.length()) {
    int len = 1;
    while (start + len <= msg.length() &&
           spr.textWidth(msg.substring(start, start+len)) < maxWidth) len++;
    len--;
    lines.push_back(msg.substring(start, start+len));
    start += len;
  }

  const int lineHeight = 18;
  int totalLines = lines.size();
  int totalHeight = totalLines * lineHeight;
  int visibleLines = scrollHeight / lineHeight;

  if (totalLines <= visibleLines) {
    int y = scrollTop + (scrollHeight - totalHeight) / 2;
    for (int i = 0; i < totalLines; i++) {
      spr.drawString(lines[i], SCR_W/2, y + i * lineHeight);
    }
  } else {
    if (millis() - lastScroll > 50) {
      lastScroll = millis();
      scrollOffset++;
      if (scrollOffset > totalHeight) scrollOffset = 0;
    }
    int y = scrollTop - scrollOffset;
    for (int i=0; i<totalLines; i++) {
      int drawY = y + i*lineHeight;
      if (drawY >= scrollTop && drawY <= scrollTop+scrollHeight-lineHeight) {
        spr.drawString(lines[i], SCR_W/2, drawY);
      }
    }
  }

  spr.pushSprite(0, zoneTop);
}
// ===== Display Options Screen =====
void drawSettingsDisplay() {
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(13, 20, 34));
  drawHeader("Display Options");

  // define options list
  const char* items[] = {
    "Clock on Idle",
    "Brightness",
    "Theme: Midnight"
  };
  const int itemCount = sizeof(items) / sizeof(items[0]);

  int visibleCount = min(5, itemCount);
  int spacing = 6;
  int itemHeight = ROW_H - spacing;
  int totalHeight = visibleCount * (itemHeight + spacing);
  int startY = CY - totalHeight / 2 + itemHeight / 2;

  // determine starting index so the selected one stays centered
  int half = visibleCount / 2;
  int start = menuIndex - half;
  if (start < 0) start = 0;
  if (start > itemCount - visibleCount) start = itemCount - visibleCount;

  for (int i = 0; i < visibleCount; i++) {
    int idx = start + i;
    int y = startY + i * (itemHeight + spacing);
    bool isSelected = (idx == menuIndex);

    uint16_t bgColor = uiPanel();

    if (idx == 0) {
      bgColor = enableClockOnIdle
                ? M5Dial.Display.color565(37, 94, 75)
                : M5Dial.Display.color565(105, 48, 66);
    }
    if (idx == 1) bgColor = M5Dial.Display.color565(58, 74, 106);
    if (idx == 2) bgColor = M5Dial.Display.color565(62, 54, 98);

    // outline if selected
    if (isSelected) {
      M5Dial.Display.fillRoundRect(SAFE_MARGIN - 2, y - itemHeight / 2 - 2,
                                   SCR_W - SAFE_MARGIN * 2 + 4, itemHeight + 4, 10,
                                   uiAccent());
    }

    // background
    M5Dial.Display.fillRoundRect(SAFE_MARGIN, y - itemHeight / 2,
                                 SCR_W - SAFE_MARGIN * 2, itemHeight, 8, bgColor);

    // text
    M5Dial.Display.setTextDatum(middle_left);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(uiText(), bgColor);
    M5Dial.Display.drawString(items[idx], SAFE_MARGIN + 8, y);

    if (idx == 0) {
      M5Dial.Display.setTextDatum(middle_right);
      M5Dial.Display.setTextColor(enableClockOnIdle ? uiSuccess() : uiDanger(), bgColor);
      M5Dial.Display.drawString(enableClockOnIdle ? "ON" : "OFF", SCR_W - SAFE_MARGIN - 8, y);
    }
  }
  drawMenuHint("Press to change • Long press back");
}



// ===== Smooth Weather Debug Screen =====
void drawSettingsWeatherDebug() {
  static LGFX_Sprite spr(&M5Dial.Display);  
  const int zoneTop = SAFE_MARGIN + 28;
  const int zoneHeight = SCR_H - SAFE_MARGIN - zoneTop;

  if (spr.width() != SCR_W || spr.height() != zoneHeight) {
    spr.deleteSprite();
    spr.createSprite(SCR_W, zoneHeight);
  }

  static int lastMsgIndex = -1;
  int currentMsgIndex = weatherLogViewIndex;
  static int scrollOffset = 0;
  static unsigned long lastScroll = 0;

  if (currentMsgIndex != lastMsgIndex) {
    scrollOffset = 0;
    lastScroll = millis();
    lastMsgIndex = currentMsgIndex;
  }

  spr.fillSprite(uiBg());

  // Draw message number
  char numBuf[32];
  snprintf(numBuf, sizeof(numBuf), "Log %d/%d", currentMsgIndex + 1, WEATHER_LOG_SIZE);
  spr.setTextDatum(middle_center);
  spr.setTextSize(1);
  spr.setTextColor(uiTextMuted(), uiBg());
  spr.drawString(numBuf, SCR_W/2, 8);

  // Scroll zone
  int scrollTop = 24;
  int scrollHeight = zoneHeight - 24;

  int total = WEATHER_LOG_SIZE;
  int idx = weatherLogIndex - 1 - currentMsgIndex;
  if (idx < 0) idx += total;
  String msg = weatherLog[idx];
  if (msg.length() == 0) msg = "(empty)";

  // Word wrap
  int maxWidth = SCR_W - SAFE_MARGIN*2;
  std::vector<String> lines;
  int start = 0;
  while (start < msg.length()) {
    int len = 1;
    while (start + len <= msg.length() &&
           spr.textWidth(msg.substring(start, start+len)) < maxWidth) len++;
    len--;
    lines.push_back(msg.substring(start, start+len));
    start += len;
  }

  const int lineHeight = 18;
  int totalLines = lines.size();
  int totalHeight = totalLines * lineHeight;
  int visibleLines = scrollHeight / lineHeight;

  if (totalLines <= visibleLines) {
    int y = scrollTop + (scrollHeight - totalHeight) / 2;
    for (int i = 0; i < totalLines; i++) {
      spr.drawString(lines[i], SCR_W/2, y + i * lineHeight);
    }
  } else {
    if (millis() - lastScroll > 50) {
      lastScroll = millis();
      scrollOffset++;
      if (scrollOffset > totalHeight) scrollOffset = 0;
    }
    int y = scrollTop - scrollOffset;
    for (int i=0; i<totalLines; i++) {
      int drawY = y + i*lineHeight;
      if (drawY >= scrollTop && drawY <= scrollTop+scrollHeight-lineHeight) {
        spr.drawString(lines[i], SCR_W/2, drawY);
      }
    }
  }

  spr.pushSprite(0, zoneTop);
}

// ===== Brightness adjustment screen =====
void drawSettingsBrightness() {
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(20, 20, 20));
  drawHeader("Brightness");

  // Bar dimensions
  int barX = SAFE_MARGIN;
  int barY = CY;
  int barW = SCR_W - SAFE_MARGIN * 2;
  int barH = 20;

  M5Dial.Display.fillRoundRect(barX - 5, barY - 8, barW + 10, barH + 16, 10, uiPanel());
  // Draw bar background
  for (int i = 0; i < barW; i++) {
    int val = map(i, 0, barW, 0, 255);
    M5Dial.Display.drawFastVLine(barX + i, barY, barH,
      M5Dial.Display.color565(val, val, val));
  }

  // Indicator position
  int pos = map(displayBrightness, 0, 255, 0, barW);
  M5Dial.Display.drawLine(barX + pos, barY - 5, barX + pos, barY + barH + 5,
                          uiAccent());

  // Value text
  char buf[32];
  int pct = map(displayBrightness, 0, 255, 0, 100);
  snprintf(buf, sizeof(buf), "%d%%", pct);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextSize(2);
  M5Dial.Display.setTextColor(uiText(), uiBg());
  M5Dial.Display.drawString(buf, CX, CY + 40);

  M5Dial.Display.setTextSize(1);
  M5Dial.Display.drawString("Rotate to adjust, click to go back", CX, CY + 70);
}

// ===== Control Screen =====
void drawControlScreen(Device* dev) {
  drawBackgroundGradient(uiBg(), M5Dial.Display.color565(14, 18, 30));
  drawHeader(dev->name);

  if (dev->type == LIGHT) {
    int r_outer = min(CX, CY) - 12;
    int r_inner = r_outer - 8;
    float startAngle = 40;
    float endAngle   = 320;
    float arcRange   = endAngle - startAngle;
    float brightAngle = startAngle + arcRange * dev->brightness / 255.0;

    M5Dial.Display.drawCircle(CX, CY, r_outer + 2, uiGlow());
    for (float a = startAngle; a <= endAngle; a += 1.5) {
      float rad = a * PI / 180.0;
      int x0 = CX + cos(rad) * r_inner;
      int y0 = CY + sin(rad) * r_inner;
      int x1 = CX + cos(rad) * r_outer;
      int y1 = CY + sin(rad) * r_outer;
      M5Dial.Display.drawLine(x0, y0, x1, y1, uiPanelSoft());
    }
    int r_fill_outer = r_outer;
    int r_fill_inner = r_inner - 2;
    for (float a = startAngle; a <= brightAngle; a += 1.5) {
      float rad = a * PI / 180.0;
      int x0 = CX + cos(rad) * r_fill_inner;
      int y0 = CY + sin(rad) * r_fill_inner;
      int x1 = CX + cos(rad) * r_fill_outer;
      int y1 = CY + sin(rad) * r_fill_outer;
      M5Dial.Display.drawLine(x0, y0, x1, y1, uiAccent());
    }

    M5Dial.Display.setTextSize(3);
    uint16_t onColor = dev->state ? uiSuccess() : uiDanger();
    String onoffStr = dev->state ? "ON" : "OFF";
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(onColor, uiBg());
    M5Dial.Display.drawString(onoffStr, CX, CY + 10);
    M5Dial.Display.setTextSize(2);
    M5Dial.Display.setTextColor(uiTextMuted(), uiBg());
    M5Dial.Display.drawString(brightnessPercentLabel(dev->brightness), CX, CY + 42);
  } else if (dev->type == BLIND) {
    uint16_t blindColor = dev->state ? uiSuccess() : uiDanger();
    M5Dial.Display.setTextSize(3);
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(blindColor, uiBg());
    M5Dial.Display.drawString(dev->state ? "OPEN" : "CLOSED", CX, CY);
    M5Dial.Display.setTextSize(2);
  } else {
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextColor(uiText());
    M5Dial.Display.drawString("Press to toggle", CX, CY);
  }
  if (dev->type == LIGHT) drawMenuHint("Press toggle • Double click color");
  else if (dev->type == BLIND) drawMenuHint("Press toggle • Long press home");
  else drawMenuHint("Press to toggle • Long press home");
}
// ===== Dispatcher =====
void drawCurrentScreen() {
  switch (currentMode) {
    case MENU_CATEGORIES: drawCategories(); break;
    case SETTINGS_WEATHER_DEBUG: drawSettingsWeatherDebug(); break;
    case MENU_LIGHTS:     drawLightsMenu(); break;
    case WHITE_SCREEN: if(selectedDevice) drawWhiteScreen(selectedDevice); break;
    case MENU_SWITCHES:   drawSwitchesMenu(); break;
    case MENU_BLINDS:     drawBlindsMenu(); break;
    case MENU_SETTINGS:   drawSettingsMenu(); break;
    case SETTINGS_DISPLAY: drawSettingsDisplay(); break;
    case SETTINGS_WIFI:   drawSettingsWiFi(); break;
    case SETTINGS_MQTT:   drawSettingsMQTT(); break;
    case SETTINGS_BRIGHTNESS: drawSettingsBrightness(); break;
    case COLOR_SCREEN: if (selectedDevice) drawColorScreen(selectedDevice); break;
    case CONTROL_SCREEN:  if (selectedDevice) drawControlScreen(selectedDevice); break;
  }
}
// ===== WiFi / MQTT =====
void connectWiFi() {
  logMessage("[WiFi] Connecting to %s...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 60) {
    delay(250);
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    logMessage("[WiFi] OK IP=%s RSSI=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    logMessage("[WiFi] Failed (continuing offline)");
  }
}

// Update device state from JSON object
void updateDeviceFromJson(JsonObject obj, Device* dev) {
  // --- State ---
  if (obj.containsKey("state") && !obj["state"].isNull()) {
    String st = obj["state"].as<String>();
    if (st == "unknown" || st == "unavailable" || st == "off" || st == "closed") {
      dev->state = false;
    } else {
      dev->state = (st == "on" || st == "ON" || st == "true" || st == "True" || st == "open");
    }
  }
  // (else: don’t overwrite dev->state)

  if (dev->type == LIGHT) {
    // --- Brightness ---
    if (obj.containsKey("brightness") && !obj["brightness"].isNull()) {
      int b = obj["brightness"].as<int>();
      if (b < 0) b = 0;
      if (b > 255) b = 255;
      dev->brightness = b;
    }
    // (else: keep previous brightness)

    // --- RGB ---
    if (obj.containsKey("r") && obj.containsKey("g") && obj.containsKey("b") &&
        !obj["r"].isNull() && !obj["g"].isNull() && !obj["b"].isNull()) {
      dev->r = obj["r"].as<int>();
      dev->g = obj["g"].as<int>();
      dev->b = obj["b"].as<int>();

      // Recompute hue
      int maxC = max(dev->r, max(dev->g, dev->b));
      int minC = min(dev->r, min(dev->g, dev->b));
      int delta = maxC - minC;
      if (delta == 0) dev->hue = 0;
      else if (maxC == dev->r) {
        dev->hue = 60 * ((dev->g - dev->b) / (float)delta);
        if (dev->hue < 0) dev->hue += 360;
      } else if (maxC == dev->g) {
        dev->hue = 60 * ((dev->b - dev->r) / (float)delta + 2);
      } else {
        dev->hue = 60 * ((dev->r - dev->g) / (float)delta + 4);
      }
    }
    // (else: don’t overwrite RGB/hue if not provided)

    // --- Color Temperature ---
    if (obj.containsKey("color_temp") && !obj["color_temp"].isNull()) {
      int ct = obj["color_temp"].as<int>();
      if (ct < 153) ct = 153;
      if (ct > 500) ct = 500;
      dev->color_temp = ct;
    }
    // (else: keep previous color_temp)
  }
}




// Update all devices from root JSON object
void updateDevicesFromJson(JsonObject root) {
  for (JsonPair kv : root) {
    String devName = kv.key().c_str();
    JsonObject obj = kv.value().as<JsonObject>();

    for (int i = 0; i < numLights; i++) {
      if (devName.equalsIgnoreCase(lights[i].name)) {
        updateDeviceFromJson(obj, &lights[i]);
      }
    }
    for (int i = 0; i < numSwitches; i++) {
      if (devName.equalsIgnoreCase(switches[i].name)) {
        updateDeviceFromJson(obj, &switches[i]);
      }
    }
    for (int i = 0; i < numBlinds; i++) {
      if (devName.equalsIgnoreCase(blinds[i].name)) {
        updateDeviceFromJson(obj, &blinds[i]);
      }
    }
  }
}

void logMessageRaw(const String &msg) {
  addWebLog(msg);   // full payload, no truncation
  Serial.println(msg);
}


// MQTT callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  addMqttLog("RCV ← " + msg);
  logMessage("RCV length=%u", length);   // <<< log size of incoming message

  if (String(topic) == "m5dial/state") {
    StaticJsonDocument<4096> doc;
    msg.replace("None", "null");
    
    logMessageRaw("Sanitized JSON: " + msg);
   // DEBUG: see final payload
    DeserializationError err = deserializeJson(doc, msg);

    if (!err) {
      JsonObject root = doc.as<JsonObject>();
      updateDevicesFromJson(root);

      if (currentMode == CONTROL_SCREEN && selectedDevice) {
        drawControlScreen(selectedDevice);
      } else {
        drawCurrentScreen();
      }
      logMessage("JSON parsed OK, memUsage=%u/%u",
        doc.memoryUsage(), doc.capacity());  // <<< debug memory usage
    } else {
      addMqttLog(String("JSON error: ") + err.c_str());
      logMessage("JSON ERR len=%u : %s", length, err.c_str()); // <<< log with len
    }
  }
}
// ===== Cinematic Boot Animation ===== 
void cinematicBoot() {
  M5Dial.Display.fillScreen(TFT_BLACK);
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;

  // Stage 1: faint pulse fade-in
  for (int b = 0; b <= 200; b += 5) {
    M5Dial.Display.setBrightness(b);
    delay(10);
  }

  // Stage 2: glowing ring animation
  int r_outer = min(cx, cy) - 10;
  for (int i = 0; i <= 360; i += 4) {
    float rad = i * PI / 180.0;
    int x0 = cx + cos(rad) * (r_outer - 4);
    int y0 = cy + sin(rad) * (r_outer - 4);
    int x1 = cx + cos(rad) * r_outer;
    int y1 = cy + sin(rad) * r_outer;
    int c = M5Dial.Display.color565(60 + i/6, 120 + i/8, 255 - i/8);
    M5Dial.Display.drawLine(x0, y0, x1, y1, c);
    if (i % 20 == 0) delay(15);
  }

  // Stage 3: fade in text logo
  const char* logoLine1 = "F A N F O";
  const char* logoLine2 = "S Y S T E M S  O N L I N E";

  for (int i = 0; i < 255; i += 8) {
    uint16_t col = M5Dial.Display.color565(i/2, i, 255);
    M5Dial.Display.setTextDatum(middle_center);
    M5Dial.Display.setTextSize(3);
    M5Dial.Display.setTextColor(col, TFT_BLACK);
    M5Dial.Display.drawString(logoLine1, cx, cy - 10);
    M5Dial.Display.setTextSize(1);
    M5Dial.Display.setTextColor(M5Dial.Display.color565(i/4, i/2, 200), TFT_BLACK);
    M5Dial.Display.drawString(logoLine2, cx, cy + 20);
    delay(20);
  }

  // Stage 4: subtle pulse before exit
  for (int pulse = 0; pulse < 3; pulse++) {
    for (int i = 0; i < 30; i++) {
      int bright = 180 + sin(i * PI / 30.0) * 40;
      M5Dial.Display.setBrightness(bright);
      delay(10);
    }
  }

  // Stage 5: fade to menu background
  for (int b = 200; b >= 50; b -= 5) {
    M5Dial.Display.setBrightness(b);
    delay(10);
  }

  M5Dial.Display.fillScreen(M5Dial.Display.color565(230,232,235));
  M5Dial.Display.setBrightness(displayBrightness);
}




// Connect to MQTT broker
void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);
  if (!mqtt.connected()) {
    if (mqtt.connect(MQTT_CLIENTID, MQTT_USER, MQTT_PASSWD)) {
      addMqttLog("MQTT Connected");
      mqtt.subscribe("m5dial/state");
    } else {
      addMqttLog(String("MQTT conn fail rc=") + mqtt.state());
    }
  }
}



// ===== Extra: Clock Screensaver =====
enum ExtraScreenMode { CLOCK_SCREEN };
unsigned long lastInteraction = 0;
const unsigned long IDLE_TIMEOUT = 30000; // 30s
int lastDrawnSecond = -1;
bool clockNeedsClear = true;
// Weather info
void drawClockScreen() {

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    if (lastDrawnSecond != -2) {
      lastDrawnSecond = -2;
      M5Dial.Display.fillScreen(M5Dial.Display.color565(0,0,0)); // pure black
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextSize(2);
      M5Dial.Display.setTextColor(M5Dial.Display.color565(255, 80, 80), 
                                  M5Dial.Display.color565(0,0,0));
      M5Dial.Display.drawString("Time Error", CX, CY);
    }
    return;
  }

  // Clear background once when entering clock mode
  if (clockNeedsClear) {
    M5Dial.Display.fillScreen(M5Dial.Display.color565(0,0,0)); // black background
    clockNeedsClear = false;
    lastDrawnSecond = -1; // force first draw
  }

  // Only update if second changed
  if (timeinfo.tm_sec == lastDrawnSecond) return;
  lastDrawnSecond = timeinfo.tm_sec;

  // Minimalistic smooth arc border that fills with seconds
  int r = min(CX, CY) - 10;
  float startAngle = -PI/2; // top
  // Fill percent depends on current second (0-59)
  float percent = (timeinfo.tm_sec % 60) / 60.0;
  float endAngle = startAngle + percent * 2 * PI;

  // At the 1 minute mark, clear the arc so it starts again
  if (timeinfo.tm_sec == 0) {
    // Clear arc area to black at the minute mark
  M5Dial.Display.fillCircle(CX, CY, r+2, M5Dial.Display.color565(0,0,0));
    // Optionally redraw the background arc if you want a static border
    for (float a = 0; a < 2*PI; a += 0.03) {
      int x0 = CX + cos(startAngle + a) * r;
      int y0 = CY + sin(startAngle + a) * r;
      int x1 = CX + cos(startAngle + a) * (r-2);
      int y1 = CY + sin(startAngle + a) * (r-2);
      M5Dial.Display.drawLine(x0, y0, x1, y1, M5Dial.Display.color565(40,40,40));
    }
  } else {
    // Always draw background arc (full border, light gray)
    for (float a = 0; a < 2*PI; a += 0.03) {
      int x0 = CX + cos(startAngle + a) * r;
      int y0 = CY + sin(startAngle + a) * r;
      int x1 = CX + cos(startAngle + a) * (r-2);
      int y1 = CY + sin(startAngle + a) * (r-2);
      M5Dial.Display.drawLine(x0, y0, x1, y1, M5Dial.Display.color565(40,40,40));
    }
    // Draw filled arc (seconds, accent color)
    if (percent > 0.0) {
      for (float a = 0; a < (endAngle-startAngle); a += 0.002) {
        int x0 = CX + cos(startAngle + a) * r;
        int y0 = CY + sin(startAngle + a) * r;
        int x1 = CX + cos(startAngle + a) * (r-2);
        int y1 = CY + sin(startAngle + a) * (r-2);
        M5Dial.Display.drawLine(x0, y0, x1, y1, M5Dial.Display.color565(120,220,180));
      }
    }
  }

  // Large, modern time
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &timeinfo); // Only HH:MM for minimalism
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextSize(6);  // bigger, modern
  M5Dial.Display.setTextColor(M5Dial.Display.color565(220, 220, 220), M5Dial.Display.color565(0,0,0));
  M5Dial.Display.drawString(buf, CX, CY - 10);

  // Subtle date below
  char dateBuf[32];
  strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y", &timeinfo);
  M5Dial.Display.setTextSize(1.7);
  M5Dial.Display.setTextColor(M5Dial.Display.color565(120,120,120), M5Dial.Display.color565(0,0,0));
  M5Dial.Display.drawString(dateBuf, CX, CY + 40);
  // Weather below date
  if (weatherTemp.length() > 0 && weatherDesc.length() > 0) {
    String weatherStr = weatherTemp + ", " + weatherDesc;
    M5Dial.Display.setTextSize(1.5);
    M5Dial.Display.setTextColor(M5Dial.Display.color565(100,180,220), M5Dial.Display.color565(0,0,0));
    M5Dial.Display.drawString(weatherStr, CX, CY + 65);
  } else {
    M5Dial.Display.setTextSize(1.5);
    M5Dial.Display.setTextColor(M5Dial.Display.color565(180,80,80), M5Dial.Display.color565(0,0,0));
    M5Dial.Display.drawString("No weather data", CX, CY + 65);
  }
} 
// Smooth fade transition when changing screens
void fadeTransition(void (*drawFunc)()) {
  const int stepDelay = 5;   // delay between brightness changes
  const int stepSize  = 10;  // brightness step amount

  // Fade out
  for (int b = 200; b >= 30; b -= stepSize) {
    M5Dial.Display.setBrightness(b);
    delay(stepDelay);
  }

  // Draw new screen
  drawFunc();

  // Fade in
  for (int b = 30; b <= 200; b += stepSize) {
    M5Dial.Display.setBrightness(b);
    delay(stepDelay);
  }
}

// Check for idle timeout and switch to clock screen
void updateIdleCheck() {
  // If idle → show clock
  if (millis() - lastInteraction > IDLE_TIMEOUT) {
    if (currentMode != (ScreenMode)CLOCK_SCREEN) {
      currentMode = (ScreenMode)CLOCK_SCREEN;
      clockNeedsClear = true;   // <<< ensure background clears once
      drawClockScreen();
    } else {
      drawClockScreen(); // keep updating once per second
    }
  }
}
// ===== OTA Setup =====
void setupOTA() {
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.setHostname("M5Dial");
    ArduinoOTA.setPassword("Fanfo1122"); // optional password
    otaAddress = WiFi.localIP().toString() + " / " + String(ArduinoOTA.getHostname()) + ".local"; 
    ArduinoOTA.onStart([]() {
      M5Dial.Display.fillScreen(M5Dial.Display.color565(0,0,0));
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextSize(2);
      M5Dial.Display.setTextColor(M5Dial.Display.color565(255,255,255));
      M5Dial.Display.drawString("OTA Update", CX, CY - 30);
      M5Dial.Display.drawString("Starting...", CX, CY);
    });

    ArduinoOTA.onEnd([]() {
      M5Dial.Display.fillScreen(M5Dial.Display.color565(0,0,0));
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextSize(2);
      M5Dial.Display.setTextColor(M5Dial.Display.color565(0,200,0));
      M5Dial.Display.drawString("Update Done!", CX, CY);
      delay(1500);
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      int percent = progress / (total / 100);

      // Clear background
      M5Dial.Display.fillScreen(M5Dial.Display.color565(0,0,0));

      // Title
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextSize(2);
      M5Dial.Display.setTextColor(M5Dial.Display.color565(255,255,255));
      M5Dial.Display.drawString("OTA Update", CX, CY - 40);

      // Progress bar background
      int barW = SCR_W - SAFE_MARGIN*2;
      int barH = 20;
      int barX = SAFE_MARGIN;
      int barY = CY;

      M5Dial.Display.fillRect(barX, barY, barW, barH, M5Dial.Display.color565(50,50,50));

      // Filled portion
      int fillW = (barW * percent) / 100;
      M5Dial.Display.fillRect(barX, barY, fillW, barH, M5Dial.Display.color565(100,180,255));

      // Percent text
      char buf[16];
      snprintf(buf, sizeof(buf), "%d%%", percent);
      M5Dial.Display.setTextColor(M5Dial.Display.color565(255,255,255));
      M5Dial.Display.drawString(buf, CX, CY + 40);
    });

    ArduinoOTA.onError([](ota_error_t error) {
      M5Dial.Display.fillScreen(M5Dial.Display.color565(0,0,0));
      M5Dial.Display.setTextDatum(middle_center);
      M5Dial.Display.setTextSize(2);
      M5Dial.Display.setTextColor(M5Dial.Display.color565(255,0,0));
      M5Dial.Display.drawString("OTA Error!", CX, CY - 20);

      if (error == OTA_AUTH_ERROR) M5Dial.Display.drawString("Auth Failed", CX, CY + 10);
      else if (error == OTA_BEGIN_ERROR) M5Dial.Display.drawString("Begin Failed", CX, CY + 10);
      else if (error == OTA_CONNECT_ERROR) M5Dial.Display.drawString("Connect Failed", CX, CY + 10);
      else if (error == OTA_RECEIVE_ERROR) M5Dial.Display.drawString("Receive Failed", CX, CY + 10);
      else if (error == OTA_END_ERROR) M5Dial.Display.drawString("End Failed", CX, CY + 10);

      delay(3000);
    });

    ArduinoOTA.begin();
    MDNS.begin("M5Dial"); // explicitly start mDNS responder

    addMqttLog("OTA Ready");   // logs it too
  }
}
// Fetch public IP address
void fetchPublicIP() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://api.ipify.org"); // simple public IP service
  int httpCode = http.GET();
  if (httpCode == 200) {
    publicIP = http.getString();
  } else {
    publicIP = "(error)";
  }
  http.end();
}
// Reset idle timer on interaction
void resetIdleTimer() {
  lastInteraction = millis();
  if (currentMode == (ScreenMode)CLOCK_SCREEN) {
    currentMode = MENU_CATEGORIES;
    drawCurrentScreen();
  }
}
// ===== Setup =====
void setup() {
  lastInteraction = millis();
  pinMode(46, OUTPUT);
  digitalWrite(46, HIGH);

  Serial.begin(115200);
  delay(100);

  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  M5Dial.Display.setBrightness(200);
   // Rotate screen by 90 degrees
  M5Dial.Display.setRotation(1);

  SCR_W = M5Dial.Display.width();
  SCR_H = M5Dial.Display.height();
  CX = SCR_W / 2;
  CY = SCR_H / 2;

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) connectMQTT();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  currentMode = MENU_CATEGORIES;
  oldDetentPos = M5Dial.Encoder.read() / 4;
  mqttRequestState();

 if (WiFi.status() == WL_CONNECTED) {
  connectMQTT();
  setupOTA(); 
  setupWebServer(); 
}

  cinematicBoot();
  drawCurrentScreen();
}
// ===== Loop =====
 void loop() {
  // === Weather Debug Encoder Scroll ===
  static int lastWeatherLogViewIndex = -1;
  if (currentMode == SETTINGS_WEATHER_DEBUG) {
    long detentPos = M5Dial.Encoder.read() / 4;
    if (detentPos != oldDetentPos) {
      int diff = detentPos - oldDetentPos;
      oldDetentPos = detentPos;
      weatherLogViewIndex += diff;
      if (weatherLogViewIndex < 0) weatherLogViewIndex = 0;
      if (weatherLogViewIndex >= WEATHER_LOG_SIZE) weatherLogViewIndex = WEATHER_LOG_SIZE - 1;
      lastWeatherLogViewIndex = -1;
    }
  }

  // === Update weather only in clock mode ===
  if (currentMode == CLOCK_SCREEN && millis() - lastWeatherUpdate > WEATHER_UPDATE_INTERVAL) {
    fetchWeather();
    lastWeatherUpdate = millis();
  }

  M5Dial.update();
  digitalWrite(46, HIGH);

  // ==== WiFi + MQTT auto reconnect ====
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 10000) {
      lastReconnectAttempt = millis();
      logMessage("[WiFi] Lost connection, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  } else {
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();
    server.handleClient();
  }

  // === Encoder handling ===
  long detentPos = M5Dial.Encoder.read() / 4;
  if (detentPos != oldDetentPos) {
    int diff = detentPos - oldDetentPos;
    oldDetentPos = detentPos;

    // Adjust values (no fade needed)
    if (currentMode == CONTROL_SCREEN && selectedDevice && selectedDevice->type == LIGHT) {
      selectedDevice->brightness += diff * 20;
      selectedDevice->brightness = constrain(selectedDevice->brightness, 0, 255);
      mqttPublishDevice(selectedDevice, "brightness", selectedDevice->brightness);
      drawCurrentScreen();
    }
    else if (currentMode == SETTINGS_MQTT) {
      mqttLogViewIndex = constrain(mqttLogViewIndex + diff, 0, MQTT_LOG_SIZE - 1);
    }
    else if (currentMode == COLOR_SCREEN && selectedDevice) {
      // Faster color hue scroll, yes preciousss!
      int speedFactor = 12;  // tweak speed here (was 3 before)
      selectedDevice->hue += diff * speedFactor;

      // wrap hue safely 0–359
      if (selectedDevice->hue < 0) selectedDevice->hue += 360;
      if (selectedDevice->hue >= 360) selectedDevice->hue -= 360;

      int r, g, b;
      hsvToRgb(selectedDevice->hue, 255, 255, r, g, b);
      selectedDevice->r = r; selectedDevice->g = g; selectedDevice->b = b;

      mqttPublishDevice(selectedDevice, "color");
      drawCurrentScreen();
    }
    else if (currentMode == WHITE_SCREEN && selectedDevice) {
      selectedDevice->color_temp += diff * 10;
      selectedDevice->color_temp = constrain(selectedDevice->color_temp, 153, 500);
      mqttPublishDevice(selectedDevice, "color_temp", selectedDevice->color_temp);
      drawCurrentScreen();
    }
    else if (currentMode == SETTINGS_BRIGHTNESS) {
      displayBrightness += diff * 10;
      displayBrightness = constrain(displayBrightness, 10, 255);
      M5Dial.Display.setBrightness(displayBrightness);
      drawCurrentScreen();
    }
    // Menu scrolling (no fade)
    else {
      menuIndex += diff;
      int maxItems = 0;
      if (currentMode == MENU_CATEGORIES) maxItems = 4;
      else if (currentMode == MENU_LIGHTS)   maxItems = numLights;
      else if (currentMode == MENU_SWITCHES) maxItems = numSwitches;
      else if (currentMode == MENU_BLINDS)   maxItems = numBlinds;
      else if (currentMode == MENU_SETTINGS) maxItems = 4;
      else if (currentMode == SETTINGS_DISPLAY) maxItems = 3;

      if (maxItems > 0) {
        if (menuIndex < 0) menuIndex = maxItems - 1;
        if (menuIndex >= maxItems) menuIndex = 0;
      }
      drawCurrentScreen();
    }

    resetIdleTimer();
  }

  // === Button handling ===
  static unsigned long btnPressTime = 0;
  static bool btnWasPressed = false;
  static unsigned long lastClickTime = 0;
  static int clickCount = 0;

  if (M5Dial.BtnA.isPressed()) {
    if (!btnWasPressed) { btnPressTime = millis(); btnWasPressed = true; }
  } else {
    if (btnWasPressed) {
      unsigned long pressDuration = millis() - btnPressTime;
      btnWasPressed = false;
      if (pressDuration < 500) {
        unsigned long now = millis();
        if (now - lastClickTime < 400) clickCount++;
        else clickCount = 1;
        lastClickTime = now;
      }
    }
  }

  // === Handle click actions ===
  if (clickCount > 0 && (millis() - lastClickTime > 350)) {
    if (clickCount == 1) {
      // --- Single click actions ---
      if (currentMode == MENU_CATEGORIES) {
        if      (menuIndex == 0) { currentMode = MENU_LIGHTS;   menuIndex = 0; mqttRequestState(); fadeTransition(drawCurrentScreen); }
        else if (menuIndex == 1) { currentMode = MENU_SWITCHES; menuIndex = 0; mqttRequestState(); fadeTransition(drawCurrentScreen); }
        else if (menuIndex == 2) { currentMode = MENU_BLINDS;   menuIndex = 0; mqttRequestState(); fadeTransition(drawCurrentScreen); }
        else if (menuIndex == 3) { currentMode = MENU_SETTINGS; menuIndex = 0; fadeTransition(drawCurrentScreen); }
      }
      else if (currentMode == MENU_LIGHTS)   { selectedDevice = &lights[menuIndex];   currentMode = CONTROL_SCREEN; fadeTransition(drawCurrentScreen); }
      else if (currentMode == MENU_SWITCHES) { 
        selectedDevice = &switches[menuIndex];
        selectedDevice->state = !selectedDevice->state;
        mqttPublishDevice(selectedDevice, "toggle", selectedDevice->state ? 1 : 0);
        drawSwitchesMenu();
      }
      else if (currentMode == MENU_BLINDS)   { selectedDevice = &blinds[menuIndex];   currentMode = CONTROL_SCREEN; fadeTransition(drawCurrentScreen); }
      else if (currentMode == MENU_SETTINGS) {
        if (menuIndex == 0) { currentMode = SETTINGS_WIFI; fadeTransition(drawCurrentScreen); }
        else if (menuIndex == 1) { currentMode = SETTINGS_MQTT; fadeTransition(drawCurrentScreen); }
        else if (menuIndex == 2) { currentMode = SETTINGS_WEATHER_DEBUG; fadeTransition(drawCurrentScreen); }
        else if (menuIndex == 3) { currentMode = SETTINGS_DISPLAY; fadeTransition(drawCurrentScreen); }
      }
      else if (currentMode == SETTINGS_DISPLAY) {
        if (menuIndex == 0) { enableClockOnIdle = !enableClockOnIdle; drawCurrentScreen(); }
        else if (menuIndex == 1) { currentMode = SETTINGS_BRIGHTNESS; fadeTransition(drawCurrentScreen); }
      }
      else if (currentMode == SETTINGS_BRIGHTNESS) {
        currentMode = SETTINGS_DISPLAY;
        fadeTransition(drawCurrentScreen);
      }
      else if (currentMode == CONTROL_SCREEN && selectedDevice) {
        selectedDevice->state = !selectedDevice->state;
        mqttPublishDevice(selectedDevice, "toggle", selectedDevice->state ? 1 : 0);
        drawCurrentScreen();
      }

      resetIdleTimer();
    }
    else if (clickCount == 2) {
      // --- Double click actions ---
      if (currentMode == CONTROL_SCREEN && selectedDevice && selectedDevice->type == LIGHT) {
        currentMode = COLOR_SCREEN;
      } else if (currentMode == COLOR_SCREEN && selectedDevice) {
        currentMode = WHITE_SCREEN;
      } else if (currentMode == WHITE_SCREEN && selectedDevice) {
        currentMode = CONTROL_SCREEN;
      }
      fadeTransition(drawCurrentScreen);
      resetIdleTimer();
    }
    clickCount = 0;
  }

  // === Long press → back to menu ===
  if (btnWasPressed && (millis() - btnPressTime >= 1000)) {
    if (currentMode != MENU_CATEGORIES) {
      currentMode = MENU_CATEGORIES;
      menuIndex = 0;
      fadeTransition(drawCurrentScreen);
      delay(500);
      btnWasPressed = false;
      resetIdleTimer();
    }
  }

  // === Continuous screens (throttled for smoother responsiveness) ===
  static unsigned long lastContinuousRedraw = 0;
  if (millis() - lastContinuousRedraw >= 33) {
    if (currentMode == SETTINGS_MQTT) drawSettingsMQTT();
    if (currentMode == SETTINGS_WEATHER_DEBUG) drawSettingsWeatherDebug();
    lastContinuousRedraw = millis();
  }

  // === Idle Clock ===
  if (enableClockOnIdle) updateIdleCheck();

  ArduinoOTA.handle();

  static unsigned long lastServerLog = 0;
  if (millis() - lastServerLog > 5000) lastServerLog = millis();

  if (millis() - lastIPCheck > IP_CHECK_INTERVAL) {
    fetchPublicIP();
    lastIPCheck = millis();
  }

  static bool serverStarted = false;
  if (WiFi.status() == WL_CONNECTED && !serverStarted) {
    setupWebServer();
    serverStarted = true;
  }

  delay(4);
}
