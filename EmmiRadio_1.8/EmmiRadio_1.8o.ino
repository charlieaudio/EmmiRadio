#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Audio.h>
#include <Update.h>
#include <Wire.h>
#include <FFat.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
// EmmiRadio 1.8o - OLED single-file scaffold
// Based on EmmiRadio 1.8 logic, adapted for SSD1306 OLED.
// Target: ESP32-S3 + PCM5102 + SSD1306 128x64 I2C OLED
// ============================================================

const char* FW_VERSION = "1.8o";

// ---------------- PINOUT (same as your current build) ----------------
static const int PIN_I2S_BCLK   = 3;   // D2 / GPIO3
static const int PIN_I2S_LRCK   = 4;   // D3 / GPIO4
static const int PIN_I2S_DOUT   = 2;   // D1 / GPIO2
static const int PIN_I2C_SDA    = 5;   // GPIO5
static const int PIN_I2C_SCL    = 6;   // GPIO6
static const int PIN_BTN_PLAY   = 1;   // D0 / GPIO1
static const int PIN_BTN_NEXT   = 43;  // D6 / GPIO43
static const int PIN_BTN_PREV   = 44;  // D7 / GPIO44

// ---------------- OLED ----------------
#define OLED_WIDTH   128
#define OLED_HEIGHT   64
#define OLED_ADDR   0x3C
#define OLED_RESET   -1
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

static const uint8_t OLED_ROWS = 4;
static const uint8_t OLED_COLS = 21;  // rough text width with textSize=1
static const uint8_t OLED_LINE_Y[OLED_ROWS] = {0, 16, 32, 48};

// ---------------- Storage ----------------
const char* WIFI_CFG_PATH = "/WiFi.json";
const char* SETTINGS_PATH = "/settings.json";
const char* STATIONS_PATH = "/radios.json";

// ---------------- Audio ----------------
Audio audio;
WebServer server(80);

// ---------------- Limits ----------------
static const int MAX_WIFI = 8;
static const int MAX_STATIONS = 64;
static const int VOL_MIN = 0;
static const int VOL_MAX = 21;
static const int VOL_DEFAULT = 10;

static const unsigned long META_TIMEOUT_MS = 5000;
static const unsigned long DISPLAY_IDLE_SLEEP_MS = 30000;
static const unsigned long VOLUME_OVERLAY_MS = 1500;
static const unsigned long BOOT_IP_SHOW_MS = 5000;
static const unsigned long MARQUEE_INTERVAL_MS = 300;
static const unsigned long LONGPRESS_MS = 450;
static const unsigned long REPEAT_MS = 180;
static const unsigned long DEBOUNCE_MS = 25;

// ---------------- Simple records ----------------
struct WiFiCred {
  String ssid;
  String pass;
};

struct Station {
  String name;
  String url;
};

WiFiCred wifiList[MAX_WIFI];
int wifiCount = 0;

Station stations[MAX_STATIONS];
int stationCount = 0;

// ---------------- Runtime state ----------------
bool isPlaying = false;
bool gotMeta = false;
bool marqueeActive = false;
bool powerSaveEnabled = true;
bool savedPowerSaveEnabled = true;
bool displaySleeping = false;
bool apMode = false;

String lastTitle = "";
String bootIpText = "";

int currentVolume = VOL_DEFAULT;
int currentStation = 0;
int savedVolume = VOL_DEFAULT;
int savedStation = 0;

unsigned long playStartMs = 0;
unsigned long lastActivityMs = 0;
unsigned long volumeOverlayUntilMs = 0;
unsigned long bootIpUntilMs = 0;
unsigned long lastMarqueeMs = 0;
int marqueePos = 0;
String marqueeText = "";

// ---------------- Button state machine ----------------
enum ButtonEventType {
  BTN_EVENT_NONE,
  BTN_EVENT_RELEASED_SHORT,
  BTN_EVENT_LONG_START,
  BTN_EVENT_LONG_REPEAT
};

struct ButtonState {
  int pin;
  bool activeLow;
  bool stablePressed;
  bool rawPressed;
  bool longStarted;
  unsigned long lastChangeMs;
  unsigned long pressedMs;
  unsigned long lastRepeatMs;
};

