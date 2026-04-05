# EmmiRadio

ESP32-based WiFi internet radio with web UI, OTA update, FFat storage, and selectable LCD / OLED support.

## Current stable version

**Recommended version:** `EmmiRadio_1.61`

Older versions were experimental steps and are kept only where needed.

---

## Main features

* WiFi internet radio
* Web control interface
* OTA firmware update
* FFat-based persistent storage
* WiFi credential storage
* Editable station list
* LCD 16x2 or SSD1306 OLED support
* AP fallback mode
* Metadata / stream title support
* Physical button control
* Display power save

---
## Arduino IDE setup

* **Install these libraries**
* Under File-Preferences add this line to boards manager field:
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
* Tools / Board / Boards Manager
 look for: esp32
 install:
  esp32 by Espressif Systems
* Library manager - install these:
  ESP32-audioI2S
  LiquidCrystal_I2C
  * For OLED screen:
  Adafruit GFX Library
  Adafruit SSD1306

## Recommended board settings

Use these settings in Arduino IDE:

* **Board:** `ESP32S3 Dev Module`
* **Flash Size:** `16MB (128Mb)`
* **Partition Scheme:** `16M Flash (3MB APP/9.9MB FATFS)`
* **Erase All Flash Before Upload:** `Enabled` for first clean upload
* **PSRAM:** `OPI PSRAM`
* **USB CDC On Boot:** `Enabled`

---

## First boot / setup

If the device cannot connect to a saved WiFi network, it starts in **AP mode**.

### Access Point details

* **SSID:** `EmmiRadio-Setup`
* **Password:** `emmipass`

Connect to that network with your phone or computer, then open:

```text
http://192.168.4.1
```

From there you can:

* add or update WiFi networks
* add or edit radio stations
* control playback
* update firmware
* change settings

---

## Normal operation

If a saved WiFi network is found and connection succeeds:

* the device switches to **STA mode**
* it gets an IP address from your router
* the IP is shown on the display
* the same IP is printed to Serial Monitor

Example:

```text
http://192.168.1.123
```

---

## AP fallback behavior

If no saved WiFi is available, or connection fails:

* EmmiRadio restores **AP mode**
* you can reconnect to:

  * `EmmiRadio-Setup`
  * password: `emmipass`
* then open:

  * `http://192.168.4.1`

---

## Files stored on FFat

The firmware stores its data in FFat:

```text
/WiFi.json
/radios.json
/settings.json
```

### Purpose

* `/WiFi.json` → saved WiFi credentials
* `/radios.json` → saved station list
* `/settings.json` → saved settings such as display power save

---

## Display selection

In `EmmiRadio_1.61.ino`, choose the display type here:

```cpp
#define DISPLAY_TYPE DISPLAY_TYPE_LCD
```

### Available options

```cpp
DISPLAY_TYPE_LCD
DISPLAY_TYPE_OLED
```

### LCD mode

Uses a standard 16x2 I2C LCD.

### OLED mode

Uses SSD1306 OLED.

If using OLED, install:

* Adafruit GFX
* Adafruit SSD1306

If needed, adjust OLED I2C address:

```cpp
#define OLED_ADDR 0x3C
```

or:

```cpp
#define OLED_ADDR 0x3D
```

---

## OTA update

Open the device in a browser and go to:

```text
http://<device-ip>/update
```

Upload the compiled `.bin` file.

### Important

The firmware includes the selected display backend.

That means:

* LCD build → use with LCD
* OLED build → use with OLED

Uploading the wrong build usually will not brick the board, but the display may not work correctly.

---

## Serial output

Version `1.61` provides more useful Serial logging, including:

* firmware version
* selected display type
* filesystem type
* number of saved WiFi entries
* number of stations
* current mode (AP / STA)
* IP address
* RSSI in STA mode
* AP credentials in fallback mode

---

## Hardware notes

### Audio

Uses I2S audio output.

### WiFi

Make sure the WiFi antenna is actually connected.
Yes, this turned out to matter. Quite a lot.

### Network compatibility

ESP32 uses **2.4 GHz WiFi**, not 5 GHz only networks.

---

## Stable version summary

### Keep

* `EmmiRadio_1.2`
* `EmmiRadio_1.61`

### Remove

* `EmmiRadio_1.3`
* `EmmiRadio_1.4`
* `EmmiRadio_1.5`
* `EmmiRadio_1.6`

---

## Final note

If it fails, check:

1. power
2. wiring
3. partition scheme
4. antenna
5. whether the network is actually 2.4 GHz

In that order. Saves time, swearing, and unnecessary existential questions.
