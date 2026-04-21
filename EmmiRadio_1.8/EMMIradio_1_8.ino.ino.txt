/* -------------------------------------------------------------------------
   EmmiRadio v1.8 – explicit settings commit + working PREV
   -------------------------------------------------------------------------
   Based on: 1.75 stable base
   Main changes:
   - WebUI PREV now works
   - volume changes are runtime-only until Save Settings
   - station changes are runtime-only until Save Settings
   - Save Settings is the only persistent settings commit point
   - keeps saved volume / saved station / boot IP display / no auto-play
   ------------------------------------------------------------------------- */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Audio.h>
#include <Update.h>
#include <Wire.h>
#include <FFat.h>

const char* FW_VERSION = "1.8";

// ---------- DISPLAY SELECTION ----------
#define DISPLAY_TYPE_LCD  1
#define DISPLAY_TYPE_OLED 2
#define DISPLAY_TYPE DISPLAY_TYPE_LCD

#define LCD_ADDR 0x27
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_RESET -1

#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
#include <LiquidCrystal_I2C.h>
#elif DISPLAY_TYPE == DISPLAY_TYPE_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#else
#error Unsupported DISPLAY_TYPE
#endif

// ---------- Forward declarations ----------
void handleSettingsPost();
void handleVolumeSet();
void safeLcdPrint(const String& l1, const String& l2);
void lcdPrintLine(uint8_t row, const String& s16);
void startMarquee(const String& full);
void tickMarquee();
void showNowPlaying();
void showIpOnLcd();
void showVolumeOverlay();
void logRuntimeSummary();
String htmlHeader(const String& title);
String htmlFooter();
String pageIndex();
String pageStations(const String& note="");
String pageWifiForm(const String& note="");
String pageOtaUpdate(const String& note="");
String buildStationOptions();
void redirectHome();
bool connectWiFiSmart(unsigned long timeoutPerNetMs=7000, int maxAttempts=5);
void startAP();
bool loadWiFiList();
bool saveWiFiList();
bool loadStations();
bool saveStations();
bool loadSettings();
bool saveSettings();
void startWebServer();
void tryIcyFallback();
String fetchIcyTitleOnce(const String& url, uint32_t overallTimeoutMs=5000);
void applyVolume();
void doPlay();
void doStop();
void changeStation(int delta);
void selectStation(int idx);
void updateBootDisplayTimer();
void handleOtaUpload();
void handleOtaPage();

void displayInit();
void displayWake();
void displaySleep();
void displayClear();
void displayPrintRawLine(uint8_t row, const String& text);
const char* displayName();

#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
#elif DISPLAY_TYPE == DISPLAY_TYPE_OLED
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
#endif

bool lcdBacklightOff = false;
unsigned long lastActivityMs = 0;
const unsigned long LCD_SLEEP_MS = 30000;
bool powerSaveEnabled = true;
bool savedPowerSaveEnabled = true;

// ---------- BUTTONS ----------
const int PIN_BTN_NEXT = 43;
const int PIN_BTN_PREV = 44;   // currently unused / not connected
const int PIN_BTN_PLAY = 1;

// ---------- I2S ----------
const int PIN_I2S_BCK  = 3;
const int PIN_I2S_LRCK = 4;
const int PIN_I2S_DATA = 2;

// ---------- I2C ----------
const int PIN_I2C_SDA = 5;
const int PIN_I2C_SCL = 6;

Audio audio;

// ---------- Stations ----------
struct Station { String name; String url; };
const int MAX_STATIONS = 32;
Station stations[MAX_STATIONS];
int stationCount = 0;
int currentStation = 0;
int savedStation = 0;
const char* RADIOS_PATH = "/radios.json";

// ---------- WiFi list ----------
struct WifiEntry { String ssid; String pass; };
const int MAX_WIFI = 8;
WifiEntry wifiList[MAX_WIFI];
int wifiCount = 0;
const char* WIFI_CFG_PATH = "/WiFi.json";
const char* SETTINGS_PATH = "/settings.json";

WebServer server(80);

// ---------- Playback/meta ----------
bool isPlaying = false;
String lastTitle = "";
bool gotMeta = false;
unsigned long playStartMs = 0;
const unsigned long META_TIMEOUT_MS = 5000;

// ---------- Button state machine ----------
enum ButtonEventType {
  BTN_EVENT_NONE,
  BTN_EVENT_PRESSED,
  BTN_EVENT_RELEASED_SHORT,
  BTN_EVENT_RELEASED_LONG,
  BTN_EVENT_LONG_START,
  BTN_EVENT_LONG_REPEAT
};

struct ButtonState {
  int pin;
  bool lastRaw;
  bool stable;
  unsigned long lastChangeMs;
  unsigned long pressStartMs;
  unsigned long lastRepeatMs;
  bool longActive;
};

const unsigned long DEBOUNCE_MS = 30;
const unsigned long LONG_PRESS_MS = 500;
const unsigned long LONG_REPEAT_MS = 200;

inline bool rawPressed(int pin){ return digitalRead(pin) == LOW; }

ButtonState BTN_PLAY_STATE{PIN_BTN_PLAY, false, false, 0, 0, 0, false};
ButtonState BTN_NEXT_STATE{PIN_BTN_NEXT, false, false, 0, 0, 0, false};

void initButton(ButtonState& b){
  pinMode(b.pin, INPUT_PULLUP);
  bool r = rawPressed(b.pin);
  b.lastRaw = r;
  b.stable = r;
  b.lastChangeMs = millis();
  b.pressStartMs = 0;
  b.lastRepeatMs = 0;
  b.longActive = false;
}