ButtonState btnPlay { PIN_BTN_PLAY, true, false, false, false, 0, 0, 0 };
ButtonState btnNext { PIN_BTN_NEXT, true, false, false, false, 0, 0, 0 };
ButtonState btnPrev { PIN_BTN_PREV, true, false, false, false, 0, 0, 0 };

// ============================================================
// Forward declarations
// ============================================================
bool loadWiFiList();
bool saveWiFiList();
bool loadStations();
bool saveStations();
bool loadSettings();
bool saveSettings();
bool connectWiFiSmart(unsigned long timeoutPerNetMs = 7000, int maxAttempts = 5);
void startAP();
void startWebServer();
void handleSettingsPost();
void handleVolumeSet();
void handleOtaUpload();
void handleOtaPage();
void doPlay();
void doStop();
void changeStation(int delta);
void selectStation(int idx);
void applyVolume();
void volumeUpStep();
void volumeDownStep();
void showNowPlaying();
void showIpOnDisplay();
void showVolumeOverlay();
void startMarquee(const String& full);
void tickMarquee();
void tryIcyFallback();
String fetchIcyTitleOnce(const String& url, uint32_t overallTimeoutMs = 5000);
void logRuntimeSummary();

// display
void displayInit();
void displayWake();
void displaySleep();
void displayClear(bool flush = true);
void displayBeginFrame();
void displayEndFrame();
void displayPrintLine(uint8_t row, const String& text);
void displayPrintLineRaw(uint8_t row, const String& text);
void displayShow4(const String& l1, const String& l2, const String& l3, const String& l4);
String fitLine(const String& s, size_t maxLen);

// html
String htmlHeader(const String& title);
String htmlFooter();
String pageIndex();
String pageStations(const String& note = "");
String pageWifiForm(const String& note = "");
String pageOtaUpdate(const String& note = "");
String buildStationOptions();
void redirectHome();

// buttons
ButtonEventType pollButton(ButtonState& b);
void processButtons();

// helpers
String jsonEscape(const String& s);
String jsonGet(const String& line, const String& key);
String jsonLineWifi(const String& ssid, const String& pass);
String jsonLineStation(const String& name, const String& url);
void updateBootDisplayTimer();

// ============================================================
// Display layer - OLED only in 1.8o
// ============================================================

void displayInit() {
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[DISPLAY] OLED init failed");
    return;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.cp437(true);
  oled.display();
}

void displayWake() {
  oled.ssd1306_command(SSD1306_DISPLAYON);
  displaySleeping = false;
}

void displaySleep() {
  oled.ssd1306_command(SSD1306_DISPLAYOFF);
  displaySleeping = true;
}

void displayClear(bool flush) {
  oled.clearDisplay();
  if (flush) oled.display();
}

void displayBeginFrame() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
}

void displayEndFrame() {
  oled.display();
}

String fitLine(const String& s, size_t maxLen) {
  String out = s;
  out.replace("\n", " ");
  out.replace("\r", " ");
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  if (out.length() > maxLen) out = out.substring(0, maxLen);
  return out;
}

void displayPrintLineRaw(uint8_t row, const String& text) {
  if (row >= OLED_ROWS) return;
  oled.setCursor(0, OLED_LINE_Y[row]);
  oled.print(text);
}

void displayPrintLine(uint8_t row, const String& text) {
  displayPrintLineRaw(row, fitLine(text, OLED_COLS));
}

void displayShow4(const String& l1, const String& l2, const String& l3, const String& l4) {
  displayBeginFrame();
  displayPrintLine(0, l1);
  displayPrintLine(1, l2);
  displayPrintLine(2, l3);
  displayPrintLine(3, l4);
  displayEndFrame();
}

// ============================================================
// JSON helpers (simple JSONL style, intentionally dumb but robust enough)
// ============================================================

String jsonEscape(const String& s) {
  String o = s;
  o.replace("\\", "\\\\");
  o.replace("\"", "\\\"");
  return o;
}

String jsonGet(const String& line, const String& key) {
  String token = "\"" + key + "\":";
  int p = line.indexOf(token);
  if (p < 0) return "";
  p += token.length();
  while (p < (int)line.length() && line[p] == ' ') p++;
  if (p >= (int)line.length()) return "";

  if (line[p] == '"') {
    p++;
    String out;
    bool esc = false;
    while (p < (int)line.length()) {
      char c = line[p++];
      if (esc) {
        out += c;
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        break;
      } else {
        out += c;
      }
    }
    return out;
  }

  int e = line.indexOf(',', p);
  if (e < 0) e = line.indexOf('}', p);
  if (e < 0) e = line.length();
  return line.substring(p, e);
}

