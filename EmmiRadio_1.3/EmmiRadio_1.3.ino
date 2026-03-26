/* -------------------------------------------------------------------------
   SOFTWARE SETUP (for EmmiRadio – XIAO ESP32S3 Plus)
   -------------------------------------------------------------------------
   1) Install ESP32 board support
      - Open: File → Preferences → "Additional Boards Manager URLs"
      - Add this URL:
        https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

      - Open: Tools → Board → Boards Manager
      - Install: "ESP32 by Espressif Systems"  (recommended: v3.3.4 or newer)

   2) Select the correct board in Arduino IDE
      Tools → Board → esp32 → "XIAO_ESP32S3_PLUS"
      (If missing, select: "ESP32S3 Dev Module")

      Recommended settings:
        - USB CDC On Boot:         Enabled
        - CPU Frequency:           240 MHz
        - Flash Size:              16MB (128Mb)
        - Flash Mode:              QIO 80MHz
        - PSRAM:                   Enabled (OPI)
        - Partition Scheme:        Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)
        - Upload Speed:            921600 (or 460800 if unstable)
        - Programmer:              esptool (default)

   3) Required Arduino Libraries (install from Library Manager)
      - ESP32-audioI2S   by Phil Schatzmann  (Audio.h)
      - LiquidCrystal_I2C (any compatible version, e.g. by Frank de Brabander)

      NOTE:
      WiFi.h, WebServer.h, Wire.h, SPIFFS.h and Update.h
      come automatically with the ESP32 board package.

   4) Recommended Tools
      - Use Arduino IDE 2.x (for stable ESP32S3 USB/JTAG support)
      - Enable verbose upload logs if debugging USB issues

   After installing everything:
      - Connect the XIAO ESP32S3 Plus via USB-C
      - Select the correct COM port
      - Upload this sketch

   -------------------------------------------------------------------------
   HARDWARE SETUP (EmmiRadio – XIAO ESP32S3 Plus + PCM5102 + 16x2 I2C LCD)
   -------------------------------------------------------------------------

   Board:
     - Seeed Studio XIAO ESP32S3 Plus (the non-Plus also works)
       (8MB PSRAM, 16MB Flash, USB-C)

   Audio DAC (I2S):
     - Module: PCM5102 or PCM510x compatible
     - Connect as follows:
         XIAO D2  (GPIO 3)  →  PCM5102 BCK
         XIAO D3  (GPIO 4)  →  PCM5102 LRCK / WS
         XIAO D1  (GPIO 2)  →  PCM5102 DIN
     - Power:
         PCM5102 VCC → 5V
         PCM5102 GND → GND

   LCD Display (I2C 16×2, addr 0x27 or 0x3F):
     - Connect:
         XIAO SDA (GPIO 5) → LCD SDA
         XIAO SCL (GPIO 6) → LCD SCL
         LCD VCC → 5V (or 3.3V depending on module)
         LCD GND → GND

   Buttons (momentary, active LOW):
     - Use simple push-buttons to GND.
       Internal pull-ups are enabled in software.
         XIAO D0  (GPIO 1)   →  PLAY / STOP
         XIAO D6  (GPIO 43)  →  NEXT
         XIAO D7  (GPIO 44)  →  PREV

   Optional Volume Switch:
     - XIAO GPIO 35 (analog-capable pin)
       The sketch supports volume control via Web UI,
       but you may connect a hardware switch for fixed gain levels.

   Power:
     - USB-C from computer or a 5V USB power supply.
     - Ensure all modules (LCD, PCM5102, buttons) share a common GND.

   Important Notes:
     - PCM5102 works best at 44.1kHz / 48kHz I2S streams.
     - Use proper wiring lengths; I2S is sensitive to noise.
     - If LCD shows garbled text, double-check the I2C address.
     - The radio will fall back to AP mode if Wi-Fi is not configured.

   -------------------------------------------------------------------------
   FEATURE OVERVIEW (EmmiRadio Firmware)
   -------------------------------------------------------------------------

   ✓ High-quality Web Radio Player using PCM5102 I2S DAC
   ✓ Clean Web UI with Play/Stop/Prev/Next controls
   ✓ Editable station list (JSON lines stored in SPIFFS)
   ✓ Wi-Fi configuration page (SSID + Password stored in /WiFi.json)
   ✓ Station metadata display (ICY StreamTitle via callbacks + fallback HTTP polling)
   ✓ 16×2 I2C LCD interface with marquee scrolling on long titles
   ✓ 30-second LCD backlight power-save (any button wakes it)
   ✓ Button controls with proper software debounce
   ✓ Automatic AP fallback mode if Wi-Fi fails
   ✓ Volume control via Web UI (+/– buttons)
   ✓ Web-based OTA firmware update at /update

   Files stored on the device:
     /radios.json    – station list (one JSON per line)
     /WiFi.json      – Wi-Fi credentials
     /settings.json  – UI/device settings

   Web Interfaces:
     http://<device-ip>/           → Main page + controls
     http://<device-ip>/stations   → Station editor (add / edit / delete / reorder)
     http://<device-ip>/wifi       → Wi-Fi settings
     http://<device-ip>/update     → Firmware Update (Web OTA)

   ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
   EmmiRadio v1.3 – E.M.M.I. = Extremely Minimal Music Interface
   XIAO ESP32S3 Plus + PCM5102 + 16x2 I2C LCD + Web UI + OTA + WiFi list
   ------------------------------------------------------------------------- */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <Audio.h>