ButtonEventType updateButton(ButtonState& b){
  unsigned long now = millis();
  bool raw = rawPressed(b.pin);

  if (raw != b.lastRaw){
    b.lastRaw = raw;
    b.lastChangeMs = now;
  }

  if ((now - b.lastChangeMs) >= DEBOUNCE_MS && raw != b.stable){
    b.stable = raw;
    if (b.stable){
      b.pressStartMs = now;
      b.lastRepeatMs = now;
      b.longActive = false;
      return BTN_EVENT_PRESSED;
    } else {
      if (b.longActive) return BTN_EVENT_RELEASED_LONG;
      return BTN_EVENT_RELEASED_SHORT;
    }
  }

  if (b.stable){
    if (!b.longActive && (now - b.pressStartMs) >= LONG_PRESS_MS){
      b.longActive = true;
      b.lastRepeatMs = now;
      return BTN_EVENT_LONG_START;
    }
    if (b.longActive && (now - b.lastRepeatMs) >= LONG_REPEAT_MS){
      b.lastRepeatMs = now;
      return BTN_EVENT_LONG_REPEAT;
    }
  }

  return BTN_EVENT_NONE;
}

// ---------- Marquee ----------
bool marqueeActive = false;
String marqueeText = "";
int marqueePos = 0;
unsigned long lastMarqueeMs = 0;
const unsigned long MARQUEE_PERIOD_MS = 300;

// ---------- Volume ----------
int currentVolume = 12;
int savedVolume = 12;
const int VOL_MIN = 0;
const int VOL_MAX = 21;
unsigned long volumeOverlayUntilMs = 0;
const unsigned long VOLUME_OVERLAY_MS = 3000;

// ---------- Boot IP timer ----------
unsigned long bootIpUntilMs = 0;
const unsigned long BOOT_IP_MS = 3000;

// ---------- ICY polling ----------
unsigned long lastIcyProbeMs = 0;
unsigned long ICY_PROBE_PERIOD_MS = 8000;

// ---------- Display abstraction ----------
const char* displayName(){
#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
  return "LCD 16x2";
#else
  return "OLED SSD1306";
#endif
}

void displayInit(){
#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
  lcd.init();
  lcd.backlight();
#else
  if(!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)){
    Serial.println("[DISPLAY] OLED init failed");
    return;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.display();
#endif
}

void displayWake(){
#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
  lcd.backlight();
#else
  oled.ssd1306_command(SSD1306_DISPLAYON);
#endif
}

void displaySleep(){
#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
  lcd.noBacklight();
#else
  oled.ssd1306_command(SSD1306_DISPLAYOFF);
#endif
}

void displayClear(){
#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
  lcd.clear();
#else
  oled.clearDisplay();
#endif
}

void displayPrintRawLine(uint8_t row, const String& text){
#if DISPLAY_TYPE == DISPLAY_TYPE_LCD
  lcd.setCursor(0, row);
  lcd.print(text);
#else
  const int y = (row == 0) ? 0 : 16;
  oled.fillRect(0, y, OLED_WIDTH, 8, SSD1306_BLACK);
  oled.setCursor(0, y);
  oled.print(text);
  oled.display();
#endif
}

void lcdPrintLine(uint8_t row, const String& s16){
  String t = s16;
  if (t.length() < 16) while (t.length() < 16) t += ' ';
  else if (t.length() > 16) t.remove(16);
  displayPrintRawLine(row, t);
}

void safeLcdPrint(const String& l1, const String& l2){
  displayClear();
  lcdPrintLine(0, l1.substring(0,16));
  lcdPrintLine(1, l2.substring(0,16));
  lastActivityMs = millis();
}

void showIpOnLcd(){
  wifi_mode_t mode = WiFi.getMode();
  IPAddress ip;
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ip = WiFi.softAPIP();
  else if (WiFi.status() == WL_CONNECTED) ip = WiFi.localIP();
  else ip = IPAddress(0,0,0,0);
  safeLcdPrint("EmmiRadio v"+String(FW_VERSION), ip.toString());
}

void showVolumeOverlay(){
  volumeOverlayUntilMs = millis() + VOLUME_OVERLAY_MS;
  marqueeActive = false;
  lcdPrintLine(1, "Vol: " + String(currentVolume));
}

void updateBootDisplayTimer(){
  if(bootIpUntilMs && millis() > bootIpUntilMs){
    bootIpUntilMs = 0;
    showNowPlaying();
  }
}

void logRuntimeSummary(){
  Serial.println();
  Serial.println("================ EmmiRadio Runtime ================");
  Serial.println(String("[SYS] Firmware: ") + FW_VERSION);
  Serial.println(String("[SYS] Display : ") + displayName());
  Serial.println(String("[SYS] FS      : FFat"));
  Serial.println(String("[SYS] Stations: ") + stationCount);
  Serial.println(String("[SYS] WiFi cfg: ") + wifiCount);
  Serial.println(String("[SYS] Volume  : ") + currentVolume);
  Serial.println(String("[SYS] Last st : ") + currentStation);
  Serial.println(String("[SYS] Saved V : ") + savedVolume);
  Serial.println(String("[SYS] Saved S : ") + savedStation);
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
    Serial.println(String("[NET] Mode    : AP"));
    Serial.println(String("[NET] SSID    : EmmiRadio-Setup"));
    Serial.println(String("[NET] PASS    : emmipass"));
    Serial.println(String("[NET] IP      : ") + WiFi.softAPIP().toString());
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.println(String("[NET] Mode    : STA"));
    Serial.println(String("[NET] SSID    : ") + WiFi.SSID());
    Serial.println(String("[NET] IP      : ") + WiFi.localIP().toString());
    Serial.println(String("[NET] RSSI    : ") + String(WiFi.RSSI()) + " dBm");
  } else {
    Serial.println(String("[NET] Mode    : STA (disconnected)"));
    Serial.println(String("[NET] IP      : none"));
  }
  Serial.println("===================================================");
  Serial.println();
}