String jsonLineWifi(const String& ssid, const String& pass) {
  return "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"pass\":\"" + jsonEscape(pass) + "\"}";
}

String jsonLineStation(const String& name, const String& url) {
  return "{\"name\":\"" + jsonEscape(name) + "\",\"url\":\"" + jsonEscape(url) + "\"}";
}

// ============================================================
// Storage
// ============================================================

bool loadWiFiList() {
  wifiCount = 0;
  if (!FFat.exists(WIFI_CFG_PATH)) return false;
  File f = FFat.open(WIFI_CFG_PATH, FILE_READ);
  if (!f) return false;

  while (f.available() && wifiCount < MAX_WIFI) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (!l.length()) continue;
    wifiList[wifiCount].ssid = jsonGet(l, "ssid");
    wifiList[wifiCount].pass = jsonGet(l, "pass");
    if (wifiList[wifiCount].ssid.length()) wifiCount++;
  }
  f.close();
  return wifiCount > 0;
}

bool saveWiFiList() {
  File f = FFat.open(WIFI_CFG_PATH, FILE_WRITE, true);
  if (!f) return false;
  f.seek(0);
  f.truncate(0);
  for (int i = 0; i < wifiCount; i++) f.println(jsonLineWifi(wifiList[i].ssid, wifiList[i].pass));
  f.close();
  return true;
}

bool loadStations() {
  stationCount = 0;
  if (!FFat.exists(STATIONS_PATH)) return false;
  File f = FFat.open(STATIONS_PATH, FILE_READ);
  if (!f) return false;

  while (f.available() && stationCount < MAX_STATIONS) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (!l.length()) continue;
    stations[stationCount].name = jsonGet(l, "name");
    stations[stationCount].url  = jsonGet(l, "url");
    if (stations[stationCount].name.length() && stations[stationCount].url.length()) stationCount++;
  }
  f.close();
  return stationCount > 0;
}

bool saveStations() {
  File f = FFat.open(STATIONS_PATH, FILE_WRITE, true);
  if (!f) return false;
  f.seek(0);
  f.truncate(0);
  for (int i = 0; i < stationCount; i++) f.println(jsonLineStation(stations[i].name, stations[i].url));
  f.close();
  return true;
}

bool loadSettings() {
  savedPowerSaveEnabled = true;
  savedVolume = VOL_DEFAULT;
  savedStation = 0;

  if (FFat.exists(SETTINGS_PATH)) {
    File f = FFat.open(SETTINGS_PATH, FILE_READ);
    if (f) {
      while (f.available()) {
        String l = f.readStringUntil('\n');
        l.trim();
        if (!l.length()) continue;

        String ps = jsonGet(l, "ps");
        if (ps.length()) savedPowerSaveEnabled = (ps.toInt() != 0);

        String vol = jsonGet(l, "vol");
        if (vol.length()) {
          int v = vol.toInt();
          if (v < VOL_MIN) v = VOL_MIN;
          if (v > VOL_MAX) v = VOL_MAX;
          savedVolume = v;
        }

        String st = jsonGet(l, "st");
        if (st.length()) {
          int s = st.toInt();
          if (s >= 0 && s < MAX_STATIONS) savedStation = s;
        }
      }
      f.close();
    }
  }

  powerSaveEnabled = savedPowerSaveEnabled;
  currentVolume = savedVolume;
  currentStation = savedStation;
  return true;
}

bool saveSettings() {
  savedPowerSaveEnabled = powerSaveEnabled;
  savedVolume = currentVolume;
  savedStation = currentStation;

  File f = FFat.open(SETTINGS_PATH, FILE_WRITE, true);
  if (!f) return false;
  f.seek(0);
  f.truncate(0);
  f.println("{\"ps\":" + String(savedPowerSaveEnabled ? 1 : 0) + ",\"vol\":" + String(savedVolume) + ",\"st\":" + String(savedStation) + "}");
  f.close();
  return true;
}

// ============================================================
// Playback / meta
// ============================================================

void applyVolume() {
  if (currentVolume < VOL_MIN) currentVolume = VOL_MIN;
  if (currentVolume > VOL_MAX) currentVolume = VOL_MAX;
  audio.setVolume(currentVolume);
}