#include <Update.h>
#include <Wire.h>
#include <SPIFFS.h>
#include <LiquidCrystal_I2C.h>

const char* FW_VERSION = "1.3";

// Forward declarations
void handleSettingsPost();
void safeLcdPrint(const String& l1, const String& l2);
void lcdPrintLine(uint8_t row, const String& s16);
void startMarquee(const String& full);
void tickMarquee();
void showNowPlaying();
String htmlHeader(const String& title);
String htmlFooter();
String pageIndex();
String pageStations(const String& note="");
String pageWifiForm(const String& note="");
String pageOtaUpdate(const String& note="");
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

// LCD
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
bool lcdBacklightOff = false;
unsigned long lastActivityMs = 0;
const unsigned long LCD_SLEEP_MS = 30000;
bool powerSaveEnabled = true;   // LCD power save on-off

// Buttons
const int PIN_BTN_NEXT = 43;
const int PIN_BTN_PREV = 44;
const int PIN_BTN_PLAY = 1;

// I2S pins
const int PIN_I2S_BCK  = 3;
const int PIN_I2S_LRCK = 4;
const int PIN_I2S_DATA = 2;

Audio audio;

// Stations
struct Station { String name; String url; };
const int MAX_STATIONS = 32;
Station stations[MAX_STATIONS];
int stationCount = 0;
int currentStation = 0;
const char* RADIOS_PATH = "/radios.json";

// WiFi list
struct WifiEntry { String ssid; String pass; };
const int MAX_WIFI = 8;
WifiEntry wifiList[MAX_WIFI];
int wifiCount = 0;
const char* WIFI_CFG_PATH = "/WiFi.json";
const char* SETTINGS_PATH = "/settings.json";

WebServer server(80);

// Playback/meta
bool isPlaying = false;
String lastTitle = "";
bool gotMeta = false;
unsigned long playStartMs = 0;
const unsigned long META_TIMEOUT_MS = 5000;

// Debounce
struct DebBtn { int pin; bool stable; bool lastStable; unsigned long lastChangeMs; };
const unsigned long DEBOUNCE_MS = 30;
inline bool rawPressed(int pin){ return digitalRead(pin)==LOW; }
DebBtn BTN_NEXT{PIN_BTN_NEXT, true, true, 0};
DebBtn BTN_PREV{PIN_BTN_PREV, true, true, 0};
DebBtn BTN_PLAY{PIN_BTN_PLAY, true, true, 0};

void initBtn(DebBtn& b){
  pinMode(b.pin, INPUT_PULLUP);
  bool r = rawPressed(b.pin);
  b.stable = b.lastStable = r;
  b.lastChangeMs = millis();
}

bool btnPressedEvent(DebBtn& b){
  unsigned long now = millis();
  bool raw = rawPressed(b.pin);
  if (raw != b.stable && (now - b.lastChangeMs) >= DEBOUNCE_MS){
    b.lastStable = b.stable;
    b.stable = raw;
    b.lastChangeMs = now;
    if (b.lastStable == true && b.stable == false) return true; // press
  }
  return false;
}