// ---------- Marquee ----------
void startMarquee(const String& full){
  marqueeText = full + "   ";
  marqueePos = 0;
  lastMarqueeMs = millis();
  marqueeActive = (marqueeText.length() > 16);
  if (!marqueeActive) lcdPrintLine(1, full);
}

void tickMarquee(){
  if (!marqueeActive) return;
  if (millis() - lastMarqueeMs < MARQUEE_PERIOD_MS) return;
  lastMarqueeMs = millis();

  String w;
  int L = marqueeText.length();
  for (int i = 0; i < 16; i++) w += marqueeText[(marqueePos + i) % L];
  lcdPrintLine(1, w);
  marqueePos = (marqueePos + 1) % L;
}

// ---------- Now playing ----------
void showNowPlaying(){
  if(bootIpUntilMs > millis()) return;

  String line1 = (stationCount>0) ? stations[currentStation].name : "(no stations)";
  lcdPrintLine(0, line1);

  if (volumeOverlayUntilMs > millis()){
    lcdPrintLine(1, "Vol: " + String(currentVolume));
    return;
  }

  if (gotMeta && lastTitle.length()){
    startMarquee(lastTitle);
    return;
  }

  marqueeActive = false;
  String line2;
  if (isPlaying && (millis() - playStartMs) > META_TIMEOUT_MS) line2 = "PLAY";
  else if (isPlaying) line2 = "Connecting...";
  else line2 = "STOP";
  lcdPrintLine(1, line2);
}

// ---------- tiny JSON helpers ----------
String jsonGet(const String& line, const char* key){
  String k="\""; k+=key; k+="\"";
  int p=line.indexOf(k); if(p<0) return "";
  p=line.indexOf(':',p); if(p<0) return "";
  p=line.indexOf('"',p); if(p<0) return "";
  int q=line.indexOf('"',p+1); if(q<0) return "";
  return line.substring(p+1,q);
}

String jsonLineStation(const String& name,const String& url){
  return "{\"name\":\""+name+"\",\"url\":\""+url+"\"}";
}
String jsonLineWifi(const String& ssid,const String& pass){
  return "{\"ssid\":\""+ssid+"\",\"pass\":\""+pass+"\"}";
}
String jsonLineSettings(){
  return String("{\"ps\":\"") + (savedPowerSaveEnabled ? "1" : "0") +
         "\",\"vol\":\"" + String(savedVolume) +
         "\",\"st\":\"" + String(savedStation) + "\"}";
}

// ---------- Settings ----------
bool loadSettings(){
  savedPowerSaveEnabled = true;
  savedVolume = currentVolume;
  savedStation = currentStation;

  if(FFat.exists(SETTINGS_PATH)){
    File f = FFat.open(SETTINGS_PATH, "r");
    if(f){
      String l = f.readStringUntil('\n');
      f.close();
      l.trim();
      if(l.length() >= 5){
        String ps = jsonGet(l, "ps");
        ps.trim();
        if(ps == "0") savedPowerSaveEnabled = false;
        else if(ps == "1") savedPowerSaveEnabled = true;

        String vol = jsonGet(l, "vol");
        vol.trim();
        if(vol.length()){
          int v = vol.toInt();
          if(v < VOL_MIN) v = VOL_MIN;
          if(v > VOL_MAX) v = VOL_MAX;
          savedVolume = v;
        }

        String st = jsonGet(l, "st");
        st.trim();
        if(st.length()){
          int s = st.toInt();
          if(s >= 0 && s < MAX_STATIONS) savedStation = s;
        }
      }
    }
  }

  powerSaveEnabled = savedPowerSaveEnabled;
  currentVolume = savedVolume;
  currentStation = savedStation;
  return true;
}

bool saveSettings(){
  savedPowerSaveEnabled = powerSaveEnabled;
  savedVolume = currentVolume;
  savedStation = currentStation;

  File f = FFat.open(SETTINGS_PATH, "w"); if(!f) return false;
  f.println(jsonLineSettings());
  f.close();
  return true;
}

// ---------- Stations ----------
void loadDefaultStations(){
  stationCount=0;
  stations[stationCount++] = {"SomaFM Metal",       "http://ice1.somafm.com/metal-128-mp3"};
  stations[stationCount++] = {"SomaFM GrooveSalad", "http://ice1.somafm.com/groovesalad-128-mp3"};
  stations[stationCount++] = {"SomaFM SecretAgent", "http://ice1.somafm.com/secretagent-128-mp3"};
  currentStation=0;
  savedStation=0;
}

bool loadStations(){
  stationCount=0;
  if(!FFat.exists(RADIOS_PATH)) return false;
  File f=FFat.open(RADIOS_PATH,"r"); if(!f) return false;
  while(f.available() && stationCount<MAX_STATIONS){
    String l=f.readStringUntil('\n'); l.trim();
    if(l.length()<5) continue;
    String n=jsonGet(l,"name"), u=jsonGet(l,"url");
    if(n.length() && u.length()) stations[stationCount++]={n,u};
  }
  f.close();
  if(stationCount==0) return false;
  if(currentStation>=stationCount) currentStation=0;
  if(savedStation>=stationCount) savedStation=0;
  return true;
}