void volumeUpStep() {
  if (currentVolume < VOL_MAX) {
    currentVolume++;
    applyVolume();
    showVolumeOverlay();
    Serial.println(String("[AUDIO] Volume -> ") + currentVolume);
  }
}

void volumeDownStep() {
  if (currentVolume > VOL_MIN) {
    currentVolume--;
    applyVolume();
    showVolumeOverlay();
    Serial.println(String("[AUDIO] Volume -> ") + currentVolume);
  }
}

void selectStation(int idx) {
  if (idx < 0 || idx >= stationCount) return;
  currentStation = idx;
  Serial.println(String("[ST] selected: ") + stations[currentStation].name);
}

void doPlay() {
  if (stationCount <= 0) return;
  if (currentStation < 0 || currentStation >= stationCount) currentStation = 0;

  Serial.println(String("[AUDIO] play -> ") + stations[currentStation].url);
  audio.stopSong();
  delay(30);
  audio.connecttohost(stations[currentStation].url.c_str());
  isPlaying = true;
  gotMeta = false;
  lastTitle = "";
  marqueeActive = false;
  playStartMs = millis();
  showNowPlaying();
}

void doStop() {
  if (!isPlaying) return;
  Serial.println("[AUDIO] stop");
  audio.stopSong();
  isPlaying = false;
  gotMeta = false;
  marqueeActive = false;
  lastTitle = "";
  showNowPlaying();
}

void changeStation(int delta) {
  if (stationCount <= 0) return;
  int next = currentStation + delta;
  if (next < 0) next = stationCount - 1;
  if (next >= stationCount) next = 0;
  selectStation(next);
  if (isPlaying) doPlay();
  else showNowPlaying();
}

void startMarquee(const String& full) {
  String s = full;
  s.trim();
  if (!s.length()) {
    marqueeActive = false;
    return;
  }
  marqueeText = s + "   ***   ";
  marqueePos = 0;
  marqueeActive = (s.length() > OLED_COLS);
  if (!marqueeActive) showNowPlaying();
}

void tickMarquee() {
  if (!marqueeActive) return;
  if (millis() - lastMarqueeMs < MARQUEE_INTERVAL_MS) return;
  lastMarqueeMs = millis();

  String src = marqueeText;
  if (src.length() < OLED_COLS) src += String(' ', OLED_COLS - src.length());

  String window;
  for (int i = 0; i < OLED_COLS; i++) {
    window += src[(marqueePos + i) % src.length()];
  }
  marqueePos = (marqueePos + 1) % src.length();

  String line1 = (stationCount > 0) ? stations[currentStation].name : "(no stations)";
  String line3 = isPlaying ? "Playing" : "Stopped";
  String line4 = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  displayShow4(line1, window, line3, line4);
}

void showVolumeOverlay() {
  volumeOverlayUntilMs = millis() + VOLUME_OVERLAY_MS;
  showNowPlaying();
}

void showIpOnDisplay() {
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  bootIpText = ip;
  bootIpUntilMs = millis() + BOOT_IP_SHOW_MS;
  displayShow4("EmmiRadio " + String(FW_VERSION), displaySleeping ? "Waking..." : displaySleeping, apMode ? "AP mode" : "STA mode", ip);
}

void showNowPlaying() {
  if (bootIpUntilMs > millis()) {
    displayShow4("EmmiRadio " + String(FW_VERSION), apMode ? "AP mode" : "Connected", displayName(), bootIpText);
    return;
  }

  String line1 = (stationCount > 0) ? stations[currentStation].name : "(no stations)";
  String line2;
  String line3;
  String line4;

  if (volumeOverlayUntilMs > millis()) {
    line2 = "Vol: " + String(currentVolume);
  } else if (gotMeta && lastTitle.length()) {
    if (lastTitle.length() > OLED_COLS) {
      startMarquee(lastTitle);
      line2 = fitLine(lastTitle, OLED_COLS);
    } else {
      marqueeActive = false;
      line2 = lastTitle;
    }
  } else {
    marqueeActive = false;
    line2 = isPlaying ? "Waiting for title..." : "Stopped";
  }

  line3 = isPlaying ? "Playing" : "Ready";
  if (powerSaveEnabled) line3 += " PS";
  line4 = apMode ? ("AP " + WiFi.softAPIP().toString()) : ("IP " + WiFi.localIP().toString());

  displayShow4(line1, line2, line3, line4);
}