// Marquee
bool marqueeActive = false;
String marqueeText = "";
int marqueePos = 0;
unsigned long lastMarqueeMs = 0;
const unsigned long MARQUEE_PERIOD_MS = 300;

// ICY polling
unsigned long lastIcyProbeMs = 0;
unsigned long ICY_PROBE_PERIOD_MS = 8000;

// Volume
int currentVolume = 12;
const int VOL_MIN = 0;
const int VOL_MAX = 21;

// ---------- LCD helpers ----------
void lcdPrintLine(uint8_t row, const String& s16){
  String t = s16;
  if (t.length() < 16) { while (t.length() < 16) t += ' '; }
  else if (t.length() > 16) t.remove(16);
  lcd.setCursor(0, row);
  lcd.print(t);
}

void safeLcdPrint(const String& l1, const String& l2){
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1.substring(0,16));
  lcd.setCursor(0,1); lcd.print(l2.substring(0,16));
  lastActivityMs = millis();
}

void showIpOnLcd(){
  IPAddress ip = (WiFi.getMode()==WIFI_MODE_AP || WiFi.status()!=WL_CONNECTED)
                 ? WiFi.softAPIP() : WiFi.localIP();
  safeLcdPrint("EmmiRadio v"+String(FW_VERSION), ip.toString());
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
  for (int i = 0; i < 16; i++){
    w += marqueeText[(marqueePos + i) % L];
  }
  lcdPrintLine(1, w);
  marqueePos = (marqueePos + 1) % L;
}

// ---------- Now playing ----------
void showNowPlaying(){
  String line1 = (stationCount>0) ? stations[currentStation].name : "(no stations)";
  String line2;

  if (gotMeta && lastTitle.length()){
    lcdPrintLine(0, line1);
    startMarquee(lastTitle);
    return;
  }

  marqueeActive = false;
  lcdPrintLine(0, line1);
  if (isPlaying && (millis() - playStartMs) > META_TIMEOUT_MS) line2 = "PLAY";
  else line2 = "Connecting...";
  lcdPrintLine(1, line2);
}

// ---------- URL shortener ----------
String shortUrl(const String& url){
  const int LEFT=15, RIGHT=10;
  int len = url.length();
  if(len <= LEFT + RIGHT + 3) return url;
  return url.substring(0,LEFT) + "..." + url.substring(len-RIGHT);
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
  return String("{\"ps\":\"") + (powerSaveEnabled ? "1" : "0") + "\"}";
}

// ---------- Settings ----------
bool loadSettings(){
  if(!SPIFFS.exists(SETTINGS_PATH)) return false;
  File f = SPIFFS.open(SETTINGS_PATH, "r"); if(!f) return false;
  String l = f.readStringUntil('\n');
  f.close();
  l.trim();
  if(l.length() < 5) return false;

  String ps = jsonGet(l, "ps");
  ps.trim();
  if(ps == "0") powerSaveEnabled = false;
  else if(ps == "1") powerSaveEnabled = true;
  else return false;

  return true;
}

bool saveSettings(){
  File f = SPIFFS.open(SETTINGS_PATH, "w"); if(!f) return false;
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
}

bool loadStations(){
  stationCount=0;
  if(!SPIFFS.exists(RADIOS_PATH)) return false;
  File f=SPIFFS.open(RADIOS_PATH,"r"); if(!f) return false;
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
  File f=SPIFFS.open(RADIOS_PATH,"w"); if(!f) return false;
  for(int i=0;i<stationCount;i++)
    f.println(jsonLineStation(stations[i].name, stations[i].url));
  f.close();
  return true;
}

// ---------- WiFi list ----------
bool loadWiFiList(){
  wifiCount=0;
  if(!SPIFFS.exists(WIFI_CFG_PATH)) return false;
  File f=SPIFFS.open(WIFI_CFG_PATH,"r"); if(!f) return false;
  while(f.available() && wifiCount<MAX_WIFI){
    String l=f.readStringUntil('\n'); l.trim();
    if(l.length()<5) continue;
    String ss=jsonGet(l,"ssid");
    String pw=jsonGet(l,"pass");
    ss.trim(); pw.trim();
    if(ss.length()>0){
      wifiList[wifiCount++] = { ss, pw };
    }
  }
  f.close();
  return wifiCount>0;
}