bool saveStations(){
  File f=FFat.open(RADIOS_PATH,"w"); if(!f) return false;
  for(int i=0;i<stationCount;i++) f.println(jsonLineStation(stations[i].name, stations[i].url));
  f.close();
  return true;
}

// ---------- WiFi list ----------
bool loadWiFiList(){
  wifiCount=0;
  if(!FFat.exists(WIFI_CFG_PATH)) return false;
  File f=FFat.open(WIFI_CFG_PATH,"r"); if(!f) return false;
  while(f.available() && wifiCount<MAX_WIFI){
    String l=f.readStringUntil('\n'); l.trim();
    if(l.length()<5) continue;
    String ss=jsonGet(l,"ssid");
    String pw=jsonGet(l,"pass");
    ss.trim(); pw.trim();
    if(ss.length()>0) wifiList[wifiCount++] = { ss, pw };
  }
  f.close();
  return wifiCount>0;
}

bool saveWiFiList(){
  File f=FFat.open(WIFI_CFG_PATH,"w"); if(!f) return false;
  for(int i=0;i<wifiCount;i++) f.println(jsonLineWifi(wifiList[i].ssid, wifiList[i].pass));
  f.close();
  return true;
}

// ---------- Volume ----------
void applyVolume(){
  if(currentVolume < VOL_MIN) currentVolume = VOL_MIN;
  if(currentVolume > VOL_MAX) currentVolume = VOL_MAX;
  audio.setVolume(currentVolume);
}

void volumeUpStep(){
  if(currentVolume < VOL_MAX){
    currentVolume++;
    applyVolume();
    showVolumeOverlay();
    Serial.println(String("[AUDIO] Volume -> ") + currentVolume);
  }
}

void volumeDownStep(){
  if(currentVolume > VOL_MIN){
    currentVolume--;
    applyVolume();
    showVolumeOverlay();
    Serial.println(String("[AUDIO] Volume -> ") + currentVolume);
  }
}

// ---------- Web settings ----------
void handleSettingsPost(){
  powerSaveEnabled = server.hasArg("ps");
  if(server.hasArg("vol")){
    int v = server.arg("vol").toInt();
    if(v < VOL_MIN) v = VOL_MIN;
    if(v > VOL_MAX) v = VOL_MAX;
    currentVolume = v;
    applyVolume();
    showVolumeOverlay();
  }
  saveSettings();

  if(!powerSaveEnabled && lcdBacklightOff){
    displayWake();
    lcdBacklightOff = false;
  }
  lastActivityMs = millis();
  redirectHome();
}

void handleVolumeSet(){
  if(server.hasArg("v")){
    int v = server.arg("v").toInt();
    if(v < VOL_MIN) v = VOL_MIN;
    if(v > VOL_MAX) v = VOL_MAX;
    currentVolume = v;
    applyVolume();
    showVolumeOverlay();
    Serial.println(String("[AUDIO] Volume -> ") + currentVolume);
  }
  server.send(200, "text/plain", "OK");
}

// ---------- HTML ----------
String htmlHeader(const String& title){
  return "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><meta charset='utf-8'><title>"+title+"</title>"
         "<style>"
         ":root{--bg:#0f172a;--panel:#111827;--text:#e5e7eb;--muted:#9ca3af;--border:#374151}"
         "*{box-sizing:border-box}"
         "body{font-family:Arial,sans-serif;margin:0;background:linear-gradient(180deg,#0b1220,#111827);color:var(--text)}"
         ".wrap{max-width:920px;margin:0 auto;padding:18px}"
         ".hero{padding:18px 18px 10px}"
         ".title{font-size:28px;font-weight:700}"
         ".sub{color:var(--muted);margin-top:6px}"
         ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}"
         ".card{background:rgba(17,24,39,.92);border:1px solid var(--border);border-radius:18px;padding:16px}"
         ".stat{display:flex;justify-content:space-between;gap:8px;padding:8px 0;border-bottom:1px solid rgba(255,255,255,.06)}"
         ".stat:last-child{border-bottom:none}"
         ".label{color:var(--muted)}"
         ".value{font-weight:700;text-align:right;word-break:break-word}"
         ".controls{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-top:10px}"
         ".btn,.btnlink{display:inline-flex;align-items:center;justify-content:center;padding:12px 14px;font-size:15px;font-weight:700;border-radius:14px;border:1px solid var(--border);text-decoration:none;color:#fff;cursor:pointer}"
         ".btn-green{background:linear-gradient(180deg,#22c55e,#15803d)}"
         ".btn-blue{background:linear-gradient(180deg,#38bdf8,#0284c7)}"
         ".btn-red{background:linear-gradient(180deg,#ef4444,#b91c1c)}"
         ".btn-gray{background:linear-gradient(180deg,#4b5563,#1f2937)}"
         ".row{margin:12px 0}"
         "input,select{width:100%;padding:12px 14px;font-size:15px;border-radius:12px;border:1px solid var(--border);background:#0b1220;color:#fff}"
         "input[type=range]{padding:0;height:36px}"
         "table{border-collapse:collapse;width:100%}"
         "th,td{border-bottom:1px solid rgba(255,255,255,.08);padding:10px 8px;text-align:left}"
         ".toolbar{display:flex;flex-wrap:wrap;gap:10px;margin-top:10px}"
         ".note{padding:12px;border-radius:14px;background:rgba(34,197,94,.08);border:1px solid rgba(34,197,94,.25)}"
         ".mono{font-family:Consolas,monospace}"
         "</style></head><body><div class='wrap'>";
}
String htmlFooter(){ return "</div></body></html>"; }