// ============================================================
// Wi-Fi / AP
// ============================================================

bool connectWiFiSmart(unsigned long timeoutPerNetMs, int maxAttempts) {
  if (wifiCount <= 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    for (int i = 0; i < wifiCount; i++) {
      Serial.println(String("[WIFI] Trying: ") + wifiList[i].ssid);
      WiFi.begin(wifiList[i].ssid.c_str(), wifiList[i].pass.c_str());
      unsigned long t0 = millis();
      while (millis() - t0 < timeoutPerNetMs) {
        if (WiFi.status() == WL_CONNECTED) {
          apMode = false;
          Serial.println(String("[WIFI] Connected, IP=") + WiFi.localIP().toString());
          return true;
        }
        delay(100);
      }
      WiFi.disconnect(true, true);
      delay(200);
    }
  }
  return false;
}

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("EmmiRadio-Setup");
  apMode = true;
  Serial.println(String("[WIFI] AP IP=") + WiFi.softAPIP().toString());
}

// ============================================================
// Web UI / routes
// ============================================================

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSettingsPost() {
  powerSaveEnabled = server.hasArg("ps");

  if (server.hasArg("vol")) {
    int v = server.arg("vol").toInt();
    if (v < VOL_MIN) v = VOL_MIN;
    if (v > VOL_MAX) v = VOL_MAX;
    currentVolume = v;
    applyVolume();
    showVolumeOverlay();
  }

  if (server.hasArg("station")) {
    int s = server.arg("station").toInt();
    if (s >= 0 && s < stationCount) currentStation = s;
  }

  saveSettings();
  if (!powerSaveEnabled && displaySleeping) displayWake();
  lastActivityMs = millis();
  redirectHome();
}

void handleVolumeSet() {
  if (server.hasArg("v")) {
    int v = server.arg("v").toInt();
    if (v < VOL_MIN) v = VOL_MIN;
    if (v > VOL_MAX) v = VOL_MAX;
    currentVolume = v;
    applyVolume();
    showVolumeOverlay();
    Serial.println(String("[AUDIO] Volume -> ") + currentVolume);
  }
  server.send(200, "text/plain", "OK");
}

String htmlHeader(const String& title) {
  String h;
  h += "<!doctype html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + title + "</title>";
  h += "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:14px;}";
  h += ".card{background:#1d1d1d;padding:14px;border-radius:12px;margin:0 0 12px 0;}button,input,select{width:100%;padding:10px;margin:6px 0;border-radius:10px;border:1px solid #444;background:#222;color:#eee;}a{color:#9cf;}table{width:100%;}th,td{padding:6px;text-align:left;} .row{display:flex;gap:8px;} .row>*{flex:1;}</style>";
  h += "</head><body>";
  return h;
}

String htmlFooter() {
  return "</body></html>";
}

String buildStationOptions() {
  String h;
  for (int i = 0; i < stationCount; i++) {
    h += "<option value='" + String(i) + "'";
    if (i == currentStation) h += " selected";
    h += ">" + String(i + 1) + ". " + stations[i].name + "</option>";
  }
  return h;
}

