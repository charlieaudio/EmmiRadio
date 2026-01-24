# EmmiRadio
A simple internet radio with the cheapest solution

   -------------------------------------------------------------------------
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
     /radios.json   – station list (one JSON per line)
     /WiFi.json     – Wi-Fi credentials

   Web Interfaces:
     http://<device-ip>/           → Main page + controls
     http://<device-ip>/stations   → Station editor (add / edit / delete / reorder)
     http://<device-ip>/wifi       → Wi-Fi settings
     http://<device-ip>/update     → Firmware Update (Web OTA)
   EmmiRadio v1.2 – E.M.M.I. = Extremely Minimal Music Interface
   XIAO ESP32S3 Plus + PCM5102 + 16x2 I2C LCD + Web UI + OTA + WiFi list
   ------------------------------------------------------------------------- */