String buildStationOptions(){
  String s;
  for(int i=0;i<stationCount;i++){
    s += "<option value='" + String(i) + "'";
    if(i == currentStation) s += " selected";
    s += ">" + stations[i].name + "</option>";
  }
  return s;
}

String pageOtaUpdate(const String& note){
  String h=htmlHeader("Firmware Update");
  h+="<div class='hero'><div class='title'>Firmware Update</div><div class='sub'>Upload a compiled .bin file and reboot the device.</div></div><div class='card'>";
  if(note.length()) h+="<div class='note'>"+note+"</div>";
  h+="<form method='POST' action='/update' enctype='multipart/form-data'><div class='row'><input type='file' name='firmware'></div><div class='toolbar'><button class='btn btn-blue' type='submit'>Upload & Flash</button><a class='btnlink btn-gray' href='/'>Back</a></div></form></div>";
  h+=htmlFooter();
  return h;
}

String pageIndex(){
  wifi_mode_t mode = WiFi.getMode();
  String netMode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ? "AP" : ((WiFi.status()==WL_CONNECTED) ? "STA" : "STA (disconnected)");
  String st=isPlaying?"PLAYING":"STOPPED";
  String ip=(WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():WiFi.softAPIP().toString();
  String cur=(stationCount>0)?stations[currentStation].name:"(no stations)";
  String meta=(gotMeta && lastTitle.length())?lastTitle:"(no metadata yet)";

  String h=htmlHeader("EmmiRadio");
  h+="<div class='hero'><div class='title'>EmmiRadio v"+String(FW_VERSION)+"</div><div class='sub'>Explicit save model, less pointless flash writing.</div></div><div class='grid'>";

  h+="<div class='card'>"
     "<div class='stat'><div class='label'>Playback</div><div class='value'>"+st+"</div></div>"
     "<div class='stat'><div class='label'>Station</div><div class='value'>"+cur+"</div></div>"
     "<div class='stat'><div class='label'>Metadata</div><div class='value'>"+meta+"</div></div>"
     "<div class='stat'><div class='label'>IP</div><div class='value mono'>"+ip+"</div></div>"
     "<div class='stat'><div class='label'>Network</div><div class='value'>"+netMode+"</div></div>"
     "<div class='stat'><div class='label'>Volume</div><div class='value' id='volValue'>"+String(currentVolume)+"</div></div>"
     "<div class='controls'>"
     "<a class='btnlink btn-gray' href='/api/prev'>Prev</a>"
     "<a class='btnlink btn-green' href='/api/play'>Play</a>"
     "<a class='btnlink btn-red' href='/api/stop'>Stop</a>"
     "<a class='btnlink btn-blue' href='/api/next'>Next</a>"
     "</div></div>";

  h+="<div class='card'>"
     "<form method='GET' action='/api/st_select'>"
     "<div class='row'><label>Select station</label><select name='i'>"+buildStationOptions()+"</select></div>"
     "<button class='btn btn-blue' type='submit'>Switch Station</button>"
     "</form>"
     "<form method='POST' action='/settings'>"
     "<div class='row'><label>Runtime / startup volume after save: <b id='volLabel'>"+String(currentVolume)+"</b></label>"
     "<input type='range' min='0' max='21' step='1' name='vol' value='"+String(currentVolume)+"' oninput='liveVolume(this.value)'></div>"
     "<div class='row'><label><input type='checkbox' name='ps' value='1' ";
  if(powerSaveEnabled) h += "checked";
  h += "> Save display power save (30s)</label></div>"
       "<div class='toolbar'>"
       "<button class='btn btn-blue' type='submit'>Save Settings</button>"
       "<a class='btnlink btn-gray' href='/stations'>Stations</a>"
       "<a class='btnlink btn-gray' href='/wifi'>Wi-Fi</a>"
       "<a class='btnlink btn-gray' href='/update'>OTA</a>"
       "</div></form></div></div>"
       "<script>"
       "let vt=null;"
       "function liveVolume(v){"
       "document.getElementById('volLabel').innerText=v;"
       "document.getElementById('volValue').innerText=v;"
       "clearTimeout(vt);"
       "vt=setTimeout(()=>{fetch('/api/vol_set?v='+v).catch(()=>{});},120);"
       "}"
       "</script>";
  h+=htmlFooter();
  return h;
}

String pageStations(const String& note){
  String h=htmlHeader("Stations");
  h+="<div class='hero'><div class='title'>Stations</div></div><div class='card'>";
  if(note.length()) h+="<div class='note'>"+note+"</div>";
  h+="<table><tr><th>#</th><th>Name</th><th>Actions</th></tr>";
  for(int i=0;i<stationCount;i++){
    h+="<tr><td>"+String(i)+"</td><td>"+stations[i].name+"</td><td><div class='toolbar'><a class='btnlink btn-gray' href='/stations?edit="+String(i)+"'>Edit</a><a class='btnlink btn-red' href='/api/st_del?i="+String(i)+"'>Delete</a><a class='btnlink btn-blue' href='/api/st_select?i="+String(i)+"'>Select</a></div></td></tr>";
  }
  h+="</table>";
  int edit = server.hasArg("edit") ? server.arg("edit").toInt() : -1;
  String fName = (edit>=0 && edit<stationCount)?stations[edit].name:"";
  String fUrl  = (edit>=0 && edit<stationCount)?stations[edit].url :"";
  h+="<form method='POST' action='/stations'>";
  if(edit>=0) h+="<input type='hidden' name='i' value='"+String(edit)+"'>";
  h+="<div class='row'><input name='name' value='"+fName+"' placeholder='Name'></div><div class='row'><input name='url' value='"+fUrl+"' placeholder='URL'></div><div class='toolbar'><button class='btn btn-blue' type='submit'>"+String(edit>=0?"Save":"Add")+"</button><a class='btnlink btn-gray' href='/'>Back</a></div></form></div>";
  h+=htmlFooter();
  return h;
}

String pageWifiForm(const String& note){
  String ip=(WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():WiFi.softAPIP().toString();
  String h=htmlHeader("Wi-Fi");
  h+="<div class='hero'><div class='title'>Wi-Fi</div></div><div class='card'>";
  if(note.length()) h+="<div class='note'>"+note+"</div>";
  h+="<div class='row'>Current IP: <span class='mono'>"+ip+"</span></div><table><tr><th>#</th><th>SSID</th><th>Actions</th></tr>";
  for(int i=0;i<wifiCount;i++) h+="<tr><td>"+String(i)+"</td><td>"+wifiList[i].ssid+"</td><td><a class='btnlink btn-red' href='/api/wifi_del?i="+String(i)+"'>Delete</a></td></tr>";
  if(wifiCount==0) h+="<tr><td colspan='3'>(no saved networks)</td></tr>";
  h+="</table><form method='POST' action='/wifi'><div class='row'><input name='ssid' placeholder='SSID'></div><div class='row'><input name='pass' placeholder='Password'></div><div class='toolbar'><button class='btn btn-blue' type='submit'>Save & Try Connect</button><a class='btnlink btn-gray' href='/'>Back</a></div></form></div>";
  h+=htmlFooter();
  return h;
}

void redirectHome(){
  server.sendHeader("Location","/");
  server.send(303,"text/plain","");
}

// ---------- ICY ----------
bool parseHttpUrl(const String& url, String& host, uint16_t& port, String& path){
  String u = url;
  if(u.startsWith("http://")) u.remove(0,7); else return false;
  int slash = u.indexOf('/');
  String hp = (slash>=0)? u.substring(0,slash) : u;
  path = (slash>=0)? u.substring(slash) : "/";
  int colon = hp.indexOf(':');
  if(colon>=0){ host = hp.substring(0,colon); port = hp.substring(colon+1).toInt(); }
  else { host = hp; port = 80; }
  host.trim();
  return host.length()>0;
}

String fetchIcyTitleOnce(const String& url, uint32_t overallTimeoutMs){
  String host, path; uint16_t port;
  if(!parseHttpUrl(url, host, port, path)) return "";

  WiFiClient client;
  client.setTimeout(1500);
  if(!client.connect(host.c_str(), port)) return "";

  String req = String("GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\nIcy-MetaData: 1\r\nUser-Agent: EmmiRadio/1.8\r\nConnection: close\r\n\r\n";
  client.print(req);

  uint32_t t0 = millis();
  int metaInt = -1;
  while(millis()-t0 < overallTimeoutMs){
    String line = client.readStringUntil('\n');
    if(line.length()==0){ delay(1); continue; }
    line.trim();
    if(line.length()==0) break;
    String L=line; L.toLowerCase();
    if(L.startsWith("icy-metaint:")){
      int p=line.indexOf(':');
      if(p>0) metaInt = line.substring(p+1).toInt();
    }
  }
  if(metaInt<=0){ client.stop(); return ""; }

  int remaining = metaInt;
  uint8_t buf[512];
  while(remaining>0 && (millis()-t0)<overallTimeoutMs){
    int need = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
    int got = client.readBytes((char*)buf, need);
    if(got<=0){ delay(2); continue; }
    remaining -= got;
  }
  if(remaining>0){ client.stop(); return ""; }

  while(!client.available() && (millis()-t0)<overallTimeoutMs) delay(2);
  if(!client.available()){ client.stop(); return ""; }

  int metaLenByte = client.read();
  int metaLen = metaLenByte * 16;
  if(metaLen<=0){ client.stop(); return ""; }

  String meta;
  while(metaLen>0 && (millis()-t0)<overallTimeoutMs){
    while(client.available() && metaLen>0){
      char c=(char)client.read();
      if(c!='\0') meta += c;
      metaLen--;
    }
    if(metaLen>0) delay(1);
  }
  client.stop();

  int a=meta.indexOf("StreamTitle='"); if(a<0) return "";
  a+=13;
  int b=meta.indexOf("';",a); if(b<0) return "";
  String title = meta.substring(a,b);
  title.trim();
  return title;
}

void tryIcyFallback(){
  if(!isPlaying || stationCount==0) return;
  if(millis() - lastIcyProbeMs < ICY_PROBE_PERIOD_MS) return;
  lastIcyProbeMs = millis();

  String t = fetchIcyTitleOnce(stations[currentStation].url, 6000);
  if(t.length()==0) return;

  if(t != lastTitle){
    lastTitle = t;
    gotMeta = true;
    Serial.print("[META/FALLBACK] ");
    Serial.println(lastTitle);
    showNowPlaying();
  }
}

// ---------- OTA ----------
void handleOtaUpload(){
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    Serial.println("[OTA] Update start");
    safeLcdPrint("UPDATING...", "Please wait");
    if(!Update.begin()) Serial.println("[OTA] Update.begin() failed");
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(Update.write(upload.buf, upload.currentSize) != upload.currentSize) Serial.println("[OTA] Write failed");
  } else if(upload.status == UPLOAD_FILE_END){
    if(Update.end(true)) Serial.println("[OTA] Update success");
    else Serial.println("[OTA] Update error");
  }
}

void handleOtaPage(){ server.send(200,"text/html",pageOtaUpdate("")); }

// ---------- Playback ----------
void doPlay(){
  if(stationCount==0) return;
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("[AUDIO] Cannot PLAY: no WiFi connection");
    safeLcdPrint("No WiFi","Cannot PLAY");
    return;
  }
  lastTitle=""; gotMeta=false; playStartMs=millis(); marqueeActive=false;
  Serial.println(String("[AUDIO] connect to ") + stations[currentStation].url);
  audio.connecttohost(stations[currentStation].url.c_str());
  isPlaying = true;
  showNowPlaying();
}

void doStop(){
  if(!isPlaying) return;
  Serial.println("[AUDIO] stop");
  audio.stopSong();
  isPlaying = false;
  gotMeta = false;
  marqueeActive = false;
  showNowPlaying();
}

void selectStation(int idx){
  if(idx<0 || idx>=stationCount) return;
  currentStation = idx;
  Serial.println(String("[ST] selected: ") + stations[currentStation].name);
}

void changeStation(int delta){
  if(stationCount==0) return;
  int newIdx = (currentStation + delta + stationCount) % stationCount;
  selectStation(newIdx);
  if(isPlaying){
    audio.stopSong();
    delay(50);
    lastTitle=""; gotMeta=false; playStartMs=millis(); marqueeActive=false;
    audio.connecttohost(stations[currentStation].url.c_str());
  }
  showNowPlaying();
}

// ---------- HTTP handlers ----------
void handleIndex(){ server.send(200,"text/html",pageIndex()); }
void handleVolUp(){ volumeUpStep(); redirectHome(); }
void handleVolDown(){ volumeDownStep(); redirectHome(); }
void handlePlay(){ doPlay(); redirectHome(); }
void handleStop(){ doStop(); redirectHome(); }
void handleNext(){ changeStation(+1); redirectHome(); }
void handlePrev(){ changeStation(-1); redirectHome(); }

void handleWifiPost(){
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim(); pass.trim();

  if(ssid=="") {
    server.send(200,"text/html",pageWifiForm("SSID required."));
    return;
  }

  int idx = -1;
  for(int i=0;i<wifiCount;i++) if(wifiList[i].ssid == ssid){ idx = i; break; }

  if(idx >= 0) wifiList[idx].pass = pass;
  else {
    if(wifiCount >= MAX_WIFI){
      server.send(200,"text/html",pageWifiForm("Wi-Fi list full."));
      return;
    }
    wifiList[wifiCount++] = { ssid, pass };
  }

  saveWiFiList();

  bool ok = connectWiFiSmart(7000, 3);
  String note = ok && WiFi.status()==WL_CONNECTED ? "Saved. Connected to " + WiFi.SSID() + "." : "Saved. Could not connect now. AP mode restored.";
  if(!(ok && WiFi.status()==WL_CONNECTED)) startAP();
  logRuntimeSummary();
  server.send(200,"text/html",pageWifiForm(note));
}

void handleWifiGet(){ server.send(200,"text/html",pageWifiForm("")); }

void handleWifiDel(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0||i>=wifiCount){ redirectHome(); return; }
  for(int k=i;k<wifiCount-1;k++) wifiList[k]=wifiList[k+1];
  wifiCount--;
  saveWiFiList();
  redirectHome();
}