String pageIndex() {
  String h = htmlHeader("EmmiRadio");
  h += "<div class='card'><h2>EmmiRadio " + String(FW_VERSION) + "</h2>";
  h += "<p><b>Station:</b> " + ((stationCount > 0) ? stations[currentStation].name : String("(none)")) + "<br>";
  h += "<b>Title:</b> " + (lastTitle.length() ? lastTitle : String("-")) + "<br>";
  h += "<b>Status:</b> " + String(isPlaying ? "Playing" : "Stopped") + "<br>";
  h += "<b>IP:</b> " + String(apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "</p></div>";

  h += "<div class='card'><div class='row'>";
  h += "<button onclick=\"fetch('/prev').then(()=>location.reload())\">Prev</button>";
  h += isPlaying
    ? "<button onclick=\"fetch('/stop').then(()=>location.reload())\">Stop</button>"
    : "<button onclick=\"fetch('/play').then(()=>location.reload())\">Play</button>";
  h += "<button onclick=\"fetch('/next').then(()=>location.reload())\">Next</button>";
  h += "</div></div>";

  h += "<div class='card'><form method='post' action='/settings'>";
  h += "<label>Volume</label><input type='range' min='0' max='21' name='vol' value='" + String(currentVolume) + "' id='vol'>";
  h += "<label>Startup station</label><select name='station'>" + buildStationOptions() + "</select>";
  h += "<label><input type='checkbox' name='ps'" + String(powerSaveEnabled ? " checked" : "") + "> Power save</label>";
  h += "<button type='submit'>Save settings</button></form></div>";

  h += "<script>let vt;const vol=document.getElementById('vol');if(vol){vol.oninput=()=>{clearTimeout(vt);const v=vol.value;vt=setTimeout(()=>{fetch('/api/vol_set?v='+v).catch(()=>{});},120);};}</script>";
  h += htmlFooter();
  return h;
}

String pageStations(const String& note) {
  String h = htmlHeader("Stations");
  h += "<div class='card'><h2>Stations</h2>";
  if (note.length()) h += "<p>" + note + "</p>";
  h += "<table><tr><th>#</th><th>Name</th></tr>";
  for (int i = 0; i < stationCount; i++) {
    h += "<tr><td>" + String(i + 1) + "</td><td>" + stations[i].name + "</td></tr>";
  }
  h += "</table></div>" + htmlFooter();
  return h;
}

String pageWifiForm(const String& note) {
  String h = htmlHeader("WiFi");
  h += "<div class='card'><h2>WiFi setup</h2>";
  if (note.length()) h += "<p>" + note + "</p>";
  h += "<p>TODO: reuse your 1.8 WiFi editor HTML here.</p></div>";
  h += htmlFooter();
  return h;
}

String pageOtaUpdate(const String& note) {
  String h = htmlHeader("OTA Update");
  h += "<div class='card'><h2>OTA</h2>";
  if (note.length()) h += "<p>" + note + "</p>";
  h += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  h += "<input type='file' name='update'><button type='submit'>Upload</button></form></div>";
  h += htmlFooter();
  return h;
}

void handleOtaPage() {
  server.send(200, "text/html", pageOtaUpdate());
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Update start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[OTA] Update success: %u bytes\n", upload.totalSize);
    else Update.printError(Serial);
  }
}

void startWebServer() {
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", pageIndex()); });
  server.on("/stations", HTTP_GET, [](){ server.send(200, "text/html", pageStations()); });
  server.on("/wifi", HTTP_GET, [](){ server.send(200, "text/html", pageWifiForm()); });
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/vol_set", HTTP_GET, handleVolumeSet);

  server.on("/play", HTTP_GET, [](){ doPlay(); redirectHome(); });
  server.on("/stop", HTTP_GET, [](){ doStop(); redirectHome(); });
  server.on("/next", HTTP_GET, [](){ changeStation(+1); redirectHome(); });
  server.on("/prev", HTTP_GET, [](){ changeStation(-1); redirectHome(); });

  server.on("/ota", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, [](){
    bool ok = !Update.hasError();
    server.send(200, "text/html", pageOtaUpdate(ok ? "Update OK, rebooting..." : "Update failed"));
    delay(500);
    if (ok) ESP.restart();
  }, handleOtaUpload);

  server.begin();
  Serial.println("[WEB] started");
}

// ============================================================
// Buttons
// ============================================================

ButtonEventType pollButton(ButtonState& b) {
  bool pressed = b.activeLow ? (digitalRead(b.pin) == LOW) : (digitalRead(b.pin) == HIGH);
  unsigned long now = millis();

  if (pressed != b.rawPressed) {
    b.rawPressed = pressed;
    b.lastChangeMs = now;
  }

  if (now - b.lastChangeMs < DEBOUNCE_MS) return BTN_EVENT_NONE;

  if (b.stablePressed != b.rawPressed) {
    b.stablePressed = b.rawPressed;
    if (b.stablePressed) {
      b.pressedMs = now;
      b.lastRepeatMs = now;
      b.longStarted = false;
      return BTN_EVENT_NONE;
    } else {
      if (!b.longStarted) return BTN_EVENT_RELEASED_SHORT;
      return BTN_EVENT_NONE;
    }
  }

  if (b.stablePressed) {
    if (!b.longStarted && now - b.pressedMs >= LONGPRESS_MS) {
      b.longStarted = true;
      b.lastRepeatMs = now;
      return BTN_EVENT_LONG_START;
    }
    if (b.longStarted && now - b.lastRepeatMs >= REPEAT_MS) {
      b.lastRepeatMs = now;
      return BTN_EVENT_LONG_REPEAT;
    }
  }

  return BTN_EVENT_NONE;
}

