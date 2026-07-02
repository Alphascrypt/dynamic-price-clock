# Bill of Materials (Stückliste)

Alle Teile, die für den Nachbau des Dynamic Price Clock benötigt werden. Mengen und Spezifikationen basieren auf dem, was die Firmware ([dynamic-price-clock.ino](dynamic-price-clock.ino)) erwartet.

## Elektronik

| Teil | Menge | Spezifikation | Hinweis |
|---|---|---|---|
| ESP32-C5 Dev Board | 1 | z.B. "ESP32C5 Dev Module" (WROOM-1) | Hauptcontroller, WLAN |
| Rundes TFT-Display | 2 | GC9A01(A), 1,28", 240×240 px, SPI | Display 1 = Preisverlauf, Display 2 = Preis-Uhr (siehe bekannter Defekt in [README](README.md)) |
| WS2812B/WS2818 LED-Ring | 1 | 60 einzeln adressierbare RGB-LEDs (5V) | Alternativ 24-LED-Ring möglich (per Web-Interface umschaltbar) |
| MAX7219 8×8 LED-Matrix-Modul | 0–4 (optional) | Daisy-Chain-fähig, SPI | Nur nötig, wenn `ENABLE_MAX7219_MATRIX` aktiviert wird; jedes Modul zeigt einen frei wählbaren Wert |
| Stromversorgung | 1 | 5V DC, mind. 3A (USB-C oder Hohlstecker-Netzteil) | LED-Ring bei voller Helligkeit ist der größte Verbraucher (60 × ~60 mA max ≈ 3,6 A) |
| Jumper-/Litzenkabel | ca. 20 | verschiedene Längen | Für SPI-Bus (SCLK/MOSI gemeinsam für Displays + Matrix), Chip-Select-Leitungen, Stromversorgung |
| Pufferkondensator | 1 | ≥ 1000 µF, ≥ 6,3V | Zwischen 5V und GND direkt am LED-Ring-Eingang, glättet Einschaltstrom |
| Vorwiderstand für LED-Datenleitung | 1 | ca. 300–500 Ω | In der Datenleitung zum LED-Ring, schützt den ersten Pixel |

## Gehäuse / Mechanik

| Teil | Menge | Spezifikation | Hinweis |
|---|---|---|---|
| Holzscheibe (Front) | 1 | Rund, Durchmesser passend zum LED-Ring (siehe Foto in [README](README.md)) | Aussparungen für 2 Displays + 60 LED-Bohrungen + Zifferblatt-Gravur |
| Standfuß/Gehäuserückwand | 1 | Individuell, siehe Foto | Enthält Montagepunkte für Elektronik und Kabeldurchführung |
| Montageschrauben | ca. 4–6 | z.B. M3/M4 | Für Displays und Gehäuseteile |
| Diffusor/Abdeckung je LED (optional) | bis zu 60 | z.B. 3mm/5mm LED-Diffusorkappen oder Acrylscheibe | Für gleichmäßigeres Licht, in den Fotos nicht sichtbar verwendet |

## Werkzeug/Sonstiges (nicht Teil der Stückliste, aber nötig)

- Lötkolben + Lötzinn (oder Steckverbinder/JST für lötfreien Aufbau)
- USB-Kabel zum Flashen des ESP32-C5
- PC mit Arduino IDE (siehe [README: Einrichtung](README.md#einrichtung-arduino-ide))

## Pin-Zuordnung

Die genaue GPIO-Belegung ist im Web-Dashboard unter **Pinout** einsehbar und dort per Dropdown änderbar (SCLK, MOSI, LED-Ring-Pin, Matrix-CS). Fixe, nicht per Web-Interface änderbare Pins: TFT DC, TFT RST, TFT1 CS, TFT2 CS (siehe Pinout-Seite bzw. [docs/screenshots/pinout-light.png](docs/screenshots/pinout-light.png)).