void handleStationsGet(){ server.send(200,"text/html",pageStations("")); }

void handleStationsPost(){
  String name=server.arg("name"), url=server.arg("url");
  name.trim(); url.trim();

  if(name==""||url==""){
    server.send(200,"text/html",pageStations("Name/URL required"));
    return;
  }

  if(server.hasArg("i")){
    int i=server.arg("i").toInt();
    if(i>=0 && i<stationCount){
      stations[i]={name,url};
      saveStations();
      server.send(200,"text/html",pageStations("Updated."));
    } else {
      server.send(200,"text/html",pageStations("Invalid index."));
    }
  } else {
    if(stationCount<MAX_STATIONS){
      stations[stationCount++]={name,url};
      saveStations();
      server.send(200,"text/html",pageStations("Added."));
    } else {
      server.send(200,"text/html",pageStations("List full."));
    }
  }
}

void handleStDel(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0||i>=stationCount){ redirectHome(); return; }
  for(int k=i;k<stationCount-1;k++) stations[k]=stations[k+1];
  stationCount--;
  if(currentStation>=stationCount) currentStation=0;
  if(savedStation>=stationCount) savedStation=0;
  saveStations();
  redirectHome();
}

void handleStSelect(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0||i>=stationCount){ redirectHome(); return; }
  selectStation(i);
  if(isPlaying){
    audio.stopSong();
    delay(50);
    lastTitle=""; gotMeta=false; playStartMs=millis(); marqueeActive=false;
    audio.connecttohost(stations[currentStation].url.c_str());
  }
  showNowPlaying();
  redirectHome();
}