bool saveWiFiList(){
  File f=SPIFFS.open(WIFI_CFG_PATH,"w"); if(!f) return false;
  for(int i=0;i<wifiCount;i++)
    f.println(jsonLineWifi(wifiList[i].ssid, wifiList[i].pass));
  f.close();
  return true;
}

void handleSettingsPost(){
  // Ha érkezik 'ps' paraméter, akkor be van pipálva
  powerSaveEnabled = server.hasArg("ps");
  saveSettings();

  if(!powerSaveEnabled && lcdBacklightOff){
    lcd.backlight();
    lcdBacklightOff = false;
  }
  lastActivityMs = millis();

  redirectHome();
}

// ---------- HTML helpers ----------
String htmlHeader(const String& title){
  return "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<meta charset='utf-8'><title>"+title+"</title>"
         "<style>"
         "body{font-family:sans-serif;margin:20px}"
         ".row{margin:8px 0}.card{padding:14px;border:1px solid #ddd;border-radius:14px;box-shadow:0 1px 4px rgba(0,0,0,.06)}"
         "button{padding:10px 14px;font-size:15px;margin:4px;border-radius:10px;border:1px solid #999}"
         "input{padding:8px;font-size:15px;border-radius:8px;border:1px solid #ccc;width:100%}"
         "table{border-collapse:collapse;width:100%} th,td{border-bottom:1px solid #eee;padding:8px;text-align:left}"
         "a.btn{display:inline-block;padding:8px 12px;border:1px solid #999;border-radius:10px;text-decoration:none;color:#000;margin:4px}"
         "small.version{font-size:12px;color:#666;margin-left:6px}"
         "</style></head><body>";
}

String htmlFooter(){ return "</body></html>"; }

// ---------- OTA page ----------
String pageOtaUpdate(const String& note){
  String h=htmlHeader("Firmware Update");
  h+="<div class='card'><h2>Firmware Update <small class='version'>v"+String(FW_VERSION)+"</small></h2>";
  if(note.length()) h+="<div class='row' style='color:green'>"+note+"</div>";
  h+="<form method='POST' action='/update' enctype='multipart/form-data'>"
     "<div class='row'><input type='file' name='firmware'></div>"
     "<div class='row'><button type='submit'>Upload & Flash</button></div>"
     "</form>"
     "<div class='row'><a class='btn' href='/'>Back</a></div>";
  h+="</div>";
  h+=htmlFooter();
  return h;
}