void processButtons() {
  ButtonEventType e;

  e = pollButton(btnPlay);
  if (e == BTN_EVENT_RELEASED_SHORT) {
    if (isPlaying) doStop(); else doPlay();
    lastActivityMs = millis();
  } else if (e == BTN_EVENT_LONG_START || e == BTN_EVENT_LONG_REPEAT) {
    volumeUpStep();
    lastActivityMs = millis();
  }

  e = pollButton(btnNext);
  if (e == BTN_EVENT_RELEASED_SHORT) {
    changeStation(+1);
    lastActivityMs = millis();
  } else if (e == BTN_EVENT_LONG_START || e == BTN_EVENT_LONG_REPEAT) {
    volumeDownStep();
    lastActivityMs = millis();
  }

  e = pollButton(btnPrev);
  if (e == BTN_EVENT_RELEASED_SHORT) {
    changeStation(-1);
    lastActivityMs = millis();
  }
}

// ============================================================
// Audio callbacks / fallback
// ============================================================

void audio_info(const char *info) {
  Serial.printf("[AUDIO] %s\n", info);
}

void audio_showstreamtitle(const char *info) {
  String t = String(info);
  t.trim();
  if (!t.length()) return;
  lastTitle = t;
  gotMeta = true;
  showNowPlaying();
}

void tryIcyFallback() {
  if (!isPlaying) return;
  if (gotMeta) return;
  if (millis() - playStartMs < META_TIMEOUT_MS) return;
  if (stationCount <= 0) return;

  String t = fetchIcyTitleOnce(stations[currentStation].url, 5000);
  if (t.length()) {
    lastTitle = t;
    gotMeta = true;
    Serial.println(String("[ICY] title: ") + t);
    showNowPlaying();
  }
}

String fetchIcyTitleOnce(const String& url, uint32_t overallTimeoutMs) {
  // TODO: paste your working 1.8 ICY fallback here unchanged.
  // This stub keeps the scaffold compile-oriented, but metadata fallback will be incomplete until added.
  (void)url;
  (void)overallTimeoutMs;
  return "";
}

// ============================================================
// Misc
// ============================================================

void updateBootDisplayTimer() {
  if (bootIpUntilMs > 0 && millis() > bootIpUntilMs) {
    bootIpUntilMs = 0;
    showNowPlaying();
  }
}

void logRuntimeSummary() {
  Serial.println("------------------------------");
  Serial.println(String("FW: ") + FW_VERSION);
  Serial.println(String("Stations: ") + stationCount);
  Serial.println(String("WiFi profiles: ") + wifiCount);
  Serial.println(String("Volume: ") + currentVolume);
  Serial.println(String("Power save: ") + (powerSaveEnabled ? "on" : "off"));
  Serial.println(String("Mode: ") + (apMode ? "AP" : "STA"));
  Serial.println("------------------------------");
}

const char* displayName() {
  return "OLED SSD1306 128x64";
}

// ============================================================
// Setup / Loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(150);

  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  displayInit();
  displayShow4("EmmiRadio " + String(FW_VERSION), "Booting...", "OLED mode", "Please wait");

  if (!FFat.begin(true)) {
    Serial.println("[FS] FFat mount failed");
    displayShow4("FFat error", "Format failed?", "Check flash cfg", "No filesystem");
  }

  loadWiFiList();
  loadStations();
  loadSettings();

  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT);
  applyVolume();

  if (!connectWiFiSmart()) startAP();
  startWebServer();
  showIpOnDisplay();

  lastActivityMs = millis();
  logRuntimeSummary();
}

void loop() {
  audio.loop();
  server.handleClient();
  processButtons();
  tickMarquee();
  tryIcyFallback();
  updateBootDisplayTimer();

  if (powerSaveEnabled && !displaySleeping && (millis() - lastActivityMs > DISPLAY_IDLE_SLEEP_MS)) {
    displaySleep();
  }

  if (!powerSaveEnabled && displaySleeping) {
    displayWake();
    showNowPlaying();
  }
}