void startWebServer(){
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/play", HTTP_GET, handlePlay);
  server.on("/api/stop", HTTP_GET, handleStop);
  server.on("/api/next", HTTP_GET, handleNext);
  server.on("/api/prev", HTTP_GET, handlePrev);
  server.on("/api/vol_up", HTTP_GET, handleVolUp);
  server.on("/api/vol_down", HTTP_GET, handleVolDown);
  server.on("/api/vol_set", HTTP_GET, handleVolumeSet);

  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/wifi_del", HTTP_GET, handleWifiDel);

  server.on("/stations", HTTP_GET, handleStationsGet);
  server.on("/stations", HTTP_POST, handleStationsPost);
  server.on("/api/st_del", HTTP_GET, handleStDel);
  server.on("/api/st_select", HTTP_GET, handleStSelect);

  server.on("/settings", HTTP_POST, handleSettingsPost);

  server.on("/update", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, [](){
    server.send(200, "text/html", pageOtaUpdate("Upload complete. Rebooting..."));
    delay(1000);
    ESP.restart();
  }, handleOtaUpload);

  server.begin();
  Serial.println("[HTTP] Web server started on port 80");
}

// ---------- WiFi ----------
bool connectWiFiSmart(unsigned long timeoutPerNetMs, int maxAttempts) {
  if (wifiCount == 0) return false;

  Serial.println("[WiFi] connectWiFiSmart()");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    WiFi.scanDelete();
    return false;
  }

  bool knownVisible[MAX_WIFI];
  for (int i=0;i<MAX_WIFI;i++) knownVisible[i]=false;

  int knownCount=0;
  for (int s = 0; s < n; s++){
    String ssidScan = WiFi.SSID(s);
    ssidScan.trim();
    for (int i = 0; i < wifiCount; i++){
      if (!knownVisible[i] && ssidScan == wifiList[i].ssid){
        knownVisible[i]=true;
        knownCount++;
        break;
      }
    }
  }
  WiFi.scanDelete();

  if (knownCount == 0) return false;

  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    for (int i = 0; i < wifiCount; i++) {
      if (!knownVisible[i]) continue;

      WiFi.disconnect();
      delay(200);
      WiFi.begin(wifiList[i].ssid.c_str(), wifiList[i].pass.c_str());

      unsigned long t0 = millis();
      wl_status_t st = WiFi.status();
      while (st != WL_CONNECTED && (millis() - t0) < timeoutPerNetMs) {
        delay(250);
        st = WiFi.status();
      }

      if (st == WL_CONNECTED) {
        WiFi.setAutoReconnect(true);
        return true;
      }
    }
    if (attempt < maxAttempts - 1) delay(1000);
  }
  return false;
}