// ---------- Index page ----------
String pageIndex(){
  String st=isPlaying?"PLAYING":"STOPPED";
  String ip=(WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():WiFi.softAPIP().toString();
  String cur=(stationCount>0)?stations[currentStation].name:"(no stations)";
  String h=htmlHeader("EmmiRadio");
  h+="<div class='card'><h2>EmmiRadio <small class='version'>v"+String(FW_VERSION)+"</small></h2>";
  h+="<div class='row'><b>Status:</b> "+st+"</div>";
  h+="<div class='row'><b>Station:</b> "+cur+"</div>";
  h+="<div class='row'><b>IP:</b> "+ip+"</div>";
  h+="<div class='row'><b>Volume:</b> "+String(currentVolume)+
     " <a class='btn' href='/api/vol_down'>-</a>"
     " <a class='btn' href='/api/vol_up'>+</a></div>";
  h+="<div class='row'><form method='POST' action='/settings'>";
  h+="<label><input type='checkbox' name='ps' value='1' ";
  if (powerSaveEnabled) h+="checked";
  h+="> LCD power save (30s)</label> ";
  h+="<button type='submit'>Apply</button>";
  h+="</form></div>";
  h+="<div class='row'>"
     "<a class='btn' href='/api/prev'>⏮ Prev</a>"
     "<a class='btn' href='/api/play'>▶ Play</a>"
     "<a class='btn' href='/api/stop'>⏹ Stop</a>"
     "<a class='btn' href='/api/next'>⏭ Next</a>"
     "</div>";
  h+="<div class='row'>"
     "<a class='btn' href='/stations'>Stations</a>"
     "<a class='btn' href='/wifi'>Wi-Fi</a>"
     "<a class='btn' href='/update'>OTA Update</a>"
     "</div>";
  h+="</div>";
  h+=htmlFooter();
  return h;
}

// ---------- Stations page ----------
String pageStations(const String& note){
  String h=htmlHeader("Stations");
  h+="<div class='card'><h2>Stations</h2>";
  if(note.length()) h+="<div class='row' style='color:green'>"+note+"</div>";
    h+="<table><tr><th>#</th><th>Name</th><th>Actions</th></tr>";
  for(int i=0;i<stationCount;i++){
    h+="<tr><td>"+String(i)+"</td><td>"+stations[i].name+"</td><td>";
    h+="<a class='btn' href='/stations?edit="+String(i)+"'>Edit</a>";
    h+="<a class='btn' href='/api/st_del?i="+String(i)+"' onclick='return confirm(\"Delete?\")'>Del</a>";
    if(i>0) h+="<a class='btn' href='/api/st_up?i="+String(i)+"'>Up</a>";
    if(i<stationCount-1) h+="<a class='btn' href='/api/st_down?i="+String(i)+"'>Down</a>";
    h+="<a class='btn' href='/api/st_select?i="+String(i)+"'>Select</a>";
    h+="</td></tr>";
  }
  h+="</table>";

  int edit = server.hasArg("edit") ? server.arg("edit").toInt() : -1;
  String fName = (edit>=0 && edit<stationCount)?stations[edit].name:"";
  String fUrl  = (edit>=0 && edit<stationCount)?stations[edit].url :"";

  h+="<hr><h3>"+String(edit>=0?"Edit station":"Add station")+"</h3>";
  h+="<form method='POST' action='/stations'>";
  if(edit>=0) h+="<input type='hidden' name='i' value='"+String(edit)+"'>";
  h+="<div class='row'><label>Name</label><br><input name='name' value='"+fName+"'></div>";
  h+="<div class='row'><label>URL</label><br><input name='url' value='"+fUrl+"'></div>";
  h+="<div class='row'><button type='submit'>"+String(edit>=0?"Save":"Add")+"</button></div>";
  h+="</form>";

  h+="<div class='row'><a class='btn' href='/'>Back</a></div>";
  h+="</div>";
  h+=htmlFooter();
  return h;
}

// ---------- Wi-Fi page ----------
String pageWifiForm(const String& note){
  String ip=(WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():WiFi.softAPIP().toString();
  String currSsid = (WiFi.status()==WL_CONNECTED)?WiFi.SSID():"";

  String h=htmlHeader("Wi-Fi");
  h+="<div class='card'><h2>Wi-Fi networks</h2>";
  if(note.length()) h+="<div class='row' style='color:green'>"+note+"</div>";

  h+="<div class='row'><b>Current IP:</b> "+ip+"</div>";

  h+="<h3>Saved networks</h3>";
  h+="<table><tr><th>#</th><th>SSID</th><th>Status</th><th>Actions</th></tr>";
  for(int i=0;i<wifiCount;i++){
    String status = (currSsid.length() && wifiList[i].ssid == currSsid && WiFi.status()==WL_CONNECTED)
                    ? "Connected" : "";
    h+="<tr><td>"+String(i)+"</td><td>"+wifiList[i].ssid+"</td><td>"+status+"</td><td>";
    h+="<a class='btn' href='/api/wifi_del?i="+String(i)+"' onclick='return confirm(\"Delete?\")'>Del</a>";
    h+="</td></tr>";
  }
  if(wifiCount==0){
    h+="<tr><td colspan='4'>(no saved networks)</td></tr>";
  }
  h+="</table>";

  h+="<hr><h3>Add / update network</h3>";
  h+="<form method='POST' action='/wifi'>";
  h+="<div class='row'><label>SSID</label><br><input name='ssid'></div>";
  h+="<div class='row'><label>Password</label><br><input name='pass'></div>";
  h+="<div class='row'><button type='submit'>Save</button></div>";
  h+="</form>";

  h+="<div class='row'><small>Device will try saved networks in this order on boot.</small></div>";
  h+="<div class='row'><a class='btn' href='/'>Back</a></div>";
  h+="</div>";
  h+=htmlFooter();
  return h;
}

// ---------- Redirect helper ----------
void redirectHome(){
  server.sendHeader("Location","/");
  server.send(303,"text/plain","");
}

void redirectStations(){
  server.sendHeader("Location","/stations");
  server.send(303,"text/plain","");
}

// ---------- ICY HTTP helper ----------
bool parseHttpUrl(const String& url, String& host, uint16_t& port, String& path){
  String u = url;
  if(u.startsWith("http://")) u.remove(0,7);
  else return false;
  int slash = u.indexOf('/');
  String hp = (slash>=0)? u.substring(0,slash) : u;
  path = (slash>=0)? u.substring(slash) : "/";
  int colon = hp.indexOf(':');
  if(colon>=0){
    host = hp.substring(0,colon);
    port = hp.substring(colon+1).toInt();
  } else {
    host = hp;
    port = 80;
  }
  host.trim();
  return host.length()>0;
}

String fetchIcyTitleOnce(const String& url, uint32_t overallTimeoutMs){
  String host, path; uint16_t port;
  if(!parseHttpUrl(url, host, port, path)){
    Serial.println("[ICY] Unsupported URL (need http): " + url);
    return "";
  }
  WiFiClient client;
  client.setTimeout(1500);
  if(!client.connect(host.c_str(), port)){
    Serial.println("[ICY] connect failed");
    return "";
  }

  String req = String("GET ") + path + " HTTP/1.0\r\n"
               "Host: " + host + "\r\n"
               "Icy-MetaData: 1\r\n"
               "User-Agent: EmmiRadio/1.3\r\n"
               "Connection: close\r\n\r\n";
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

  int a=meta.indexOf("StreamTitle='");
  if(a<0) return "";
  a+=13;
  int b=meta.indexOf("';",a);
  if(b<0) return "";
  String title = meta.substring(a,b);
  title.trim();
  return title;
}

void tryIcyFallback(){
  if(!isPlaying || stationCount==0) return;
  if(millis() - lastIcyProbeMs < ICY_PROBE_PERIOD_MS) return;
  lastIcyProbeMs = millis();

  String url = stations[currentStation].url;
  String t = fetchIcyTitleOnce(url, 6000);
  if(t.length()==0) return;

  t.trim();
  if(t != lastTitle){
    lastTitle = t;
    gotMeta = true;
    Serial.print("[META/FALLBACK] "); Serial.println(lastTitle);
    showNowPlaying();
  }
}
// ---------- OTA handlers ----------
void handleOtaUpload(){
  HTTPUpload& upload = server.upload();

  if(upload.status == UPLOAD_FILE_START){
    safeLcdPrint("UPDATING...", "Please wait");
    Serial.println("[OTA] Update start");
    if(!Update.begin()){
      Serial.println("[OTA] Update.begin() failed");
    }
  }
  else if(upload.status == UPLOAD_FILE_WRITE){
    if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
      Serial.println("[OTA] Write failed");
    }
  }
  else if(upload.status == UPLOAD_FILE_END){
    if(Update.end(true)){
      Serial.println("[OTA] Update success");
    } else {
      Serial.println("[OTA] Update error");
    }
  }
}

void handleOtaPage(){
  server.send(200,"text/html",pageOtaUpdate(""));
}

// ---------- Volume / playback helpers ----------
void applyVolume(){
  if(currentVolume < VOL_MIN) currentVolume = VOL_MIN;
  if(currentVolume > VOL_MAX) currentVolume = VOL_MAX;
  audio.setVolume(currentVolume);
}

void doPlay(){
  if(stationCount==0) return;
  if(WiFi.status()!=WL_CONNECTED){
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
  safeLcdPrint(stations[currentStation].name,"STOP");
}

void changeStation(int delta){
  if(stationCount==0) return;
  currentStation = (currentStation + delta + stationCount) % stationCount;
  if(isPlaying){
    audio.stopSong(); delay(50);
    lastTitle=""; gotMeta=false; playStartMs=millis(); marqueeActive=false;
    Serial.println(String("[AUDIO] connect to ") + stations[currentStation].url);
    audio.connecttohost(stations[currentStation].url.c_str());
    showNowPlaying();
  } else {
    safeLcdPrint(stations[currentStation].name,"STOP");
  }
}

// ---------- HTTP handlers ----------
void handleIndex(){ server.send(200,"text/html",pageIndex()); }

void handleVolUp(){ currentVolume++; applyVolume(); redirectHome(); }
void handleVolDown(){ currentVolume--; applyVolume(); redirectHome(); }

void handlePlay(){ doPlay(); redirectHome(); }
void handleStop(){ doStop(); redirectHome(); }
void handleNext(){ changeStation(+1); redirectHome(); }
void handlePrev(){ changeStation(-1); redirectHome(); }

// Wi-Fi POST = add/update entry
void handleWifiPost(){
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim(); pass.trim();

  if(ssid==""){
    server.send(200,"text/html",pageWifiForm("SSID required."));
    return;
  }

  int idx = -1;
  for(int i=0;i<wifiCount;i++){
    if(wifiList[i].ssid == ssid){
      idx = i;
      break;
    }
  }

  if(idx >= 0){
    // update existing
    wifiList[idx].pass = pass;
  } else {
    if(wifiCount >= MAX_WIFI){
      server.send(200,"text/html",pageWifiForm("Wi-Fi list full."));
      return;
    }
    wifiList[wifiCount++] = { ssid, pass };
  }

  saveWiFiList();
  bool ok = connectWiFiSmart(7000, 3);   // itt lehet elég 3, mert ezt a user hívja kézzel

  String note = "Saved.";
  if(ok && WiFi.status()==WL_CONNECTED){
    note += " Connected to " + WiFi.SSID() + ".";
  } else {
    note += " Could not connect now.";
  }

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
  redirectHome();
}

// Stations
void handleStationsGet(){ server.send(200,"text/html",pageStations("")); }

void handleStationsPost(){
  String name=server.arg("name");
  String url=server.arg("url");
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
  if(i<0 || i>=stationCount){ redirectHome(); return; }
  for(int k=i;k<stationCount-1;k++) stations[k]=stations[k+1];
  stationCount--;
  if(currentStation>=stationCount) currentStation=0;
  saveStations();
  redirectHome();
}

void handleStUp(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<=0 || i>=stationCount){ redirectHome(); return; }
  Station t=stations[i-1];
  stations[i-1]=stations[i];
  stations[i]=t;
  saveStations();
  redirectHome();
}

void handleStDown(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0 || i>=stationCount-1){ redirectHome(); return; }
  Station t=stations[i+1];
  stations[i+1]=stations[i];
  stations[i]=t;
  saveStations();
  redirectHome();
}

