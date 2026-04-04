/*
  EmmiRadio v1.73
  Fix: compile-safe button event enum setup, based on v1.71 button handling.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Audio.h>
#include <Update.h>
#include <Wire.h>
#include <FFat.h>

const char* FW_VERSION = "1.73";

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

void handleSettingsPost();
void handleVolumeSet();
void safeLcdPrint(const String& l1, const String& l2);
void lcdPrintLine(uint8_t row, const String& s16);
void startMarquee(const String& full);
void tickMarquee();
void showNowPlaying();
void showIpOnLcd();
void showVolumeOnDisplay();
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
void setVolumeAndPersist(int v, bool showOnDisplayNow=true);
void selectStation(int idx, bool restartIfPlaying);

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

const int PIN_BTN_NEXT = 43;
const int PIN_BTN_PREV = 44;
const int PIN_BTN_PLAY = 1;

const int PIN_I2S_BCK  = 3;
const int PIN_I2S_LRCK = 4;
const int PIN_I2S_DATA = 2;

Audio audio;

struct Station { String name; String url; };
const int MAX_STATIONS = 32;
Station stations[MAX_STATIONS];
int stationCount = 0;
int currentStation = 0;
const char* RADIOS_PATH = "/radios.json";

struct WifiEntry { String ssid; String pass; };
const int MAX_WIFI = 8;
WifiEntry wifiList[MAX_WIFI];
int wifiCount = 0;
const char* WIFI_CFG_PATH = "/WiFi.json";
const char* SETTINGS_PATH = "/settings.json";

WebServer server(80);

bool isPlaying = false;
String lastTitle = "";
bool gotMeta = false;
unsigned long playStartMs = 0;
const unsigned long META_TIMEOUT_MS = 5000;

struct DebBtn {
  int pin;
  bool stable;
  unsigned long lastChangeMs;
  unsigned long pressedSinceMs;
  unsigned long lastRepeatMs;
  bool longPressActive;
};
const unsigned long DEBOUNCE_MS = 30;
const unsigned long LONG_PRESS_MS = 500;
const unsigned long LONG_REPEAT_MS = 200;
inline bool rawPressed(int pin){ return digitalRead(pin)==LOW; }
DebBtn BTN_NEXT{PIN_BTN_NEXT, false, 0, 0, 0, false};
DebBtn BTN_PREV{PIN_BTN_PREV, false, 0, 0, 0, false};
DebBtn BTN_PLAY{PIN_BTN_PLAY, false, 0, 0, 0, false};

enum BtnEventType { BTN_NONE, BTN_PRESSED, BTN_RELEASED_SHORT, BTN_RELEASED_LONG };

void initBtn(DebBtn& b){
  pinMode(b.pin, INPUT_PULLUP);
  b.stable = rawPressed(b.pin);
  b.lastChangeMs = millis();
  b.pressedSinceMs = b.stable ? millis() : 0;
  b.lastRepeatMs = 0;
  b.longPressActive = false;
}

BtnEventType pollButton(DebBtn& b){
  unsigned long now = millis();
  bool raw = rawPressed(b.pin);

  if(raw != b.stable){
    if((now - b.lastChangeMs) >= DEBOUNCE_MS){
      bool prev = b.stable;
      b.stable = raw;
      b.lastChangeMs = now;

      if(!prev && b.stable){
        b.pressedSinceMs = now;
        b.lastRepeatMs = now;
        b.longPressActive = false;
        return BTN_PRESSED;
      }

      if(prev && !b.stable){
        b.pressedSinceMs = 0;
        b.lastRepeatMs = 0;
        bool wasLong = b.longPressActive;
        b.longPressActive = false;
        return wasLong ? BTN_RELEASED_LONG : BTN_RELEASED_SHORT;
      }
    }
  }

  return BTN_NONE;
}

bool btnHoldRepeat(DebBtn& b){
  unsigned long now = millis();
  if(!b.stable) return false;
  if(b.pressedSinceMs == 0) return false;
  if((now - b.pressedSinceMs) < LONG_PRESS_MS) return false;
  if((now - b.lastRepeatMs) >= LONG_REPEAT_MS){
    b.longPressActive = true;
    b.lastRepeatMs = now;
    return true;
  }
  return false;
}

bool marqueeActive = false;
String marqueeText = "";
int marqueePos = 0;
unsigned long lastMarqueeMs = 0;
const unsigned long MARQUEE_PERIOD_MS = 300;

unsigned long volumeOverlayUntilMs = 0;
const unsigned long VOLUME_OVERLAY_MS = 3000;

unsigned long lastIcyProbeMs = 0;
unsigned long ICY_PROBE_PERIOD_MS = 8000;

int currentVolume = 12;
const int VOL_MIN = 0;
const int VOL_MAX = 21;

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

void showVolumeOnDisplay(){
  volumeOverlayUntilMs = millis() + VOLUME_OVERLAY_MS;
  marqueeActive = false;
  lcdPrintLine(1, "Vol: " + String(currentVolume));
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

void showNowPlaying(){
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

String jsonGet(const String& line, const char* key){
  String k="\""; k+=key; k+="\"";
  int p=line.indexOf(k); if(p<0) return "";
  p=line.indexOf(':',p); if(p<0) return "";
  p=line.indexOf('"',p); if(p<0) return "";
  int q=line.indexOf('"',p+1); if(q<0) return "";
  return line.substring(p+1,q);
}

String jsonLineStation(const String& name,const String& url){ return "{\"name\":\""+name+"\",\"url\":\""+url+"\"}"; }
String jsonLineWifi(const String& ssid,const String& pass){ return "{\"ssid\":\""+ssid+"\",\"pass\":\""+pass+"\"}"; }
String jsonLineSettings(){
  return String("{\"ps\":\"") + (powerSaveEnabled ? "1" : "0") +
         "\",\"vol\":\"" + String(currentVolume) +
         "\",\"st\":\"" + String(currentStation) + "\"}";
}

bool loadSettings(){
  if(!FFat.exists(SETTINGS_PATH)) return false;
  File f = FFat.open(SETTINGS_PATH, "r"); if(!f) return false;
  String l = f.readStringUntil('\n');
  f.close();
  l.trim();
  if(l.length() < 5) return false;
  String ps = jsonGet(l, "ps");
  ps.trim();
  if(ps == "0") powerSaveEnabled = false;
  else if(ps == "1") powerSaveEnabled = true;
  String vol = jsonGet(l, "vol");
  vol.trim();
  if(vol.length()){
    int v = vol.toInt();
    if(v < VOL_MIN) v = VOL_MIN;
    if(v > VOL_MAX) v = VOL_MAX;
    currentVolume = v;
  }
  String st = jsonGet(l, "st");
  st.trim();
  if(st.length()){
    int s = st.toInt();
    if(s >= 0 && s < MAX_STATIONS) currentStation = s;
  }
  return true;
}

bool saveSettings(){
  File f = FFat.open(SETTINGS_PATH, "w"); if(!f) return false;
  f.println(jsonLineSettings());
  f.close();
  return true;
}

void loadDefaultStations(){
  stationCount=0;
  stations[stationCount++] = {"SomaFM Metal",       "http://ice1.somafm.com/metal-128-mp3"};
  stations[stationCount++] = {"SomaFM GrooveSalad", "http://ice1.somafm.com/groovesalad-128-mp3"};
  stations[stationCount++] = {"SomaFM SecretAgent", "http://ice1.somafm.com/secretagent-128-mp3"};
  currentStation=0;
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
  return true;
}

bool saveStations(){
  File f=FFat.open(RADIOS_PATH,"w"); if(!f) return false;
  for(int i=0;i<stationCount;i++) f.println(jsonLineStation(stations[i].name, stations[i].url));
  f.close();
  return true;
}

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

void applyVolume(){
  if(currentVolume < VOL_MIN) currentVolume = VOL_MIN;
  if(currentVolume > VOL_MAX) currentVolume = VOL_MAX;
  audio.setVolume(currentVolume);
}

void setVolumeAndPersist(int v, bool showOnDisplayNow){
  if(v < VOL_MIN) v = VOL_MIN;
  if(v > VOL_MAX) v = VOL_MAX;
  if(v == currentVolume) return;
  currentVolume = v;
  applyVolume();
  saveSettings();
  Serial.println(String("[AUDIO] Volume -> ") + currentVolume + " (saved)");
  if(showOnDisplayNow) showVolumeOnDisplay();
}

void selectStation(int idx, bool restartIfPlaying){
  if(idx < 0 || idx >= stationCount) return;
  currentStation = idx;
  saveSettings();
  Serial.println(String("[ST] selected: ") + stations[currentStation].name + " (saved)");
  if(restartIfPlaying && isPlaying){
    audio.stopSong(); delay(50);
    lastTitle=""; gotMeta=false; playStartMs=millis(); marqueeActive=false;
    Serial.println(String("[AUDIO] connect to ") + stations[currentStation].url);
    audio.connecttohost(stations[currentStation].url.c_str());
  }
  showNowPlaying();
}

void handleSettingsPost(){
  powerSaveEnabled = server.hasArg("ps");
  if(server.hasArg("vol")){
    int v = server.arg("vol").toInt();
    setVolumeAndPersist(v, false);
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
    setVolumeAndPersist(v, true);
  }
  server.send(200, "text/plain", "OK");
}

void handleIndex(){ server.send(200,"text/html",pageIndex()); }
void handleVolUp(){ setVolumeAndPersist(currentVolume+1, true); redirectHome(); }
void handleVolDown(){ setVolumeAndPersist(currentVolume-1, true); redirectHome(); }
void handlePlay(){ doPlay(); redirectHome(); }
void handleStop(){ doStop(); redirectHome(); }
void handleNext(){ changeStation(+1); redirectHome(); }
void handlePrev(){ changeStation(-1); redirectHome(); }

void handleWifiPost(){
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim(); pass.trim();
  if(ssid=="") { server.send(200,"text/html",pageWifiForm("SSID required.")); return; }
  int idx = -1;
  for(int i=0;i<wifiCount;i++) if(wifiList[i].ssid == ssid){ idx = i; break; }
  if(idx >= 0) wifiList[idx].pass = pass;
  else {
    if(wifiCount >= MAX_WIFI){ server.send(200,"text/html",pageWifiForm("Wi-Fi list full.")); return; }
    wifiList[wifiCount++] = { ssid, pass };
  }
  saveWiFiList();
  bool ok = connectWiFiSmart(7000, 3);
  String note = "Saved.";
  if(ok && WiFi.status()==WL_CONNECTED) note += " Connected to " + WiFi.SSID() + ".";
  else { note += " Could not connect now. AP mode restored."; startAP(); }
  logRuntimeSummary();
  server.send(200,"text/html",pageWifiForm(note));
}

void handleWifiGet(){ server.send(200,"text/html",pageWifiForm("")); }
void handleWifiDel(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i = server.arg("i").toInt();
  if(i<0 || i>=wifiCount){ redirectHome(); return; }
  for(int k=i;k<wifiCount-1;k++) wifiList[k]=wifiList[k+1];
  wifiCount--;
  saveWiFiList();
  Serial.println(String("[WiFi] deleted entry index ") + i);
  redirectHome();
}

void handleStationsGet(){ server.send(200,"text/html",pageStations("")); }
void handleStationsPost(){
  String name=server.arg("name");
  String url=server.arg("url");
  name.trim(); url.trim();
  if(name==""||url==""){ server.send(200,"text/html",pageStations("Name/URL required")); return; }
  if(server.hasArg("i")){
    int i=server.arg("i").toInt();
    if(i>=0 && i<stationCount){ stations[i]={name,url}; saveStations(); Serial.println(String("[ST] updated: ") + name); server.send(200,"text/html",pageStations("Updated.")); }
    else server.send(200,"text/html",pageStations("Invalid index."));
  } else {
    if(stationCount<MAX_STATIONS){ stations[stationCount++]={name,url}; saveStations(); Serial.println(String("[ST] added: ") + name); server.send(200,"text/html",pageStations("Added.")); }
    else server.send(200,"text/html",pageStations("List full."));
  }
}

void handleStDel(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0 || i>=stationCount){ redirectHome(); return; }
  Serial.println(String("[ST] deleted index ") + i + " : " + stations[i].name);
  for(int k=i;k<stationCount-1;k++) stations[k]=stations[k+1];
  stationCount--;
  if(currentStation>=stationCount) currentStation=0;
  saveStations();
  saveSettings();
  redirectHome();
}
void handleStUp(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<=0 || i>=stationCount){ redirectHome(); return; }
  Station t=stations[i-1]; stations[i-1]=stations[i]; stations[i]=t; saveStations(); Serial.println(String("[ST] moved up index ") + i); redirectHome();
}
void handleStDown(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0 || i>=stationCount-1){ redirectHome(); return; }
  Station t=stations[i+1]; stations[i+1]=stations[i]; stations[i]=t; saveStations(); Serial.println(String("[ST] moved down index ") + i); redirectHome();
}
void handleStSelect(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0 || i>=stationCount){ redirectHome(); return; }
  selectStation(i, true);
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
  server.on("/api/st_up", HTTP_GET, handleStUp);
  server.on("/api/st_down", HTTP_GET, handleStDown);
  server.on("/api/st_select", HTTP_GET, handleStSelect);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/update", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, [](){ server.send(200, "text/html", pageOtaUpdate("Upload complete. Rebooting...")); delay(1000); ESP.restart(); }, handleOtaUpload);
  server.begin();
  Serial.println("[HTTP] Web server started on port 80");
}

bool connectWiFiSmart(unsigned long timeoutPerNetMs, int maxAttempts) {
  if (wifiCount == 0) {
    Serial.println("[WiFi] No saved networks in /WiFi.json");
    return false;
  }
  Serial.println("[WiFi] connectWiFiSmart()");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);
  Serial.println("[WiFi] Scanning for networks...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[WiFi] No networks found in scan");
    WiFi.scanDelete();
    return false;
  }
  bool knownVisible[MAX_WIFI];
  for (int i = 0; i < MAX_WIFI; i++) knownVisible[i] = false;
  int knownCount = 0;
  for (int s = 0; s < n; s++){
    String ssidScan = WiFi.SSID(s); ssidScan.trim();
    for (int i = 0; i < wifiCount; i++) {
      if (!knownVisible[i] && ssidScan == wifiList[i].ssid) {
        knownVisible[i] = true;
        knownCount++;
        Serial.println(String("[WiFi] Known SSID in range: ") + wifiList[i].ssid);
        break;
      }
    }
  }
  WiFi.scanDelete();
  if (knownCount == 0) {
    Serial.println("[WiFi] No known SSID in range");
    return false;
  }

  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    Serial.println(String("[WiFi] Connect attempt ") + (attempt+1) + "/" + maxAttempts);
    for (int i = 0; i < wifiCount; i++) {
      if (!knownVisible[i]) continue;
      Serial.println(String("[WiFi] Trying saved SSID: ") + wifiList[i].ssid);
      WiFi.disconnect();
      delay(200);
      WiFi.begin(wifiList[i].ssid.c_str(), wifiList[i].pass.c_str());
      unsigned long t0 = millis();
      wl_status_t st = WiFi.status();
      while (st != WL_CONNECTED && (millis() - t0) < timeoutPerNetMs) {
        delay(250);
        st = WiFi.status();
      }
      Serial.print("[WiFi] Status after try: ");
      Serial.println((int)st);
      if (st == WL_CONNECTED) {
        WiFi.setAutoReconnect(true);
        Serial.println(String("[WiFi] Connected to ") + WiFi.SSID());
        Serial.println(String("[WiFi] Local IP: ") + WiFi.localIP().toString());
        return true;
      }
    }
    if (attempt < maxAttempts - 1) {
      Serial.println("[WiFi] No success this round, retrying...");
      delay(1000);
    }
  }
  Serial.println("[WiFi] Giving up after retries");
  return false;
}

void startAP(){
  WiFi.mode(WIFI_AP);
  WiFi.softAP("EmmiRadio-Setup","emmipass");
  Serial.println("[WiFi] AP mode: EmmiRadio-Setup / emmipass");
  Serial.println(String("[WiFi] AP IP: ") + WiFi.softAPIP().toString());
}

void setup(){
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("[BOOT] EmmiRadio starting...");
  Wire.begin(5,6);
  displayInit();
  displayWake();
  safeLcdPrint("EmmiRadio v1.73","Boot...");

  if(!FFat.begin(true)){
    Serial.println("[FS] FFat mount failed");
    safeLcdPrint("FFat error", "Mount failed");
    delay(1500);
  } else {
    Serial.println("[FS] FFat mounted");
  }

  loadSettings();
  initBtn(BTN_PLAY);
  initBtn(BTN_NEXT);
  initBtn(BTN_PREV);
  Serial.println("[GPIO] Buttons initialized");

  audio.setPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DATA);
  applyVolume();
  Serial.println(String("[AUDIO] I2S ready, startup volume = ") + currentVolume);

  if(!loadStations()){
    Serial.println("[ST] No station list found, loading defaults");
    loadDefaultStations();
    saveStations();
  } else {
    Serial.println(String("[ST] Loaded ") + stationCount + " stations");
  }
  if(currentStation >= stationCount) currentStation = 0;

  bool wifiOk = loadWiFiList() && connectWiFiSmart(7000, 5);
  if(!wifiOk) startAP();

  startWebServer();
  showNowPlaying();
  lastActivityMs = millis();
  lcdBacklightOff = false;
  logRuntimeSummary();
}

void loop(){
  audio.loop();
  server.handleClient();

  if (volumeOverlayUntilMs > 0 && millis() > volumeOverlayUntilMs){
    volumeOverlayUntilMs = 0;
    showNowPlaying();
  }

  tickMarquee();

  if (powerSaveEnabled && !lcdBacklightOff && (millis()-lastActivityMs > LCD_SLEEP_MS)){
    displaySleep();
    lcdBacklightOff = true;
    Serial.println("[DISPLAY] Power save active");
  }

  BtnEventType evPlay = pollButton(BTN_PLAY);
  BtnEventType evNext = pollButton(BTN_NEXT);
  BtnEventType evPrev = pollButton(BTN_PREV);

  if (evPlay == BTN_PRESSED || evNext == BTN_PRESSED || evPrev == BTN_PRESSED){
    lastActivityMs = millis();
    if(lcdBacklightOff){ displayWake(); lcdBacklightOff=false; Serial.println("[DISPLAY] Wake"); }
  }

  if (btnHoldRepeat(BTN_PLAY)) {
    setVolumeAndPersist(currentVolume + 1, true);
    lastActivityMs = millis();
  }
  if (btnHoldRepeat(BTN_NEXT)) {
    setVolumeAndPersist(currentVolume - 1, true);
    lastActivityMs = millis();
  }

  if (evPlay == BTN_RELEASED_SHORT) {
    lastActivityMs = millis();
    if(!isPlaying) doPlay(); else doStop();
  }
  if (evNext == BTN_RELEASED_SHORT) {
    lastActivityMs = millis();
    changeStation(+1);
  }
  if (evPrev == BTN_RELEASED_SHORT) {
    lastActivityMs = millis();
    changeStation(-1);
  }

  if(isPlaying && !gotMeta && volumeOverlayUntilMs == 0){
    static unsigned long lastRefresh=0;
    if(millis()-lastRefresh>1000){
      showNowPlaying();
      lastRefresh=millis();
    }
  }

  tryIcyFallback();

  static wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
  wl_status_t nowWiFiStatus = WiFi.status();
  if(nowWiFiStatus != lastWiFiStatus){
    lastWiFiStatus = nowWiFiStatus;
    Serial.println(String("[WiFi] State changed. Status = ") + (int)nowWiFiStatus);
    logRuntimeSummary();
  }
}

void audio_info(const char *info){ Serial.print("[INFO] "); Serial.println(info); }
void audio_showstreamtitle(const char* t){
  String nt=String(t); nt.trim();
  if(nt.length()==0) return;
  if(nt!=lastTitle){
    lastTitle=nt;
    gotMeta=true;
    Serial.print("[META/CALLBACK] "); Serial.println(lastTitle);
    if(volumeOverlayUntilMs == 0) showNowPlaying();
  }
}
void audio_eof_mp3(const char* f){ Serial.println("[AUDIO] EOF"); }