void startAP(){
  WiFi.mode(WIFI_AP);
  WiFi.softAP("EmmiRadio-Setup","emmipass");
  Serial.println(String("[WiFi] AP IP: ") + WiFi.softAPIP().toString());
}

// ---------- setup / loop ----------
void setup(){
  Serial.begin(115200);
  delay(250);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  displayInit();
  displayWake();
  safeLcdPrint("EmmiRadio v1.8","Boot...");

  if(!FFat.begin(true)){
    safeLcdPrint("FFat error", "Mount failed");
    delay(1500);
  }

  loadSettings();

  initButton(BTN_PLAY_STATE);
  initButton(BTN_NEXT_STATE);

  audio.setPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DATA);
  applyVolume();

  if(!loadStations()){
    loadDefaultStations();
    saveStations();
  }
  if(currentStation >= stationCount) currentStation = 0;
  if(savedStation >= stationCount) savedStation = 0;

  bool wifiOk = loadWiFiList() && connectWiFiSmart(7000, 5);
  if(!wifiOk) startAP();

  startWebServer();

  showIpOnLcd();
  bootIpUntilMs = millis() + BOOT_IP_MS;

  lastActivityMs = millis();
  lcdBacklightOff = false;
  logRuntimeSummary();
}

void loop(){
  audio.loop();
  server.handleClient();

  updateBootDisplayTimer();

  if(volumeOverlayUntilMs && millis() > volumeOverlayUntilMs){
    volumeOverlayUntilMs = 0;
    showNowPlaying();
  }

  tickMarquee();

  if (powerSaveEnabled && !lcdBacklightOff && (millis()-lastActivityMs > LCD_SLEEP_MS)){
    displaySleep();
    lcdBacklightOff = true;
  }

  ButtonEventType evPlay = updateButton(BTN_PLAY_STATE);
  ButtonEventType evNext = updateButton(BTN_NEXT_STATE);

  if (evPlay == BTN_EVENT_PRESSED || evNext == BTN_EVENT_PRESSED){
    lastActivityMs = millis();
    if(lcdBacklightOff){
      displayWake();
      lcdBacklightOff = false;
    }
  }

  // PLAY: short = play/stop, long = volume up
  if (evPlay == BTN_EVENT_RELEASED_SHORT){
    lastActivityMs = millis();
    if(!isPlaying) doPlay(); else doStop();
  } else if (evPlay == BTN_EVENT_LONG_START || evPlay == BTN_EVENT_LONG_REPEAT){
    lastActivityMs = millis();
    volumeUpStep();
  }

  // NEXT: short = next station, long = volume down
  if (evNext == BTN_EVENT_RELEASED_SHORT){
    lastActivityMs = millis();
    changeStation(+1);
  } else if (evNext == BTN_EVENT_LONG_START || evNext == BTN_EVENT_LONG_REPEAT){
    lastActivityMs = millis();
    volumeDownStep();
  }

  if(isPlaying && !gotMeta && volumeOverlayUntilMs == 0){
    static unsigned long lastRefresh=0;
    if(millis()-lastRefresh>1000){
      showNowPlaying();
      lastRefresh=millis();
    }
  }

  tryIcyFallback();
}

// ---------- audio callbacks ----------
void audio_info(const char *info){
  Serial.print("[INFO] ");
  Serial.println(info);
}

void audio_showstreamtitle(const char* t){
  String nt=String(t);
  nt.trim();
  if(nt.length()==0) return;
  if(nt!=lastTitle){
    lastTitle=nt;
    gotMeta=true;
    Serial.print("[META/CALLBACK] ");
    Serial.println(lastTitle);
    showNowPlaying();
  }
}

void audio_eof_mp3(const char* f){
  Serial.println("[AUDIO] EOF");
}