void handleStSelect(){
  if(!server.hasArg("i")){ redirectHome(); return; }
  int i=server.arg("i").toInt();
  if(i<0 || i>=stationCount){ redirectHome(); return; }
  currentStation = i;
  if(isPlaying){
    audio.stopSong(); delay(50);
    lastTitle=""; gotMeta=false; playStartMs=millis(); marqueeActive=false;
    Serial.println(String("[AUDIO] connect to ") + stations[currentStation].url);
    audio.connecttohost(stations[currentStation].url.c_str());
    showNowPlaying();
  }
  redirectHome();
}

// ---------- Web server startup ----------
void startWebServer(){
  server.on("/", HTTP_GET, handleIndex);

  server.on("/api/play", HTTP_GET, handlePlay);
  server.on("/api/stop",  HTTP_GET, handleStop);
  server.on("/api/next",  HTTP_GET, handleNext);
  server.on("/api/prev",  HTTP_GET, handlePrev);

  server.on("/api/vol_up",   HTTP_GET, handleVolUp);
  server.on("/api/vol_down", HTTP_GET, handleVolDown);

  server.on("/wifi", HTTP_GET,  handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/wifi_del", HTTP_GET, handleWifiDel);

  server.on("/stations", HTTP_GET,  handleStationsGet);
  server.on("/stations", HTTP_POST, handleStationsPost);

  server.on("/api/st_del",    HTTP_GET, handleStDel);
  server.on("/api/st_up",     HTTP_GET, handleStUp);
  server.on("/api/st_down",   HTTP_GET, handleStDown);
  server.on("/api/st_select", HTTP_GET, handleStSelect);
  server.on("/settings", HTTP_POST, handleSettingsPost);

  // OTA
  server.on("/update", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, [](){
    server.send(200, "text/html", pageOtaUpdate("Upload complete. Rebooting..."));
    delay(1000);
    ESP.restart();
  }, handleOtaUpload);

  server.begin();
}

// ---------- Wi-Fi helpers ----------
bool connectWiFiSmart(unsigned long timeoutPerNetMs, int maxAttempts) {
  if (wifiCount == 0) {
    Serial.println("[WiFi] No saved networks in WiFi.json");
    return false;
  }

  Serial.println("[WiFi] connectWiFiSmart()");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  WiFi.disconnect(true, true);    // teljes reset
  delay(200);

  // --- 1) SCAN: megnézzük, mely mentett SSID-k vannak hatótávon belül ---
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
  for (int s = 0; s < n; s++) {
    String ssidScan = WiFi.SSID(s);
    ssidScan.trim();
    for (int i = 0; i < wifiCount; i++) {
      if (!knownVisible[i] && ssidScan == wifiList[i].ssid) {
        knownVisible[i] = true;
        knownCount++;
        Serial.println("[WiFi] Known SSID in range: " + wifiList[i].ssid);
        break;
      }
    }
  }
  WiFi.scanDelete();

  if (knownCount == 0) {
    Serial.println("[WiFi] No known SSID in range");
    return false;
  }

  // --- 2) Többszöri próbálkozás CSAK az elérhető, ismert SSID-kre ---
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    Serial.println(String("[WiFi] Connect attempt ") + (attempt+1) + "/" + maxAttempts);

    for (int i = 0; i < wifiCount; i++) {
      if (!knownVisible[i]) continue;   // ezt az SSID-t most nem látjuk

      Serial.println(String("[WiFi] Trying saved SSID: ") + wifiList[i].ssid);

      WiFi.disconnect();    // előző próbálkozás biztosan leálljon
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
        Serial.print("[WiFi] Connected to ");
        Serial.println(WiFi.SSID());
        WiFi.setAutoReconnect(true);
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
}

// ---------- Setup / Loop ----------
void setup(){
  Serial.begin(115200);
  delay(200);
  Wire.begin(5,6);
  lcd.init();
  lcd.backlight();
  safeLcdPrint("EmmiRadio v1.3","Boot...");

  SPIFFS.begin(true);
  loadSettings();

  initBtn(BTN_PLAY);
  initBtn(BTN_NEXT);
  initBtn(BTN_PREV);

  audio.setPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DATA);
  applyVolume();

  if(!loadStations()){
    loadDefaultStations();
    saveStations();
  }

  bool wifiOk = loadWiFiList() && connectWiFiSmart(7000, 5);   // 5 tries
  if(!wifiOk) startAP();

  startWebServer();
  showIpOnLcd();

  lastActivityMs = millis();
  lcdBacklightOff = false;
  }

void loop(){
  audio.loop();
  server.handleClient();
  tickMarquee();

  // LCD power save
  if (powerSaveEnabled && !lcdBacklightOff && (millis()-lastActivityMs > LCD_SLEEP_MS)){
  lcd.noBacklight();
  lcdBacklightOff = true;
  }


  // Button handling
  if (btnPressedEvent(BTN_PLAY)){
    lastActivityMs = millis();
    if(lcdBacklightOff){ lcd.backlight(); lcdBacklightOff=false; }
    if(!isPlaying) doPlay();
    else doStop();
  }

  if (btnPressedEvent(BTN_NEXT)){
    lastActivityMs = millis();
    if(lcdBacklightOff){ lcd.backlight(); lcdBacklightOff=false; }
    changeStation(+1);
  }

  if (btnPressedEvent(BTN_PREV)){
    lastActivityMs = millis();
    if(lcdBacklightOff){ lcd.backlight(); lcdBacklightOff=false; }
    changeStation(-1);
  }

  // Keep LCD alive while waiting for metadata
  if(isPlaying && !gotMeta){
    static unsigned long lastRefresh=0;
    if(millis()-lastRefresh>1000){
      showNowPlaying();
      lastRefresh=millis();
    }
  }

  // ICY fallback polling
  tryIcyFallback();

  // Re-print IP when WiFi state changes
  static bool wasConn = (WiFi.status()==WL_CONNECTED);
  bool nowConn = (WiFi.status()==WL_CONNECTED);
  if(nowConn!=wasConn){
    wasConn=nowConn;
    showIpOnLcd();
  }
}

// ---------- Audio callbacks ----------
void audio_info(const char *info){
  Serial.print("[INFO] ");
  Serial.println(info);
}

void audio_showstreamtitle(const char* t){
  String nt=String(t); nt.trim();
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
