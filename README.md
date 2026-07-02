# Dynamic Price Clock

ESP32-C5-Firmware für ein Tibber-Strompreis-Display mit zwei runden GC9A01-Bildschirmen, WS2812B-LED-Tagesring und optionaler MAX7219-Matrix – inkl. Web-Dashboard mit Layout-Editor, Hell/Dunkel-Modus und GitHub-OTA-Updates.

## Einrichtung (Arduino IDE)

**Board:** ESP32C5 Dev Module (Board-Paket `esp32:esp32`, Boards-Manager-URL `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)

**Werkzeuge → Partition Scheme:** **"Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"**
Das Standardschema ("Default 4MB with spiffs") reicht nicht aus – der Sketch belegt mit aktiviertem GitHub-OTA-Update ca. 1,5 MB Flash und braucht ein Schema mit größerer, aber weiterhin OTA-fähiger App-Partition. Mit dem Standardschema schlägt der Build mit "Sketch is too large" fehl.

**Benötigte Bibliotheken** (über den Bibliotheksverwalter installierbar):
- ArduinoJson
- Adafruit GFX Library
- Adafruit BusIO
- Adafruit GC9A01A
- Adafruit NeoPixel

Nach dem Flashen läuft das Gerät beim ersten Start als WLAN-Access-Point ("Tibber-Display-Setup") zur Erstkonfiguration.

## Lizenz

Siehe [LICENSE](LICENSE) (GNU Affero General Public License v3.0).
