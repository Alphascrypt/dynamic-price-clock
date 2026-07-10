#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Adafruit_NeoPixel.h>
// ArduinoWebsockets statt Links2004/WebSockets: Live gegen das echte Geraet
// getestet - die vorherige Bibliothek stellte die Verbindung zu Tibbers
// liveMeasurement-Subscription her und authentifizierte sich erfolgreich,
// aber das anschliessende "subscribe"-Kommando erhielt NIE eine Antwort
// (kein Fehler, kein Ping, keine Daten - komplette Stille), egal ob direkt
// aus dem Event-Callback oder verzoegert danach gesendet. Andere
// Implementierung (eigener TLS-/Framing-Code) als gezielter Versuch, ob das
// Problem an dieser spezifischen Bibliothek liegt.
#include <ArduinoWebsockets.h>
using namespace websockets;

// Fuer die (inoffizielle) Anker-Solix-Cloud-Anmeldung: ECDH(SECP256R1) +
// AES-256-CBC + MD5, siehe updateAnkerSolarData().
#include <mbedtls/ecdh.h>
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/base64.h>

// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240

#define DISPLAY_BLACK GC9A01A_BLACK
#define DISPLAY_WHITE GC9A01A_WHITE
#define DISPLAY_RED   GC9A01A_RED
#define DISPLAY_GREEN GC9A01A_GREEN
#define DISPLAY_BLUE  GC9A01A_BLUE
#define DISPLAY_CYAN  GC9A01A_CYAN
#define DISPLAY_YELLOW GC9A01A_YELLOW

// -----------------------------------------------------------------------------
// ESP32-C5-WROOM-1 Pinout / Verdrahtung
// -----------------------------------------------------------------------------
// Dieser Sketch ist NUR fuer ESP32-C5-WROOM-1 / ESP32-C5-DevKitC-1 ausgelegt.
// Andere ESP32-Varianten werden absichtlich nicht mehr unterstuetzt.

#if !defined(CONFIG_IDF_TARGET_ESP32C5)
  #error "Dieser Sketch ist nur fuer ESP32-C5-WROOM-1 / ESP32-C5-DevKitC-1 vorgesehen. Bitte in der Arduino IDE ein ESP32-C5 Board auswaehlen."
#endif

#define HARDWARE_PROFILE "ESP32-C5-WROOM-1"

// -----------------------------------------------------------------------------
// Safe-Boot / Diagnose
// -----------------------------------------------------------------------------
#define SAFE_BOOT_MODE 0

#if SAFE_BOOT_MODE
  #define ENABLE_TFT_DISPLAYS 0
  #define ENABLE_MAX7219_MATRIX 0
  #define ENABLE_WS2812_RING 1
  #define ENABLE_WIFI_STARTUP 0
#else
  #define ENABLE_TFT_DISPLAYS 1
  #define ENABLE_MAX7219_MATRIX 0
  #define ENABLE_WS2812_RING 1
  #define ENABLE_WIFI_STARTUP 1
#endif

#define MODULE_START_DELAY_MS 1500UL

// Wie lange beim Boot die automatisch erzeugten Zugangsdaten (Admin-Login auf
// Display 1, Setup-WLAN auf Display 2) angezeigt werden, bevor der normale
// Betrieb (Layout/WLAN-Verbindung) weiterlaeuft.
#define BOOT_CREDENTIALS_HOLD_MS 20000UL

// Version fuer /style.css und /app.js. Bei Aenderungen an CSS/JS erhoehen,
// damit Browser-Caches (siehe Cache-Control dort) sofort ungueltig werden.
#define ASSET_VERSION "10"

// Aktuelle Firmware-Version. Vor jedem GitHub-Release von Hand erhoehen -
// der Update-Check vergleicht dies gegen den neuesten Release-Tag.
#define FIRMWARE_VERSION "4.5.2"

// TFT_SCLK_PIN, TFT_MOSI_PIN, LED_RING_PIN und MATRIX_CS_PIN sind ueber
// Preferences (NVS) veraenderbar und werden in setup() geladen, bevor sie
// verwendet werden - siehe /pinout Seite. Die uebrigen 4 Pins (DC/RST/CS1/CS2)
// stecken in den Display-Objekten, die vor setup() angelegt werden, und
// bleiben deshalb feste Compile-Zeit-Konstanten.
int tftSclkPin = 4;
int tftMosiPin = 5;
#define TFT_SCLK_PIN tftSclkPin
#define TFT_MOSI_PIN tftMosiPin
#define TFT_DC_PIN   8
#define TFT_RST_PIN  9
#define TFT1_CS_PIN  10
#define TFT2_CS_PIN  23
#define TFT_BL_PIN   -1

int ledRingPinVar = 24;
#define LED_RING_PIN ledRingPinVar

#define MATRIX_DIN_PIN TFT_MOSI_PIN
#define MATRIX_CLK_PIN TFT_SCLK_PIN
int matrixCsPinVar = 21;
#define MATRIX_CS_PIN  matrixCsPinVar
#define MATRIX_DEVICE_COUNT 4

#define DISPLAY2_ENABLED 1
#define TFT_SPI_FREQUENCY 10000000UL

#define MAX_QUARTERS 192
#define MAX_HOMES 8
#define LAYOUT_ITEMS 8

#define LED_RING_COUNT 60

#define STATUS_RGB_LED_PIN 27
#define STATUS_RGB_LED_COUNT 1
#define STATUS_RGB_LED_ENABLED 0

Adafruit_GC9A01A displayCurrent(TFT1_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
Adafruit_GC9A01A displayBest(TFT2_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

bool displayCurrentOk = false;
bool displayBestOk = false;

Adafruit_NeoPixel ledRing(LED_RING_COUNT, LED_RING_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel statusRgbLed(STATUS_RGB_LED_COUNT, STATUS_RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
uint8_t matrixRows[MATRIX_DEVICE_COUNT][8] = {
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, 0, 0}
};

// -----------------------------------------------------------------------------
// WLAN-Konfiguration
// -----------------------------------------------------------------------------

// Setup-Access-Point
const char* AP_SSID = "Dynamic-Price-Clock-Setup";
String apPassword = "";

// Kein Default-WLAN im Code. Erstkonfiguration laeuft ueber den
// Setup-Access-Point (siehe ensureWifiConnectedRobust()).
const char* DEFAULT_WIFI_SSID = "";
const char* DEFAULT_WIFI_PASSWORD = "";

String wifiSsid = "";
String wifiPassword = "";
bool apMode = false;

const char* DEFAULT_WIFI_SETUP_AP_SSID = "Dynamic-Price-Clock-Setup";
String setupApSsid = "Dynamic-Price-Clock-Setup";
const unsigned long WIFI_SETUP_AP_TIMEOUT = 300000UL;
bool setupApActive = false;
bool setupApPermanent = false;
unsigned long setupApStartedAt = 0;
unsigned long lastWifiStatusDisplay = 0;
bool pendingWifiReconnect = false;
unsigned long pendingWifiReconnectAt = 0;
unsigned long lastAutoReconnectTry = 0;
const unsigned long AUTO_RECONNECT_INTERVAL = 60000UL;

// Nicht-blockierender WLAN-Verbindungsaufbau (manuell ausgeloest ueber
// Speichern/Verbinden im Webinterface oder beim Boot). Statt einer
// while(){delay()}-Schleife wird der Status bei jedem loop()-Durchlauf
// einmal kurz geprueft, damit der Webserver waehrend des Verbindens
// weiter erreichbar bleibt.
bool wifiConnectActive = false;
String wifiConnectSsid = "";
String wifiConnectPasswordAttempt = "";
unsigned long wifiConnectStartedAt = 0;
unsigned long wifiConnectTimeoutMs = 45000UL;
wl_status_t wifiConnectLastStatus = WL_IDLE_STATUS;
unsigned long wifiConnectLastStatusPrintAt = 0;

// Sanfter Reconnect (kurze Verbindungsaussetzer im laufenden Betrieb),
// ebenfalls nicht-blockierend nach demselben Prinzip.
bool wifiSoftReconnectActive = false;
unsigned long wifiSoftReconnectStartedAt = 0;

bool wifiMacRotationEnabled = false;

int wifiBandPreference = 0;
bool wifiStaticIpEnabled = false;
bool wifiAutoFifthLastIpEnabled = false;
String wifiStaticIpText = "";
String wifiGatewayText = "";
String wifiSubnetText = "255.255.255.0";
String wifiDns1Text = "";
String wifiDns2Text = "";

// -----------------------------------------------------------------------------
// Web / Speicher / Sicherheit
// -----------------------------------------------------------------------------

WebServer server(80);
Preferences prefs;

const char* TIBBER_URL = "https://api.tibber.com/v1-beta/gql";
const char* TIBBER_HOST = "api.tibber.com";

const char* TZ_INFO = "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:00";


// TLS-Zertifikatspruefung fuer die Tibber-API.
// Es wird bewusst KEIN Root-CA-Zertifikat hier im Quelltext fest verdrahtet:
// ein von Hand abgetipptes/aus dem Gedaechtnis rekonstruiertes PEM waere ein
// Sicherheitsrisiko (entweder bricht TLS komplett, oder es taeuscht eine
// Pruefung vor, die nie tatsaechlich verifiziert wurde).
//
// Stattdessen: Der Nutzer hinterlegt das echte Root-CA-Zertifikat von
// api.tibber.com einmalig ueber die Weboberflaeche (/account, Feld
// "Tibber Root-CA"). Solange keins hinterlegt ist, faellt das Geraet auf
// WiFiClientSecure::setInsecure() zurueck UND zeigt das im Webinterface
// und im Seriellen Monitor deutlich als "TLS ungeprueft" an, statt das
// stillschweigend zu verstecken.
//
// Zertifikat ermitteln (auf einem PC, nicht auf dem ESP32):
//   openssl s_client -connect api.tibber.com:443 -showcerts </dev/null \
//     | openssl x509 -outform PEM
// Die Root-CA-PEM (letztes Zertifikat der Kette) hier im Webinterface einfuegen.
String tibberRootCaPem = "";

// GitHub-Update: "besitzer/repo" (z.B. "Alphascrypt/dynamic-price-clock"), optionales
// Token (fuer private Repos oder hoehere Rate-Limits) und optionales
// Root-CA-Zertifikat fuer api.github.com. Ohne Zertifikat faellt die
// Verbindung bewusst-protokolliert auf setInsecure() zurueck, exakt wie bei
// der Tibber-API.
String githubRepo = "Alphascrypt/dynamic-price-clock";
String githubToken = "";

// -----------------------------------------------------------------------------
// OTA-Zustandsautomat
// -----------------------------------------------------------------------------
// Ersetzt eine fruehere Ansammlung einzelner, lose gekoppelter Variablen
// (githubLatestVersion/githubLatestUrl/versionCheckLatest/versionCheckError/
// otaBytesWritten/otaBytesTotal/otaTaskRunning/otaHeartbeatMs/otaTaskDone/
// otaTaskSuccess/otaTaskError/otaLastDiag/otaManualUploadActive) durch ein
// einziges Struct mit explizitem Eigentuemer-Konzept. Jeder bisherige Bug in
// diesem Bereich war eine neue Interaktion zwischen genau diesen Flags (z.B.
// ein abgebrochener, von vornherein abgewiesener manueller Upload, der den
// Fehlertext eines fremden, tatsaechlich laufenden GitHub-Updates ueberschrieben
// hat) - ein Zustandsmodell mit den vier ownerpruefenden Mutatoren weiter unten
// (otaTryAcquire/otaProgress/otaSetPhase/otaFail/otaFinishSuccess) verhindert
// falsche Kombinationen strukturell, statt sich auf Kommentar-Disziplin an
// jeder einzelnen Aufrufstelle zu verlassen.
enum class OtaPhase : uint8_t {
  Idle, Checking, CheckFailed, UpdateAvailable, UpToDate,
  Downloading, // GitHub-Pfad: manueller Download-Loop schreibt Bytes direkt in die Flash-Partition
  Uploading,   // manueller Pfad: Multipart-Body kommt an
  Flashing,    // manueller Pfad: alle Bytes da, Update.end() steht noch aus
  Rebooting,
  Failed
};
enum class OtaOwner : uint8_t { None, Github, ManualUpload };

struct OtaState {
  volatile OtaPhase phase = OtaPhase::Idle;
  volatile OtaOwner owner = OtaOwner::None;

  // Ergebnis des letzten Versions-Checks (GitHub-API) - von otaCheckLatest()
  // befuellt. "updateAvailable" wird bewusst NICHT hier gespeichert, sondern
  // wie im Original von den Handlern lokal aus latestVersion vs.
  // FIRMWARE_VERSION berechnet - vermeidet eine zusaetzliche Quelle, die mit
  // latestVersion synchron gehalten werden muesste.
  String latestVersion = "";
  String latestUrl = "";
  String checkError = "";

  // Fortschritt/Ergebnis des laufenden oder letzten Updates (GitHub-Download
  // oder manueller Upload, je nach owner).
  volatile int bytesWritten = 0;
  volatile int bytesTotal = 0;
  String diag = ""; // WLAN-Signal/Heap/DNS-Status des letzten Verbindungsversuchs, siehe otaPreflightDiag()
  String error = "";
  volatile bool success = false;
  volatile unsigned long heartbeatMs = 0; // millis() der letzten Task-Aktivitaet, fuer den loop()-Haenger-Detektor
  volatile unsigned long startedAtMs = 0;
};
OtaState ota;

// Uebergabe-Variable HTTP-Handler -> FreeRTOS-Task: xTaskCreate kann keine
// Strings direkt per Wert uebergeben, deshalb dieser einfache, unzweideutige
// Hand-off (kein Teil des Eigentuemer-Modells oben - reine Ein-Schuss-Uebergabe
// eines Parameters, von handleGithubUpdate() gesetzt bevor otaGithubTask()
// gestartet wird, danach nur einmal beim Task-Start gelesen).
String otaPendingUrl = "";

// Beantwortet eine rein anfrage-lokale Frage ("wurde DIESER Upload-Versuch
// schon VOR Erhalt der Sperre abgelehnt?"), kein Teil des OtaState-Modells:
// handleUploadFirmwareData() und handleUploadFirmware() werden vom
// WebServer garantiert nacheinander fuer dieselbe Anfrage aufgerufen, ohne
// dass etwas anderes dazwischenfunkt (single-threaded) - hier geht es nur
// um die Weitergabe eines Zwischenergebnisses zwischen zwei Callbacks
// derselben Anfrage, nicht um geteilten Zustand ueber die Zeit.
String otaUploadPreflightRejection = "";

String tibberToken = "";
String selectedHomeId = "";

// Monatliche Stromkosten (aktueller, noch laufender Monat), aus derselben
// Tibber-Abfrage wie die Preisdaten mitgeliefert (consumption(resolution:
// MONTHLY, last: 1)). Kommt von Tibbers eigener Zaehlerauswertung, nicht vom
// Pulse - funktioniert also auch ohne Pulse, kann aber je nach Netzbetreiber
// 1-2 Tage nachhinken.
float tibberMonthCost = -1;
float tibberMonthConsumptionKwh = -1;
String tibberMonthCurrency = "";
int tibberMonthDaysCounted = 0;

// Monatliche Grundgebuehr (EUR) - die API liefert nur die reinen
// Energiekosten, die tatsaechliche Rechnung enthaelt aber zusaetzlich die
// Tibber-Grundgebuehr. Wird auf der Anbieter-Seite eingetragen und zu den
// Monatskosten addiert, damit die Anzeige dem echten Rechnungsbetrag
// entspricht.
float tibberBaseFeeEur = 0.0;
// Anpassbare Skala fuer die Live-Verbrauch-Balken-Anzeige (Modern-Template).
float livePowerMaxKw = 10.0f;     // Bar-Endwert
float livePowerGreenKw = 2.0f;    // Grenze gruene Zone
float livePowerYellowKw = 5.0f;   // Grenze gelbe Zone
String kioskLivePowerStyle = "text"; // "text" oder "bar"

// Tibber Pulse: Live-Verbrauch per GraphQL-Subscription ueber WebSocket
// (graphql-transport-ws). Wird automatisch versucht, sobald ein Tibber-Token
// und eine Home-ID bekannt sind - ohne Pulse kommen einfach nie "next"-
// Nachrichten an und livePowerW bleibt -1, dann wird nirgends etwas angezeigt.
const char* TIBBER_WS_HOST = "websocket-api.tibber.com";
const char* TIBBER_WS_URL = "/v1-beta/gql/subscriptions";
// Fallback-Root-CA, falls der Nutzer auf /anbieter kein eigenes Zertifikat
// hinterlegt hat (siehe tibberRootCaPem) - noetig, weil ArduinoWebsockets'
// setInsecure() auf ESP32 nachweislich nicht funktioniert (siehe Kommentar
// in updateTibberLiveMeasurement()). Amazon Root CA 1, per openssl s_client
// gegen websocket-api.tibber.com verifiziert (deckt *.tibber.com ab).
const char* TIBBER_WS_FALLBACK_CA =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIEkjCCA3qgAwIBAgITBn+USionzfP6wq4rAfkI7rnExjANBgkqhkiG9w0BAQsF\n"
  "ADCBmDELMAkGA1UEBhMCVVMxEDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNj\n"
  "b3R0c2RhbGUxJTAjBgNVBAoTHFN0YXJmaWVsZCBUZWNobm9sb2dpZXMsIEluYy4x\n"
  "OzA5BgNVBAMTMlN0YXJmaWVsZCBTZXJ2aWNlcyBSb290IENlcnRpZmljYXRlIEF1\n"
  "dGhvcml0eSAtIEcyMB4XDTE1MDUyNTEyMDAwMFoXDTM3MTIzMTAxMDAwMFowOTEL\n"
  "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
  "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
  "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
  "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
  "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
  "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
  "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
  "jgSubJrIqg0CAwEAAaOCATEwggEtMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/\n"
  "BAQDAgGGMB0GA1UdDgQWBBSEGMyFNOy8DJSULghZnMeyEE4KCDAfBgNVHSMEGDAW\n"
  "gBScXwDfqgHXMCs4iKK4bUqc8hGRgzB4BggrBgEFBQcBAQRsMGowLgYIKwYBBQUH\n"
  "MAGGImh0dHA6Ly9vY3NwLnJvb3RnMi5hbWF6b250cnVzdC5jb20wOAYIKwYBBQUH\n"
  "MAKGLGh0dHA6Ly9jcnQucm9vdGcyLmFtYXpvbnRydXN0LmNvbS9yb290ZzIuY2Vy\n"
  "MD0GA1UdHwQ2MDQwMqAwoC6GLGh0dHA6Ly9jcmwucm9vdGcyLmFtYXpvbnRydXN0\n"
  "LmNvbS9yb290ZzIuY3JsMBEGA1UdIAQKMAgwBgYEVR0gADANBgkqhkiG9w0BAQsF\n"
  "AAOCAQEAYjdCXLwQtT6LLOkMm2xF4gcAevnFWAu5CIw+7bMlPLVvUOTNNWqnkzSW\n"
  "MiGpSESrnO09tKpzbeR/FoCJbM8oAxiDR3mjEH4wW6w7sGDgd9QIpuEdfF7Au/ma\n"
  "eyKdpwAJfqxGF4PcnCZXmTA5YpaP7dreqsXMGz7KQ2hsVxa81Q4gLv7/wmpdLqBK\n"
  "bRRYh5TmOTFffHPLkIhqhBGWJ6bt2YFGpn6jcgAKUj6DiAdjd4lpFw85hdKrCEVN\n"
  "0FE6/V1dN2RMfjCyVSRCnTawXZwXgWHxyvkQAiSr6w10kY17RSlQOYiypok1JR4U\n"
  "akcjMS9cmvqtmg5iUaQqqcT5NJ0hGA==\n"
  "-----END CERTIFICATE-----\n";
WebsocketsClient tibberWs;
bool tibberWsInited = false;      // Callbacks/Header einmalig gesetzt
bool tibberWsConnected = false;   // aktuell verbunden (per Events selbst nachgefuehrt, keine Auto-Reconnect-Bibliotheksfunktion vorhanden)
unsigned long tibberWsLastConnectAttemptMs = 0;
float livePowerW = -1;
unsigned long livePowerUpdatedAtMs = 0;
// Reine Diagnose (siehe handleTibberWsEvent()) - zeigt in /livepower, was die
// WebSocket-Verbindung zuletzt tatsaechlich getan/erhalten hat, da Server-
// seitige GraphQL-Fehler (z.B. abgelehnte Subscription) bisher stillschweigend
// verworfen wurden und "livePowerW bleibt -1" ohne Erklaerung aussah wie ein
// fehlendes Pulse-Geraet, obwohl ein echter Fehler vorlag.
String tibberWsLastEvent = "nie verbunden";
unsigned long tibberWsLastEventMs = 0;
// Laut Tibber-API-Doku sollte vor dem (Re-)Verbinden geprueft werden, ob
// viewer.home.features.realTimeConsumptionEnabled true ist - wird hier per
// normaler HTTPS-GraphQL-Abfrage (updatePrices()) huckepack mitgeholt, reine
// Diagnose, keine Steuerlogik (die WS-Verbindung wird trotzdem versucht,
// falls dieser Wert aus irgendeinem Grund nicht ermittelt werden konnte).
bool tibberRealTimeEnabledKnown = false;
bool tibberRealTimeEnabled = false;
// Siehe updateTibberLiveMeasurement(): das Abo-Kommando darf nicht direkt aus
// handleTibberWsEvent() heraus gesendet werden, deshalb nur hier vormerken.
bool tibberSubscribePending = false;

// Strompreis-Quelle: "tibber" (Standard), "awattar_de" oder "awattar_at".
// aWATTar liefert nur den Boersenpreis ohne Netzentgelte/Steuern, deshalb
// zusaetzlich ein fixer Aufschlag (ct/kWh) und ein Mehrwertsteuersatz (%).
String priceProvider = "tibber";
float priceSurchargeCt = 0.0;
float priceVatPercent = 0.0;

// -----------------------------------------------------------------------------
// Anker Solix Cloud (Solarbank) - inoffizielle Cloud-API, siehe updateAnkerSolarData()
// -----------------------------------------------------------------------------
String ankerEmail = "";
String ankerPassword = "";
String ankerSiteId = "";
String ankerAuthToken = "";
String ankerUserId = "";
String ankerGtoken = "";
unsigned long ankerTokenObtainedMs = 0;
const unsigned long ANKER_TOKEN_LIFETIME_MS = 20UL * 60UL * 60UL * 1000UL; // vorsorglich re-login nach 20h
unsigned long lastAnkerPoll = 0;
const unsigned long ANKER_POLL_INTERVAL_MS = 60UL * 1000UL; // Anker-Empfehlung: nicht schneller als 60s pollen
float ankerPvW = -1;        // total_photovoltaic_power
float ankerBatteryW = -1;   // total_battery_power (positiv=laden, negativ=entladen je nach API)
float ankerOutputW = -1;    // total_output_power (Ausgang der Solarbank Richtung Haus)
float ankerHomeLoadW = -1;  // home_load_power (Gesamt-Hausverbrauch)
int ankerBatterySoc = -1;   // Batterie-Ladezustand in %, falls verfuegbar
String ankerLastError = "";
bool ankerConfigured = false;
String ankerLastRawJson = ""; // letzte rohe Antwort von get_scen_info, zur Fehlersuche auf der Konto-Seite

// Lebenszeit-Summen aus dem bisher ungenutzten "statistics"-Array der
// get_scen_info-Antwort (type 1=kWh Gesamtertrag, 2=kg CO2 gespart,
// 3=Euro gespart seit Inbetriebnahme) - keine neue Anfrage noetig, die
// Werte stecken schon in der Antwort, die ohnehin alle 60s abgerufen wird.
float ankerTotalYieldKwh = -1;
float ankerCo2SavedKg = -1;
float ankerMoneySavedEur = -1;

// Tagesertrag PV (kWh): echte Tagesenergie liefert die Anker-API nicht,
// deshalb hier selbst durch Aufintegrieren der PV-Momentanleistung ueber die
// Zeit angenaehert (Riemannsumme aus den 60s-Polls) - eine Annaeherung, keine
// exakte Messung, aber ohne unsicheren, unbestaetigten Zusatz-Endpunkt.
// Ueberlebt keinen Neustart (nur RAM), analog zu anderen Tageszaehlern in
// diesem Projekt.
float pvYieldTodayKwh = 0;
String pvYieldTodayDate = "";
unsigned long lastPvIntegrationMs = 0;

String lastError = "Noch kein Update";
String webInterfaceName = "Dynamic Price Clock";
// Akzentfarbe fuer das UI (iOS-Systemfarben). Erlaubt: blue, green, orange, red, pink, purple, teal, indigo.
String accentColor = "blue";
String appearanceMode = "solid"; // "solid" oder "glass"

// -----------------------------------------------------------------------------
// Zeiten
// -----------------------------------------------------------------------------

int apiUpdateMinutes = 5;
int displayRefreshSeconds = 10;

bool display1Enabled = true;
bool display2Enabled = true;
int display1Mode = 0;
int display2Mode = 0;

bool ledRingEnabled = true;
int ledBrightness = 40;
int ledRefreshSeconds = 10;
int ledActiveCount = LED_RING_COUNT;
int ledYellowCent = 30;
int ledRedCent = 35;
int gaugeMinCent = 0;   // Fester linker Rand des Preis-Balkens
int gaugeMaxCent = 60;  // Fester rechter Rand des Preis-Balkens
int ledCheapColorId = 0;
int ledMidColorId = 1;
int ledHighColorId = 2;
int ledCurrentColorId = 4;
int ledLowBlockColorId = 3;

bool matrixEnabled[MATRIX_DEVICE_COUNT] = {false, false, false, false};
int matrixBrightness[MATRIX_DEVICE_COUNT] = {6, 6, 6, 6};
int matrixMode[MATRIX_DEVICE_COUNT] = {8, 8, 8, 8};
int matrixRefreshSeconds = 5;
unsigned long matrixRefreshInterval = 5000UL;
unsigned long lastMatrixRefresh = 0;

volatile uint32_t cpuIdleCounter = 0;
uint32_t cpuIdleCounterLast = 0;
uint32_t cpuIdleCounterMaxDelta = 1;
float cpuLoadPercent = 0.0f;
unsigned long cpuLoadLastSample = 0;
TaskHandle_t cpuIdleTaskHandle = NULL;

bool displayPriceBarEnabled = false;
int displayPriceBarMinCent = 0;
int displayPriceBarMaxCent = 60;
int displayPriceBarWidth = 10;
bool displayPriceBarTextEnabled = false;

bool display1DayChartEnabled = false;
bool display2DayChartEnabled = false;
int displayDayChartX = 18;
int displayDayChartY = 154;
int displayDayChartWidth = 204;
int displayDayChartHeight = 76;

bool display2CheapClockRingEnabled = true;
bool display2CheapClockRingTextEnabled = false;
bool display2CheapClockRingLabelsEnabled = true;
int display2CheapClockRingWidth = 14;
int display2RingCheapColorId = 0;
int display2RingMidColorId = 1;
int display2RingHighColorId = 2;
int display2RingBestColorId = 4;
int display2RingCurrentColorId = 3;
bool layoutMigratedTo240 = false;

unsigned long updateInterval = 5UL * 60UL * 1000UL;
unsigned long displayRefreshInterval = 10UL * 1000UL;
unsigned long ledRefreshInterval = 10UL * 1000UL;

unsigned long lastUpdate = 0;
unsigned long lastDisplayRefresh = 0;
unsigned long lastLedRefresh = 0;

// -----------------------------------------------------------------------------
// Tibber-Daten
// -----------------------------------------------------------------------------

String homeIds[MAX_HOMES];
String homeNames[MAX_HOMES];
int homeCount = 0;

float currentPrice = -1.0;
String currentStartsAt = "";

float quarterPrices[MAX_QUARTERS];
String quarterTimes[MAX_QUARTERS];
int quarterCount = 0;

float metricCurrent15 = -1.0;
float metricCurrent60 = -1.0;
float metricDayAvg = -1.0;

float metricLow15Day = -1.0;
String metricLow15DayTime = "";

float metricLow60Day = -1.0;
String metricLow60DayTime = "";

float metricSecondLow60Day = -1.0;
String metricSecondLow60DayTime = "";

// -----------------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------------

struct LayoutItem {
  String key;
  String customText;
  String prefix;
  String suffix;
  int x;
  int y;
  int size;
  bool visible;
  bool autoScale;
  int align;
};

LayoutItem d1Layout[LAYOUT_ITEMS];
LayoutItem d2Layout[LAYOUT_ITEMS];

// -----------------------------------------------------------------------------
// Tablet-/Kiosk-Modus: frei anordenbares Widget-Layout
// -----------------------------------------------------------------------------
// Jedes Widget hat eine Position/Groesse in Prozent eines Design-Canvas -
// getrennt fuer Hoch- und Querformat, damit beide Ausrichtungen unabhaengig
// voneinander gestaltet werden koennen. Prozent statt Pixel, damit es auf
// jeder Aufloesung (Handy bis grosses Tablet) proportional gleich aussieht.
#define KIOSK_WIDGET_COUNT 6

// Grid-basiertes Layout ab v2.0.0: statt Absolut-Position auf einem Canvas
// belegt jedes Widget Zellen in einem CSS-Grid. Portrait = 6 Spalten x 12
// Reihen, Landscape = 12 Spalten x 8 Reihen. Widgets sind ueber grid-column
// und grid-row platziert; das Grid selbst passt sich responsiv der verfuegbaren
// Flaeche an (auch auf TV-Browsern die aspect-ratio nicht koennen).
#define KIOSK_GRID_COLS_PORTRAIT 6
#define KIOSK_GRID_ROWS_PORTRAIT 12
#define KIOSK_GRID_COLS_LANDSCAPE 12
#define KIOSK_GRID_ROWS_LANDSCAPE 8

struct KioskWidgetLayout {
  uint8_t colStart;  // 1-basiert, 1..gridCols
  uint8_t colSpan;   // 1..gridCols
  uint8_t rowStart;  // 1-basiert, 1..gridRows
  uint8_t rowSpan;   // 1..gridRows
  bool visible;
};

const char* KIOSK_WIDGET_KEYS[KIOSK_WIDGET_COUNT] = { "clock", "gauge", "status", "livepower", "chart", "meta" };
const char* KIOSK_WIDGET_LABELS[KIOSK_WIDGET_COUNT] = { "Uhrzeit", "Preis-Gauge", "Status-Badge", "Live-Verbrauch", "Diagramm", "Infozeile" };

// Portrait: 6 Spalten x 12 Reihen
const KioskWidgetLayout KIOSK_PORTRAIT_DEFAULTS[KIOSK_WIDGET_COUNT] = {
  { 1, 6, 1,  1, true }, // clock:     ganze Breite, oberste Reihe
  { 1, 6, 2,  4, true }, // gauge:     ganze Breite, 4 Reihen hoch
  { 2, 4, 6,  1, true }, // status:    mittig, schmal
  { 2, 4, 7,  1, true }, // livepower: mittig, schmal
  { 1, 6, 8,  4, true }, // chart:     ganze Breite, 4 Reihen hoch
  { 1, 6, 12, 1, true }, // meta:      ganze Breite, untere Reihe
};

// Landscape: 12 Spalten x 8 Reihen
const KioskWidgetLayout KIOSK_LANDSCAPE_DEFAULTS[KIOSK_WIDGET_COUNT] = {
  { 5, 4,  1, 1, true }, // clock:     mittig oben
  { 1, 6,  2, 5, true }, // gauge:     linke Haelfte gross
  { 1, 6,  7, 1, true }, // status:    linke Haelfte unten
  { 1, 6,  8, 1, true }, // livepower: linke Haelfte ganz unten
  { 7, 6,  2, 5, true }, // chart:     rechte Haelfte gross
  { 7, 6,  7, 2, true }, // meta:      rechte Haelfte unten
};

KioskWidgetLayout kioskPortrait[KIOSK_WIDGET_COUNT];
KioskWidgetLayout kioskLandscape[KIOSK_WIDGET_COUNT];

// Energiefluss-Kiosk-Seite (/kiosk2): eigenes Widget-Set, gleiche
// Grid-Architektur/Groesse (6x12 Portrait, 12x8 Landscape) wie Kiosk-Seite 1,
// damit derselbe Layout-Editor (Drag/Resize) wiederverwendet werden kann.
// Statt vier lose verschiebbaren Einzelkarten (PV/Batterie/Haus/Netz) EIN
// kombiniertes Hub-Diagramm-Widget ("energyflow", siehe buildEnergyFlowSvg())
// - zeigt den tatsaechlichen Energiefluss zwischen den Quellen statt vier
// fuer sich stehender Zahlen, die man selbst gedanklich verknuepfen musste.
// "stats" ergaenzt Autarkie/Eigenverbrauch (berechnet) sowie Tagesertrag/
// Gesamtertrag/CO2/Geld gespart (siehe buildEnergyStatsHtml()) als eigenes,
// kompaktes Kennzahlen-Widget neben dem Hub-Diagramm.
#define KIOSK2_WIDGET_COUNT 5
const char* KIOSK2_WIDGET_KEYS[KIOSK2_WIDGET_COUNT] = { "clock", "pricegauge", "pricechart", "energyflow", "stats" };
const char* KIOSK2_WIDGET_LABELS[KIOSK2_WIDGET_COUNT] = { "Uhrzeit", "Preis-Gauge", "Preis-Diagramm", "Energiefluss", "Kennzahlen" };

// Portrait: 6 Spalten x 12 Reihen
const KioskWidgetLayout KIOSK2_PORTRAIT_DEFAULTS[KIOSK2_WIDGET_COUNT] = {
  { 1, 6, 1,  1, true }, // clock:      ganze Breite, oberste Reihe
  { 1, 6, 2,  3, true }, // pricegauge: ganze Breite
  { 1, 6, 5,  3, true }, // pricechart: ganze Breite
  { 1, 6, 8,  3, true }, // energyflow: ganze Breite
  { 1, 6, 11, 2, true }, // stats:      ganze Breite, unterste Reihen
};

// Landscape: 12 Spalten x 8 Reihen
const KioskWidgetLayout KIOSK2_LANDSCAPE_DEFAULTS[KIOSK2_WIDGET_COUNT] = {
  { 5, 4,  1, 1, true }, // clock:      mittig oben
  { 1, 5,  2, 4, true }, // pricegauge: linke Haelfte oben
  { 6, 7,  2, 4, true }, // pricechart: rechte Haelfte oben
  { 1, 7,  6, 3, true }, // energyflow: unten links, breiter
  { 8, 5,  6, 3, true }, // stats:      unten rechts, schmaler
};

KioskWidgetLayout kiosk2Portrait[KIOSK2_WIDGET_COUNT];
KioskWidgetLayout kiosk2Landscape[KIOSK2_WIDGET_COUNT];

// Positionen der 4 Knoten im Energiefluss-Hub-Diagramm (siehe
// buildEnergyFlowSvg()), Reihenfolge PV/Batterie/Netz/Haus, Koordinaten im
// SVG-eigenen viewBox 0..EF_CANVAS_W x 0..EF_CANVAS_H (unabhaengig von der
// tatsaechlichen Widget-Groesse auf dem Bildschirm). Bewusst ein breites statt
// quadratisches Koordinatensystem: das Widget selbst ist in beiden
// Orientierungen ein breites Rechteck (Kiosk2 Portrait/Landscape-Defaults
// jeweils ca. 2:1), ein quadratisches viewBox wurde per preserveAspectRatio
// mittig eingepasst und liess links/rechts viel Platz ungenutzt - Knoten
// liessen sich dadurch nicht sehr weit auseinanderziehen. Frei verschiebbar
// im Anordnen-Modus, die Verbindungslinien werden bei jeder Verschiebung neu
// aus den aktuellen Positionen berechnet (siehe efLineBetween() clientseitig
// und die serverseitige Entsprechung in buildEnergyFlowSvg()).
#define EF_CANVAS_W 640
#define EF_CANVAS_H 360
struct EfNodePos { float x, y; };
const EfNodePos EF_NODE_DEFAULTS[4] = {
  { 320, 72  },  // 0 = PV
  { 128, 288 },  // 1 = Batterie
  { 512, 288 },  // 2 = Netz
  { 320, 212 },  // 3 = Haus
};
EfNodePos efNodePos[4] = {
  { 320, 72  },
  { 128, 288 },
  { 512, 288 },
  { 320, 212 },
};
// Docking-Radius je Knoten - bei der Batterie bewusst der AEUSSERE
// Ladering (r=50), nicht der innere Kreis (r=42), damit die Linie am Ring
// andockt statt darunter zu verschwinden.
const float EF_NODE_RADIUS[4] = { 42, 50, 42, 56 };

// Startseiten-Dashboard (/): drittes Widget-Set, gleiche KioskWidgetLayout-
// Struktur und dieselben kioskWidgetCss()/kioskLayoutJson()-Helfer wie oben,
// aber nur EIN Layout (kein Hoch-/Querformat) - die Startseite ist eine
// normal scrollbare Seite, keine feste Vollbild-Kiosk-Flaeche. Eine einzelne
// CSS-Media-Query (siehe .hw-canvas) stapelt die Widgets auf schmalen
// Bildschirmen, statt ein zweites persistiertes Layout vorzuhalten.
#define HOME_GRID_COLS 8
// 13 statt urspruenglich 7 Reihen: Platz fuer die zwei zusaetzlichen Widgets
// "status" (Statuszeile + Aktions-Buttons) und "pricetable" (aufklappbare
// Preistabelle) unterhalb der urspruenglichen 4 Widgets, die die Reihen 1-7
// bereits vollstaendig belegen.
#define HOME_GRID_ROWS 13
#define HOME_WIDGET_COUNT 6
const char* HOME_WIDGET_KEYS[HOME_WIDGET_COUNT] = { "gauge", "livepower", "chart", "metrics", "status", "pricetable" };
const char* HOME_WIDGET_LABELS[HOME_WIDGET_COUNT] = { "Preis-Gauge", "Live-Verbrauch", "Diagramm", "Metriken", "Status & Aktionen", "Preistabelle" };

const KioskWidgetLayout HOME_DEFAULTS[HOME_WIDGET_COUNT] = {
  { 1, 3, 1, 3, true },  // gauge:      links oben, quadratisch
  { 1, 3, 4, 2, true },  // livepower:  links, unter der Gauge
  { 4, 5, 1, 5, true },  // chart:      rechts, hoch
  { 1, 8, 6, 2, true },  // metrics:    ganze Breite
  { 1, 8, 8, 2, true },  // status:     ganze Breite, unter den Metriken
  { 1, 8, 10, 4, true }, // pricetable: ganze Breite, mehr Hoehe fuer die aufklappbare Tabelle (scrollt intern, siehe kw-pricetable-CSS)
};

KioskWidgetLayout homeLayout[HOME_WIDGET_COUNT];

// -----------------------------------------------------------------------------
// Funktions-Prototypen
// -----------------------------------------------------------------------------

void ensureWifiConnected();
void retryWifiFromPermanentApMode();

void updateTibber();
void updateAwattarPrices();
void updatePrices();
void updateTibberLiveMeasurement();
void handleTibberWsEventCb(WebsocketsEvent event, String data);
void handleTibberWsMessage(WebsocketsMessage message);
String otaPreflightDiag(const String &host);
enum class OtaPhase : uint8_t;
enum class OtaOwner : uint8_t;
String otaTryAcquire(OtaOwner who);
void otaProgress(OtaOwner who, int written, int total);
void otaSetPhase(OtaOwner who, OtaPhase p);
void otaFail(OtaOwner who, const String &msg, bool releaseOwner);
void otaFinishSuccess(OtaOwner who);
bool otaCheckLatest(bool withRetryAndDiag);
void handleVersionCheck();
void otaWdtEnter();
void otaWdtFeed();
void otaWdtExit();
bool otaDownloadAndFlashGithub(String url);
void otaGithubTask(void *param);
void calculateMetrics();
bool ankerLogin();
void updateAnkerSolarData();
String buildAnkerFlowSvg();

String priceToCentText(float price);
String euroCostText(float value);
float estimateFullMonthCost();
String formatLivePowerValue();
String getLayoutValue(String key);

void drawLayoutDisplay(Adafruit_GC9A01A &disp, bool ok, LayoutItem layout[]);
void showLayoutDisplays();
void updateLedRing();
void clearLedRing();
uint32_t priceToLedColor(int cent);
uint32_t ledColorFromId(int colorId);
void addColorSelect(String &html, const char* name, const char* label, int selected);
String gpioOwnerLabel(int gpio);
void addGpioSelect(String &html, String fieldName, String label, String hint, int currentValue);
int mapDayLedToClockLed(int logicalLed, int activeCount);

String getLayoutItemText(LayoutItem item);
String getPreviewText(LayoutItem item);
int getTextWidth(Adafruit_GC9A01A &disp, String text, int textSize);
int getTextHeight(Adafruit_GC9A01A &disp, String text, int textSize);
int getAlignedX(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int textSize, int align);
bool fitsTextAligned(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int textSize, int align);
int getBestTextSizeAligned(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int maxSize, int align);
String trimTextToFitAligned(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int textSize, int align);

void loadLayoutDefaults();
void loadLayoutFromPrefs();
void saveLayoutItem(int d, int i, LayoutItem item);

void loadKioskLayoutFromPrefs();
void saveKioskWidget(bool landscape, int i, KioskWidgetLayout item);
void saveKiosk2Widget(bool landscape, int i, KioskWidgetLayout item);
void saveHomeLayoutToPrefs();
void handleKioskLayoutPage();
void handleSaveKioskLayoutAjax();
void handleSaveEfNodePos();
void handleResetKioskLayout();

bool checkAuth();
String generateRandomToken(int length);
String generateNumericPassword(int length);
void bootstrapApPassword();
void randomizeWifiStaMac();
String getStaMacText();
uint32_t ipToUint32(IPAddress ip);
IPAddress uint32ToIp(uint32_t v);
IPAddress calculateFifthLastIp(IPAddress gateway, IPAddress subnet);
bool parseIpSafe(String text, IPAddress &ip);
void prepareWifiIdentityAndIp();
void applyWifiBandPreference();
String getChipInfoText();
void showWifiBootStatus(String line1, String line2, String line3 = "");
void showBootCredentials();
void startSetupPortal(bool permanent);
void stopSetupPortalIfAllowed();
bool wifiConnectStart(const String &ssidInput, const String &passwordInput, unsigned long timeoutMs);
int wifiConnectPoll();
void updateWifiConnectAttempt();
int wifiRssiToQuality(int rssi);
String wifiQualityLabel(int quality);

void handleRoot();
void handleWifiPage();
void handleKioskPage();
void handleKioskData();
void handleKiosk2Page();
void handleAnkerData();
void handleGaugeStatus();
void handlePinoutPage();
void handleSavePins();
void handleRestartDevice();
void handleCheckGithubUpdate();
void handleGithubUpdate();
void handleOtaProgress();
void handleUploadFirmware();
void handleUploadFirmwareData();
void handleAccountPage();
void handleProviderPage();
void handleSaveProviderAjax();
void handleSaveTibberAjax();
void handleSaveAccount();
void handleWifiScanJson();
void handleWifiStatusJson();
void handleSaveWifiAjax();
void handleConnectWifiAjax();
void handleDisplaysPage();
void handleSaveDisplays();
void handleSaveDisplayChartAjax();
void handleLedRingPage();
void handleMatrixPage();
void handleSaveMatrix();
void handleSaveMatrixAjax();
void saveMatrixFromRequest();
void handleSaveLedRing();
void handleSaveLedRingAjax();
void saveLedRingFromRequest();
void initPriceMatrix();
void clearPriceMatrix();
void updatePriceMatrix();
void showMatrixNumber(int number);
void showMatrixTwoChars(char leftChar, char rightChar);
void showMatrixText2(String text);
String matrixModeLabel(int mode);
String matrixValuePreview();
void initCpuLoadMonitor();
void updateCpuLoadMonitor();
String getCpuLoadText();
String getFreeHeapText();
String getUptimeText();
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
uint16_t priceBarGradientColor(int value, int minValue, int maxValue);
void drawCurrentPriceBar(Adafruit_GC9A01A &disp, float priceEuro);
void drawDisplay2CheapClockRing(Adafruit_GC9A01A &disp);
void drawTftDayLineChart(Adafruit_GC9A01A &disp, int x, int y, int w, int h);
void drawOptionalDisplayCharts(Adafruit_GC9A01A &disp);
uint16_t tftColorFromId(int colorId);
uint16_t tftBlendColor(uint16_t c1, uint16_t c2, float t);
uint16_t tftPriceRingColor(int value, int minValue, int maxValue);
void handleSaveLayout();
void handleSaveLayoutAjax();
void saveLayoutFromRequest();
void handleResetLayout();
void handlePresetLayout();
void handleExportLayout();
void handleImportLayout();
void applyLayoutPreset(String preset);
void handleSave();
void handleResetWifi();
void handleRefresh();
void handleJson();
void handleStyleCss();
void handleFaviconSvg();
void handleAppJs();
void handleLayoutEditorJs();
void handleAccountUpdateJs();
void handleWidgetEngineJs();
void handleLivePower();

String buildSvgChart(String lineColor = "", String fillColor = "");
String buildChartPointsJson();
String buildPinoutSvg();
String buildPriceGaugeSvg();
String buildEnergyFlowSvg();
String buildEnergyStatsHtml();
void getKioskPriceStatus(String &statusText, String &statusColor);
String kioskWidgetCss(KioskWidgetLayout arr[], const char* const keys[] = KIOSK_WIDGET_KEYS, int count = KIOSK_WIDGET_COUNT);
String kioskLayoutJson(KioskWidgetLayout arr[], const char* const keys[] = KIOSK_WIDGET_KEYS, const char* const labels[] = KIOSK_WIDGET_LABELS, int count = KIOSK_WIDGET_COUNT);
String pinoutPinSvg(bool leftSide, int y, String title, String sub, String color);

String htmlHeader(String title);
String htmlFooter();
String navTabs(String current);
String navTabsItem(String href, String label, String icon, String current);

String getCurrentIsoPrefix();
String getLocalDatePrefix();
String getDisplayDateText();
String getDisplayTimeText();
bool isTodaySlot(String isoTime);

int euroToCentRounded(float euroPrice);
String formatTimeOnly(String isoTime);
String addMinutesToIsoTime(String isoTime, int minutesToAdd);
String htmlEscape(String s);
String jsEscape(String s);
void showMessage(Adafruit_GC9A01A &disp, bool ok, const char* msg);

// -----------------------------------------------------------------------------
// Auth / Bootstrap-Secrets
// -----------------------------------------------------------------------------
// Erzeugt einen zufaelligen, URL-/Passwort-tauglichen Zufallsstring aus dem
// Hardware-RNG des ESP32 (esp_random()). Wird genutzt, um beim allerersten
// Boot ein Admin-Passwort und ein Setup-AP-Passwort zu erzeugen, statt feste
// Werte im Quelltext zu verteilen.
String generateRandomToken(int length) {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  String out = "";
  out.reserve(length);

  for (int i = 0; i < length; i++) {
    uint32_t r = esp_random();
    out += alphabet[r % (sizeof(alphabet) - 1)];
  }

  return out;
}

String generateNumericPassword(int length) {
  String out = "";
  out.reserve(length);
  for (int i = 0; i < length; i++) {
    out += (char)('0' + (esp_random() % 10));
  }
  return out;
}

// Gleiches Prinzip fuer das Setup-WLAN-Passwort: kein fester Wert im Code,
// stattdessen einmalig zufaellig erzeugt und dauerhaft gespeichert.
void bootstrapApPassword() {
  if (prefs.isKey("apPass")) {
    apPassword = prefs.getString("apPass", "");
    return;
  }

  apPassword = generateNumericPassword(10);
  prefs.putString("apPass", apPassword);

  Serial.println();
  Serial.println("================================================================");
  Serial.println("ERSTSTART: Setup-WLAN-Passwort wurde erzeugt.");
  Serial.print("Setup-WLAN SSID:     "); Serial.println(AP_SSID);
  Serial.print("Setup-WLAN Passwort: "); Serial.println(apPassword);
  Serial.println("Bitte notieren. Aenderbar unter http://<IP>/account");
  Serial.println("================================================================");
  Serial.println();
}
// -----------------------------------------------------------------------------
// Setup / Loop
// -----------------------------------------------------------------------------

String wifiStatusToText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void showWifiBootStatus(String line1, String line2, String line3) {
  Serial.print("WLAN STATUS: ");
  Serial.print(line1);
  Serial.print(" | ");
  Serial.print(line2);
  if (line3.length() > 0) {
    Serial.print(" | ");
    Serial.print(line3);
  }
  Serial.println();

  if (displayCurrentOk) {
    displayCurrent.fillScreen(DISPLAY_BLACK);
    displayCurrent.setTextColor(DISPLAY_WHITE);
    displayCurrent.setTextSize(1);
    displayCurrent.setCursor(0, 0);
    displayCurrent.print("WLAN");
    displayCurrent.setCursor(0, 18);
    displayCurrent.print(line1.substring(0, 21));
    displayCurrent.setCursor(0, 34);
    displayCurrent.print(line2.substring(0, 21));
    if (line3.length() > 0) {
      displayCurrent.setCursor(0, 50);
      displayCurrent.print(line3.substring(0, 21));
    }
  }

  if (displayBestOk) {
    displayBest.fillScreen(DISPLAY_BLACK);
    displayBest.setTextColor(DISPLAY_WHITE);
    displayBest.setTextSize(1);
    displayBest.setCursor(0, 0);
    displayBest.print("Netzwerk");
    displayBest.setCursor(0, 18);
    displayBest.print(line1.substring(0, 21));
    displayBest.setCursor(0, 34);
    displayBest.print(line2.substring(0, 21));
    if (line3.length() > 0) {
      displayBest.setCursor(0, 50);
      displayBest.print(line3.substring(0, 21));
    }
  }
}

String getStaMacText() {
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void randomizeWifiStaMac() {
  uint8_t mac[6];

  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();

  mac[0] = 0x02;
  mac[1] = (r1 >> 0) & 0xFF;
  mac[2] = (r1 >> 8) & 0xFF;
  mac[3] = (r1 >> 16) & 0xFF;
  mac[4] = (r1 >> 24) & 0xFF;
  mac[5] = (r2 >> 0) & 0xFF;

  mac[0] = (mac[0] & 0xFE) | 0x02;

  esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);

  if (err == ESP_OK) {
    Serial.print("Neue zufaellige WLAN STA MAC: ");
    Serial.println(getStaMacText());
  } else {
    Serial.print("Konnte zufaellige WLAN STA MAC nicht setzen. Fehler: ");
    Serial.println((int)err);
  }
}

uint32_t ipToUint32(IPAddress ip) {
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

IPAddress uint32ToIp(uint32_t v) {
  return IPAddress((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

IPAddress calculateFifthLastIp(IPAddress gateway, IPAddress subnet) {
  uint32_t gw = ipToUint32(gateway);
  uint32_t mask = ipToUint32(subnet);
  uint32_t network = gw & mask;
  uint32_t broadcast = network | (~mask);
  uint32_t target = broadcast - 5;
  return uint32ToIp(target);
}

bool parseIpSafe(String text, IPAddress &ip) {
  text.trim();
  if (text.length() == 0) return false;
  return ip.fromString(text);
}

void prepareWifiIdentityAndIp() {
  if (wifiMacRotationEnabled) {
    randomizeWifiStaMac();
    showWifiBootStatus("Neue MAC", getStaMacText(), "Verbinde WLAN");
  } else {
    Serial.print("MAC-Rotation aus. STA MAC: ");
    Serial.println(getStaMacText());
  }

  if (!wifiStaticIpEnabled && !wifiAutoFifthLastIpEnabled) {
    Serial.println("IP-Modus: DHCP");
    return;
  }

  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns1;
  IPAddress dns2;

  bool gatewayOk = parseIpSafe(wifiGatewayText, gateway);
  bool subnetOk = parseIpSafe(wifiSubnetText, subnet);
  bool dns1Ok = parseIpSafe(wifiDns1Text, dns1);
  bool dns2Ok = parseIpSafe(wifiDns2Text, dns2);

  if (!subnetOk) subnet = IPAddress(255, 255, 255, 0);

  if (wifiAutoFifthLastIpEnabled) {
    if (!gatewayOk) {
      Serial.println("Auto-IP aktiv, aber Gateway fehlt. DHCP wird verwendet.");
      lastError = "Auto-IP: Gateway fehlt";
      return;
    }
    ip = calculateFifthLastIp(gateway, subnet);
    wifiStaticIpText = ip.toString();
  } else {
    if (!parseIpSafe(wifiStaticIpText, ip) || !gatewayOk) {
      Serial.println("Feste IP aktiv, aber IP/Gateway ungueltig. DHCP wird verwendet.");
      lastError = "Feste IP ungueltig";
      return;
    }
  }

  if (!dns1Ok) dns1 = gateway;
  if (!dns2Ok) dns2 = IPAddress(1, 1, 1, 1);

  bool ok = WiFi.config(ip, gateway, subnet, dns1, dns2);
  Serial.print("IP-Modus: ");
  Serial.println(wifiAutoFifthLastIpEnabled ? "Auto fünftletzte IP" : "Feste IP");
  Serial.print("WiFi.config: "); Serial.println(ok ? "OK" : "FEHLER");

  showWifiBootStatus("IP gesetzt", ip.toString(), wifiAutoFifthLastIpEnabled ? "Auto" : "Fest");
}

String getChipInfoText() {
  return "ESP32-C5";
}

void applyWifiBandPreference() {
  wifi_band_mode_t mode = WIFI_BAND_MODE_AUTO;

  if (wifiBandPreference == 1) {
    mode = WIFI_BAND_MODE_2G_ONLY;
  } else if (wifiBandPreference == 2) {
    mode = WIFI_BAND_MODE_5G_ONLY;
  } else {
    mode = WIFI_BAND_MODE_AUTO;
  }

  esp_err_t err = esp_wifi_set_band_mode(mode);

  Serial.print("WLAN Bandmodus ESP32-C5: ");
  if (mode == WIFI_BAND_MODE_2G_ONLY) Serial.print("nur 2,4 GHz");
  else if (mode == WIFI_BAND_MODE_5G_ONLY) Serial.print("nur 5 GHz");
  else Serial.print("Auto 2,4 + 5 GHz");
  Serial.print(" Ergebnis: ");
  Serial.println(err == ESP_OK ? "OK" : String((int)err));
}

void configureWifiRadio() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname("dynamic-price-clock");
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  applyWifiBandPreference();

  wifi_country_t country = {"DE", 1, 13, WIFI_COUNTRY_POLICY_AUTO};
  esp_wifi_set_country(&country);
}

void startSetupPortal(bool permanent) {
  setupApPermanent = permanent;

  WiFi.mode(WIFI_AP_STA);
  configureWifiRadio();
  delay(250);

  if (!setupApActive) {
    showWifiBootStatus("Setup WLAN", "startet", setupApSsid);
    bool ok = WiFi.softAP(setupApSsid.c_str(), apPassword.c_str());
    setupApActive = ok;
    setupApStartedAt = millis();

    if (ok) {
      Serial.print("Setup AP aktiv: ");
      Serial.println(setupApSsid);
      Serial.print("Setup AP Passwort: ");
      Serial.println(apPassword);
      Serial.print("Setup AP IP: ");
      Serial.println(WiFi.softAPIP());
      lastError = "Setup WLAN aktiv: " + WiFi.softAPIP().toString();
    } else {
      Serial.println("Setup AP konnte nicht gestartet werden");
      lastError = "Setup AP Fehler";
    }
  }

  if (setupApActive) {
    showWifiBootStatus("Setup aktiv", WiFi.softAPIP().toString(), permanent ? "bleibt aktiv" : "5 Min aktiv");
  }
}

void stopSetupPortalIfAllowed() {
  if (!setupApActive) return;
  if (setupApPermanent) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - setupApStartedAt < WIFI_SETUP_AP_TIMEOUT) return;

  Serial.println("Setup AP wird nach Timeout deaktiviert.");
  WiFi.softAPdisconnect(true);
  setupApActive = false;
  WiFi.mode(WIFI_STA);
  configureWifiRadio();
}

// Startet einen WLAN-Verbindungsversuch und kehrt SOFORT zurueck (kein
// delay()-Wartezyklus). Macht die fuer einen sauberen Verbindungsaufbau
// noetigen einmaligen Schritte (Setup-AP herunterfahren, Modus setzen,
// WiFi.begin()) und merkt sich den Fortschritt in globalen Variablen.
// Der eigentliche Fortschritt wird danach per wifiConnectPoll() abgefragt.
// Rueckgabe false = Eingabe ungueltig (keine SSID / Passwort zu kurz),
// in dem Fall wurde gar nicht erst versucht zu verbinden.
bool wifiConnectStart(const String &ssidInput, const String &passwordInput, unsigned long timeoutMs) {
  String ssid = ssidInput;
  ssid.trim();

  if (ssid.length() == 0) {
    showWifiBootStatus("Keine SSID", "Setup noetig", "");
    lastError = "WLAN SSID leer";
    return false;
  }

  if (passwordInput.length() > 0 && passwordInput.length() < 8) {
    showWifiBootStatus("Passwort Fehler", "min. 8 Zeichen", "");
    lastError = "WLAN Passwort zu kurz";
    return false;
  }

  Serial.println();
  Serial.println("=== WLAN VERBINDUNG (nicht blockierend) ===");
  Serial.print("SSID: '"); Serial.print(ssid); Serial.println("'");

  // Diese Schritte duerfen kurz blockieren (insgesamt < 1s): sie passieren
  // nur einmal beim Start eines Verbindungsversuchs, nicht waehrend des
  // Wartens auf das Verbindungsergebnis. Das unterscheidet sich von der
  // alten Implementierung, die bis zu 45s am Stueck blockiert hat.
  if (setupApActive) {
    Serial.println("Setup-AP wird fuer WLAN-Verbindung deaktiviert.");
    WiFi.softAPdisconnect(true);
    setupApActive = false;
    delay(300);
  }

  WiFi.disconnect(false, false);
  delay(300);
  WiFi.mode(WIFI_STA);
  configureWifiRadio();

  showWifiBootStatus("Verbinde WLAN", ssid, "bitte warten");

  prepareWifiIdentityAndIp();
  WiFi.begin(ssid.c_str(), passwordInput.c_str());

  wifiConnectActive = true;
  wifiConnectSsid = ssid;
  wifiConnectPasswordAttempt = passwordInput;
  wifiConnectStartedAt = millis();
  wifiConnectTimeoutMs = timeoutMs;
  wifiConnectLastStatus = WiFi.status();
  wifiConnectLastStatusPrintAt = millis();

  return true;
}

// Nicht-blockierende Fortschrittspruefung eines mit wifiConnectStart()
// gestarteten Verbindungsversuchs. Bei jedem loop()-Durchlauf einmal
// aufrufen, KEIN delay() darin. Rueckgabe: 0 = laeuft noch,
// 1 = erfolgreich verbunden, -1 = Timeout/Fehler.
int wifiConnectPoll() {
  if (!wifiConnectActive) return 0;

  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    wifiConnectActive = false;
    apMode = false;
    setupApPermanent = false;
    Serial.print("WLAN verbunden: "); Serial.println(WiFi.localIP());
    showWifiBootStatus("WLAN verbunden", WiFi.localIP().toString(), "RSSI " + String(WiFi.RSSI()));
    lastError = "WLAN verbunden: " + WiFi.localIP().toString();
    return 1;
  }

  if (millis() - wifiConnectStartedAt >= wifiConnectTimeoutMs) {
    wifiConnectActive = false;
    showWifiBootStatus("WLAN Fehler", wifiStatusToText(status), "Setup startet");
    lastError = "WLAN Fehler: " + wifiStatusToText(status);
    return -1;
  }

  if (status != wifiConnectLastStatus || millis() - wifiConnectLastStatusPrintAt > 2500) {
    wifiConnectLastStatus = status;
    wifiConnectLastStatusPrintAt = millis();
    showWifiBootStatus("Verbinde...", wifiStatusToText(status), wifiConnectSsid);
  }

  return 0;
}

// Wird bei jedem loop()-Durchlauf aufgerufen. Treibt einen laufenden
// Verbindungsversuch voran und reagiert auf Erfolg/Timeout, ohne den
// Webserver dabei zu blockieren.
void updateWifiConnectAttempt() {
  if (!wifiConnectActive) return;

  int result = wifiConnectPoll();

  if (result == -1) {
    setupApPermanent = true;
    apMode = true;
    startSetupPortal(true);
    showWifiBootStatus("WLAN Fehler", WiFi.softAPIP().toString(), "Setup aktiv");
  }
  // result == 1: wifiConnectPoll() hat bereits alles Noetige gesetzt.
  // result == 0: laeuft noch, naechster loop()-Durchlauf prueft erneut.
}

bool ensureWifiConnectedRobust() {
  wifiSsid.trim();

  if (wifiSsid.length() == 0) {
    setupApPermanent = true;
    apMode = true;
    startSetupPortal(true);
    showWifiBootStatus("Keine SSID", WiFi.softAPIP().toString(), "Setup aktiv");
    return false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    setupApPermanent = false;
    showWifiBootStatus("WLAN verbunden", WiFi.localIP().toString(), "bereits aktiv");
    return true;
  }

  // Boot-Phase: der Webserver laeuft noch nicht (server.begin() kommt
  // erst spaeter in setup()), darum darf hier weiterhin gewartet werden.
  // Intern nutzt das dieselbe nicht-blockierende Pollfunktion wie der
  // Laufzeitbetrieb, nur in einer lokalen Warteschleife.
  if (!wifiConnectStart(wifiSsid, wifiPassword, 45000UL)) {
    setupApPermanent = true;
    apMode = true;
    startSetupPortal(true);
    showWifiBootStatus("Setup aktiv", WiFi.softAPIP().toString(), "WLAN Fehler");
    return false;
  }

  int result = 0;
  while (result == 0) {
    delay(200);
    result = wifiConnectPoll();
  }

  if (result == 1) {
    return true;
  }

  setupApPermanent = true;
  apMode = true;
  startSetupPortal(true);
  showWifiBootStatus("Setup aktiv", WiFi.softAPIP().toString(), "WLAN Fehler");
  return false;
}

// Wird vom Webinterface (Speichern/Verbinden) ausgeloest. Startet den
// Verbindungsversuch nur noch und kehrt sofort zurueck - der Webserver
// bleibt waehrend des Verbindens durchgehend erreichbar. Den Fortschritt
// uebernimmt updateWifiConnectAttempt() in jedem loop()-Durchlauf.
void processPendingWifiReconnect() {
  if (!pendingWifiReconnect) return;
  if (millis() < pendingWifiReconnectAt) return;

  pendingWifiReconnect = false;

  Serial.println();
  Serial.println("=== WLAN NEU VERBINDEN NACH SPEICHERN (nicht blockierend) ===");

  if (!wifiConnectStart(wifiSsid, wifiPassword, 45000UL)) {
    setupApPermanent = true;
    apMode = true;
    startSetupPortal(true);
    showWifiBootStatus("WLAN Fehler", WiFi.softAPIP().toString(), "Setup aktiv");
  }
}

void initStatusRgbLed() {
#if STATUS_RGB_LED_ENABLED
  statusRgbLed.begin();
  statusRgbLed.setBrightness(35);
  statusRgbLed.clear();
  statusRgbLed.show();
#endif
}

void setStatusRgbLed(uint8_t r, uint8_t g, uint8_t b) {
#if STATUS_RGB_LED_ENABLED
  statusRgbLed.setPixelColor(0, statusRgbLed.Color(r, g, b));
  statusRgbLed.show();
#endif
}

void setStatusRgbGreen() {
  setStatusRgbLed(0, 80, 0);
}

void printSerialDivider(const char* title) {
  Serial.println();
  Serial.println("========================================");
  Serial.println(title);
  Serial.println("========================================");
}

void printWifiStatusLine(const char* prefix) {
  wl_status_t st = WiFi.status();
  Serial.print(prefix);
  Serial.print(" Status="); Serial.println(wifiStatusToText(st));

  if (st == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  }
}

void printStartupDiagnostics() {
  printSerialDivider("STARTDIAGNOSE ESP32-C5 DYNAMIC PRICE CLOCK");
  Serial.print("Hardware-Profil: "); Serial.println(HARDWARE_PROFILE);
  Serial.print("Free Heap: "); Serial.println(ESP.getFreeHeap());
  printWifiStatusLine("WLAN direkt nach Start:");
  Serial.println("========================================");
}

void delayedModuleStart(const char* name) {
  Serial.print("Starte Modul: "); Serial.println(name);
  delay(MODULE_START_DELAY_MS);
}

void moduleStartedOk(const char* name) {
  Serial.print("Modul OK: "); Serial.println(name);
}

void moduleSkipped(const char* name) {
  Serial.print("Modul deaktiviert: "); Serial.println(name);
}

void drawTftBootTest(Adafruit_GC9A01A &disp, const char* name, uint16_t color) {
  disp.fillScreen(color);
  delay(350);
  disp.fillScreen(DISPLAY_BLACK);
  disp.drawCircle(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, 112, DISPLAY_WHITE);
  disp.drawCircle(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, 100, color);
  disp.setTextColor(DISPLAY_WHITE);
  disp.setTextSize(2);
  disp.setCursor(42, 92);
  disp.print(name);
}

// Zeigt das Setup-WLAN (falls aktiv) bei jedem Boot direkt auf dem TFT an.
// Ergaenzt die Serial-Ausgabe fuer den Fall, dass niemand eine serielle
// Konsole offen hat.
void showBootCredentials() {
  if (displayBestOk) {
    displayBest.fillScreen(DISPLAY_BLACK);
    displayBest.setTextColor(DISPLAY_WHITE);

    displayBest.setTextSize(2);
    displayBest.setCursor(38, 36);
    displayBest.print("Setup-WLAN");

    displayBest.setTextSize(1);
    displayBest.setCursor(20, 78);
    displayBest.print("Netzwerkname:");

    displayBest.setTextSize(1);
    displayBest.setCursor(20, 96);
    displayBest.print(AP_SSID);

    displayBest.setTextSize(1);
    displayBest.setCursor(20, 138);
    displayBest.print("Passwort:");

    displayBest.setTextSize(2);
    displayBest.setCursor(20, 156);
    displayBest.print(apPassword);

    displayBest.setTextSize(1);
    displayBest.setCursor(20, 206);
    displayBest.print("Aenderbar unter /account");
  }

  Serial.println("Zugangsdaten werden " + String(BOOT_CREDENTIALS_HOLD_MS / 1000UL) + "s auf den Displays angezeigt.");
}

// -----------------------------------------------------------------------------
// Auth
// -----------------------------------------------------------------------------
// HTTP Basic Auth ist deaktiviert - das Geraet wird nur im lokalen Netzwerk
// genutzt, ein Login ist dort nicht noetig. Jeder Handler ruft weiterhin
// checkAuth() auf, es gibt aber immer Zugriff frei.
bool checkAuth() {
  return true;
}

// -----------------------------------------------------------------------------
// OTA-Rollback-Absicherung
// -----------------------------------------------------------------------------
//
// Der ESP32-Bootloader kann ein frisch geflashtes Firmware-Image automatisch
// wieder auf die vorherige (funktionierende) Partition zurueckrollen, wenn das
// neue Image abstuerzt, bevor es sich selbst als "gut" bestaetigt hat - siehe
// esp32-hal-misc.c im Arduino-ESP32-Core. Diese Absicherung ist fuer dieses
// Projekt bereits ueber das Partitionsschema (zwei OTA-App-Slots + otadata)
// aktiv, ABER die Standard-Implementierung bestaetigt das neue Image als
// gueltig SOFORT beim Boot, noch bevor setup() ueberhaupt laeuft - ein Absturz
// waehrend setup() (z.B. durch eine fehlerhafte Display-/Sensor-Initialisierung
// in einem kaputten Update) wuerde dadurch NICHT zum Rollback fuehren, weil
// das Image zu diesem Zeitpunkt schon als gueltig markiert wurde. Das
// verzoegert die Bestaetigung stattdessen auf "hat ein paar Sekunden echten
// Betrieb ueberstanden" (siehe otaRollbackConfirmed in loop()), damit die
// Absicherung tatsaechlich etwas bringt, statt nur dekorativ zu sein.
//
// WICHTIG: extern "C" ist hier zwingend - der schwache (weak) Hook wird in
// einer .c-Datei des Cores deklariert; eine C++-Definition ohne extern "C"
// wuerde per Name-Mangling einen anderen Symbolnamen erzeugen und den Hook
// stillschweigend NICHT ueberschreiben.
extern "C" bool verifyRollbackLater() {
  return true;
}
bool otaRollbackConfirmed = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.print("Profil: "); Serial.println(HARDWARE_PROFILE);

  initCpuLoadMonitor();

#if SAFE_BOOT_MODE
  Serial.println("SAFE_BOOT_MODE aktiv: TFT, Matrix, WS2812B/WS2818 und WLAN werden NICHT initialisiert.");
  return;
#endif

  initStatusRgbLed();
  setStatusRgbGreen();

  prefs.begin("tibber", false);

  bootstrapApPassword();

  wifiSsid = prefs.getString("wifiSsid", DEFAULT_WIFI_SSID);
  wifiPassword = prefs.getString("wifiPass", DEFAULT_WIFI_PASSWORD);
  webInterfaceName = prefs.getString("webName", "Dynamic Price Clock");
  if (webInterfaceName.length() == 0) webInterfaceName = "Dynamic Price Clock";
  accentColor = prefs.getString("accent", "green");
  if (accentColor != "blue" && accentColor != "green" && accentColor != "orange" && accentColor != "red" && accentColor != "pink" && accentColor != "purple" && accentColor != "teal" && accentColor != "indigo") accentColor = "green";
  appearanceMode = prefs.getString("appear", "solid");
  if (appearanceMode != "glass") appearanceMode = "solid";
  livePowerMaxKw = prefs.getFloat("lpMax", 10.0f);
  livePowerGreenKw = prefs.getFloat("lpGreen", 2.0f);
  livePowerYellowKw = prefs.getFloat("lpYellow", 5.0f);
  if (livePowerMaxKw < 1.0f) livePowerMaxKw = 10.0f;
  if (livePowerGreenKw < 0.1f || livePowerGreenKw >= livePowerMaxKw) livePowerGreenKw = livePowerMaxKw * 0.2f;
  if (livePowerYellowKw <= livePowerGreenKw || livePowerYellowKw >= livePowerMaxKw) livePowerYellowKw = livePowerMaxKw * 0.5f;
  kioskLivePowerStyle = prefs.getString("klpStyle", "text");
  if (kioskLivePowerStyle != "bar") kioskLivePowerStyle = "text";
  if (webInterfaceName.length() > 32) webInterfaceName = webInterfaceName.substring(0, 32);

  setupApSsid = prefs.getString("setupSsid", DEFAULT_WIFI_SETUP_AP_SSID);
  setupApSsid.trim();
  if (setupApSsid.length() == 0) setupApSsid = DEFAULT_WIFI_SETUP_AP_SSID;
  if (setupApSsid.length() > 32) setupApSsid = setupApSsid.substring(0, 32);

  wifiMacRotationEnabled = prefs.getBool("macRotate", false);
  wifiBandPreference = prefs.getInt("wifiBand", 0);
  if (wifiBandPreference < 0) wifiBandPreference = 0;
  if (wifiBandPreference > 2) wifiBandPreference = 2;
  wifiStaticIpEnabled = prefs.getBool("staticIpOn", false);
  wifiAutoFifthLastIpEnabled = prefs.getBool("auto5thIp", false);
  wifiStaticIpText = prefs.getString("staticIp", "");
  wifiGatewayText = prefs.getString("gateway", "");
  wifiSubnetText = prefs.getString("subnet", "255.255.255.0");
  wifiDns1Text = prefs.getString("dns1", "");
  wifiDns2Text = prefs.getString("dns2", "");
  tibberToken = prefs.getString("token", "");
  selectedHomeId = prefs.getString("homeId", "");
  tibberRootCaPem = prefs.getString("tibberCa", "");
  priceProvider = prefs.getString("priceProv", "tibber");
  if (priceProvider != "tibber" && priceProvider != "awattar_de" && priceProvider != "awattar_at") priceProvider = "tibber";
  priceSurchargeCt = prefs.getFloat("priceSurchg", 0.0);
  priceVatPercent = prefs.getFloat("priceVat", 0.0);
  tibberBaseFeeEur = prefs.getFloat("baseFee", 0.0);
  githubRepo = prefs.getString("ghRepo", "Alphascrypt/dynamic-price-clock");
  githubToken = prefs.getString("ghToken", "");

  ankerEmail = prefs.getString("ankerEmail", "");
  ankerPassword = prefs.getString("ankerPass", "");
  ankerSiteId = prefs.getString("ankerSite", "");
  ankerConfigured = (ankerEmail.length() > 0 && ankerPassword.length() > 0);

  tftSclkPin = prefs.getInt("tftSclkPin", 4);
  tftMosiPin = prefs.getInt("tftMosiPin", 5);
  ledRingPinVar = prefs.getInt("ledRingPin", 24);
  matrixCsPinVar = prefs.getInt("matrixCsPin", 21);
  if (tftSclkPin < 0 || tftSclkPin > 48) tftSclkPin = 4;
  if (tftMosiPin < 0 || tftMosiPin > 48) tftMosiPin = 5;
  if (ledRingPinVar < 0 || ledRingPinVar > 48) ledRingPinVar = 24;
  if (matrixCsPinVar < 0 || matrixCsPinVar > 48) matrixCsPinVar = 21;

  printStartupDiagnostics();

  apiUpdateMinutes = prefs.getInt("apiMinutes", 5);
  if (apiUpdateMinutes < 1) apiUpdateMinutes = 1;
  if (apiUpdateMinutes > 60) apiUpdateMinutes = 60;
  updateInterval = (unsigned long)apiUpdateMinutes * 60UL * 1000UL;

  displayRefreshSeconds = prefs.getInt("dispRefresh", 10);
  if (displayRefreshSeconds < 1) displayRefreshSeconds = 1;
  if (displayRefreshSeconds > 300) displayRefreshSeconds = 300;
  displayRefreshInterval = (unsigned long)displayRefreshSeconds * 1000UL;

  display1Enabled = prefs.getBool("d1Enabled", true);
  display2Enabled = prefs.getBool("d2Enabled", true);
  display1Mode = prefs.getInt("d1Mode", 0);
  display2Mode = prefs.getInt("d2Mode", 0);
  if (display1Mode < 0) display1Mode = 0;
  if (display1Mode > 4) display1Mode = 4;
  if (display2Mode < 0) display2Mode = 0;
  if (display2Mode > 4) display2Mode = 4;

  ledRingEnabled = prefs.getBool("ledOn", true);
  ledBrightness = prefs.getInt("ledBright", 40);
  if (ledBrightness < 0) ledBrightness = 0;
  if (ledBrightness > 255) ledBrightness = 255;

  ledRefreshSeconds = prefs.getInt("ledRefresh", 10);
  if (ledRefreshSeconds < 1) ledRefreshSeconds = 1;
  if (ledRefreshSeconds > 300) ledRefreshSeconds = 300;
  ledRefreshInterval = (unsigned long)ledRefreshSeconds * 1000UL;
  ledCheapColorId = prefs.getInt("ledCheapCol", 0);
  ledMidColorId = prefs.getInt("ledMidCol", 1);
  ledHighColorId = prefs.getInt("ledHighCol", 2);
  ledCurrentColorId = prefs.getInt("ledCurCol", 4);
  ledLowBlockColorId = prefs.getInt("ledLowCol", 3);

  for (int m = 0; m < MATRIX_DEVICE_COUNT; m++) {
    String onKey = "matrixOn" + String(m);
    String brightKey = "matrixBright" + String(m);
    String modeKey = "matrixMode" + String(m);

    matrixEnabled[m] = prefs.getBool(onKey.c_str(), false);
    matrixBrightness[m] = prefs.getInt(brightKey.c_str(), 6);
    if (matrixBrightness[m] < 0) matrixBrightness[m] = 0;
    if (matrixBrightness[m] > 15) matrixBrightness[m] = 15;

    matrixMode[m] = prefs.getInt(modeKey.c_str(), 8);
    if (matrixMode[m] < 0) matrixMode[m] = 0;
    if (matrixMode[m] > 8) matrixMode[m] = 8;
  }

  matrixRefreshSeconds = prefs.getInt("matrixRefresh", 5);
  if (matrixRefreshSeconds < 1) matrixRefreshSeconds = 1;
  if (matrixRefreshSeconds > 300) matrixRefreshSeconds = 300;
  matrixRefreshInterval = (unsigned long)matrixRefreshSeconds * 1000UL;

  displayPriceBarEnabled = prefs.getBool("priceBarOn", false);
  displayPriceBarMinCent = prefs.getInt("priceBarMin", 0);
  displayPriceBarMaxCent = prefs.getInt("priceBarMax", 60);
  displayPriceBarWidth = prefs.getInt("priceBarW", 10);
  displayPriceBarTextEnabled = prefs.getBool("priceBarText", false);
  display1DayChartEnabled = prefs.getBool("d1DayChart", false);
  display2DayChartEnabled = prefs.getBool("d2DayChart", false);
  displayDayChartX = prefs.getInt("dayChartX", 18);
  displayDayChartY = prefs.getInt("dayChartY", 154);
  displayDayChartWidth = prefs.getInt("dayChartW", 204);
  displayDayChartHeight = prefs.getInt("dayChartH", 76);
  if (displayDayChartX < 0) displayDayChartX = 0;
  if (displayDayChartX > SCREEN_WIDTH - 40) displayDayChartX = SCREEN_WIDTH - 40;
  if (displayDayChartY < 0) displayDayChartY = 0;
  if (displayDayChartY > SCREEN_HEIGHT - 30) displayDayChartY = SCREEN_HEIGHT - 30;
  if (displayDayChartWidth < 60) displayDayChartWidth = 60;
  if (displayDayChartWidth > SCREEN_WIDTH) displayDayChartWidth = SCREEN_WIDTH;
  if (displayDayChartHeight < 40) displayDayChartHeight = 40;
  if (displayDayChartHeight > 150) displayDayChartHeight = 150;
  if (displayDayChartX + displayDayChartWidth > SCREEN_WIDTH) displayDayChartX = SCREEN_WIDTH - displayDayChartWidth;
  if (displayDayChartY + displayDayChartHeight > SCREEN_HEIGHT) displayDayChartY = SCREEN_HEIGHT - displayDayChartHeight;
  display2CheapClockRingEnabled = prefs.getBool("d2ClockRing", true);
  display2CheapClockRingTextEnabled = prefs.getBool("d2ClockText", false);
  display2CheapClockRingLabelsEnabled = prefs.getBool("d2ClockLabels", true);
  display2CheapClockRingWidth = prefs.getInt("d2ClockRingW", 14);
  if (display2CheapClockRingWidth < 6) display2CheapClockRingWidth = 6;
  if (display2CheapClockRingWidth > 28) display2CheapClockRingWidth = 28;
  display2RingCheapColorId = prefs.getInt("d2RingCheap", 0);
  display2RingMidColorId = prefs.getInt("d2RingMid", 1);
  display2RingHighColorId = prefs.getInt("d2RingHigh", 2);
  display2RingBestColorId = prefs.getInt("d2RingBest", 4);
  display2RingCurrentColorId = prefs.getInt("d2RingNow", 3);
  if (display2RingCheapColorId < 0 || display2RingCheapColorId > 10) display2RingCheapColorId = 0;
  if (display2RingMidColorId < 0 || display2RingMidColorId > 10) display2RingMidColorId = 1;
  if (display2RingHighColorId < 0 || display2RingHighColorId > 10) display2RingHighColorId = 2;
  if (display2RingBestColorId < 0 || display2RingBestColorId > 10) display2RingBestColorId = 4;
  if (display2RingCurrentColorId < 0 || display2RingCurrentColorId > 10) display2RingCurrentColorId = 3;
  layoutMigratedTo240 = prefs.getBool("layout240", false);
  if (displayPriceBarMinCent < 0) displayPriceBarMinCent = 0;
  if (displayPriceBarMaxCent <= displayPriceBarMinCent) displayPriceBarMaxCent = displayPriceBarMinCent + 10;
  if (displayPriceBarWidth < 4) displayPriceBarWidth = 4;
  if (displayPriceBarWidth > 24) displayPriceBarWidth = 24;

  ledActiveCount = prefs.getInt("ledCount", LED_RING_COUNT);
  if (ledActiveCount != 24 && ledActiveCount != 60) ledActiveCount = LED_RING_COUNT;
  if (ledActiveCount > LED_RING_COUNT) ledActiveCount = LED_RING_COUNT;

  ledYellowCent = prefs.getInt("ledYellow", 30);
  ledRedCent = prefs.getInt("ledRed", 40);
  gaugeMinCent = prefs.getInt("gaugeMin", 0);
  gaugeMaxCent = prefs.getInt("gaugeMax", 60);
  if (ledYellowCent < 0) ledYellowCent = 0;
  if (ledRedCent < ledYellowCent) ledRedCent = ledYellowCent + 1;

  loadLayoutDefaults();
  loadLayoutFromPrefs();
  loadKioskLayoutFromPrefs();

  if (!layoutMigratedTo240) {
    for (int d = 1; d <= 2; d++) {
      LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;
      for (int i = 0; i < LAYOUT_ITEMS; i++) {
        layout[i].x = constrain(layout[i].x * 240 / 128, 0, SCREEN_WIDTH - 1);
        layout[i].y = constrain(layout[i].y * 240 / 64, 0, SCREEN_HEIGHT - 1);
        saveLayoutItem(d, i, layout[i]);
      }
    }
    prefs.putBool("layout240", true);
    layoutMigratedTo240 = true;
  }

  #if ENABLE_TFT_DISPLAYS
  delayedModuleStart("GC9A01 TFT Displays");

  if (TFT_BL_PIN >= 0) {
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);
  }

  pinMode(TFT1_CS_PIN, OUTPUT);
  pinMode(TFT2_CS_PIN, OUTPUT);
  pinMode(TFT_DC_PIN, OUTPUT);
  pinMode(TFT_RST_PIN, OUTPUT);

  digitalWrite(TFT1_CS_PIN, HIGH);
  digitalWrite(TFT2_CS_PIN, HIGH);
  digitalWrite(TFT_DC_PIN, HIGH);

  digitalWrite(TFT_RST_PIN, HIGH);
  delay(50);
  digitalWrite(TFT_RST_PIN, LOW);
  delay(80);
  digitalWrite(TFT_RST_PIN, HIGH);
  delay(180);

  SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, -1);
  delay(300);

  displayCurrent.begin(TFT_SPI_FREQUENCY);
  displayCurrent.setRotation(0);
  drawTftBootTest(displayCurrent, "TFT 1", DISPLAY_GREEN);
  displayCurrentOk = true;

#if DISPLAY2_ENABLED
  delay(600);
  displayBest.begin(TFT_SPI_FREQUENCY);
  displayBest.setRotation(0);
  drawTftBootTest(displayBest, "TFT 2", DISPLAY_BLUE);
  displayBestOk = true;
#else
  displayBestOk = false;
#endif
  moduleStartedOk("GC9A01 TFT Displays");
  showBootCredentials();
  delay(BOOT_CREDENTIALS_HOLD_MS);
#else
  displayCurrentOk = false;
  displayBestOk = false;
  moduleSkipped("GC9A01 TFT Displays");
#endif

  #if ENABLE_WS2812_RING
  delayedModuleStart("WS2812B/WS2818 Tagesring");
  ledRing.setPin(ledRingPinVar);
  ledRing.begin();
  ledRing.setBrightness(ledBrightness);
  ledRing.clear();
  ledRing.show();

  for (int i = 0; i < LED_RING_COUNT; i++) {
    ledRing.setPixelColor(i, ledRing.Color(0, 80, 0));
  }
  ledRing.show();
  delay(1200);
  ledRing.clear();
  ledRing.show();

  moduleStartedOk("WS2812B/WS2818 Tagesring");
#else
  moduleSkipped("WS2812B/WS2818 Tagesring");
#endif

#if ENABLE_MAX7219_MATRIX
  delayedModuleStart("MAX7219 Matrix Daisy-Chain");
  initPriceMatrix();
  updatePriceMatrix();
  moduleStartedOk("MAX7219 Matrix Daisy-Chain");
#else
  moduleSkipped("MAX7219 Matrix Daisy-Chain");
#endif

  #if ENABLE_WIFI_STARTUP
  delayedModuleStart("WLAN / Setup-AP");
  ensureWifiConnectedRobust();
  moduleStartedOk("WLAN / Setup-AP");
#else
  moduleSkipped("WLAN / Setup-AP");
#endif

#if ENABLE_TFT_DISPLAYS
  showLayoutDisplays();
#endif

  if (!apMode && WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  }

  server.on("/", handleRoot);
  server.on("/wifi", handleWifiPage);
  server.on("/kiosk", handleKioskPage);
  server.on("/kioskdata", HTTP_GET, handleKioskData);
  server.on("/kiosk2", handleKiosk2Page);
  server.on("/ankerdata", HTTP_GET, handleAnkerData);
  server.on("/gaugestatus", HTTP_GET, handleGaugeStatus);
  server.on("/kiosklayout", handleKioskLayoutPage);
  server.on("/savekiosklayoutajax", HTTP_POST, handleSaveKioskLayoutAjax);
  server.on("/resetkiosklayout", HTTP_POST, handleResetKioskLayout);
  server.on("/saveefnodepos", HTTP_POST, handleSaveEfNodePos);
  server.on("/pinout", handlePinoutPage);
  server.on("/savepins", HTTP_POST, handleSavePins);
  server.on("/restartdevice", HTTP_POST, handleRestartDevice);
  server.on("/checkgithubupdate", HTTP_GET, handleCheckGithubUpdate);
  server.on("/versioncheck", HTTP_GET, handleVersionCheck);
  server.on("/githubupdate", HTTP_POST, handleGithubUpdate);
  server.on("/otaprogress", HTTP_GET, handleOtaProgress);
  server.on("/uploadfirmware", HTTP_POST, handleUploadFirmware, handleUploadFirmwareData);
  server.on("/account", handleAccountPage);
  server.on("/anbieter", handleProviderPage);
  server.on("/saveproviderajax", HTTP_POST, handleSaveProviderAjax);
  server.on("/savetibberajax", HTTP_POST, handleSaveTibberAjax);
  server.on("/saveaccount", HTTP_POST, handleSaveAccount);
  server.on("/wifiscanjson", HTTP_GET, handleWifiScanJson);
  server.on("/wifistatusjson", HTTP_GET, handleWifiStatusJson);
  server.on("/savewifiajax", HTTP_POST, handleSaveWifiAjax);
  server.on("/connectwifiajax", HTTP_POST, handleConnectWifiAjax);
  server.on("/displays", handleDisplaysPage);
  server.on("/savedisplaychartajax", HTTP_POST, handleSaveDisplayChartAjax);
  server.on("/savedisplays", handleSaveDisplays);
  server.on("/ring", handleLedRingPage);
  server.on("/matrix", handleMatrixPage);
  server.on("/savematrix", HTTP_POST, handleSaveMatrix);
  server.on("/savematrixajax", HTTP_POST, handleSaveMatrixAjax);
  server.on("/savering", HTTP_POST, handleSaveLedRing);
  server.on("/saveringajax", HTTP_POST, handleSaveLedRingAjax);
  server.on("/resetwifi", handleResetWifi);
  server.on("/save", handleSave);
  server.on("/layout", handleDisplaysPage);
  server.on("/savelayout", HTTP_POST, handleSaveLayout);
  server.on("/savelayoutajax", HTTP_POST, handleSaveLayoutAjax);
  server.on("/savelayoutajax", HTTP_GET, handleSaveLayoutAjax);
  server.on("/resetlayout", handleResetLayout);
  server.on("/presetlayout", handlePresetLayout);
  server.on("/exportlayout", handleExportLayout);
  server.on("/importlayout", handleImportLayout);
  server.on("/refresh", handleRefresh);
  server.on("/json", handleJson);
  server.on("/style.css", handleStyleCss);
  server.on("/favicon.svg", HTTP_GET, handleFaviconSvg);
  server.on("/app.js", handleAppJs);
  server.on("/layout-editor.js", handleLayoutEditorJs);
  server.on("/account-update.js", handleAccountUpdateJs);
  server.on("/widget-engine.js", handleWidgetEngineJs);
  server.on("/livepower", HTTP_GET, handleLivePower);
  server.begin();

  if (!apMode) {
    updatePrices();
  } else {
    showLayoutDisplays();
  }
}

void loop() {
  updateCpuLoadMonitor();
#if SAFE_BOOT_MODE
  static unsigned long lastSafeBootPrint = 0;
  if (millis() - lastSafeBootPrint > 5000) {
    lastSafeBootPrint = millis();
    Serial.println("SAFE_BOOT_MODE laeuft.");
  }
  delay(50);
  return;
#endif
  server.handleClient();
  stopSetupPortalIfAllowed();
  processPendingWifiReconnect();
  updateWifiConnectAttempt();

  #if ENABLE_WIFI_STARTUP
  ensureWifiConnected();
  retryWifiFromPermanentApMode();
#endif

  // Siehe Kommentar bei verifyRollbackLater() weiter oben: die automatische
  // Rollback-Bestaetigung wurde bewusst verzoegert, damit ein Absturz waehrend
  // setup() noch zum Rollback auf die letzte funktionierende Version fuehrt.
  // Nach dieser Frist gilt die Firmware als stabil genug, um sie endgueltig
  // zu bestaetigen (kein Abwarten auf WLAN-Verbindung o.ae. - ein zeitweise
  // nicht erreichbarer Router soll keinen falschen Rollback ausloesen).
  if (!otaRollbackConfirmed && millis() > 45000UL) {
    esp_ota_mark_app_valid_cancel_rollback();
    otaRollbackConfirmed = true;
  }

  // Sicherheitsnetz: Falls der OTA-Vorgang aus irgendeinem Grund haengen
  // bleibt (z.B. Task haengt in einem Netzwerk-Timeout fest, ohne je den
  // Heartbeat zu aktualisieren), wuerden sonst Preis-/Live-Verbrauchs-/Anker-
  // Updates dauerhaft blockiert bleiben. Nach 3 Minuten ohne Heartbeat gilt
  // der OTA-Vorgang als haengen geblieben. WICHTIG: otaFail() wird hier mit
  // releaseOwner=false aufgerufen - die Sperre bleibt absichtlich bestehen
  // (blockiert weiterhin neue Update-Versuche), der eigentliche FreeRTOS-Task
  // wird hier NICHT beendet und koennte noch am Leben sein; ein zweiter,
  // parallel gestarteter Task wuerde gleichzeitig in dieselbe OTA-Partition
  // schreiben. Ein haengen gebliebener Task erfordert einen manuellen
  // Neustart, statt das Risiko eines doppelten Flash-Vorgangs einzugehen.
  // (Der GitHub-Pfad hat zusaetzlich einen esp_task_wdt-Hardware-Watchdog,
  // der einen echten Haenger automatisch per Reboot behebt - dieser
  // Software-Detektor bleibt trotzdem als zweite Ebene UND fuer den
  // manuellen Upload-Pfad bestehen, der keinen Hardware-Watchdog hat.)
  if (ota.owner != OtaOwner::None && ota.phase != OtaPhase::Failed && ota.phase != OtaPhase::Rebooting
      && ota.heartbeatMs > 0 && millis() - ota.heartbeatMs > 180000UL) {
    otaFail(ota.owner, "OTA-Vorgang haengen geblieben (kein Fortschritt seit 3 Minuten) - bitte Geraet manuell neu starten, bevor ein erneuter Versuch gestartet wird", false);
  }

  // Waehrend eines laufenden OTA-Downloads (manuell oder ueber GitHub) keine
  // weiteren TLS-Verbindungen aufbauen: Auf dem ESP32 konkurrieren mehrere
  // gleichzeitige HTTPS-Sitzungen um den knappen Heap, was den OTA-Download
  // mit "Connection refused/lost" abbrechen kann.
  if (!apMode && ota.owner == OtaOwner::None && millis() - lastUpdate >= updateInterval) {
    updatePrices();
  }

  if (!apMode && ota.owner == OtaOwner::None) {
    updateTibberLiveMeasurement();
  }

  if (!apMode && ota.owner == OtaOwner::None && ankerConfigured && millis() - lastAnkerPoll >= ANKER_POLL_INTERVAL_MS) {
    lastAnkerPoll = millis();
    updateAnkerSolarData();
  }

  // SPI-gebundenes Rendering (Displays/LED-Ring/Matrix) pausiert waehrend
  // eines laufenden OTA-Vorgangs ebenfalls: es konkurriert sonst um die
  // einzige CPU des ESP32-C5 mit dem Update-Task, waehrend Download+Flash
  // laufen.
  #if ENABLE_TFT_DISPLAYS
  if (ota.owner == OtaOwner::None && millis() - lastDisplayRefresh >= displayRefreshInterval) {
    lastDisplayRefresh = millis();
    showLayoutDisplays();
  }
#endif

  #if ENABLE_WS2812_RING
  if (ota.owner == OtaOwner::None && millis() - lastLedRefresh >= ledRefreshInterval) {
    lastLedRefresh = millis();
    updateLedRing();
  }
#endif

  #if ENABLE_MAX7219_MATRIX
  if (ota.owner == OtaOwner::None && millis() - lastMatrixRefresh >= matrixRefreshInterval) {
    lastMatrixRefresh = millis();
    updatePriceMatrix();
  }
#endif
}

// -----------------------------------------------------------------------------
// WLAN Reconnect (laufender Betrieb)
// -----------------------------------------------------------------------------

// Sanfter Reconnect bei kurzen Verbindungsaussetzern im laufenden Betrieb.
// Nicht-blockierend: WiFi.reconnect() wird einmal angestossen, das Ergebnis
// wird ueber mehrere loop()-Durchlaeufe per wifiSoftReconnectActive verfolgt
// statt in einer while(){delay()}-Schleife abgewartet.
void ensureWifiConnected() {
  static unsigned long disconnectedSince = 0;
  static unsigned long lastReconnectAttempt = 0;

  // Ein manueller Verbindungsversuch (neue Zugangsdaten) hat Vorrang -
  // nicht gleichzeitig per WiFi.reconnect() dazwischenfunken.
  if (wifiConnectActive) return;
  if (apMode) return;
  if (wifiSsid.length() == 0) return;

  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    disconnectedSince = 0;
    wifiSoftReconnectActive = false;
    return;
  }

  if (wifiSoftReconnectActive) {
    if (millis() - wifiSoftReconnectStartedAt >= 8000UL) {
      wifiSoftReconnectActive = false;
      lastError = "WLAN getrennt: " + wifiStatusToText(status);
    }
    return;
  }

  if (disconnectedSince == 0) {
    disconnectedSince = millis();
    return;
  }

  if (millis() - disconnectedSince < 15000) return;

  if (millis() - lastReconnectAttempt < 60000) return;
  lastReconnectAttempt = millis();

  Serial.print("WLAN seit ueber 15s getrennt, sanfter reconnect. Status: ");
  Serial.println(wifiStatusToText(status));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.reconnect();

  wifiSoftReconnectActive = true;
  wifiSoftReconnectStartedAt = millis();
}

// ensureWifiConnected() gibt sich sofort auf, sobald apMode aktiv ist (siehe
// dortiger fruehzeitiger return) - das Geraet landet dauerhaft im Setup-AP,
// wenn die gespeicherten Zugangsdaten beim Boot einmal fehlschlagen (z.B.
// Router war gerade neu gestartet), und versucht von selbst NIE wieder, sich
// mit ihnen zu verbinden, obwohl der Router laengst wieder erreichbar sein
// koennte - erfordert bisher immer manuelles Eingreifen ueber die Setup-Seite.
// Dieser Hintergrund-Retry probiert alle 5 Minuten erneut, aber nur wenn
// gerade niemand aktiv am Setup-AP konfiguriert (WiFi.softAPgetStationNum()
// == 0), damit ein laufender Konfigurationsvorgang nicht unterbrochen wird.
void retryWifiFromPermanentApMode() {
  static unsigned long lastPermanentApRetry = 0;

  if (!apMode || !setupApPermanent) return;
  if (wifiConnectActive) return;
  if (wifiSsid.length() == 0) return;
  if (millis() - lastPermanentApRetry < 300000UL) return;
  if (WiFi.softAPgetStationNum() > 0) return;

  lastPermanentApRetry = millis();
  Serial.println("Dauerhafter Setup-AP-Modus: automatischer Verbindungsversuch mit gespeicherten Zugangsdaten.");
  wifiConnectStart(wifiSsid, wifiPassword, 15000UL);
}

// -----------------------------------------------------------------------------
// Tibber API
// -----------------------------------------------------------------------------

void updateTibber() {
  lastUpdate = millis();

  if (apMode || WiFi.status() != WL_CONNECTED) {
    lastError = "Kein Internet / AP Modus";
    showLayoutDisplays();
    return;
  }

  if (tibberToken.length() < 10) {
    lastError = "Tibber Token fehlt";
    showLayoutDisplays();
    return;
  }

  showMessage(displayCurrent, displayCurrentOk, "Lade Preis...");
  showMessage(displayBest, displayBestOk, "Lade Daten...");

  WiFiClientSecure client;

  // TLS-Zertifikatspruefung: echtes Root-CA-Zertifikat nutzen, wenn der
  // Nutzer eins hinterlegt hat. Ohne hinterlegtes Zertifikat faellt das
  // Geraet auf setInsecure() zurueck, was bewusst als unsicher protokolliert
  // wird (kein stilles Downgrade).
  if (tibberRootCaPem.length() > 0) {
    client.setCACert(tibberRootCaPem.c_str());
  } else {
    client.setInsecure();
    Serial.println("WARNUNG: Kein Tibber-Root-CA hinterlegt, TLS-Zertifikat wird NICHT geprueft (setInsecure). Siehe /account.");
  }

  HTTPClient http;
  http.setTimeout(15000);

  if (!http.begin(client, TIBBER_URL)) {
    lastError = "HTTP Fehler";
    showLayoutDisplays();
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + tibberToken);

  String body =
    "{\"query\":\"{ viewer { homes { id appNickname features { realTimeConsumptionEnabled } currentSubscription { priceInfo(resolution: QUARTER_HOURLY) { current { total startsAt } today { total startsAt } tomorrow { total startsAt } } } consumption(resolution: DAILY, last: 40) { nodes { from cost consumption currency } } } } }\"}";

  int httpCode = http.POST(body);

  if (httpCode != HTTP_CODE_OK) {
    lastError = "Tibber HTTP " + String(httpCode);
    if (httpCode < 0) {
      // Negativer Code = Verbindungsfehler (z.B. TLS-Zertifikatspruefung fehlgeschlagen).
      lastError += " (" + String(http.errorToString(httpCode)) + ")";
    }
    Serial.println(http.getString());
    http.end();
    showLayoutDisplays();
    return;
  }

  String payload = http.getString();
  http.end();

  // Filter: nur die Felder deserialisieren die wir wirklich brauchen. Sonst
  // sprengt die DAILY-Konsum-Antwort mit 40 Nodes plus QUARTER_HOURLY-Preisen
  // fuer heute+morgen den Speicher (NoMemory).
  JsonDocument filter;
  JsonObject fHome = filter["data"]["viewer"]["homes"][0].to<JsonObject>();
  fHome["id"] = true;
  fHome["appNickname"] = true;
  fHome["features"]["realTimeConsumptionEnabled"] = true;
  JsonObject fPrice = fHome["currentSubscription"]["priceInfo"].to<JsonObject>();
  fPrice["current"]["total"] = true;
  fPrice["current"]["startsAt"] = true;
  fPrice["today"][0]["total"] = true;
  fPrice["today"][0]["startsAt"] = true;
  fPrice["tomorrow"][0]["total"] = true;
  fPrice["tomorrow"][0]["startsAt"] = true;
  JsonObject fCons = fHome["consumption"]["nodes"][0].to<JsonObject>();
  fCons["from"] = true;
  fCons["cost"] = true;
  fCons["consumption"] = true;
  fCons["currency"] = true;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));

  if (error) {
    lastError = "JSON Fehler: " + String(error.c_str());
    showLayoutDisplays();
    return;
  }

  JsonArray homes = doc["data"]["viewer"]["homes"];

  if (homes.isNull() || homes.size() == 0) {
    lastError = "Keine Homes";
    showLayoutDisplays();
    return;
  }

  homeCount = 0;

  for (JsonObject home : homes) {
    if (homeCount >= MAX_HOMES) break;

    String id = home["id"] | "";
    String nick = home["appNickname"] | "";

    if (id.length() > 0) {
      homeIds[homeCount] = id;

      if (nick.length() > 0) {
        homeNames[homeCount] = nick;
      } else {
        homeNames[homeCount] = "Home " + String(homeCount + 1);
      }

      homeCount++;
    }
  }

  JsonObject selectedSub;

  for (JsonObject home : homes) {
    String id = home["id"] | "";

    if (selectedHomeId.length() > 0 && id != selectedHomeId) {
      continue;
    }

    tibberRealTimeEnabledKnown = true;
    tibberRealTimeEnabled = home["features"]["realTimeConsumptionEnabled"] | false;

    JsonArray dayNodes = home["consumption"]["nodes"];
    if (!dayNodes.isNull() && dayNodes.size() > 0) {
      String monthPrefix = getLocalDatePrefix().substring(0, 7); // "YYYY-MM"
      float sumCost = 0.0f;
      float sumKwh = 0.0f;
      int daysCounted = 0;
      String curr = "";
      for (JsonObject dayNode : dayNodes) {
        String from = String((const char*)(dayNode["from"] | ""));
        if (from.length() < 7) continue;
        if (from.substring(0, 7) != monthPrefix) continue;
        float c = dayNode["cost"] | 0.0f;
        float k = dayNode["consumption"] | 0.0f;
        sumCost += c;
        sumKwh += k;
        daysCounted++;
        String cur = String((const char*)(dayNode["currency"] | ""));
        if (cur.length() > 0) curr = cur;
      }
      if (daysCounted > 0) {
        tibberMonthCost = sumCost;
        tibberMonthConsumptionKwh = sumKwh;
        tibberMonthCurrency = curr;
        tibberMonthDaysCounted = daysCounted;
      }
    }

    JsonObject sub = home["currentSubscription"];

    if (!sub.isNull()) {
      selectedSub = sub;

      if (selectedHomeId.length() == 0) {
        selectedHomeId = id;
        prefs.putString("homeId", selectedHomeId);
      }

      break;
    }
  }

  if (selectedSub.isNull()) {
    lastError = "Kein Vertrag fuer Home";
    showLayoutDisplays();
    return;
  }

  JsonObject priceInfo = selectedSub["priceInfo"];

  currentPrice = priceInfo["current"]["total"] | -1.0;
  currentStartsAt = String(priceInfo["current"]["startsAt"] | "");

  JsonArray today = priceInfo["today"];
  JsonArray tomorrow = priceInfo["tomorrow"];

  if (currentPrice < 0 || today.isNull() || today.size() == 0) {
    lastError = "Keine Today-Preisdaten";
    showLayoutDisplays();
    return;
  }

  quarterCount = 0;

  for (JsonObject node : today) {
    if (quarterCount >= MAX_QUARTERS) break;

    float price = node["total"] | -1.0;
    String startsAt = String(node["startsAt"] | "");

    if (price >= 0 && startsAt.length() > 0) {
      quarterPrices[quarterCount] = price;
      quarterTimes[quarterCount] = startsAt;
      quarterCount++;
    }
  }

  for (JsonObject node : tomorrow) {
    if (quarterCount >= MAX_QUARTERS) break;

    float price = node["total"] | -1.0;
    String startsAt = String(node["startsAt"] | "");

    if (price >= 0 && startsAt.length() > 0) {
      quarterPrices[quarterCount] = price;
      quarterTimes[quarterCount] = startsAt;
      quarterCount++;
    }
  }

  calculateMetrics();

  lastError = "";
  showLayoutDisplays();
  updatePriceMatrix();
}

// -----------------------------------------------------------------------------
// Tibber Pulse - Live-Verbrauch (GraphQL-Subscription per WebSocket)
// -----------------------------------------------------------------------------

// Startet/erhaelt die WebSocket-Verbindung, sobald Token+Home-ID bekannt
// sind. Wird jeden loop()-Durchlauf aufgerufen, die Guards machen das billig.
// ArduinoWebsockets hat (anders als die vorherige Bibliothek) keine eingebaute
// Auto-Reconnect-Funktion - der Verbindungsstatus wird hier selbst per Events
// nachgefuehrt und ein Wiederverbindungsversuch alle 15s unternommen.
void updateTibberLiveMeasurement() {
  if (priceProvider != "tibber" || tibberToken.length() < 10 || selectedHomeId.length() == 0) {
    if (tibberWsConnected) {
      tibberWs.close();
      tibberWsConnected = false;
      livePowerW = -1;
    }
    return;
  }

  if (!tibberWsInited) {
    tibberWs.onMessage(handleTibberWsMessage);
    tibberWs.onEvent(handleTibberWsEventCb);
    // WICHTIG: ArduinoWebsockets' setInsecure() ist auf ESP32 nachweislich
    // ein No-Op (loescht nur interne Zertifikat-Zeiger, ruft aber NIE
    // WiFiClientSecure::setInsecure() auf - siehe websockets_client.cpp,
    // Methode setInsecure() im #elif defined(ESP32)-Zweig). Ohne echtes
    // CACert schlaegt der TLS-Handshake deshalb IMMER fehl. Testweise mit
    // Amazons oeffentlichem Root (deckt *.tibber.com ab, per openssl
    // s_client gegen websocket-api.tibber.com verifiziert) statt unsicherer
    // Verbindung - falls tibberRootCaPem gesetzt ist, wird das bevorzugt.
    tibberWs.setCACert(tibberRootCaPem.length() > 0 ? tibberRootCaPem.c_str() : TIBBER_WS_FALLBACK_CA);
    tibberWs.addHeader("Sec-WebSocket-Protocol", "graphql-transport-ws");
    tibberWsInited = true;
  }

  if (!tibberWsConnected && millis() - tibberWsLastConnectAttemptMs >= 15000UL) {
    tibberWsLastConnectAttemptMs = millis();
    bool ok = tibberWs.connectSecure(TIBBER_WS_HOST, 443, TIBBER_WS_URL);
    if (ok) {
      tibberWsConnected = true;
      tibberWsLastEvent = "verbunden, sende connection_init";
      tibberWsLastEventMs = millis();
      String initMsg = "{\"type\":\"connection_init\",\"payload\":{\"token\":\"" + tibberToken + "\"}}";
      tibberWs.send(initMsg);
    } else {
      tibberWsLastEvent = "Verbindungsaufbau fehlgeschlagen";
      tibberWsLastEventMs = millis();
    }
  }

  if (tibberWsConnected) {
    tibberWs.poll();
  }

  if (tibberSubscribePending) {
    tibberSubscribePending = false;
    String subMsg =
      "{\"id\":\"live1\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription($homeId: ID!){ liveMeasurement(homeId: $homeId){ power } }\",\"variables\":{\"homeId\":\"" + selectedHomeId + "\"}}}";
    bool sendOk = tibberWs.send(subMsg);
    // Diagnose: Laenge und homeId-Praefix der TATSAECHLICH gesendeten
    // Nachricht sichtbar machen, um eine leere/falsche selectedHomeId an
    // dieser Stelle auszuschliessen, statt es nur anzunehmen.
    tibberWsLastEvent = "Abo gesendet (len=" + String(subMsg.length()) + ", homeId=" + selectedHomeId.substring(0, 8) + "..., send()=" + (sendOk ? "ok" : "FALSE") + "), warte auf Antwort";
    tibberWsLastEventMs = millis();
    unsigned long sentAtMs = tibberWsLastEventMs;

    // Testweise: statt bis zum naechsten regulaeren loop()-Durchlauf zu
    // warten, hier selbst bis zu 3s eng pollen. Traf schon beim OTA-Download
    // dieser Sitzung ein aehnliches TLS-Timing-Muster auf, moeglich, dass
    // die Antwort im normalen loop()-Rhythmus (mit Display-/LED-Refresh
    // dazwischen) verpasst wird, obwohl der Server sie zeitnah schickt.
    for (int i = 0; i < 40; i++) {
      delay(75);
      tibberWs.poll();
      if (tibberWsLastEventMs != sentAtMs) {
        // handleTibberWsMessage() hat tibberWsLastEvent inzwischen auf einen
        // NEUEN Wert aktualisiert - es kam tatsaechlich etwas an.
        break;
      }
    }
  }
}

void handleTibberWsEventCb(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionClosed) {
    tibberWsConnected = false;
    livePowerW = -1;
    tibberWsLastEvent = "getrennt";
    tibberWsLastEventMs = millis();
  }
  // ConnectionOpened wird nicht extra behandelt - connectSecure() liefert den
  // Erfolg direkt synchron zurueck (siehe updateTibberLiveMeasurement()).
}

void handleTibberWsMessage(WebsocketsMessage message) {
  if (!message.isText()) return;

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, message.data())) {
    tibberWsLastEvent = "JSON-Parse-Fehler bei eingehender Nachricht";
    tibberWsLastEventMs = millis();
    return;
  }

  String msgType = doc["type"] | "";

  if (msgType == "connection_ack") {
    tibberWsLastEvent = "connection_ack erhalten, Abo vorgemerkt";
    tibberWsLastEventMs = millis();
    // Nicht direkt hier senden (siehe tibberSubscribePending) - nur vormerken,
    // aus Vorsicht/Konsistenz zur alten Bibliothek beibehalten, auch wenn
    // unklar ist, ob ArduinoWebsockets dasselbe Problem haette.
    tibberSubscribePending = true;
  } else if (msgType == "next") {
    float p = doc["payload"]["data"]["liveMeasurement"]["power"] | -1.0;
    if (p >= 0) {
      livePowerW = p;
      livePowerUpdatedAtMs = millis();
      tibberWsLastEvent = "next: " + String(p, 0) + " W";
      tibberWsLastEventMs = millis();
    }
  } else if (msgType == "ping") {
    // Nur als Diagnose vermerken, WANN der letzte Ping kam (Alter wird
    // ueber tibberWsLastEventMs abgelesen) - unterscheidet "Verbindung lebt,
    // wartet nur auf echte Messwerte" von "seit der Subscription kam
    // ueberhaupt nichts mehr an".
    tibberWsLastEvent = "ping erhalten (Verbindung lebt)";
    tibberWsLastEventMs = millis();
    tibberWs.send("{\"type\":\"pong\"}");
  } else if (msgType == "error" || msgType == "connection_error") {
    // Server lehnt Verbindung/Subscription ab (z.B. abgelaufener Token,
    // falsche Home-ID, kein Pulse fuer diese Home-ID registriert) - bisher
    // stillschweigend verworfen, jetzt sichtbar statt "livePowerW bleibt
    // grundlos -1".
    String detail = doc["payload"]["message"] | doc["payload"].as<String>();
    tibberWsLastEvent = "Server-Fehler (" + msgType + "): " + detail;
    tibberWsLastEventMs = millis();
  } else {
    tibberWsLastEvent = "unbekannte Nachricht: " + msgType;
    tibberWsLastEventMs = millis();
  }
}

// -----------------------------------------------------------------------------
// aWATTar API (freie Alternative zu Tibber fuer Deutschland/Oesterreich)
// -----------------------------------------------------------------------------

void updateAwattarPrices() {
  lastUpdate = millis();

  if (apMode || WiFi.status() != WL_CONNECTED) {
    lastError = "Kein Internet / AP Modus";
    showLayoutDisplays();
    return;
  }

  showMessage(displayCurrent, displayCurrentOk, "Lade Preis...");
  showMessage(displayBest, displayBestOk, "Lade Daten...");

  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("WARNUNG: aWATTar-Verbindung laeuft ohne TLS-Zertifikatspruefung (setInsecure).");

  HTTPClient http;
  http.setTimeout(15000);

  String url = (priceProvider == "awattar_at")
    ? "https://api.awattar.at/v1/marketdata"
    : "https://api.awattar.de/v1/marketdata";

  if (!http.begin(client, url)) {
    lastError = "HTTP Fehler";
    showLayoutDisplays();
    return;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    lastError = "aWATTar HTTP " + String(httpCode);
    if (httpCode < 0) {
      lastError += " (" + String(http.errorToString(httpCode)) + ")";
    }
    http.end();
    showLayoutDisplays();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    lastError = "JSON Fehler: " + String(error.c_str());
    showLayoutDisplays();
    return;
  }

  JsonArray data = doc["data"];

  if (data.isNull() || data.size() == 0) {
    lastError = "Keine aWATTar-Preisdaten";
    showLayoutDisplays();
    return;
  }

  quarterCount = 0;

  long long nowMs = (long long)time(nullptr) * 1000LL;
  float foundCurrentPrice = -1.0;
  String foundCurrentStartsAt = "";
  const int quarterMinutes[4] = {0, 15, 30, 45};

  for (JsonObject node : data) {
    if (quarterCount + 4 > MAX_QUARTERS) break;

    long long startMs = node["start_timestamp"] | 0LL;
    float marketPriceEurPerMwh = node["marketprice"] | 0.0;

    if (startMs == 0) continue;

    // Boersenpreis (EUR/MWh) -> EUR/kWh, plus Aufschlag (Netzentgelte/Steuern) und MwSt.
    float pricePerKwh = ((marketPriceEurPerMwh / 1000.0) + (priceSurchargeCt / 100.0)) * (1.0 + priceVatPercent / 100.0);

    time_t hourEpoch = (time_t)(startMs / 1000LL);
    struct tm timeinfo;
    localtime_r(&hourEpoch, &timeinfo);

    char dateBuf[11];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &timeinfo);
    int hour = timeinfo.tm_hour;

    for (int q = 0; q < 4; q++) {
      char slotBuf[24];
      snprintf(slotBuf, sizeof(slotBuf), "%sT%02d:%02d:00", dateBuf, hour, quarterMinutes[q]);
      String slotIso = String(slotBuf);

      quarterPrices[quarterCount] = pricePerKwh;
      quarterTimes[quarterCount] = slotIso;

      long long slotMs = startMs + (long long)quarterMinutes[q] * 60000LL;

      if (slotMs <= nowMs && (nowMs - slotMs) < 15LL * 60LL * 1000LL) {
        foundCurrentPrice = pricePerKwh;
        foundCurrentStartsAt = slotIso;
      }

      quarterCount++;
    }
  }

  if (quarterCount == 0) {
    lastError = "Keine aWATTar-Preisdaten";
    showLayoutDisplays();
    return;
  }

  if (foundCurrentPrice >= 0) {
    currentPrice = foundCurrentPrice;
    currentStartsAt = foundCurrentStartsAt;
  } else {
    currentPrice = quarterPrices[0];
    currentStartsAt = quarterTimes[0];
  }

  calculateMetrics();

  lastError = "";
  showLayoutDisplays();
  updatePriceMatrix();
}

void updatePrices() {
  if (priceProvider == "awattar_de" || priceProvider == "awattar_at") {
    updateAwattarPrices();
  } else {
    updateTibber();
  }
}

// -----------------------------------------------------------------------------
// GitHub-Update
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// OTA-Zustandsautomat: Mutatoren
// -----------------------------------------------------------------------------
// Genau diese Funktionen duerfen ota.* nach Abschluss eines Versions-Checks
// veraendern. Jede prueft die Eigentuemerschaft (ota.owner) als allererstes -
// ein Aufrufer, der otaTryAcquire() nie erfolgreich aufgerufen hat (oder
// dessen Owner nicht mehr mit ota.owner uebereinstimmt, weil inzwischen ein
// anderer Vorgang die Sperre haelt), kann folglich NIE mehr otaFail()/
// otaProgress()/otaSetPhase()/otaFinishSuccess() fuer sich selbst wirksam
// aufrufen.

// Versucht die OTA-Sperre fuer `who` zu erhalten. Rueckgabe "" = erhalten;
// sonst ein fertiger, anzeigbarer Ablehnungsgrund (laeuft bereits ein anderer
// Vorgang, oder zu wenig freier Heap). Der Heap-Schwellenwert wird hier
// EINMAL fuer beide Pfade geprueft (frueher zweimal dupliziert, beim
// GitHub-Pfad sogar erst nach dem Start des FreeRTOS-Tasks).
String otaTryAcquire(OtaOwner who) {
  if (ota.owner != OtaOwner::None) {
    return "Es laeuft bereits ein anderer Update-Vorgang";
  }
  if (ESP.getFreeHeap() < 40000) {
    return "Zu wenig freier Speicher fuer Update (" + String(ESP.getFreeHeap() / 1024) + " KB frei, mind. 40 KB noetig)";
  }
  ota.owner = who;
  ota.bytesWritten = 0;
  ota.bytesTotal = 0;
  ota.error = "";
  ota.success = false;
  ota.heartbeatMs = millis();
  ota.startedAtMs = millis();
  return "";
}

// Owner-geprueftes Fortschritts-Update; no-op, wenn `who` gerade nicht (mehr)
// die Sperre haelt.
void otaProgress(OtaOwner who, int written, int total) {
  if (ota.owner != who) return;
  ota.bytesWritten = written;
  ota.bytesTotal = total;
  ota.heartbeatMs = millis();
}

void otaSetPhase(OtaOwner who, OtaPhase p) {
  if (ota.owner != who) return;
  ota.phase = p;
  ota.heartbeatMs = millis();
}

// Markiert den Vorgang als fehlgeschlagen. `releaseOwner=true` (Normalfall)
// gibt die Sperre sofort wieder frei, da der aufrufende Task/Handler in
// diesem Fall garantiert gleich beendet ist. Der loop()-Haenger-Detektor
// (siehe dort) ruft dagegen mit `releaseOwner=false` auf - dort ist NICHT
// sicher, ob der zugehoerige FreeRTOS-Task wirklich beendet ist (er koennte
// noch in einem Netzwerk-Timeout haengen und weiter auf die OTA-Partition
// schreiben), ein zweiter, parallel gestarteter Vorgang darf deshalb nicht
// zugelassen werden - das Geraet muss in diesem Fall manuell neugestartet
// werden. No-op, wenn `who` gar nicht (mehr) der Eigentuemer ist - macht den
// historischen Bug "abgebrochener, von vornherein abgewiesener Upload
// ueberschreibt den Fehlertext eines fremden laufenden Updates" dadurch
// strukturell unmoeglich statt nur per Konvention vermieden.
void otaFail(OtaOwner who, const String &msg, bool releaseOwner = true) {
  if (ota.owner != who) return;
  ota.error = msg;
  ota.success = false;
  ota.phase = OtaPhase::Failed;
  ota.heartbeatMs = millis();
  if (releaseOwner) ota.owner = OtaOwner::None;
}

// Markiert Erfolg. Gibt die Sperre bewusst NICHT frei - ein erfolgreicher
// Vorgang endet immer in ESP.restart(), das Geraet ist ohnehin in wenigen
// hundert Millisekunden weg; die Sperre soll bis zu diesem Neustart bestehen
// bleiben, damit in der kurzen Restzeit kein zweiter Vorgang starten kann.
void otaFinishSuccess(OtaOwner who) {
  if (ota.owner != who) return;
  ota.success = true;
  ota.phase = OtaPhase::Rebooting;
  ota.heartbeatMs = millis();
}

// Ersetzt checkGithubUpdate() (laut, 3 Versuche + Diagnose) UND
// checkGithubUpdateQuiet() (leise, 1 Versuch, kein Diagnose-Overhead fuer
// den alle-10-Minuten-Hintergrund-Poll, der von JEDER geladenen Seite aus
// feuert) - beide fragten bisher identischen Code zweimal gepflegt ab.
// withRetryAndDiag=true fuer den Button-ausgeloesten Check, false fuer den
// passiven Nav-Badge-Poll. Schreibt ausschliesslich in ota.* - NIE mehr in
// das allgemeine lastError: das war schon vorher nur ein Trick, damit ein
// Hintergrund-Fehler nicht den Status-OK/Fehler-Indikator der Startseite
// verfaelscht, kein Nebenlaeufigkeits-Schutz (WebServer ist single-threaded)
// - mit dem eigenen ota.checkError-Feld ist der Trick nicht mehr noetig.
bool otaCheckLatest(bool withRetryAndDiag) {
  ota.latestVersion = "";
  ota.latestUrl = "";

  if (githubRepo.length() == 0) {
    ota.checkError = "Kein GitHub-Repository hinterlegt";
    return false;
  }

  if (apMode || WiFi.status() != WL_CONNECTED) {
    ota.checkError = "Kein Internet / AP Modus";
    return false;
  }

  String url = "https://api.github.com/repos/" + githubRepo + "/releases/latest";
  if (withRetryAndDiag) {
    ota.diag = otaPreflightDiag("api.github.com");
  }

  // Der allererste TLS-Verbindungsaufbau auf dem ESP32-C5 schlaegt gelegentlich
  // mit "Connection refused" (HTTPClient-Code -1) fehl, ein sofortiger
  // erneuter Versuch klappt praktisch immer (gleiches Muster wie beim
  // OTA-Download) - deshalb bei withRetryAndDiag bis zu 3x versuchen statt
  // sofort aufzugeben; der leise Hintergrund-Poll bekommt beim naechsten
  // 10-Minuten-Intervall ohnehin einen neuen Versuch, daher dort nur 1
  // Versuch, um den Overhead fuer den Normalfall klein zu halten.
  int code = -1;
  const int maxAttempts = withRetryAndDiag ? 3 : 1;
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    WiFiClientSecure client;
    // Keine manuelle Root-CA-Pinnung fuer GitHub: der Download laeuft ueber
    // zwei verschiedene Hosts (api.github.com fuer die Metadaten, per Redirect
    // objects.githubusercontent.com fuer die eigentliche .bin-Datei) mit
    // rotierenden Zertifikaten - ein einzelnes gepinntes Zertifikat waere kaum
    // wartbar und wuerde ohne zusaetzlichen Nutzen regelmaessig brechen.
    client.setInsecure();
    client.setHandshakeTimeout(12000);

    HTTPClient http;
    http.setTimeout(withRetryAndDiag ? 15000 : 10000);

    if (!http.begin(client, url)) {
      ota.checkError = "GitHub HTTP Fehler";
      code = -1;
    } else {
      http.addHeader("User-Agent", "dynamic-price-clock-esp32");
      http.addHeader("Accept", "application/vnd.github+json");
      if (githubToken.length() > 0) {
        http.addHeader("Authorization", "Bearer " + githubToken);
      }

      code = http.GET();

      if (code == 200) {
        // Wachsendes JsonDocument statt fester Kapazitaet: die Release-JSON
        // (inkl. Release-Notes-Text und allen Asset-Eintraegen) kann je nach
        // Laenge der Notes/Anzahl Assets deutlich groesser werden als ein
        // frueher gewaehltes festes Limit - ein zu kleines Limit fuehrt zu
        // "GitHub JSON Fehler" (NoMemory), obwohl die Antwort valide ist.
        //
        // Antwort ERST komplett in einen String puffern (getString()), dann
        // daraus deserialisieren - NICHT direkt aus http.getStream() lesen.
        // Bei WiFiClientSecure/BearSSL kann stream.available() zwischen
        // TLS-Record-Grenzen kurzzeitig 0 zurueckgeben, obwohl noch weitere
        // Bytes unterwegs sind - deserializeJson() liest dann direkt vom
        // Stream faelschlich "Incomplete Input", bevor der komplette Body
        // angekommen ist. In der Praxis reproduzierbar: von 3 aufeinander-
        // folgenden echten Abfragen gegen die echte GitHub-API schlugen 2
        // genau mit "GitHub JSON Fehler" fehl, obwohl die Antwort beide Male
        // identisch und vollstaendig gueltig war. http.getString() wartet
        // dagegen zuverlaessig auf den kompletten Content-Length-Body, bevor
        // es zurueckkehrt - alle anderen HTTPS-JSON-Aufrufe in dieser Datei
        // (Anker, Tibber) nutzen bereits dieses Muster.
        String body = http.getString();
        JsonDocument doc;
        DeserializationError jsonErr = deserializeJson(doc, body);
        http.end();
        client.stop();

        if (jsonErr) {
          ota.checkError = "GitHub JSON Fehler";
          // Diese Schleife existiert genau fuer solche transienten Fehler
          // (siehe Kommentar oben bei getString()) - ein sofortiges
          // return false haette den verbleibenden Wiederholungsversuchen nie
          // eine Chance gegeben, obwohl attempt < maxAttempts war.
          if (attempt < maxAttempts) { delay(1500); continue; }
          return false;
        }

        String tag = String(doc["tag_name"] | "");
        tag.trim();
        if (tag.startsWith("v") || tag.startsWith("V")) tag = tag.substring(1);

        // Nur das App-only-.bin ist fuer Update.begin()/OTA geeignet - das
        // volle Flash-Abbild (Bootloader+Partitionstabelle+App, z.B. fuer
        // einen einmaligen USB-Reflash bei geaendertem Partitionsschema)
        // waere als OTA-Payload falsch dimensioniert und wuerde die
        // App-Partition mit falschem Byte-Offset ueberschreiben - deshalb
        // beide bekannten Namensmuster explizit ausschliessen, nicht nur
        // "merged" (der interne Build-Dateiname), da beim Hochladen auf
        // GitHub bisher "fullflash" statt "merged" im Namen verwendet wurde.
        String binUrl = "";
        JsonArray assets = doc["assets"];
        for (JsonObject asset : assets) {
          String name = String(asset["name"] | "");
          if (name.endsWith(".bin") && name.indexOf("merged") < 0 && name.indexOf("fullflash") < 0) {
            binUrl = String(asset["browser_download_url"] | "");
            break;
          }
        }

        if (tag.length() == 0) {
          ota.checkError = "Kein gueltiges Release gefunden";
          return false;
        }

        ota.latestVersion = tag;
        ota.latestUrl = binUrl;
        ota.checkError = "";
        return true;
      }

      http.end();
    }

    client.stop();
    if (attempt < maxAttempts) delay(1500);
  }

  ota.checkError = "GitHub API Fehler " + String(code);
  return false;
}

// -----------------------------------------------------------------------------
// Anker Solix Cloud (Solarbank) - inoffizielle Cloud-API
// -----------------------------------------------------------------------------
// Reverse-engineert aus Community-Projekten (ha-anker-solix). Anker bietet
// keine offizielle/lokale API an. Der Login verwendet ECDH(SECP256R1) +
// AES-256-CBC statt der bei anderen Anbietern ueblichen RSA-Verschluesselung.

static void ankerHexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  auto hv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (size_t i = 0; i < outLen; i++) {
    out[i] = (uint8_t)((hv(hex[i * 2]) << 4) | hv(hex[i * 2 + 1]));
  }
}

static String ankerBytesToHex(const uint8_t* data, size_t len) {
  static const char* hexChars = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hexChars[(data[i] >> 4) & 0xF];
    out += hexChars[data[i] & 0xF];
  }
  return out;
}

// Berechnet ein ECDH-Shared-Secret (SECP256R1) mit Ankers fest hinterlegtem
// Server-Public-Key und liefert zusaetzlich den eigenen (ephemeren)
// Public-Key als Hex-String, den der Server zur Berechnung desselben
// Secrets braucht.
static bool ankerEcdh(String &clientPubHexOut, uint8_t sharedSecretOut[32]) {
  const char* serverPubHex = "04c5c00c4f8d1197cc7c3167c52bf7acb054d722f0ef08dcd7e0883236e0d72a3868d9750cb47fa4619248f3d83f0f662671dadc6e2d31c2f41db0161651c7c076";
  size_t serverPubLen = strlen(serverPubHex) / 2;
  if (serverPubLen == 0 || serverPubLen > 65) return false;
  uint8_t serverPubBytes[65];
  ankerHexToBytes(serverPubHex, serverPubBytes, serverPubLen);

  mbedtls_ecp_group grp;
  mbedtls_ecp_point serverPub, clientPub, sharedPoint;
  mbedtls_mpi clientPriv;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctrDrbg;

  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&serverPub);
  mbedtls_ecp_point_init(&clientPub);
  mbedtls_ecp_point_init(&sharedPoint);
  mbedtls_mpi_init(&clientPriv);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctrDrbg);

  bool ok = true;
  const char* pers = "anker_ecdh";
  if (mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers)) != 0) ok = false;
  if (ok && mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) ok = false;
  if (ok && mbedtls_ecp_point_read_binary(&grp, &serverPub, serverPubBytes, serverPubLen) != 0) ok = false;
  if (ok && mbedtls_ecp_gen_keypair(&grp, &clientPriv, &clientPub, mbedtls_ctr_drbg_random, &ctrDrbg) != 0) ok = false;
  if (ok && mbedtls_ecp_mul(&grp, &sharedPoint, &clientPriv, &serverPub, mbedtls_ctr_drbg_random, &ctrDrbg) != 0) ok = false;

  if (ok) {
    // X-Koordinate ueber die oeffentliche Export-Funktion holen statt
    // direkt auf sharedPoint.X zuzugreifen - je nach mbedTLS-Build (z.B.
    // mit Hardware-Beschleunigung) sind die internen Felder nicht ueberall
    // gleich benannt/zugreifbar.
    uint8_t sharedRaw[65];
    size_t sharedRawLen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &sharedPoint, MBEDTLS_ECP_PF_UNCOMPRESSED, &sharedRawLen, sharedRaw, sizeof(sharedRaw)) != 0 || sharedRawLen != 65) {
      ok = false;
    } else {
      memcpy(sharedSecretOut, sharedRaw + 1, 32); // Byte 0 = 0x04-Praefix, dann X (32) + Y (32)
    }
  }

  if (ok) {
    uint8_t clientPubBytes[65];
    size_t outLen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &clientPub, MBEDTLS_ECP_PF_UNCOMPRESSED, &outLen, clientPubBytes, sizeof(clientPubBytes)) != 0) {
      ok = false;
    } else {
      clientPubHexOut = ankerBytesToHex(clientPubBytes, outLen);
    }
  }

  mbedtls_ecp_group_free(&grp);
  mbedtls_ecp_point_free(&serverPub);
  mbedtls_ecp_point_free(&clientPub);
  mbedtls_ecp_point_free(&sharedPoint);
  mbedtls_mpi_free(&clientPriv);
  mbedtls_ctr_drbg_free(&ctrDrbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

// AES-256-CBC mit PKCS7-Padding, Ergebnis Base64-kodiert. key=32 Byte, iv=16 Byte.
static String ankerAesEncryptB64(const uint8_t key[32], const uint8_t ivIn[16], const String &plain) {
  int plainLen = plain.length();
  int padLen = 16 - (plainLen % 16);
  int totalLen = plainLen + padLen;

  uint8_t *buf = (uint8_t*)malloc(totalLen);
  uint8_t *outBuf = (uint8_t*)malloc(totalLen);
  if (!buf || !outBuf) { if (buf) free(buf); if (outBuf) free(outBuf); return ""; }

  memcpy(buf, plain.c_str(), plainLen);
  for (int i = plainLen; i < totalLen; i++) buf[i] = (uint8_t)padLen;

  uint8_t ivCopy[16];
  memcpy(ivCopy, ivIn, 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 256);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, totalLen, ivCopy, buf, outBuf);
  mbedtls_aes_free(&aes);

  size_t b64Len = 0;
  mbedtls_base64_encode(NULL, 0, &b64Len, outBuf, totalLen);
  uint8_t *b64Buf = (uint8_t*)malloc(b64Len + 1);
  String result = "";
  if (b64Buf) {
    size_t written = 0;
    mbedtls_base64_encode(b64Buf, b64Len, &written, outBuf, totalLen);
    b64Buf[written] = 0;
    result = String((char*)b64Buf);
    free(b64Buf);
  }

  free(buf);
  free(outBuf);
  return result;
}

// Lokaler UTC-Offset in Sekunden (positiv = oestlich von UTC).
static long ankerTzOffsetSeconds() {
  time_t nowSec = time(nullptr);
  struct tm utcTm, localTm;
  gmtime_r(&nowSec, &utcTm);
  localtime_r(&nowSec, &localTm);
  time_t utcAsTime = mktime(&utcTm);
  time_t localAsTime = mktime(&localTm);
  return (long)difftime(localAsTime, utcAsTime);
}

// Format "GMT+01:00" / "GMT-03:30", wie von der Anker-Cloud als
// timezone-Header erwartet (siehe ha-anker-solix helpers.py getTimezoneGMTString()).
static String ankerTzGmtString() {
  long offsetSec = ankerTzOffsetSeconds();
  char sign = (offsetSec < 0) ? '-' : '+';
  long absSec = labs(offsetSec);
  int hh = absSec / 3600;
  int mm = (absSec % 3600) / 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "GMT%c%02d:%02d", sign, hh, mm);
  return String(buf);
}

// Gemeinsame Header, die die Anker-Cloud auf JEDER Anfrage erwartet (nicht
// nur beim Login) - ohne country/timezone/model-type/app-name/os-type
// antwortet der Server bei authentifizierten Endpunkten mit HTTP 500.
static void ankerAddCommonHeaders(HTTPClient &http, bool withAuth) {
  http.addHeader("content-type", "application/json");
  http.addHeader("model-type", "DESKTOP");
  http.addHeader("app-name", "anker_power");
  http.addHeader("os-type", "android");
  http.addHeader("country", "DE");
  http.addHeader("timezone", ankerTzGmtString());
  if (withAuth) {
    http.addHeader("gtoken", ankerGtoken);
    http.addHeader("x-auth-token", ankerAuthToken);
  }
}

// Meldet sich bei der Anker-Cloud an und speichert auth_token/user_id/gtoken
// im RAM (nicht in Preferences - Token sind kurzlebig, bei Bedarf Re-Login).
bool ankerLogin() {
  if (ankerEmail.length() == 0 || ankerPassword.length() == 0) {
    ankerLastError = "Keine Anker-Zugangsdaten hinterlegt";
    return false;
  }
  if (apMode || WiFi.status() != WL_CONNECTED) {
    ankerLastError = "Kein Internet / AP-Modus";
    return false;
  }

  String clientPubHex;
  uint8_t sharedSecret[32];
  if (!ankerEcdh(clientPubHex, sharedSecret)) {
    ankerLastError = "ECDH-Schluesselaustausch fehlgeschlagen";
    return false;
  }

  uint8_t iv[16];
  memcpy(iv, sharedSecret, 16);
  String encPassB64 = ankerAesEncryptB64(sharedSecret, iv, ankerPassword);
  if (encPassB64.length() == 0) {
    ankerLastError = "Passwort-Verschluesselung fehlgeschlagen";
    return false;
  }

  time_t nowSec = time(nullptr);
  unsigned long long transactionMs = (unsigned long long)nowSec * 1000ULL;
  long tzOffsetMs = ankerTzOffsetSeconds() * 1000L;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://ankerpower-api-eu.anker.com/passport/login");
  ankerAddCommonHeaders(http, false);

  DynamicJsonDocument reqDoc(1024);
  reqDoc["ab"] = "DE";
  reqDoc["client_secret_info"]["public_key"] = clientPubHex;
  reqDoc["enc"] = 0;
  reqDoc["email"] = ankerEmail;
  reqDoc["password"] = encPassB64;
  reqDoc["time_zone"] = tzOffsetMs;
  // transactionMs ist ein 64-Bit-Millisekunden-Zeitstempel - String((unsigned
  // long)transactionMs) wuerde ihn auf 32 Bit kappen (unsigned long ist auf
  // dem ESP32 32-Bit) und damit einen sinnlosen, umgelaufenen Wert erzeugen,
  // der nichts mehr mit der tatsaechlichen Uhrzeit zu tun hat.
  char transactionBuf[24];
  snprintf(transactionBuf, sizeof(transactionBuf), "%llu", transactionMs);
  reqDoc["transaction"] = String(transactionBuf);
  String reqBody;
  serializeJson(reqDoc, reqBody);

  int code = http.POST(reqBody);
  if (code != 200) {
    ankerLastError = "Login HTTP-Fehler " + String(code);
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();

  DynamicJsonDocument respDoc(2048);
  if (deserializeJson(respDoc, resp)) {
    ankerLastError = "Login: ungueltige Antwort";
    return false;
  }

  int retCode = respDoc["code"] | -1;
  if (retCode != 0) {
    ankerLastError = "Login fehlgeschlagen: " + respDoc["msg"].as<String>();
    return false;
  }

  ankerAuthToken = respDoc["data"]["auth_token"].as<String>();
  ankerUserId = respDoc["data"]["user_id"].as<String>();
  if (ankerAuthToken.length() == 0 || ankerUserId.length() == 0) {
    ankerLastError = "Login: Token fehlt in Antwort";
    return false;
  }

  uint8_t md5Out[16];
  mbedtls_md5((const unsigned char*)ankerUserId.c_str(), ankerUserId.length(), md5Out);
  ankerGtoken = ankerBytesToHex(md5Out, 16);

  ankerTokenObtainedMs = millis();
  ankerLastError = "";
  return true;
}

// Fuehrt einen authentifizierten POST gegen die Anker-Cloud aus (mit
// x-auth-token/gtoken-Headern). Loggt bei Bedarf automatisch neu ein bzw.
// bei 401 einmal erneut.
static bool ankerAuthedPost(const String &path, const String &bodyJson, DynamicJsonDocument &outDoc) {
  bool needLogin = (ankerAuthToken.length() == 0) || (millis() - ankerTokenObtainedMs > ANKER_TOKEN_LIFETIME_MS);
  if (needLogin && !ankerLogin()) return false;

  String url = "https://ankerpower-api-eu.anker.com/" + path;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  ankerAddCommonHeaders(http, true);
  int code = http.POST(bodyJson);

  if (code == 401) {
    http.end();
    if (!ankerLogin()) return false;
    http.begin(client, url);
    ankerAddCommonHeaders(http, true);
    code = http.POST(bodyJson);
  }

  if (code != 200) {
    ankerLastError = path + " HTTP-Fehler " + String(code);
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();
  if (path.indexOf("get_scen_info") >= 0) ankerLastRawJson = resp;

  if (deserializeJson(outDoc, resp)) {
    ankerLastError = path + ": ungueltige Antwort";
    return false;
  }
  int retCode = outDoc["code"] | -1;
  if (retCode != 0) {
    ankerLastError = path + " fehlgeschlagen: " + outDoc["msg"].as<String>();
    return false;
  }
  return true;
}

static bool ankerFetchSiteId() {
  DynamicJsonDocument doc(4096);
  if (!ankerAuthedPost("power_service/v1/site/get_site_list", "{}", doc)) return false;

  JsonArray sites = doc["data"]["site_list"].as<JsonArray>();
  if (sites.isNull() || sites.size() == 0) {
    ankerLastError = "Kein Standort im Anker-Konto gefunden";
    return false;
  }
  ankerSiteId = sites[0]["site_id"].as<String>();
  if (ankerSiteId.length() == 0) {
    ankerLastError = "site_id fehlt in Antwort";
    return false;
  }
  prefs.putString("ankerSite", ankerSiteId);
  return true;
}

// Fragt PV-Erzeugung, Batterie- und Ausgangsleistung sowie den
// Gesamt-Hausverbrauch ab. Wird alle ANKER_POLL_INTERVAL_MS aus loop()
// aufgerufen (siehe unten).
void updateAnkerSolarData() {
  if (!ankerConfigured) return;
  if (apMode || WiFi.status() != WL_CONNECTED) return;

  if (ankerSiteId.length() == 0 && !ankerFetchSiteId()) return;

  DynamicJsonDocument reqDoc(256);
  reqDoc["site_id"] = ankerSiteId;
  String reqBody;
  serializeJson(reqDoc, reqBody);

  DynamicJsonDocument doc(10240);
  if (!ankerAuthedPost("power_service/v1/site/get_scen_info", reqBody, doc)) return;

  // PV-Erzeugung, Batterie und Solarbank-Ausgang stecken NICHT direkt unter
  // "data" (nur home_load_power sitzt dort), sondern unter
  // data.solarbank_info - siehe reale Beispielantwort in ankerLastRawJson.
  JsonObject data = doc["data"];
  JsonObject solarbankInfo = data["solarbank_info"];
  ankerPvW = solarbankInfo["total_photovoltaic_power"].as<String>().toFloat();
  ankerOutputW = solarbankInfo["total_output_power"].as<String>().toFloat();

  // Netto-Batterieleistung (positiv=laedt, negativ=entlaedt) ueber alle
  // Solarbank-Geraete aufsummiert statt ueber "total_battery_power" (das ist
  // dort tatsaechlich der Ladezustand in Prozent/Fraktion, keine Leistung).
  // "battery_power" (Singular) pro Geraet ist dagegen bereits der echte
  // Ladezustand in Prozent (z.B. "95" = 95%), siehe reale Beispielantwort.
  float battNet = 0;
  ankerBatterySoc = -1;
  JsonArray sbList = solarbankInfo["solarbank_list"].as<JsonArray>();
  if (!sbList.isNull()) {
    for (JsonObject sb : sbList) {
      float chg = sb["bat_charge_power"].as<String>().toFloat();
      float dis = sb["bat_discharge_power"].as<String>().toFloat();
      battNet += (chg - dis);
    }
    if (sbList.size() > 0) {
      ankerBatterySoc = sbList[0]["battery_power"].as<String>().toInt();
    }
  } else {
    battNet = solarbankInfo["total_charging_power"].as<String>().toFloat();
  }
  ankerBatteryW = battNet;

  ankerHomeLoadW = data["home_load_power"].as<String>().toFloat();

  // Lebenszeit-Statistik (Gesamtertrag/CO2/Geld gespart) - steckt in
  // "statistics", type je Eintrag: 1=kWh, 2=kg CO2, 3=Euro.
  JsonArray stats = data["statistics"].as<JsonArray>();
  if (!stats.isNull()) {
    for (JsonObject s : stats) {
      String t = s["type"].as<String>();
      float total = s["total"].as<String>().toFloat();
      if (t == "1") ankerTotalYieldKwh = total;
      else if (t == "2") ankerCo2SavedKg = total;
      else if (t == "3") ankerMoneySavedEur = total;
    }
  }

  // Tagesertrag PV per Riemannsumme annaehern (siehe Kommentar bei den
  // globalen Variablen) - Datumswechsel setzt den Zaehler zurueck, ein
  // ungewoehnlich grosser Poll-Abstand (Neustart, laengere Unterbrechung)
  // wird verworfen statt faelschlich als durchgehende PV-Leistung
  // hochgerechnet zu werden.
  String todayStr = getLocalDatePrefix();
  if (todayStr.length() > 0) {
    if (pvYieldTodayDate != todayStr) {
      pvYieldTodayDate = todayStr;
      pvYieldTodayKwh = 0;
      lastPvIntegrationMs = millis();
    } else if (lastPvIntegrationMs > 0 && ankerPvW >= 0) {
      unsigned long dtMs = millis() - lastPvIntegrationMs;
      if (dtMs > 0 && dtMs < (unsigned long)ANKER_POLL_INTERVAL_MS * 3) {
        pvYieldTodayKwh += ankerPvW * (dtMs / 3600000.0f) / 1000.0f;
      }
      lastPvIntegrationMs = millis();
    } else {
      lastPvIntegrationMs = millis();
    }
  }

  ankerLastError = "";
}

// Extrahiert den Host-Teil aus einer https://host/pfad-URL, um DNS/Praeflight-
// Checks vor dem eigentlichen Verbindungsversuch machen zu koennen.
static String extractHostFromUrl(const String &url) {
  int start = url.indexOf("://");
  if (start < 0) return "";
  start += 3;
  int end = url.indexOf('/', start);
  if (end < 0) end = url.length();
  return url.substring(start, end);
}

// Sammelt Diagnosedaten (WLAN-Signal, freier Heap, DNS-Aufloesung) VOR einem
// Verbindungsversuch. Wird im Fehlerfall mit angezeigt, damit ein
// fehlgeschlagenes Update ueber die Web-Oberflaeche nachvollziehbar ist, ohne
// dass ein serieller Monitor angeschlossen werden muss.
String otaPreflightDiag(const String &host) {
  String diag = "RSSI " + String(WiFi.RSSI()) + " dBm, frei " + String(ESP.getFreeHeap() / 1024) + " KB";
  if (host.length() > 0) {
    IPAddress resolvedIp;
    bool dnsOk = WiFi.hostByName(host.c_str(), resolvedIp) == 1;
    diag += dnsOk ? (", DNS " + host + " -> " + resolvedIp.toString()) : (", DNS-Aufloesung fuer " + host + " fehlgeschlagen");
  }
  return diag;
}

// GitHub-Release-Asset-Links (github.com/.../releases/download/...) antworten
// immer mit einem 302 auf einen signierten objects.githubusercontent.com-Link.
// Die ESP32-HTTPClient-Bibliothek behandelt diesen Redirect intern ueber
// dieselbe WiFiClientSecure-Instanz - deren Keep-Alive-Reuse (Default: an)
// haelt den alten Socket zum ersten Host faelschlich offen, statt ihn beim
// Hostwechsel neu zu verbinden, was danach deterministisch "connection
// refused" auf JEDEN Versuch produziert (kein transientes Problem, siehe
// HTTPClient::disconnect()/connect() in der ESP32-Arduino-Core-Bibliothek).
// Umgehung: Redirect hier selbst mit einer eigenen, kurzlebigen Verbindung
// aufloesen und der eigentlichen Download-Verbindung direkt die finale
// CDN-URL uebergeben, damit sie nie den Host wechseln muss.
static String resolveGithubDownloadUrl(const String &url) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    return url;
  }
  http.addHeader("User-Agent", "dynamic-price-clock-esp32");
  if (githubToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + githubToken);
  }

  int code = http.GET();
  String resolved = url;
  if (code == HTTP_CODE_FOUND || code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_TEMPORARY_REDIRECT || code == 303) {
    String loc = http.getLocation();
    if (loc.length() > 0) {
      resolved = loc;
    }
  } else if (code <= 0) {
    // Live beobachtet: schlaegt schon diese leichte Vorab-Anfrage mit einem
    // Verbindungsfehler fehl (z.B. HTTPC_ERROR_CONNECTION_REFUSED), landete
    // der Aufrufer bisher stillschweigend bei der UNAUFGELOESTEN github.com-URL
    // und der eigentliche Downloadversuch ist dann prompt am selben
    // Verbindungsproblem gescheitert - mit einer irrefuehrenden Meldung
    // ("HTTP-Fehler -1 beim Download"), die wie ein Download-Problem aussah,
    // obwohl es ein Verbindungsproblem schon bei der Redirect-Aufloesung war.
    // Leerer Rueckgabewert macht das fuer den Aufrufer unterscheidbar.
    resolved = "";
  }
  http.end();
  return resolved;
}

#define OTA_WDT_TIMEOUT_MS 240000UL   // 4 Min - grosszuegig ueber DNS+Redirect+TLS-Handshake (~30s worst case) und ueber dem 180s-Software-Haenger-Detektor in loop(), damit dessen Hinweistext im UI zuerst erscheint, bevor der harte Reboot greift
#define DEFAULT_WDT_TIMEOUT_MS 5000UL // Projekt-Standard (siehe sdkconfig CONFIG_ESP_TASK_WDT_TIMEOUT_S=5), wird nach dem OTA-Versuch wiederhergestellt

// Meldet den GitHub-OTA-Task beim ESP-IDF Task-Watchdog an, mit grosszuegigem
// Timeout - reagiert der Task minutenlang gar nicht mehr (haengt z.B. in
// einem Netzwerk-Timeout fest, ohne dass otaDownloadToFlash() je Fortschritt
// meldet), loest das automatisch einen Reboot aus, statt wie bisher einen manuellen
// Stromausfall/Reset zu erfordern. Das ist sicher: Update.begin()/
// Update.write() schreiben ausschliesslich in die inaktive Partition, der
// eigentliche Boot-Partitions-Wechsel (esp_ota_set_boot_partition(), intern
// in Update.end(true)) passiert erst NACH einem vollstaendig erfolgreichen
// Abschluss - ein Reboot mitten im Download/Flash landet deshalb garantiert
// wieder auf dem alten, bereits bestaetigten Image (kein Bricking moeglich).
// WICHTIG: esp_task_wdt_init() darf hier NICHT aufgerufen werden - der
// Watchdog ist bereits beim Boot durch den Arduino-Core aktiv (sdkconfig:
// CONFIG_ESP_TASK_WDT_INIT=y), ein erneuter init()-Aufruf wuerde
// ESP_ERR_INVALID_STATE liefern; esp_task_wdt_reconfigure() ist hier richtig.
void otaWdtEnter() {
  esp_task_wdt_config_t cfg = { OTA_WDT_TIMEOUT_MS, 1, true };
  esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_add(NULL);
}
void otaWdtFeed() {
  esp_task_wdt_reset();
}
void otaWdtExit() {
  esp_task_wdt_delete(NULL);
  esp_task_wdt_config_t cfg = { DEFAULT_WDT_TIMEOUT_MS, 1, true };
  esp_task_wdt_reconfigure(&cfg);
}

// Liest den Binaer-Body eines bereits laufenden HTTP-GET manuell in
// Bloecken und schreibt sie direkt in die OTA-Partition - ersetzt den
// frueheren Aufruf von httpUpdate.update(), der intern denselben
// Stream::available()-Bug treffen kann, der schon bei der Versionspruefung
// (otaCheckLatest(), siehe dortiger Kommentar) reproduzierbar war: bei
// WiFiClientSecure/BearSSL kann available() zwischen TLS-Record-Grenzen
// kurzzeitig 0 liefern, obwohl noch Bytes unterwegs sind. Der manuelle
// Firmware-Upload (handleUploadFirmwareData()) hat dieses Problem nie, weil
// er nicht ueber eine TLS-Verbindung liest, sondern direkt vom
// WebServer-Upload-Callback.
//
// Live gegen das echte Geraet getestet (siehe Commit-Beschreibung): die
// Verbindung bricht bei der fuer BearSSL-Software-TLS auf dem ESP32-C5
// typischen langsamen Rate (~10-15 KB/s) reproduzierbar nach einer gewissen
// Zeit/Datenmenge ab (mehrfach fast an derselben Byte-Position, ~474 KB von
// 1,78 MB) - das ist kein zu kurzes Client-Timeout, sondern eine echte
// Verbindungsunterbrechung, die laenger warten allein nicht behebt. Deshalb
// schreibt *written hier fortlaufend in die Variable des Aufrufers zurueck:
// otaDownloadAndFlashGithub() nutzt das, um beim naechsten Versuch per
// HTTP-Range genau dort weiterzumachen, statt die Datei bei 0 neu
// anzufangen und wieder an derselben Stelle zu scheitern.
bool otaDownloadToFlash(HTTPClient &http, size_t &written, size_t contentLength) {
  NetworkClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  unsigned long lastDataMs = millis();

  while (written < contentLength) {
    size_t avail = stream->available();

    if (avail == 0) {
      if (!http.connected()) {
        ota.error = "Verbindung waehrend des Downloads verloren (" + String(written) + "/" + String(contentLength) + " Bytes)";
        return false;
      }
      // 45s ohne auch nur ein einziges neues Byte: kein TLS-Record-Grenzfall
      // mehr, sondern ein echter Haenger - der esp_task_wdt (siehe
      // otaWdtEnter()) greift zusaetzlich nach 4 Minuten als letztes
      // Sicherheitsnetz, falls diese Schleife selbst haengen bleiben sollte.
      if (millis() - lastDataMs > 45000UL) {
        ota.error = "Download haengt - seit 45s keine neuen Daten (" + String(written) + "/" + String(contentLength) + " Bytes)";
        return false;
      }
      delay(20);
      continue;
    }

    size_t toRead = (avail > sizeof(buf)) ? sizeof(buf) : avail;
    size_t got = stream->readBytes(buf, toRead);
    if (got == 0) {
      delay(10);
      continue;
    }

    if (Update.write(buf, got) != got) {
      ota.error = "Update.write fehlgeschlagen: " + String(Update.errorString());
      return false;
    }

    written += got;
    lastDataMs = millis();
    otaProgress(OtaOwner::Github, (int)written, (int)contentLength);
    otaWdtFeed();
  }

  if (!Update.end(true)) {
    ota.error = "Update.end fehlgeschlagen: " + String(Update.errorString());
    return false;
  }

  return true;
}

bool otaDownloadAndFlashGithub(String url) {
  if (url.length() == 0 || url.indexOf("github") < 0) {
    ota.error = "Ungueltige Update-URL";
    return false;
  }

  if (apMode || WiFi.status() != WL_CONNECTED) {
    ota.error = "Kein Internet / AP Modus";
    return false;
  }

  // Heap-Schwellenwert wird bereits einmalig in otaTryAcquire() geprueft,
  // bevor dieser Task ueberhaupt gestartet wird (siehe handleGithubUpdate()).

  String host = extractHostFromUrl(url);
  ota.diag = otaPreflightDiag(host);
  if (ota.diag.indexOf("fehlgeschlagen") >= 0) {
    ota.error = "DNS-Aufloesung fehlgeschlagen (" + ota.diag + ")";
    return false;
  }

  // Der Redirect-Hostwechsel wird jetzt vorab selbst aufgeloest
  // (resolveGithubDownloadUrl()), trotzdem hier mehrfach versuchen fuer
  // echte transiente Netzwerkfehler, statt den Nutzer manuell erneut
  // klicken zu lassen. Progressive Pausen (3.5s/5s/6.5s) geben dem Heap
  // Zeit, sich von der vorherigen TLS-Sitzung zu erholen.
  //
  // written/updateStarted leben AUSSERHALB der Schleife (nicht pro Versuch
  // neu) - live am echten Geraet beobachtet: die Verbindung bricht
  // reproduzierbar mitten im Download ab (siehe otaDownloadToFlash()), immer
  // wieder nahe derselben Byte-Position. Ohne Resume wuerde jeder Versuch bei
  // 0 neu anfangen und garantiert wieder an derselben Stelle scheitern. Mit
  // Range: bytes=written- macht der naechste Versuch stattdessen genau dort
  // weiter, wo der vorherige aufgehoert hat, und die Datei kommt ueber
  // mehrere kurze erfolgreiche Teilstuecke zusammen statt in einem Stueck.
  const int maxAttempts = 4;
  size_t written = 0;
  bool updateStarted = false;
  size_t fullContentLength = 0;

  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    otaWdtFeed(); // Watchdog-Reset zu Beginn jedes Versuchs, nicht nur bei jedem Fortschritts-Ticks
    {
      // Redirect selbst aufloesen (siehe resolveGithubDownloadUrl()), damit
      // die eigentliche Download-Verbindung nie den Host wechseln muss -
      // frisch pro Versuch, da signierte CDN-Links von GitHub nur kurz
      // gueltig sind.
      String downloadUrl = resolveGithubDownloadUrl(url);

      bool ok = false;
      if (downloadUrl.length() == 0) {
        // resolveGithubDownloadUrl() konnte nicht einmal die leichte
        // Vorab-Anfrage absetzen (echter Verbindungsfehler, nicht nur "kein
        // Redirect noetig") - live beobachtet als Ursache fuer irrefuehrende
        // "HTTP-Fehler -1"-Meldungen beim eigentlichen Download, obwohl das
        // Problem schon hier lag. Klar benennen statt mit der raten, ob
        // stillschweigend die unaufgeloeste URL weiterprobiert werden soll.
        ota.diag = otaPreflightDiag(extractHostFromUrl(url)) + ", Versuch " + String(attempt) + "/" + String(maxAttempts);
        ota.error = "Redirect-Aufloesung fehlgeschlagen (Verbindungsfehler)";
      } else {
      String downloadHost = extractHostFromUrl(downloadUrl);

      WiFiClientSecure client;
      client.setInsecure();
      // Explizites Handshake-Limit statt auf Bibliotheks-Default zu
      // vertrauen - verhindert, dass ein einzelner Verbindungsversuch
      // unbegrenzt haengen bleibt und den ganzen OTA-Task blockiert.
      client.setHandshakeTimeout(12000);
      otaProgress(OtaOwner::Github, (int)written, (int)fullContentLength);
      ota.diag = otaPreflightDiag(downloadHost) + ", Versuch " + String(attempt) + "/" + String(maxAttempts);

      HTTPClient http;
      http.setTimeout(15000);
      if (!http.begin(client, downloadUrl)) {
        ota.error = "Verbindung zum Download-Server fehlgeschlagen";
      } else {
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        if (updateStarted && written > 0) {
          http.addHeader("Range", "bytes=" + String(written) + "-");
        }
        int code = http.GET();
        if (code == HTTP_CODE_OK || code == HTTP_CODE_PARTIAL_CONTENT) {
          int len = http.getSize();
          bool serverHonoredRange = (code == HTTP_CODE_PARTIAL_CONTENT);
          if (updateStarted && written > 0 && !serverHonoredRange) {
            // Server ignoriert Range und schickt die Datei komplett von
            // vorne - die bereits geschriebene Partition passt dann nicht
            // mehr zu den ankommenden Bytes, also sauber neu anfangen.
            Update.abort();
            updateStarted = false;
            written = 0;
          }
          if (len > 0) {
            if (!updateStarted) {
              fullContentLength = serverHonoredRange ? (written + (size_t)len) : (size_t)len;
              if (Update.begin(fullContentLength)) {
                updateStarted = true;
              } else {
                ota.error = "Update.begin fehlgeschlagen: " + String(Update.errorString());
              }
            }
            if (updateStarted) {
              ok = otaDownloadToFlash(http, written, fullContentLength);
            }
          } else {
            ota.error = "Server hat keine Dateigroesse gemeldet";
          }
        } else {
          ota.error = "HTTP-Fehler " + String(code) + " beim Download";
        }
        http.end();
      }
      client.stop();
      } // downloadUrl.length() > 0

      if (ok) {
        return true;
      }

      ota.error += " (Versuch " + String(attempt) + "/" + String(maxAttempts) + ") [" + ota.diag + "]";
    }
    // client explizit vor der Pause aus dem Scope laufen lassen, damit der
    // TLS-Speicher (mbedTLS-Puffer, ~40KB je Sitzung) sicher freigegeben ist,
    // bevor der naechste Versuch startet - sonst kann der naechste
    // Verbindungsaufbau mangels Heap mit "Connection refused/lost" scheitern.
    if (attempt < maxAttempts) {
      delay(2000 + attempt * 1500);
    }
  }

  // Alle Versuche ausgeschoepft: begonnene OTA-Partition sauber freigeben,
  // statt sie fuer den naechsten kompletten Update-Anlauf (neuer Aufruf
  // dieser Funktion) belegt zu lassen.
  if (updateStarted) {
    Update.abort();
  }

  return false;
}

// Laeuft in einem eigenen FreeRTOS-Task, damit server.handleClient() im
// Hauptloop waehrend des Downloads weiterlaeuft und /otaprogress bedienen
// kann. otaPendingUrl wird von handleGithubUpdate() vor dem Task-Start gesetzt.
void otaGithubTask(void *param) {
  otaWdtEnter();
  otaSetPhase(OtaOwner::Github, OtaPhase::Downloading);
  bool ok = otaDownloadAndFlashGithub(otaPendingUrl);
  otaWdtExit();

  if (ok) {
    otaFinishSuccess(OtaOwner::Github);
    delay(800);
    ESP.restart();
  } else {
    otaFail(OtaOwner::Github, ota.error);
  }

  vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// Metriken
// -----------------------------------------------------------------------------

void calculateMetrics() {
  metricCurrent15 = -1.0;
  metricCurrent60 = -1.0;
  metricDayAvg = -1.0;
  metricLow15Day = -1.0;
  metricLow15DayTime = "";
  metricLow60Day = -1.0;
  metricLow60DayTime = "";
  metricSecondLow60Day = -1.0;
  metricSecondLow60DayTime = "";

  if (quarterCount == 0) return;

  String ref = "";

  if (currentStartsAt.length() >= 16) {
    ref = currentStartsAt.substring(0, 16);
  } else {
    ref = getCurrentIsoPrefix();
  }

  int currentIndex = 0;
  bool foundCurrent = false;

  for (int i = 0; i < quarterCount; i++) {
    if (quarterTimes[i].length() < 16) continue;

    String slotTime = quarterTimes[i].substring(0, 16);

    if (ref.length() > 0 && slotTime <= ref) {
      currentIndex = i;
      foundCurrent = true;
    }
  }

  if (!foundCurrent) {
    currentIndex = 0;
  }

  metricCurrent15 = currentPrice >= 0 ? currentPrice : quarterPrices[currentIndex];

  if (quarterCount >= 4) {
    int start60 = currentIndex;

    if (start60 > quarterCount - 4) {
      start60 = quarterCount - 4;
    }

    if (start60 < 0) {
      start60 = 0;
    }

    metricCurrent60 =
      (quarterPrices[start60] +
       quarterPrices[start60 + 1] +
       quarterPrices[start60 + 2] +
       quarterPrices[start60 + 3]) / 4.0;
  }

  float sum = 0.0;
  int count = 0;

  for (int i = 0; i < quarterCount; i++) {
    if (!isTodaySlot(quarterTimes[i])) continue;
    if (quarterPrices[i] < 0) continue;

    sum += quarterPrices[i];
    count++;

    if (metricLow15Day < 0 || quarterPrices[i] < metricLow15Day) {
      metricLow15Day = quarterPrices[i];
      metricLow15DayTime = quarterTimes[i];
    }
  }

  if (count > 0) {
    metricDayAvg = sum / count;
  }

  // Erster Durchlauf: das guenstigste 60-Minuten-Fenster (4 aufeinanderfolgende
  // 15-Minuten-Slots) finden.
  int low60Index = -1;
  for (int i = 0; i <= quarterCount - 4; i++) {
    if (!isTodaySlot(quarterTimes[i])) continue;
    if (!isTodaySlot(quarterTimes[i + 1])) continue;
    if (!isTodaySlot(quarterTimes[i + 2])) continue;
    if (!isTodaySlot(quarterTimes[i + 3])) continue;

    float avg =
      (quarterPrices[i] +
       quarterPrices[i + 1] +
       quarterPrices[i + 2] +
       quarterPrices[i + 3]) / 4.0;

    if (metricLow60Day < 0 || avg < metricLow60Day) {
      metricLow60Day = avg;
      metricLow60DayTime = quarterTimes[i];
      low60Index = i;
    }
  }

  // Zweiter Durchlauf: bestes 60-Minuten-Fenster suchen, das sich NICHT mit dem
  // eben gefundenen guenstigsten Fenster ueberschneidet. Fenster sind 4 Slots
  // lang, ueberschneiden sich also bei |i - low60Index| < 4 - der fruehere
  // Vergleich per exaktem Start-Zeitstempel liess fast identische, nur um
  // 15/30/45 Minuten verschobene Fenster faelschlich als "zweitguenstigstes"
  // Fenster durch.
  for (int i = 0; i <= quarterCount - 4; i++) {
    if (!isTodaySlot(quarterTimes[i])) continue;
    if (!isTodaySlot(quarterTimes[i + 1])) continue;
    if (!isTodaySlot(quarterTimes[i + 2])) continue;
    if (!isTodaySlot(quarterTimes[i + 3])) continue;
    if (low60Index >= 0 && abs(i - low60Index) < 4) continue;

    float avg =
      (quarterPrices[i] +
       quarterPrices[i + 1] +
       quarterPrices[i + 2] +
       quarterPrices[i + 3]) / 4.0;

    if (metricSecondLow60Day < 0 || avg < metricSecondLow60Day) {
      metricSecondLow60Day = avg;
      metricSecondLow60DayTime = quarterTimes[i];
    }
  }

  if (count == 0) {
    lastError = "Keine heutigen Slots gefunden";
  }
}

String priceToCentText(float price) {
  if (price < 0) return "--";
  return String(euroToCentRounded(price));
}

String euroCostText(float value) {
  if (value < 0) return "--";
  String s = String(value, 2);
  s.replace(".", ",");
  return s;
}

// Hochrechnung: Durchschnittskosten pro tatsaechlich vorhandenem Tag * Tage im Monat
// + Grundgebuehr. Nutzt tibberMonthDaysCounted (Anzahl Tage in der Tibber-Antwort
// fuer den aktuellen Monat), damit die Rechnung nicht davon abhaengt, ob Tibber
// den laufenden Tag mitschickt oder nicht.
float estimateFullMonthCost() {
  if (tibberMonthCost < 0 || tibberMonthDaysCounted <= 0) return -1.0;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 500)) return -1.0;

  int month = timeinfo.tm_mon;
  int year = timeinfo.tm_year + 1900;

  int daysInMonth = 31;
  if (month == 3 || month == 5 || month == 8 || month == 10) {
    daysInMonth = 30;
  } else if (month == 1) {
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    daysInMonth = leap ? 29 : 28;
  }

  float avgPerDay = tibberMonthCost / (float)tibberMonthDaysCounted;
  float projected = avgPerDay * (float)daysInMonth;
  return projected + tibberBaseFeeEur;
}

// Formatiert den aktuellen Verbrauch. Bis 1200 W in Watt, ab 1200 W in Kilowatt
// mit 2 Nachkommastellen (z.B. "1.23 kW"), Home-Assistant-Konvention. Leerer
// String, wenn kein Live-Wert vorliegt.
String formatLivePowerValue() {
  if (livePowerW < 0 || millis() - livePowerUpdatedAtMs > 60000) return "";
  if (livePowerW < 1200.0f) {
    return String((int)livePowerW) + " W";
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f kW", livePowerW / 1000.0f);
  return String(buf);
}

String getLayoutValue(String key) {
  if (key == "cpuLoad") return getCpuLoadText();
  if (key == "freeHeap") return getFreeHeapText();
  if (key == "uptime") return getUptimeText();
  if (key == "current15") return priceToCentText(metricCurrent15);
  if (key == "current60") return priceToCentText(metricCurrent60);
  if (key == "dayAvg") return priceToCentText(metricDayAvg);

  if (key == "low15Day") return priceToCentText(metricLow15Day);
  if (key == "low15DayTime") return formatTimeOnly(metricLow15DayTime);

  if (key == "low15DayFull") {
    if (metricLow15Day < 0) return "--";
    return priceToCentText(metricLow15Day) + " um " + formatTimeOnly(metricLow15DayTime);
  }

  if (key == "low60Day") return priceToCentText(metricLow60Day);
  if (key == "low60DayTime") return formatTimeOnly(metricLow60DayTime);

  if (key == "low60DayEndTime") {
    return addMinutesToIsoTime(metricLow60DayTime, 60);
  }

  if (key == "low60DayTimeRange") {
    if (metricLow60DayTime.length() < 16) return "";
    return formatTimeOnly(metricLow60DayTime) + "-" + addMinutesToIsoTime(metricLow60DayTime, 60);
  }

  if (key == "low60DayFull") {
    if (metricLow60Day < 0) return "--";
    return priceToCentText(metricLow60Day) + " ab " + formatTimeOnly(metricLow60DayTime);
  }

  if (key == "low60DayFullRange") {
    if (metricLow60Day < 0) return "--";
    return priceToCentText(metricLow60Day) + " " + formatTimeOnly(metricLow60DayTime) + "-" + addMinutesToIsoTime(metricLow60DayTime, 60);
  }

  if (key == "secondLow60Day") return priceToCentText(metricSecondLow60Day);
  if (key == "secondLow60DayTime") return formatTimeOnly(metricSecondLow60DayTime);

  if (key == "secondLow60DayEndTime") {
    return addMinutesToIsoTime(metricSecondLow60DayTime, 60);
  }

  if (key == "secondLow60DayTimeRange") {
    if (metricSecondLow60DayTime.length() < 16) return "";
    return formatTimeOnly(metricSecondLow60DayTime) + "-" + addMinutesToIsoTime(metricSecondLow60DayTime, 60);
  }

  if (key == "secondLow60DayFull") {
    if (metricSecondLow60Day < 0) return "--";
    return priceToCentText(metricSecondLow60Day) + " ab " + formatTimeOnly(metricSecondLow60DayTime);
  }

  if (key == "secondLow60DayFullRange") {
    if (metricSecondLow60Day < 0) return "--";
    return priceToCentText(metricSecondLow60Day) + " " + formatTimeOnly(metricSecondLow60DayTime) + "-" + addMinutesToIsoTime(metricSecondLow60DayTime, 60);
  }

  if (key == "ip") {
    if (apMode) return WiFi.softAPIP().toString();
    return WiFi.localIP().toString();
  }

  if (key == "time") {
    String now = getCurrentIsoPrefix();
    if (now.length() >= 16) return now.substring(11, 16);
    return "--:--";
  }

  if (key == "lastUpdate") return formatTimeOnly(currentStartsAt);

  if (key == "labelCurrent15") return "15 Min";
  if (key == "labelCurrent60") return "60 Min";
  if (key == "labelDayAvg") return "Durchschnitt";
  if (key == "labelLow15") return "Tief 15 Min";
  if (key == "labelLow60") return "Tief 60 Min";
  if (key == "labelIp") return "IP";
  if (key == "labelTime") return "Uhrzeit";
  if (key == "customText") return "";
  if (key == "error") return lastError;

  return "";
}

// -----------------------------------------------------------------------------
// Display Layout-Ausgabe mit automatischer Textskalierung
// -----------------------------------------------------------------------------

void drawLayoutDisplay(Adafruit_GC9A01A &disp, bool ok, LayoutItem layout[]) {
  if (!ok) return;

  disp.fillScreen(DISPLAY_BLACK);
  disp.setTextColor(DISPLAY_WHITE);

  for (int i = 0; i < LAYOUT_ITEMS; i++) {
    if (!layout[i].visible) continue;

    String value = getLayoutItemText(layout[i]);

    int maxSize = layout[i].size;
    if (maxSize < 1) maxSize = 1;
    if (maxSize > 4) maxSize = 4;

    int chosenSize = maxSize;
    if (layout[i].autoScale) {
      chosenSize = getBestTextSizeAligned(disp, value, layout[i].x, layout[i].y, maxSize, layout[i].align);
    }

    if (!fitsTextAligned(disp, value, layout[i].x, layout[i].y, chosenSize, layout[i].align)) {
      value = trimTextToFitAligned(disp, value, layout[i].x, layout[i].y, chosenSize, layout[i].align);
    }

    int drawX = getAlignedX(disp, value, layout[i].x, layout[i].y, chosenSize, layout[i].align);
    if (drawX < 0) drawX = 0;
    if (drawX > SCREEN_WIDTH - 1) drawX = SCREEN_WIDTH - 1;

    disp.setTextSize(chosenSize);
    disp.setCursor(drawX, layout[i].y);
    disp.print(value);
  }

  drawCurrentPriceBar(disp, metricCurrent15);
  if (&disp == &displayBest) {
    drawDisplay2CheapClockRing(disp);
  }
  drawOptionalDisplayCharts(disp);
}

void showSpecialDisplay(Adafruit_GC9A01A &disp, bool ok, int mode, const char* title) {
  if (!ok) return;

  disp.fillScreen(DISPLAY_BLACK);

  if (mode == 1) {
    return;
  }

  disp.setTextColor(DISPLAY_WHITE);
  disp.setTextSize(1);
  disp.setCursor(90, 20);
  disp.print(title);

  if (mode == 2) {
    String t = getDisplayTimeText();
    disp.setTextSize(4);
    int w = getTextWidth(disp, t, 4);
    disp.setCursor((SCREEN_WIDTH - w) / 2, 92);
    disp.print(t);
  }

  if (mode == 3) {
    String d = getDisplayDateText();
    disp.setTextSize(2);
    int w = getTextWidth(disp, d, 2);
    disp.setCursor((SCREEN_WIDTH - w) / 2, 102);
    disp.print(d);
  }

  if (mode == 4) {
    String d = getDisplayDateText();
    String t = getDisplayTimeText();

    disp.setTextSize(4);
    int tw = getTextWidth(disp, t, 4);
    disp.setCursor((SCREEN_WIDTH - tw) / 2, 78);
    disp.print(t);

    disp.setTextSize(2);
    int dw = getTextWidth(disp, d, 2);
    disp.setCursor((SCREEN_WIDTH - dw) / 2, 130);
    disp.print(d);
  }

  drawCurrentPriceBar(disp, metricCurrent15);
  if (&disp == &displayBest) {
    drawDisplay2CheapClockRing(disp);
  }
  drawOptionalDisplayCharts(disp);
}

void showLayoutDisplays() {
  if (!display1Enabled || display1Mode == 1) {
    showSpecialDisplay(displayCurrent, displayCurrentOk, 1, "Display 1");
  } else if (display1Mode == 0) {
    drawLayoutDisplay(displayCurrent, displayCurrentOk, d1Layout);
  } else {
    showSpecialDisplay(displayCurrent, displayCurrentOk, display1Mode, "Display 1");
  }

  if (!display2Enabled || display2Mode == 1) {
    showSpecialDisplay(displayBest, displayBestOk, 1, "Display 2");
  } else if (display2Mode == 0) {
    drawLayoutDisplay(displayBest, displayBestOk, d2Layout);
  } else {
    showSpecialDisplay(displayBest, displayBestOk, display2Mode, "Display 2");
  }
}

void clearLedRing() {
  ledRing.clear();
  ledRing.show();
}

uint32_t ledColorFromId(int colorId) {
  switch (colorId) {
    case 0: return ledRing.Color(0, 180, 0);
    case 1: return ledRing.Color(255, 180, 0);
    case 2: return ledRing.Color(255, 0, 0);
    case 3: return ledRing.Color(0, 0, 255);
    case 4: return ledRing.Color(255, 255, 255);
    case 5: return ledRing.Color(0, 180, 255);
    case 6: return ledRing.Color(180, 0, 255);
    case 7: return ledRing.Color(255, 80, 0);
    case 8: return ledRing.Color(255, 0, 120);
    case 9: return ledRing.Color(120, 120, 120);
    case 10: return ledRing.Color(0, 0, 0);
    default: return ledRing.Color(0, 180, 0);
  }
}

uint32_t priceToLedColor(int cent) {
  if (cent >= ledRedCent) {
    return ledColorFromId(ledHighColorId);
  }

  if (cent >= ledYellowCent) {
    return ledColorFromId(ledMidColorId);
  }

  return ledColorFromId(ledCheapColorId);
}

int mapDayLedToClockLed(int logicalLed, int activeCount) {
  if (activeCount <= 0) return logicalLed;

  int offset = activeCount / 2;
  int physicalLed = (logicalLed + offset) % activeCount;

  if (physicalLed < 0) physicalLed += activeCount;
  if (physicalLed >= LED_RING_COUNT) physicalLed = physicalLed % LED_RING_COUNT;

  return physicalLed;
}

void updateLedRing() {
  if (!ledRingEnabled) {
    clearLedRing();
    return;
  }

  if (quarterCount <= 0) {
    clearLedRing();
    return;
  }

  int todayIndices[MAX_QUARTERS];
  int todayCount = 0;

  for (int i = 0; i < quarterCount; i++) {
    if (!isTodaySlot(quarterTimes[i])) continue;
    if (quarterPrices[i] < 0) continue;

    todayIndices[todayCount] = i;
    todayCount++;

    if (todayCount >= MAX_QUARTERS) break;
  }

  if (todayCount <= 0) {
    clearLedRing();
    return;
  }

  ledRing.setBrightness(ledBrightness);

  for (int led = 0; led < LED_RING_COUNT; led++) {
    ledRing.setPixelColor(led, ledRing.Color(0, 0, 0));
  }

  int activeCount = ledActiveCount;
  if (activeCount < 1) activeCount = LED_RING_COUNT;
  if (activeCount > LED_RING_COUNT) activeCount = LED_RING_COUNT;

  for (int led = 0; led < activeCount; led++) {
    int startIndex = (led * todayCount) / activeCount;
    int endIndex = ((led + 1) * todayCount) / activeCount;

    if (endIndex <= startIndex) endIndex = startIndex + 1;
    if (endIndex > todayCount) endIndex = todayCount;

    float sum = 0.0;
    int count = 0;

    for (int j = startIndex; j < endIndex; j++) {
      int priceIndex = todayIndices[j];
      sum += quarterPrices[priceIndex];
      count++;
    }

    int cent = 0;
    if (count > 0) {
      cent = euroToCentRounded(sum / count);
    }

    int physicalLed = mapDayLedToClockLed(led, activeCount);
    ledRing.setPixelColor(physicalLed, priceToLedColor(cent));
  }

  if (currentStartsAt.length() >= 16) {
    String ref = currentStartsAt.substring(0, 16);
    int currentTodayPos = -1;

    for (int i = 0; i < todayCount; i++) {
      int priceIndex = todayIndices[i];
      if (quarterTimes[priceIndex].length() < 16) continue;

      if (quarterTimes[priceIndex].substring(0, 16) <= ref) {
        currentTodayPos = i;
      }
    }

    if (currentTodayPos >= 0) {
      int currentLed = (currentTodayPos * activeCount) / todayCount;
      if (currentLed < 0) currentLed = 0;
      if (currentLed >= activeCount) currentLed = activeCount - 1;
      int physicalCurrentLed = mapDayLedToClockLed(currentLed, activeCount);
      ledRing.setPixelColor(physicalCurrentLed, ledColorFromId(ledCurrentColorId));
    }
  }

  if (metricLow60DayTime.length() >= 16) {
    int lowTodayPos = -1;

    for (int i = 0; i < todayCount; i++) {
      int priceIndex = todayIndices[i];
      if (quarterTimes[priceIndex] == metricLow60DayTime) {
        lowTodayPos = i;
        break;
      }
    }

    if (lowTodayPos >= 0) {
      int lowLed = (lowTodayPos * activeCount) / todayCount;
      if (lowLed < 0) lowLed = 0;
      if (lowLed >= activeCount) lowLed = activeCount - 1;

      int physicalLowLed = mapDayLedToClockLed(lowLed, activeCount);
      ledRing.setPixelColor(physicalLowLed, ledColorFromId(ledLowBlockColorId));

      int nextLed = lowLed + 1;
      if (nextLed >= activeCount) nextLed = 0;
      int physicalNextLed = mapDayLedToClockLed(nextLed, activeCount);
      ledRing.setPixelColor(physicalNextLed, ledColorFromId(ledLowBlockColorId));
    }
  }

  ledRing.show();
}

String getLayoutItemText(LayoutItem item) {
  String value;

  if (item.key == "customText") {
    value = item.customText;
  } else {
    value = getLayoutValue(item.key);
  }

  return item.prefix + value + item.suffix;
}

String getPreviewText(LayoutItem item) {
  String value = getLayoutItemText(item);
  if (value.length() > 0) return value;
  if (item.key == "customText") return "Eigener Text";
  return item.key;
}

int getTextWidth(Adafruit_GC9A01A &disp, String text, int textSize) {
  int16_t x1, y1;
  uint16_t w, h;
  disp.setTextSize(textSize);
  disp.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

int getTextHeight(Adafruit_GC9A01A &disp, String text, int textSize) {
  int16_t x1, y1;
  uint16_t w, h;
  disp.setTextSize(textSize);
  disp.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return (int)h;
}

int getAlignedX(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int textSize, int align) {
  int w = getTextWidth(disp, text, textSize);
  if (align == 1) return anchorX - (w / 2);
  if (align == 2) return anchorX - w;
  return anchorX;
}

bool fitsTextAligned(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int textSize, int align) {
  int drawX = getAlignedX(disp, text, anchorX, y, textSize, align);
  int w = getTextWidth(disp, text, textSize);
  int h = getTextHeight(disp, text, textSize);

  if (drawX < 0) return false;
  if (drawX + w > SCREEN_WIDTH) return false;
  if (y + h > SCREEN_HEIGHT) return false;

  return true;
}

int getBestTextSizeAligned(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int maxSize, int align) {
  if (maxSize < 1) maxSize = 1;
  if (maxSize > 4) maxSize = 4;

  for (int s = maxSize; s >= 1; s--) {
    if (fitsTextAligned(disp, text, anchorX, y, s, align)) return s;
  }

  return 1;
}

String trimTextToFitAligned(Adafruit_GC9A01A &disp, String text, int anchorX, int y, int textSize, int align) {
  String result = text;

  while (result.length() > 0 && !fitsTextAligned(disp, result, anchorX, y, textSize, align)) {
    result.remove(result.length() - 1);
  }

  return result;
}

// -----------------------------------------------------------------------------
// Layout Defaults / Preferences
// -----------------------------------------------------------------------------

void loadLayoutDefaults() {
  // Entspricht dem Preset "Preis gross" (siehe applyLayoutPreset), damit die
  // Werkseinstellung/Reset direkt sinnvolle, ueberschneidungsfreie Positionen
  // zeigt statt der frueheren, zu eng gesetzten Default-Werte.
  d1Layout[0] = {"customText", "Aktueller Preis", "", "", 120, 36, 2, true, true, 1};
  d1Layout[1] = {"current15", "", "", "", 120, 86, 4, true, true, 1};
  d1Layout[2] = {"customText", "ct/kWh", "", "", 120, 138, 2, true, true, 1};
  d1Layout[3] = {"ip", "", "", "", 120, 182, 1, true, true, 1};
  d1Layout[4] = {"time", "", "", "", 120, 202, 2, true, true, 1};
  d1Layout[5] = {"lastUpdate", "", "Upd ", "", 120, 216, 1, false, true, 1};
  d1Layout[6] = {"current60", "", "AVG60 ", "", 120, 158, 1, false, true, 1};
  d1Layout[7] = {"error", "", "", "", 120, 16, 1, false, true, 1};

  d2Layout[0] = {"customText", "Tief 60 Min", "", "", 120, 34, 2, true, true, 1};
  d2Layout[1] = {"low60Day", "", "", "", 120, 76, 4, true, true, 1};
  d2Layout[2] = {"low60DayTimeRange", "", "", "", 120, 128, 2, true, true, 1};
  d2Layout[3] = {"customText", "2. Tief", "", "", 120, 162, 1, true, true, 1};
  d2Layout[4] = {"secondLow60DayFullRange", "", "", "", 120, 180, 1, true, true, 1};
  d2Layout[5] = {"dayAvg", "", "AVG ", "", 120, 204, 1, false, true, 1};
  d2Layout[6] = {"low15DayFull", "", "15m ", "", 120, 218, 1, false, true, 1};
  d2Layout[7] = {"error", "", "", "", 120, 16, 1, false, true, 1};
}

void loadLayoutFromPrefs() {
  for (int d = 1; d <= 2; d++) {
    LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;

    for (int i = 0; i < LAYOUT_ITEMS; i++) {
      String prefix = "d" + String(d) + "e" + String(i);

      layout[i].key = prefs.getString((prefix + "key").c_str(), layout[i].key);
      layout[i].customText = prefs.getString((prefix + "txt").c_str(), layout[i].customText);
      layout[i].prefix = prefs.getString((prefix + "pre").c_str(), layout[i].prefix);
      layout[i].suffix = prefs.getString((prefix + "suf").c_str(), layout[i].suffix);
      layout[i].x = prefs.getInt((prefix + "x").c_str(), layout[i].x);
      layout[i].y = prefs.getInt((prefix + "y").c_str(), layout[i].y);
      layout[i].size = prefs.getInt((prefix + "s").c_str(), layout[i].size);
      layout[i].visible = prefs.getBool((prefix + "v").c_str(), layout[i].visible);
      layout[i].autoScale = prefs.getBool((prefix + "a").c_str(), layout[i].autoScale);
      layout[i].align = prefs.getInt((prefix + "al").c_str(), layout[i].align);

      if (layout[i].x < 0) layout[i].x = 0;
      if (layout[i].x > SCREEN_WIDTH - 1) layout[i].x = SCREEN_WIDTH - 1;
      if (layout[i].y < 0) layout[i].y = 0;
      if (layout[i].y > SCREEN_HEIGHT - 1) layout[i].y = SCREEN_HEIGHT - 1;
      if (layout[i].size < 1) layout[i].size = 1;
      if (layout[i].size > 4) layout[i].size = 4;
      if (layout[i].align < 0) layout[i].align = 0;
      if (layout[i].align > 2) layout[i].align = 2;
    }
  }
}

void saveLayoutItem(int d, int i, LayoutItem item) {
  String prefix = "d" + String(d) + "e" + String(i);

  prefs.putString((prefix + "key").c_str(), item.key);
  prefs.putString((prefix + "txt").c_str(), item.customText);
  prefs.putString((prefix + "pre").c_str(), item.prefix);
  prefs.putString((prefix + "suf").c_str(), item.suffix);
  prefs.putInt((prefix + "x").c_str(), item.x);
  prefs.putInt((prefix + "y").c_str(), item.y);
  prefs.putInt((prefix + "s").c_str(), item.size);
  prefs.putBool((prefix + "v").c_str(), item.visible);
  prefs.putBool((prefix + "a").c_str(), item.autoScale);
  prefs.putInt((prefix + "al").c_str(), item.align);
}

void loadKioskLayoutFromPrefs() {
  // Neue Grid-Preferences: kgP<i>{c,s,r,p,v} = col,colSpan,row,rowSpan,visible.
  // Alte x/y/w/h-Werte (v1.x) werden ignoriert -> auf Grid-Defaults zurueckgesetzt.
  for (int i = 0; i < KIOSK_WIDGET_COUNT; i++) {
    String pPrefix = "kgP" + String(i);
    kioskPortrait[i].colStart = prefs.getUChar((pPrefix + "c").c_str(), KIOSK_PORTRAIT_DEFAULTS[i].colStart);
    kioskPortrait[i].colSpan  = prefs.getUChar((pPrefix + "s").c_str(), KIOSK_PORTRAIT_DEFAULTS[i].colSpan);
    kioskPortrait[i].rowStart = prefs.getUChar((pPrefix + "r").c_str(), KIOSK_PORTRAIT_DEFAULTS[i].rowStart);
    kioskPortrait[i].rowSpan  = prefs.getUChar((pPrefix + "p").c_str(), KIOSK_PORTRAIT_DEFAULTS[i].rowSpan);
    kioskPortrait[i].visible  = prefs.getBool((pPrefix + "v").c_str(), KIOSK_PORTRAIT_DEFAULTS[i].visible);

    String lPrefix = "kgL" + String(i);
    kioskLandscape[i].colStart = prefs.getUChar((lPrefix + "c").c_str(), KIOSK_LANDSCAPE_DEFAULTS[i].colStart);
    kioskLandscape[i].colSpan  = prefs.getUChar((lPrefix + "s").c_str(), KIOSK_LANDSCAPE_DEFAULTS[i].colSpan);
    kioskLandscape[i].rowStart = prefs.getUChar((lPrefix + "r").c_str(), KIOSK_LANDSCAPE_DEFAULTS[i].rowStart);
    kioskLandscape[i].rowSpan  = prefs.getUChar((lPrefix + "p").c_str(), KIOSK_LANDSCAPE_DEFAULTS[i].rowSpan);
    kioskLandscape[i].visible  = prefs.getBool((lPrefix + "v").c_str(), KIOSK_LANDSCAPE_DEFAULTS[i].visible);
  }

  // Einmalige Aufraeumaktion: die verwaisten k2P/k2L-Schluessel der urspruenglichen
  // 7-Widget-Generation (siehe Kommentar bei k2Pb/k2Lb unten) werden nie wieder
  // gelesen, belegen aber weiterhin NVS-Eintraege. Live nachgewiesen: diese ~70
  // toten Eintraege fragmentieren die Partition so stark, dass ein neuer Blob-Key
  // (efNodePos) mit ESP_ERR_NVS_NOT_ENOUGH_SPACE fehlschlaegt, obwohl
  // nvs_get_stats() insgesamt noch freie Eintraege zeigt (Seiten-Fragmentierung,
  // keine echte Erschoepfung). Per Flag-Key nur einmal ausgefuehrt.
  if (!prefs.getBool("k2CleanDone", false)) {
    const char* oldSuffixes[] = { "c", "s", "r", "p", "v" };
    for (int i = 0; i < 7; i++) {
      for (int s = 0; s < 5; s++) {
        prefs.remove(("k2P" + String(i) + oldSuffixes[s]).c_str());
        prefs.remove(("k2L" + String(i) + oldSuffixes[s]).c_str());
      }
    }
    prefs.putBool("k2CleanDone", true);
  }

  // Energiefluss-Kiosk (Kiosk 2): eigener Preferences-Praefix (k2Pb/k2Lb).
  // WICHTIG: "b" (statt des urspruenglichen k2P/k2L) ist bewusst ein NEUER
  // Praefix, kein Tippfehler - diese Werte werden pro ARRAY-INDEX gespeichert,
  // nicht pro Widget-Schluessel. Als das Widget-Set heute von
  // pv/battery/house/grid (7 Eintraege) auf energyflow/stats (5 Eintraege)
  // umgebaut wurde, haette Index 3 sonst stillschweigend die alte,
  // inzwischen bedeutungslose Position des FRUEHEREN Widgets an dieser
  // Stelle geerbt (live beobachtet: energyflow landete auf einer
  // Uralt-Position). Ein neuer Praefix macht das genauso sauber wie der
  // bereits bestehende Wechsel von den alten x/y/w/h-Keys auf kgP/kgL (siehe
  // Kommentar oben) - alte kiosk2-Eintraege werden schlicht nie wieder
  // gelesen, kein Migrationscode noetig.
  for (int i = 0; i < KIOSK2_WIDGET_COUNT; i++) {
    String pPrefix = "k2Pb" + String(i);
    kiosk2Portrait[i].colStart = prefs.getUChar((pPrefix + "c").c_str(), KIOSK2_PORTRAIT_DEFAULTS[i].colStart);
    kiosk2Portrait[i].colSpan  = prefs.getUChar((pPrefix + "s").c_str(), KIOSK2_PORTRAIT_DEFAULTS[i].colSpan);
    kiosk2Portrait[i].rowStart = prefs.getUChar((pPrefix + "r").c_str(), KIOSK2_PORTRAIT_DEFAULTS[i].rowStart);
    kiosk2Portrait[i].rowSpan  = prefs.getUChar((pPrefix + "p").c_str(), KIOSK2_PORTRAIT_DEFAULTS[i].rowSpan);
    kiosk2Portrait[i].visible  = prefs.getBool((pPrefix + "v").c_str(), KIOSK2_PORTRAIT_DEFAULTS[i].visible);

    String lPrefix2 = "k2Lb" + String(i);
    kiosk2Landscape[i].colStart = prefs.getUChar((lPrefix2 + "c").c_str(), KIOSK2_LANDSCAPE_DEFAULTS[i].colStart);
    kiosk2Landscape[i].colSpan  = prefs.getUChar((lPrefix2 + "s").c_str(), KIOSK2_LANDSCAPE_DEFAULTS[i].colSpan);
    kiosk2Landscape[i].rowStart = prefs.getUChar((lPrefix2 + "r").c_str(), KIOSK2_LANDSCAPE_DEFAULTS[i].rowStart);
    kiosk2Landscape[i].rowSpan  = prefs.getUChar((lPrefix2 + "p").c_str(), KIOSK2_LANDSCAPE_DEFAULTS[i].rowSpan);
    kiosk2Landscape[i].visible  = prefs.getBool((lPrefix2 + "v").c_str(), KIOSK2_LANDSCAPE_DEFAULTS[i].visible);
  }

  // Energiefluss-Knotenpositionen: EIN Blob-Key statt Einzel-Keys pro Knoten
  // (siehe Kommentar bei k2Pb/k2Lb weiter oben - genau dieser Fehler soll
  // sich hier nicht wiederholen, falls sich die Knotenanzahl je aendert).
  // Key bewusst "efNodePos2" statt "efNodePos": die Groesse (4x2 Floats,
  // 32 Byte) blieb beim Umstieg auf das breitere Koordinatensystem
  // (EF_CANVAS_W/H, vorher festes 400x400) GLEICH, der Groessen-Check allein
  // haette also alte, jetzt falsch skalierte Positionen unbemerkt uebernommen.
  size_t efNodeBytes = prefs.getBytesLength("efNodePos2");
  if (efNodeBytes == sizeof(efNodePos)) {
    prefs.getBytes("efNodePos2", efNodePos, sizeof(efNodePos));
  }
  // sonst: Default-Werte aus dem Initialisierer oben bleiben unveraendert.
  // Alten "efNodePos"-Key entfernen, damit er nicht als weiterer verwaister
  // Eintrag die NVS-Partition fragmentiert (siehe Kommentar bei k2CleanDone
  // oben) - remove() auf einen bereits geloeschten/nicht existenten Key ist
  // ein sicherer No-Op, daher hier ohne eigenen Einmal-Flag.
  prefs.remove("efNodePos");

  // Startseiten-Dashboard: das ganze Array als EIN Blob-Key statt 5
  // Sub-Keys pro Widget (siehe Kommentar bei der homeLayout-Deklaration) -
  // die NVS-Partition ist nur 20KB gross und kiosk1+kiosk2 belegen darin
  // bereits ueber 100 Eintraege.
  size_t homeBytes = prefs.getBytesLength("homeLay");
  if (homeBytes == sizeof(homeLayout)) {
    prefs.getBytes("homeLay", homeLayout, sizeof(homeLayout));
  } else {
    memcpy(homeLayout, HOME_DEFAULTS, sizeof(homeLayout));
  }
}

static void saveKioskWidgetGeneric(const String &prefix, KioskWidgetLayout item) {
  prefs.putUChar((prefix + "c").c_str(), item.colStart);
  prefs.putUChar((prefix + "s").c_str(), item.colSpan);
  prefs.putUChar((prefix + "r").c_str(), item.rowStart);
  prefs.putUChar((prefix + "p").c_str(), item.rowSpan);
  prefs.putBool((prefix + "v").c_str(), item.visible);
}

void saveKioskWidget(bool landscape, int i, KioskWidgetLayout item) {
  saveKioskWidgetGeneric((landscape ? "kgL" : "kgP") + String(i), item);
}

void saveKiosk2Widget(bool landscape, int i, KioskWidgetLayout item) {
  // k2Pb/k2Lb, nicht k2P/k2L - siehe Kommentar in loadKioskLayoutFromPrefs().
  saveKioskWidgetGeneric((landscape ? "k2Lb" : "k2Pb") + String(i), item);
}

void saveHomeLayoutToPrefs() {
  prefs.putBytes("homeLay", (uint8_t*)homeLayout, sizeof(homeLayout));
}

void saveEfNodePosToPrefs() {
  size_t written = prefs.putBytes("efNodePos2", (uint8_t*)efNodePos, sizeof(efNodePos));
  if (written != sizeof(efNodePos)) {
    // Ein Overwrite eines bestehenden Blob-Keys legt intern einen neuen
    // Eintrag an und markiert den alten nur als "erloescht", nicht sofort
    // freigegeben - bei einem oft verschobenen Knoten (viele Overwrites auf
    // denselben Key) sammeln sich auf der zugehoerigen NVS-Seite so ungenutzte,
    // aber noch nicht kompaktierte Eintraege an, bis ein Schreibversuch mit
    // ESP_ERR_NVS_NOT_ENOUGH_SPACE fehlschlaegt (live beobachtet), obwohl die
    // Partition insgesamt noch freie Eintraege hat. Ein expliziter remove()
    // vor dem Retry stoesst die noetige Seiten-Kompaktierung an.
    prefs.remove("efNodePos2");
    prefs.putBytes("efNodePos2", (uint8_t*)efNodePos, sizeof(efNodePos));
  }
}

// Speichert die Position GENAU EINES Hub-Diagramm-Knotens (0=PV, 1=Batterie,
// 2=Netz, 3=Haus), wird beim Loslassen nach dem Verschieben im Anordnen-
// Modus aufgerufen (siehe efNodeDragEnd() im Kiosk-2-JS). Koordinaten
// werden auf den viewBox-Bereich (mit etwas Rand fuer den jeweiligen
// Docking-Radius) geclampt, damit ein Knoten nicht komplett aus dem
// sichtbaren Bereich gezogen werden kann.
void handleSaveEfNodePos() {
  if (!checkAuth()) return;

  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  if (idx < 0 || idx >= 4) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Ungueltiger Knoten-Index\"}");
    return;
  }

  float x = server.hasArg("x") ? server.arg("x").toFloat() : efNodePos[idx].x;
  float y = server.hasArg("y") ? server.arg("y").toFloat() : efNodePos[idx].y;
  float r = EF_NODE_RADIUS[idx];
  if (x < r) x = r;
  if (x > EF_CANVAS_W - r) x = EF_CANVAS_W - r;
  if (y < r) y = r;
  if (y > EF_CANVAS_H - r) y = EF_CANVAS_H - r;

  efNodePos[idx].x = x;
  efNodePos[idx].y = y;
  saveEfNodePosToPrefs();

  server.send(200, "application/json", "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// Systemmonitor / CPU-Last
// -----------------------------------------------------------------------------

void cpuIdleCounterTask(void* parameter) {
  while (true) {
    cpuIdleCounter++;
    taskYIELD();
  }
}

void initCpuLoadMonitor() {
  cpuIdleCounter = 0;
  cpuIdleCounterLast = 0;
  cpuIdleCounterMaxDelta = 1;
  cpuLoadPercent = 0;
  cpuLoadLastSample = millis();

  if (cpuIdleTaskHandle == NULL) {
    xTaskCreate(cpuIdleCounterTask, "cpuIdle", 2048, NULL, 0, &cpuIdleTaskHandle);
  }
}

void updateCpuLoadMonitor() {
  unsigned long now = millis();
  if (now - cpuLoadLastSample < 1000UL) return;

  uint32_t current = cpuIdleCounter;
  uint32_t delta = current - cpuIdleCounterLast;
  cpuIdleCounterLast = current;
  cpuLoadLastSample = now;

  if (delta > cpuIdleCounterMaxDelta) cpuIdleCounterMaxDelta = delta;
  if (cpuIdleCounterMaxDelta < 1) cpuIdleCounterMaxDelta = 1;

  float idleRatio = (float)delta / (float)cpuIdleCounterMaxDelta;
  if (idleRatio < 0) idleRatio = 0;
  if (idleRatio > 1) idleRatio = 1;

  cpuLoadPercent = (1.0f - idleRatio) * 100.0f;
  if (cpuLoadPercent < 0) cpuLoadPercent = 0;
  if (cpuLoadPercent > 100) cpuLoadPercent = 100;
}

String getCpuLoadText() {
  return String((int)round(cpuLoadPercent)) + "%";
}

String getFreeHeapText() {
  return String((int)(ESP.getFreeHeap() / 1024)) + " KB";
}

String getUptimeText() {
  unsigned long s = millis() / 1000UL;
  unsigned long h = s / 3600UL;
  unsigned long m = (s % 3600UL) / 60UL;

  String out = "";
  if (h < 10) out += "0";
  out += String(h);
  out += ":";
  if (m < 10) out += "0";
  out += String(m);
  return out;
}

// -----------------------------------------------------------------------------
// Runder Display-Preisrandbalken
// -----------------------------------------------------------------------------

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t priceBarGradientColor(int value, int minValue, int maxValue) {
  if (maxValue <= minValue) maxValue = minValue + 1;
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;

  float t = (float)(value - minValue) / (float)(maxValue - minValue);

  uint8_t r, g, b;

  if (t < 0.333f) {
    float k = t / 0.333f;
    r = (uint8_t)(0 + k * 255);
    g = 200;
    b = 0;
  } else if (t < 0.666f) {
    float k = (t - 0.333f) / 0.333f;
    r = 255;
    g = (uint8_t)(200 - k * 80);
    b = 0;
  } else {
    float k = (t - 0.666f) / 0.334f;
    r = 255;
    g = (uint8_t)(120 - k * 120);
    b = 0;
  }

  return rgb565(r, g, b);
}

uint16_t tftColorFromId(int colorId) {
  switch (colorId) {
    case 0: return rgb565(0, 180, 0);
    case 1: return rgb565(255, 180, 0);
    case 2: return rgb565(255, 0, 0);
    case 3: return rgb565(0, 0, 255);
    case 4: return rgb565(255, 255, 255);
    case 5: return rgb565(0, 180, 255);
    case 6: return rgb565(180, 0, 255);
    case 7: return rgb565(255, 80, 0);
    case 8: return rgb565(255, 0, 120);
    case 9: return rgb565(120, 120, 120);
    case 10: return DISPLAY_BLACK;
    default: return rgb565(0, 180, 0);
  }
}

uint16_t tftBlendColor(uint16_t c1, uint16_t c2, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;

  uint8_t r1 = ((c1 >> 11) & 0x1F) << 3;
  uint8_t g1 = ((c1 >> 5) & 0x3F) << 2;
  uint8_t b1 = (c1 & 0x1F) << 3;

  uint8_t r2 = ((c2 >> 11) & 0x1F) << 3;
  uint8_t g2 = ((c2 >> 5) & 0x3F) << 2;
  uint8_t b2 = (c2 & 0x1F) << 3;

  uint8_t r = (uint8_t)(r1 + (r2 - r1) * t);
  uint8_t g = (uint8_t)(g1 + (g2 - g1) * t);
  uint8_t b = (uint8_t)(b1 + (b2 - b1) * t);

  return rgb565(r, g, b);
}

uint16_t tftPriceRingColor(int value, int minValue, int maxValue) {
  if (maxValue <= minValue) maxValue = minValue + 1;
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;

  float t = (float)(value - minValue) / (float)(maxValue - minValue);
  uint16_t cheapCol = tftColorFromId(display2RingCheapColorId);
  uint16_t midCol = tftColorFromId(display2RingMidColorId);
  uint16_t highCol = tftColorFromId(display2RingHighColorId);

  if (t < 0.5f) {
    return tftBlendColor(cheapCol, midCol, t / 0.5f);
  }
  return tftBlendColor(midCol, highCol, (t - 0.5f) / 0.5f);
}

void drawCurrentPriceBar(Adafruit_GC9A01A &disp, float priceEuro) {
  if (!displayPriceBarEnabled) return;

  int cent = euroToCentRounded(priceEuro);
  if (cent < 0) return;

  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int outerR = (SCREEN_WIDTH < SCREEN_HEIGHT ? SCREEN_WIDTH : SCREEN_HEIGHT) / 2 - 2;
  int innerR = outerR - displayPriceBarWidth;
  if (innerR < 70) innerR = 70;

  int minC = displayPriceBarMinCent;
  int maxC = displayPriceBarMaxCent;
  if (maxC <= minC) maxC = minC + 10;

  const int startDeg = 135;
  const int endDeg = 405;

  for (int a = startDeg; a <= endDeg; a += 2) {
    float p = (float)(a - startDeg) / (float)(endDeg - startDeg);
    int simulatedCent = minC + (int)((maxC - minC) * p);
    uint16_t col = priceBarGradientColor(simulatedCent, minC, maxC);

    float rad = (float)a * 0.01745329252f;
    float cs = cos(rad);
    float sn = sin(rad);

    for (int r = innerR; r <= outerR; r++) {
      int x = cx + (int)(cs * r);
      int y = cy + (int)(sn * r);
      if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        disp.drawPixel(x, y, col);
      }
    }
  }

  float t = (float)(cent - minC) / (float)(maxC - minC);
  if (t < 0) t = 0;
  if (t > 1) t = 1;

  int markerDeg = startDeg + (int)((endDeg - startDeg) * t);

  for (int da = -2; da <= 2; da++) {
    float r2 = (float)(markerDeg + da) * 0.01745329252f;
    float cs2 = cos(r2);
    float sn2 = sin(r2);
    for (int r = innerR - 2; r <= outerR + 1; r++) {
      int x = cx + (int)(cs2 * r);
      int y = cy + (int)(sn2 * r);
      if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        disp.drawPixel(x, y, DISPLAY_WHITE);
      }
    }
  }

  if (displayPriceBarTextEnabled) {
    disp.setTextColor(DISPLAY_WHITE);
    disp.setTextSize(1);
    disp.setCursor(82, 218);
    disp.print(String(cent));
    disp.print(" ct");
  }
}

int dayMinuteToRingSegment(int minuteOfDay) {
  if (minuteOfDay < 0) minuteOfDay = 0;
  if (minuteOfDay > 1439) minuteOfDay = 1439;

  int seg = (minuteOfDay * LED_RING_COUNT) / 1440;
  if (seg < 0) seg = 0;
  if (seg >= LED_RING_COUNT) seg = LED_RING_COUNT - 1;
  return seg;
}

int dayHourToRingSegment(int hour) {
  if (hour < 0) hour = 0;
  if (hour > 23) hour = 23;
  return dayMinuteToRingSegment(hour * 60);
}

int visualSegmentForDayRing(int logicalSegment) {
  if (logicalSegment < 0) logicalSegment = 0;
  if (logicalSegment >= LED_RING_COUNT) logicalSegment = LED_RING_COUNT - 1;

  int visualSegment = mapDayLedToClockLed(logicalSegment, LED_RING_COUNT);
  if (visualSegment < 0) visualSegment = 0;
  if (visualSegment >= LED_RING_COUNT) visualSegment = LED_RING_COUNT - 1;
  return visualSegment;
}

float visualSegmentToAngleDeg(int visualSegment) {
  if (visualSegment < 0) visualSegment = 0;
  if (visualSegment >= LED_RING_COUNT) visualSegment = LED_RING_COUNT - 1;

  float degPerSegment = 360.0f / (float)LED_RING_COUNT;
  return -90.0f + ((float)visualSegment + 0.5f) * degPerSegment;
}

void drawRingSegmentArc(Adafruit_GC9A01A &disp, int visualSegment, int innerR, int outerR, uint16_t col) {
  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  float degPerSegment = 360.0f / (float)LED_RING_COUNT;

  float startF = -90.0f + (float)visualSegment * degPerSegment;
  float endF = -90.0f + (float)(visualSegment + 1) * degPerSegment;

  int startDeg = (int)floor(startF);
  int endDeg = (int)ceil(endF);
  if (endDeg < startDeg) endDeg = startDeg;

  for (int a = startDeg; a <= endDeg; a++) {
    float rad = (float)a * 0.01745329252f;
    float cs = cos(rad);
    float sn = sin(rad);

    for (int r = innerR; r <= outerR; r++) {
      int x = cx + (int)(cs * r);
      int y = cy + (int)(sn * r);
      if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        disp.drawPixel(x, y, col);
      }
    }
  }
}

void drawDisplay2CheapClockRing(Adafruit_GC9A01A &disp) {
  if (!display2CheapClockRingEnabled) return;

  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int outerR = (SCREEN_WIDTH < SCREEN_HEIGHT ? SCREEN_WIDTH : SCREEN_HEIGHT) / 2 - 2;
  int innerR = outerR - display2CheapClockRingWidth;
  if (innerR < 72) innerR = 72;

  int currentHour = 0;
  int currentMinute = 0;
  String nowText = getDisplayTimeText();
  if (nowText.length() >= 5) {
    currentHour = nowText.substring(0, 2).toInt();
    currentMinute = nowText.substring(3, 5).toInt();
  }
  if (currentHour < 0) currentHour = 0;
  if (currentHour > 23) currentHour = 23;
  if (currentMinute < 0) currentMinute = 0;
  if (currentMinute > 59) currentMinute = 59;

  int currentMinuteOfDay = currentHour * 60 + currentMinute;
  int currentLogicalSegment = dayMinuteToRingSegment(currentMinuteOfDay);
  int currentVisualSegment = visualSegmentForDayRing(currentLogicalSegment);

  int cheapestHour = -1;
  String lowRange = getLayoutValue("low60DayTimeRange");
  if (lowRange.length() < 2) lowRange = getLayoutValue("low60DayFullRange");
  if (lowRange.length() >= 2) {
    int p = lowRange.indexOf(':');
    if (p >= 2) cheapestHour = lowRange.substring(p - 2, p).toInt();
    else cheapestHour = lowRange.substring(0, 2).toInt();
  }
  if (cheapestHour < 0 || cheapestHour > 23) cheapestHour = 0;

  int cheapestLogicalSegment = dayHourToRingSegment(cheapestHour);

  int currentCent = euroToCentRounded(metricCurrent15);
  int lowCent = euroToCentRounded(metricLow60Day);
  int avgCent = euroToCentRounded(metricDayAvg);

  if (currentCent < 0) currentCent = avgCent >= 0 ? avgCent : 0;
  if (lowCent < 0) lowCent = currentCent;
  if (avgCent < 0) avgCent = currentCent;

  int minCent = displayPriceBarMinCent;
  int maxCent = displayPriceBarMaxCent;
  if (maxCent <= minCent) maxCent = minCent + 10;

  for (int logicalSeg = 0; logicalSeg < LED_RING_COUNT; logicalSeg++) {
    int dist = abs(logicalSeg - cheapestLogicalSegment);
    if (dist > LED_RING_COUNT / 2) dist = LED_RING_COUNT - dist;

    float t = (float)dist / (float)(LED_RING_COUNT / 2);
    int syntheticCent = lowCent + (int)((maxCent - lowCent) * t);
    if (syntheticCent < minCent) syntheticCent = minCent;
    if (syntheticCent > maxCent) syntheticCent = maxCent;

    uint16_t col = tftPriceRingColor(syntheticCent, minCent, maxCent);
    int visualSeg = visualSegmentForDayRing(logicalSeg);
    drawRingSegmentArc(disp, visualSeg, innerR, outerR, col);
  }

  uint16_t bestCol = tftColorFromId(display2RingBestColorId);
  uint16_t nowCol = tftColorFromId(display2RingCurrentColorId);

  for (int h = 0; h < 24; h++) {
    int logicalSeg = dayHourToRingSegment(h);
    int visualSeg = visualSegmentForDayRing(logicalSeg);
    float deg = visualSegmentToAngleDeg(visualSeg);
    float rad = deg * 0.01745329252f;
    float cs = cos(rad);
    float sn = sin(rad);
    int markLen = (h % 6 == 0) ? 13 : ((h % 3 == 0) ? 9 : 5);

    for (int r = outerR - markLen; r <= outerR; r++) {
      int x = cx + (int)(cs * r);
      int y = cy + (int)(sn * r);
      if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        disp.drawPixel(x, y, DISPLAY_WHITE);
      }
    }
  }

  int blockSegments = max(1, (LED_RING_COUNT + 23) / 24);
  for (int b = 0; b < blockSegments; b++) {
    int logicalSeg = (cheapestLogicalSegment + b) % LED_RING_COUNT;
    int visualSeg = visualSegmentForDayRing(logicalSeg);
    drawRingSegmentArc(disp, visualSeg, innerR - 3, outerR + 1, bestCol);
  }

  float nowDeg = visualSegmentToAngleDeg(currentVisualSegment);
  float nowRad = nowDeg * 0.01745329252f;
  int mx = cx + (int)(cos(nowRad) * (innerR - 9));
  int my = cy + (int)(sin(nowRad) * (innerR - 9));
  disp.fillCircle(mx, my, 4, nowCol);
  disp.drawCircle(mx, my, 5, DISPLAY_WHITE);

  if (display2CheapClockRingLabelsEnabled) {
    disp.setTextColor(DISPLAY_WHITE);
    disp.setTextSize(1);

    for (int h = 0; h < 24; h += 3) {
      int logicalSeg = dayHourToRingSegment(h);
      int visualSeg = visualSegmentForDayRing(logicalSeg);
      float deg = visualSegmentToAngleDeg(visualSeg);
      float rad = deg * 0.01745329252f;
      int labelR = innerR - 18;
      if (labelR < 66) labelR = 66;

      int x = cx + (int)(cos(rad) * labelR);
      int y = cy + (int)(sin(rad) * labelR);

      String label = "";
      if (h < 10) label += "0";
      label += String(h);

      disp.setCursor(x - 6, y - 4);
      disp.print(label);
    }
  }

  if (display2CheapClockRingTextEnabled) {
    disp.setTextColor(DISPLAY_WHITE);
    disp.setTextSize(1);
    disp.setCursor(68, 216);
    if (cheapestHour < 10) disp.print("0");
    disp.print(cheapestHour);
    disp.print(":00 ");
    disp.print(lowCent);
    disp.print("ct");
  }
}

void drawTftDayLineChart(Adafruit_GC9A01A &disp, int x, int y, int w, int h) {
  if (w < 40 || h < 30) return;

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
  if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
  if (w < 40 || h < 30) return;

  uint16_t gridCol = rgb565(31, 41, 55);
  uint16_t frameCol = rgb565(75, 85, 99);

  disp.fillRect(x, y, w, h, DISPLAY_BLACK);
  disp.drawRect(x, y, w, h, frameCol);

  for (int i = 1; i < 4; i++) {
    int gy = y + (h * i) / 4;
    for (int gx = x + 1; gx < x + w - 1; gx += 4) {
      disp.drawPixel(gx, gy, gridCol);
    }
  }

  for (int i = 1; i < 4; i++) {
    int gx = x + (w * i) / 4;
    for (int gy = y + 1; gy < y + h - 1; gy += 4) {
      disp.drawPixel(gx, gy, gridCol);
    }
  }

  int pNow = euroToCentRounded(metricCurrent15);
  int pAvg = euroToCentRounded(metricDayAvg);
  int pLow15 = euroToCentRounded(metricLow15Day);
  int pLow60 = euroToCentRounded(metricLow60Day);
  int pCur60 = euroToCentRounded(metricCurrent60);

  if (pNow < 0 && pAvg >= 0) pNow = pAvg;
  if (pAvg < 0 && pNow >= 0) pAvg = pNow;
  if (pLow15 < 0 && pNow >= 0) pLow15 = pNow;
  if (pLow60 < 0 && pLow15 >= 0) pLow60 = pLow15;
  if (pCur60 < 0 && pNow >= 0) pCur60 = pNow;

  if (pNow < 0) {
    disp.setTextColor(DISPLAY_WHITE);
    disp.setTextSize(1);
    disp.setCursor(x + 8, y + 8);
    disp.print("keine Daten");
    return;
  }

  int vals[6];
  vals[0] = pAvg;
  vals[1] = pLow60;
  vals[2] = pLow15;
  vals[3] = pCur60;
  vals[4] = pNow;
  vals[5] = pAvg;

  int minP = vals[0];
  int maxP = vals[0];
  for (int i = 0; i < 6; i++) {
    if (vals[i] < minP) minP = vals[i];
    if (vals[i] > maxP) maxP = vals[i];
  }
  if (maxP <= minP) maxP = minP + 1;

  int lastX = -1;
  int lastY = -1;

  for (int i = 0; i < 6; i++) {
    int px = x + 4 + (i * (w - 8)) / 5;
    int py = y + h - 5 - ((vals[i] - minP) * (h - 10)) / (maxP - minP);
    uint16_t col = tftPriceRingColor(vals[i], minP, maxP);

    if (lastX >= 0) {
      disp.drawLine(lastX, lastY, px, py, col);
      disp.drawLine(lastX, lastY + 1, px, py + 1, col);
    }

    disp.fillCircle(px, py, 2, col);
    lastX = px;
    lastY = py;
  }

  int hour = 0;
  int minute = 0;
  String t = getDisplayTimeText();
  if (t.length() >= 5) {
    hour = t.substring(0, 2).toInt();
    minute = t.substring(3, 5).toInt();
  }

  int dayMinute = hour * 60 + minute;
  if (dayMinute < 0) dayMinute = 0;
  if (dayMinute > 1439) dayMinute = 1439;

  int markerX = x + 4 + (dayMinute * (w - 8)) / 1439;
  for (int yy = y + 2; yy < y + h - 2; yy += 3) {
    disp.drawPixel(markerX, yy, DISPLAY_WHITE);
    if (markerX + 1 < x + w) disp.drawPixel(markerX + 1, yy, DISPLAY_WHITE);
  }

  int markerY = y + h - 5 - ((pNow - minP) * (h - 10)) / (maxP - minP);
  disp.fillCircle(markerX, markerY, 4, DISPLAY_WHITE);
  disp.fillCircle(markerX, markerY, 2, tftPriceRingColor(pNow, minP, maxP));

  disp.setTextColor(DISPLAY_WHITE);
  disp.setTextSize(1);
  disp.setCursor(x + 4, y + 4);
  disp.print(t);
  disp.print(" ");
  disp.print(pNow);
  disp.print("ct");

  disp.setCursor(x + 4, y + h - 10);
  disp.print(minP);
  disp.print("-");
  disp.print(maxP);
  disp.print("ct");
}

void drawOptionalDisplayCharts(Adafruit_GC9A01A &disp) {
  if (&disp == &displayCurrent && display1DayChartEnabled) {
    drawTftDayLineChart(disp, displayDayChartX, displayDayChartY, displayDayChartWidth, displayDayChartHeight);
  }

  if (&disp == &displayBest && display2DayChartEnabled) {
    drawTftDayLineChart(disp, displayDayChartX, displayDayChartY, displayDayChartWidth, displayDayChartHeight);
  }
}

// -----------------------------------------------------------------------------
// MAX7219 8x8 Matrix
// -----------------------------------------------------------------------------

String matrixModeLabel(int mode) {
  switch (mode) {
    case 0: return "Aktueller 15-Min Preis";
    case 1: return "Aktueller 60-Min Durchschnitt";
    case 2: return "Tagesdurchschnitt";
    case 3: return "Tiefster 15-Min Preis";
    case 4: return "Tiefster 60-Min Block";
    case 5: return "Uhrzeit Stunde";
    case 6: return "IP-Endung";
    case 7: return "Text ct";
    case 8: return "Aus";
    default: return "Aus";
  }
}

String matrixValuePreviewForModule(int module) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return "--";
  if (!matrixEnabled[module] || matrixMode[module] == 8) return "Aus";

  int mode = matrixMode[module];

  if (mode == 0) return priceToCentText(metricCurrent15);
  if (mode == 1) return priceToCentText(metricCurrent60);
  if (mode == 2) return priceToCentText(metricDayAvg);
  if (mode == 3) return priceToCentText(metricLow15Day);
  if (mode == 4) return priceToCentText(metricLow60Day);

  if (mode == 5) {
    String t = getDisplayTimeText();
    if (t.length() >= 2) return t.substring(0, 2);
    return "--";
  }

  if (mode == 6) {
    IPAddress ip = apMode ? WiFi.softAPIP() : WiFi.localIP();
    return String(ip[3]);
  }

  if (mode == 7) return "ct";

  return "--";
}

String matrixValuePreview() {
  return matrixValuePreviewForModule(0);
}

void max7219ShiftByte(uint8_t value) {
  for (int8_t bit = 7; bit >= 0; bit--) {
    digitalWrite(MATRIX_CLK_PIN, LOW);
    digitalWrite(MATRIX_DIN_PIN, (value & (1 << bit)) ? HIGH : LOW);
    delayMicroseconds(1);
    digitalWrite(MATRIX_CLK_PIN, HIGH);
    delayMicroseconds(1);
  }
}

void max7219WriteAll(uint8_t reg, uint8_t data) {
  digitalWrite(TFT1_CS_PIN, HIGH);
  digitalWrite(TFT2_CS_PIN, HIGH);

  digitalWrite(MATRIX_CS_PIN, LOW);
  for (int dev = MATRIX_DEVICE_COUNT - 1; dev >= 0; dev--) {
    max7219ShiftByte(reg);
    max7219ShiftByte(data);
  }
  digitalWrite(MATRIX_CS_PIN, HIGH);
}

void max7219WriteDevice(int module, uint8_t reg, uint8_t data) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;

  digitalWrite(TFT1_CS_PIN, HIGH);
  digitalWrite(TFT2_CS_PIN, HIGH);

  digitalWrite(MATRIX_CS_PIN, LOW);
  for (int dev = MATRIX_DEVICE_COUNT - 1; dev >= 0; dev--) {
    if (dev == module) {
      max7219ShiftByte(reg);
      max7219ShiftByte(data);
    } else {
      max7219ShiftByte(0x00);
      max7219ShiftByte(0x00);
    }
  }
  digitalWrite(MATRIX_CS_PIN, HIGH);
}

void matrixFlushModule(int module) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;

  for (uint8_t row = 0; row < 8; row++) {
    max7219WriteDevice(module, row + 1, matrixRows[module][row]);
  }
}

void matrixFlush() {
  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    matrixFlushModule(module);
  }
}

void clearPriceMatrixModule(int module) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;

  for (uint8_t i = 0; i < 8; i++) {
    matrixRows[module][i] = 0;
  }
  matrixFlushModule(module);
}

void clearPriceMatrix() {
  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    clearPriceMatrixModule(module);
  }
}

void initPriceMatrix() {
  pinMode(MATRIX_DIN_PIN, OUTPUT);
  pinMode(MATRIX_CLK_PIN, OUTPUT);
  pinMode(MATRIX_CS_PIN, OUTPUT);
  digitalWrite(MATRIX_DIN_PIN, LOW);
  digitalWrite(MATRIX_CLK_PIN, LOW);
  digitalWrite(MATRIX_CS_PIN, HIGH);
  pinMode(TFT1_CS_PIN, OUTPUT);
  pinMode(TFT2_CS_PIN, OUTPUT);
  digitalWrite(TFT1_CS_PIN, HIGH);
  digitalWrite(TFT2_CS_PIN, HIGH);

  max7219WriteAll(0x0F, 0x00);
  max7219WriteAll(0x0C, 0x01);
  max7219WriteAll(0x0B, 0x07);
  max7219WriteAll(0x09, 0x00);

  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    max7219WriteDevice(module, 0x0A, matrixBrightness[module] & 0x0F);
  }

  clearPriceMatrix();

  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    for (uint8_t row = 0; row < 8; row++) {
      matrixRows[module][row] = 0xFF;
    }
  }
  matrixFlush();
  delay(3000);
  clearPriceMatrix();
}

void matrixPixel(int module, int x, int y, bool on) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;
  if (x < 0 || x > 7 || y < 0 || y > 7) return;

  uint8_t bitMask = 1 << (7 - x);

  if (on) {
    matrixRows[module][y] |= bitMask;
  } else {
    matrixRows[module][y] &= ~bitMask;
  }
}

void matrixPixel(int x, int y, bool on) {
  matrixPixel(0, x, y, on);
}

uint16_t matrixGlyph(char c) {
  switch (c) {
    case '0': return 0b111101101101111;
    case '1': return 0b010110010010111;
    case '2': return 0b111001111100111;
    case '3': return 0b111001111001111;
    case '4': return 0b101101111001001;
    case '5': return 0b111100111001111;
    case '6': return 0b111100111101111;
    case '7': return 0b111001010010010;
    case '8': return 0b111101111101111;
    case '9': return 0b111101111001111;
    case '-': return 0b000000111000000;
    case 'A': return 0b010101111101101;
    case 'C': return 0b111100100100111;
    case 'E': return 0b111100111100111;
    case 'H': return 0b101101111101101;
    case 'L': return 0b100100100100111;
    case 'P': return 0b111101111100100;
    case 'T': return 0b111010010010010;
    default: return 0;
  }
}

void drawMatrixChar3x5(int module, char c, int x, int y) {
  uint16_t g = matrixGlyph(c);

  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 3; col++) {
      int bitIndex = 14 - (row * 3 + col);
      bool on = (g >> bitIndex) & 0x01;
      matrixPixel(module, x + col, y + row, on);
    }
  }
}

void showMatrixText2(int module, String text) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;

  clearPriceMatrixModule(module);
  text.trim();
  text.toUpperCase();

  if (text.length() == 0) {
    drawMatrixChar3x5(module, '-', 2, 1);
    matrixFlushModule(module);
    return;
  }

  if (text.length() == 1) {
    drawMatrixChar3x5(module, text.charAt(0), 2, 1);
    matrixFlushModule(module);
    return;
  }

  drawMatrixChar3x5(module, text.charAt(0), 0, 1);
  drawMatrixChar3x5(module, text.charAt(1), 4, 1);
  matrixFlushModule(module);
}

void showMatrixText2(String text) {
  showMatrixText2(0, text);
}

void showMatrixTwoChars(int module, char leftChar, char rightChar) {
  String t = "";
  t += leftChar;
  t += rightChar;
  showMatrixText2(module, t);
}

void showMatrixTwoChars(char leftChar, char rightChar) {
  showMatrixTwoChars(0, leftChar, rightChar);
}

void showMatrixNumber(int module, int number) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;

  if (number < 0) {
    showMatrixText2(module, "--");
    return;
  }

  if (number > 99) number = 99;
  showMatrixText2(module, String(number));
}

void showMatrixNumber(int number) {
  showMatrixNumber(0, number);
}

void updatePriceMatrixModule(int module) {
  if (module < 0 || module >= MATRIX_DEVICE_COUNT) return;

  if (!matrixEnabled[module] || matrixMode[module] == 8) {
    clearPriceMatrixModule(module);
    return;
  }

  max7219WriteDevice(module, 0x0A, matrixBrightness[module] & 0x0F);

  int mode = matrixMode[module];

  if (mode == 0) {
    showMatrixNumber(module, euroToCentRounded(metricCurrent15));
  } else if (mode == 1) {
    showMatrixNumber(module, euroToCentRounded(metricCurrent60));
  } else if (mode == 2) {
    showMatrixNumber(module, euroToCentRounded(metricDayAvg));
  } else if (mode == 3) {
    showMatrixNumber(module, euroToCentRounded(metricLow15Day));
  } else if (mode == 4) {
    showMatrixNumber(module, euroToCentRounded(metricLow60Day));
  } else if (mode == 5) {
    String t = getDisplayTimeText();
    if (t.length() >= 2) showMatrixText2(module, t.substring(0, 2));
    else showMatrixText2(module, "--");
  } else if (mode == 6) {
    IPAddress ip = apMode ? WiFi.softAPIP() : WiFi.localIP();
    showMatrixNumber(module, (int)ip[3]);
  } else if (mode == 7) {
    showMatrixText2(module, "ct");
  } else {
    clearPriceMatrixModule(module);
  }
}

void updatePriceMatrix() {
  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    updatePriceMatrixModule(module);
  }
}

// -----------------------------------------------------------------------------
// Hauptseite
// -----------------------------------------------------------------------------

void handleRoot() {
  if (!checkAuth()) return;

  // Live auf dem Geraet nachgewiesen: der Arduino-String-Typ verwirft beim
  // Anhaengen (+=) still und leise Daten, wenn dafuer eine Speichererweiterung
  // noetig ist und der Heap gerade keinen ausreichend grossen zusammen-
  // haengenden Block frei hat - kein Absturz, keine Fehlermeldung, der Rest
  // des Aufrufs geht einfach verloren. Ein groesseres html.reserve() allein
  // loest das NICHT zuverlaessig (auf diesem Geraet reproduzierbar sogar
  // fruehere Abschneidung bei reserve(40000) als bei reserve(29000), weil ein
  // einzelner grosser reserve()-Aufruf bei fragmentiertem Heap komplett
  // fehlschlagen kann). Stattdessen wird die Antwort jetzt in mehreren
  // kleinen Chunks per server.sendContent() gesendet (HTTP Chunked Transfer
  // Encoding) - jeder einzelne Chunk braucht nur ein kleines, viel eher
  // verfuegbares zusammenhaengendes Speicherstueck, statt eines einzigen
  // Blocks fuer die komplette Seite.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String html;
  html.reserve(6000);

  html += htmlHeader("Übersicht");
  html += "<section class='hero'><h1>";
  html += htmlEscape(webInterfaceName);
  html += "</h1><p>Tibber Preise, Displays und Tagesring zentral steuern.</p></section>";
  html += navTabs("/");

  {
    bool needsWifi = apMode;
    bool needsToken = (tibberToken.length() == 0) && (priceProvider == "tibber");
    int openSteps = (needsWifi ? 1 : 0) + (needsToken ? 1 : 0);

    if (openSteps > 0) {
      html += "<section class='card' style='border-color:rgba(250,204,21,.35)'>";
      html += "<div class='panelTitle'><h2>Einrichtung</h2><span class='badge warnb'>";
      html += String(openSteps);
      html += " offen</span></div>";
      html += "<div style='display:grid;gap:10px'>";

      if (needsWifi) {
        html += "<div class='formSection' style='display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap'><span>Setup-Modus aktiv - WLAN noch nicht eingerichtet</span><a href='/wifi'><button type='button' class='secondary'>WLAN einrichten</button></a></div>";
      }

      if (needsToken) {
        html += "<div class='formSection' style='display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap'><span>Kein Tibber-Token hinterlegt</span><a href='/anbieter'><button type='button' class='secondary'>Jetzt eintragen</button></a></div>";
      }

      html += "</div></section>";
    }
  }

  server.sendContent(html);
  html = "";

  html += R"CSS(<style>
.kw-toolbar{display:flex;justify-content:flex-end;gap:8px;margin:4px 0 -2px}
/* grid-auto-rows:minmax(64px,auto) liess jede Reihe eine andere Hoehe
   bekommen (je nachdem wie viel Inhalt gerade in ihr steckt) - die Drag/
   Resize-Mathematik (cellSize.h = Container-Hoehe / Anzahl Reihen, siehe
   kwGrid()/cellSize() weiter unten) geht dagegen von GLEICH hohen Reihen
   aus. Bei unterschiedlich hohen Reihen war diese Rechnung falsch, wodurch
   Widgets beim Ziehen/Skalieren sichtbar "verrutscht" sind (die berechnete
   Zielreihe passte nicht zur tatsaechlichen Reihe unter dem Mauszeiger).
   Feste, gleich hohe Reihen beheben das - der Inhalt jedes Widgets skaliert
   dank Container-Queries (cqi) ohnehin auf die ihm zugewiesene Box. */
)CSS";
  html += ".kw-canvas{position:relative;display:grid;grid-template-columns:repeat(8,1fr);grid-template-rows:repeat(" + String(HOME_GRID_ROWS) + ",90px);grid-auto-rows:90px;gap:14px;margin:14px 0}";
  html += R"CSS(
.kw{position:relative;container-type:inline-size;background:var(--card);border:1px solid var(--surface-border);border-radius:22px;padding:clamp(14px,3cqi,26px);box-shadow:0 1px 2px var(--shadow-soft),0 8px 24px rgba(0,0,0,.06);backdrop-filter:blur(20px) saturate(180%);-webkit-backdrop-filter:blur(20px) saturate(180%);box-sizing:border-box;overflow:hidden;display:flex;flex-direction:column;transition:box-shadow .2s var(--ease)}
.kw.wg-dragging,.kw.wg-resizing{box-shadow:0 20px 44px rgba(0,0,0,.28);z-index:50}
.kw-arrange-mode .kw{cursor:grab;user-select:none}
.kw-arrange-mode .kw.wg-dragging{cursor:grabbing}
.kw-resize{display:none;position:absolute;right:8px;bottom:8px;width:18px;height:18px;border-radius:50%;background:var(--accent2);border:2px solid var(--card);cursor:nwse-resize;z-index:5}
.kw-arrange-mode .kw-resize{display:block}
.kw-gauge .panelTitle{margin-bottom:6px;flex:0 0 auto}
.kw-gauge .gaugeWrap{flex:1;min-height:0;display:flex;align-items:center;justify-content:center}
/* width:100%;height:auto skaliert die SVG NUR anhand der verfuegbaren
   Breite - wenn die Box (durch feste Reihenhoehen im Grid) nicht genau das
   Seitenverhaeltnis der SVG hat, ragt die dadurch zu hoch berechnete SVG
   ueber die Titelzeile hinaus. width:100%;height:100% (wie bei Kiosk 1/2)
   laesst die SVG stattdessen per eingebautem preserveAspectRatio in BEIDE
   Richtungen passend einskalieren, ohne zu ueberlappen. */
.kw-gauge .gaugeWrap svg{width:100%;height:100%;max-width:420px}
.kw-chart{min-height:180px}
.kw-chart svg{width:100%;height:100%;display:block;flex:1;min-height:0}
.hw-chart-wrap{position:relative;flex:1;min-height:0;width:100%;touch-action:none;cursor:crosshair}
.hw-chart-wrap svg{width:100%;height:100%;display:block}
.hw-chart-wrap .kiosk-crosshair-line{stroke:var(--muted);stroke-width:1;stroke-dasharray:4,4;opacity:0;pointer-events:none}
.hw-chart-wrap .kiosk-crosshair-dot{fill:var(--accent);stroke:var(--card);stroke-width:2;opacity:0;pointer-events:none}
.hw-chart-tooltip{position:absolute;transform:translate(-50%,-115%);background:var(--card);border:1px solid var(--surface-border);border-radius:12px;padding:6px 12px;font-size:13px;font-weight:600;white-space:nowrap;pointer-events:none;opacity:0;color:var(--text);box-shadow:0 8px 24px var(--shadow-hover)}
.kw-metrics .gridCards{margin:0;height:100%;grid-template-columns:repeat(auto-fit,minmax(140px,1fr))}
/* .metric .value/.label/.sub kommen aus dem globalen style.css und nutzen dort
   vw-basiertes clamp() (fuer die statischen Info-Karten auf /pinout, /wifi etc.
   gedacht) - ohne diese cqi-Overrides wuerden die Zahlen im Metriken-Widget
   NICHT mit der Widget-Groesse mitskalieren, sondern nur mit der Fensterbreite,
   obwohl das Widget selbst per Drag/Resize beliebig verkleinert/vergroessert
   werden kann. */
.kw-metrics .metric .value{font-size:clamp(14px,3.2cqi,30px)}
.kw-metrics .metric .label{font-size:clamp(7px,1.2cqi,11px)}
.kw-metrics .metric .sub{font-size:clamp(7px,1.3cqi,12px)}
.kw-livepower{align-items:center;justify-content:center}
.kw-livepower .live-power{margin:0;padding:0;background:transparent;max-width:none}
.kw-livepower .pg-value{font-size:clamp(28px,14cqi,72px)}
.kw-livepower .pg-label{font-size:clamp(11px,3.4cqi,15px)}
.kw-livepower .pg-scale{font-size:clamp(9px,2.6cqi,12px)}
.kw-livepower .pg-track{height:clamp(4px,1.4cqi,12px)}
.kw-livepower .pg-marker{width:clamp(10px,3.6cqi,20px);height:clamp(10px,3.6cqi,20px)}
.kw-status{align-items:stretch;justify-content:flex-start;overflow-y:auto}
.kw-status .small{font-size:clamp(9px,2cqi,13px)}
.kw-status .actions{margin-top:auto}
/* Die aufklappbare Preistabelle kann bei 192 Zeilen (heute+morgen) mehrere
   tausend Pixel hoch werden - deutlich mehr als die feste Widget-Hoehe. Die
   Tabelle bekommt deshalb eine eigene, vom Widget unabhaengige Scroll-Grenze
   (max-height + overflow-y), statt entweder den ganzen Kasten zu sprengen
   oder (durch das overflow:hidden der Basis-.kw-Klasse) den Rest der Zeilen
   ohne jede Moeglichkeit, sie zu sehen, einfach abzuschneiden. */
.kw-pricetable{align-items:stretch;justify-content:flex-start}
.kw-pricetable details{width:100%;overflow:hidden;display:flex;flex-direction:column;min-height:0}
/* .kw hat nur container-type:inline-size (Breite), keine Block-Achsen-
   Containment - cqh/cqb-Einheiten waeren hier ungueltig. Fester max-height-
   Wert stattdessen, grosszuegig innerhalb der Default-Widget-Hoehe
   (rowSpan 4 = ca. 400px). */
.kw-pricetable table{display:block;max-height:300px;overflow-y:auto}
@container (max-width: 220px){
  .kw-metrics .gridCards{grid-template-columns:1fr}
  .kw-metrics .metric .sub{display:none}
}
@media(max-width:700px){
  /* Mobil werden Widgets ohnehin nur uebereinander gestapelt (kein Ziehen
     noetig) - die feste 90px-Reihenhoehe vom Desktop-Grid soll hier nicht
     gelten, sonst wuerde jedes Widget auf 90px Hoehe zusammengequetscht
     statt sich an seinen tatsaechlichen Inhalt anzupassen. */
  .kw-canvas{grid-template-columns:1fr;grid-template-rows:none;grid-auto-rows:auto}
  .kw{grid-column:1/-1!important;grid-row:auto!important;min-height:120px}
}
</style>)CSS";

  server.sendContent(html);
  html = "";

  html += "<div class='kw-toolbar'><button type='button' id='kwResetBtn' class='secondary' onclick='kwResetLayout()' style='display:none'>Zuruecksetzen</button><button type='button' id='kwArrangeBtn' class='secondary' onclick='kwToggleArrange()'>Anordnen</button></div>";
  html += "<div class='kw-canvas'>";

  // Widget 0: Preis-Gauge
  html += "<div class='kw kw-gauge' data-idx='0'>";
  html += "<div class='panelTitle'><h2>Aktuelle Werte</h2><div style='display:flex;gap:8px;flex-wrap:wrap'>";
  html += "<span class='badge okb' title='Woher die Preise gerade kommen, aenderbar unter Anbieter'>";
  if (priceProvider == "awattar_de") {
    html += "aWATTar DE";
  } else if (priceProvider == "awattar_at") {
    html += "aWATTar AT";
  } else {
    html += "Tibber";
  }
  html += "</span>";
  if (metricCurrent15 >= 0) {
    int nowCent = euroToCentRounded(metricCurrent15);
    if (nowCent >= ledRedCent) {
      html += "<span id='priceBadge' class='badge errb'>Jetzt teuer</span>";
    } else if (nowCent >= ledYellowCent) {
      html += "<span id='priceBadge' class='badge warnb'>Jetzt mittel</span>";
    } else {
      html += "<span id='priceBadge' class='badge okb'>Jetzt guenstig</span>";
    }
  } else {
    html += "<span id='priceBadge' class='badge'>Keine Daten</span>";
  }
  html += "<span class='badge ";
  html += (lastError.length() == 0) ? "okb'><span class='status-dot pulse ok'></span>Status OK" : "errb'><span class='status-dot pulse'></span>Fehler";
  html += "</span></div></div>";
  html += "<div class='gaugeWrap'>";
  html += buildPriceGaugeSvg();
  html += "</div>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  // Widget 1: Live-Verbrauch
  String liveHomeText = "";
  if (livePowerW >= 0 && millis() - livePowerUpdatedAtMs < 60000) {
    float kw = livePowerW / 1000.0f;
    float lppct = kw / livePowerMaxKw * 100.0f;
    if (lppct < 0) lppct = 0;
    if (lppct > 100) lppct = 100;
    String zc = "zc";
    if (kw >= livePowerYellowKw) zc = "ze";
    else if (kw >= livePowerGreenKw) zc = "zm";
    liveHomeText = "<div class='pg-label'>&#9889; Aktueller Verbrauch</div>";
    liveHomeText += "<div class='pg-value'>" + formatLivePowerValue() + "</div>";
    liveHomeText += "<div class='pg-track'><div class='pg-fill " + zc + "' style='width:" + String((int)lppct) + "%'></div><div class='pg-marker' style='left:" + String((int)lppct) + "%'></div></div>";
    liveHomeText += "<div class='pg-scale'><span>0</span><span>" + String(livePowerMaxKw, 1) + " kW</span></div>";
  }
  html += "<div class='kw kw-livepower' data-idx='1'>";
  html += "<div class='live-power priceGauge' id='livePowerBadge'>" + liveHomeText + "</div>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  // Widget 2: Preisverlauf-Diagramm
  html += "<div class='kw kw-chart' data-idx='2'>";
  html += "<div class='panelTitle'><h2>Preisverlauf</h2><span class='badge okb'>" + String(quarterCount) + " Slots</span></div>";
  html += "<div class='hw-chart-wrap' id='hwChartWrap'>";
  html += buildSvgChart();
  html += "<div class='hw-chart-tooltip' id='hwChartTooltip'></div>";
  html += "</div>";
  html += "<p class='small' style='margin-top:6px'>Mit Finger oder Maus über das Diagramm fahren, um Preise zu sehen.</p>";
  html += "<div style='display:flex;flex-wrap:wrap;gap:14px;margin-top:4px;font-size:12px;color:var(--muted)'>";
  html += "<span><span style='display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--accent);margin-right:5px'></span>Preis</span>";
  html += "<span><span style='display:inline-block;width:10px;height:10px;border-radius:50%;background:#34C759;margin-right:5px'></span>Jetzt</span>";
  html += "<span><span style='display:inline-block;width:10px;height:10px;border-radius:50%;background:#FFD60A;margin-right:5px'></span>Tiefstpreis (60 Min)</span>";
  html += "<span><span style='display:inline-block;width:10px;height:2px;background:var(--muted);margin-right:5px;vertical-align:middle'></span>Tagesschnitt</span>";
  html += "</div>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  // Widget 3: Metriken
  html += "<div class='kw kw-metrics' data-idx='3'>";
  html += "<div class='gridCards'>";

  html += "<div class='metric'><div class='label'>15-Minuten-Preis</div><div class='value'>";
  html += priceToCentText(metricCurrent15);
  html += "</div></div>";

  html += "<div class='metric'><div class='label'>60-Minuten-Durchschnitt</div><div class='value'>";
  html += priceToCentText(metricCurrent60);
  html += "</div></div>";

  html += "<div class='metric'><div class='label'>Tagesdurchschnitt</div><div class='value'>";
  html += priceToCentText(metricDayAvg);
  html += "</div></div>";

  html += "<div class='metric'><div class='label'>Tief 15 Min heute</div><div class='value'>";
  html += priceToCentText(metricLow15Day);
  html += "</div><div class='sub'>um ";
  html += formatTimeOnly(metricLow15DayTime);
  html += " Uhr</div></div>";

  html += "<div class='metric'><div class='label'>Tief 60-Minuten-Block</div><div class='value'>";
  html += priceToCentText(metricLow60Day);
  html += "</div><div class='sub'>";
  html += formatTimeOnly(metricLow60DayTime);
  html += "-";
  html += addMinutesToIsoTime(metricLow60DayTime, 60);
  html += " Uhr</div></div>";

  html += "<div class='metric'><div class='label'>Zweiter 60-Minuten-Block</div><div class='value'>";
  html += priceToCentText(metricSecondLow60Day);
  html += "</div><div class='sub'>";
  html += formatTimeOnly(metricSecondLow60DayTime);
  html += "-";
  html += addMinutesToIsoTime(metricSecondLow60DayTime, 60);
  html += " Uhr</div></div>";

  if (tibberMonthCost >= 0) {
    html += "<div class='metric'><div class='label'>Energiekosten (bisher)</div><div class='value'>";
    html += euroCostText(tibberMonthCost) + " " + htmlEscape(tibberMonthCurrency);
    html += "</div><div class='sub'>";
    html += String(tibberMonthConsumptionKwh, 1) + " kWh seit Monatsanfang";
    html += "</div></div>";

    float projected = estimateFullMonthCost();
    if (projected >= 0) {
      html += "<div class='metric'><div class='label'>Prognose Monatsende</div><div class='value'>";
      html += euroCostText(projected) + " " + htmlEscape(tibberMonthCurrency);
      html += "</div><div class='sub'>";
      html += "hochgerechnet";
      if (tibberBaseFeeEur > 0) {
        html += " inkl. " + euroCostText(tibberBaseFeeEur) + " Grundgebühr";
      }
      html += "</div></div>";
    }
  }

  html += "</div>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  // Widget 4: Status & Aktionen (frueher eine feste <section class='card'>
  // unterhalb des Rasters - auf Nutzerwunsch jetzt ein echtes, verschieb-
  // und groessenveraenderbares Widget wie die anderen).
  html += "<div class='kw kw-status' data-idx='4'>";
  html += "<div class='small' style='display:flex;flex-wrap:wrap;gap:6px 16px'>";
  html += "<span>";
  html += (priceProvider == "tibber") ? "Tibber" : "aWATTar";
  html += " aktuell ab: " + htmlEscape(currentStartsAt) + "</span>";
  html += "<span>ESP-Zeit: " + htmlEscape(getCurrentIsoPrefix()) + "</span>";
  html += "<span>Slots geladen: " + String(quarterCount) + "</span>";
  html += "<span>API-Update: alle " + String(apiUpdateMinutes) + " Min</span>";
  html += "<span>Display-Refresh: alle " + String(displayRefreshSeconds) + " s</span>";
  html += "<span>IP: ";
  if (apMode) {
    html += WiFi.softAPIP().toString();
  } else {
    html += WiFi.localIP().toString();
  }
  html += "</span>";
  html += "</div>";

  if (lastError.length() > 0) {
    html += "<p class='err' style='margin-top:12px'>";
    html += htmlEscape(lastError);
    html += "</p>";
  } else {
    html += "<p class='ok' style='margin-top:12px'>Status OK</p>";
  }

  html += "<div class='actions'><a href='/refresh'><button>Jetzt aktualisieren</button></a><a href='/kiosk'><button type='button' class='secondary'>Tablet-Modus</button></a></div>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  // Widget 5: Preistabelle (frueher eine feste <details class='card'>
  // unterhalb des Rasters - jetzt ebenfalls ein echtes Widget. Die Tabelle
  // kann bei 192 Zeilen (heute+morgen) deutlich hoeher werden als die feste
  // Widget-Groesse - scrollt deshalb intern (siehe .kw-pricetable table CSS)
  // statt den Kasten zu sprengen oder Zeilen kommentarlos abzuschneiden.
  html += "<div class='kw kw-pricetable' data-idx='5'>";
  if (quarterCount > 0) {
    html += "<details><summary><h2 style='display:inline'>Geladene Preise heute/morgen (";
    html += String(quarterCount);
    html += " Slots)</h2></summary>";
    html += "<table><tr><th>Zeit</th><th>ct/kWh</th></tr>";

    for (int i = 0; i < quarterCount; i++) {
      html += "<tr><td>";
      html += htmlEscape(quarterTimes[i]);
      html += "</td><td>";
      html += String(euroToCentRounded(quarterPrices[i]));
      html += "</td></tr>";

      // Periodisch flushen statt die komplette Tabelle (bis zu 192 Zeilen
      // fuer heute+morgen) in einem Stueck im Speicher zu halten.
      if ((i + 1) % 40 == 0) {
        server.sendContent(html);
        html = "";
      }
    }

    html += "</table></details>";
  } else {
    html += "<p class='small'>Noch keine Preisdaten geladen.</p>";
  }
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  html += "</div>"; // .kw-canvas

  html += "<style>" + kioskWidgetCss(homeLayout, HOME_WIDGET_KEYS, HOME_WIDGET_COUNT) + "</style>";

  server.sendContent(html);
  html = "";

  html += "<p class='small'><a href='/json'>JSON-API (fuer Entwickler/Automatisierung)</a></p>";
  html += R"JS(<script>(function(){
function updatePriceBadge(){fetch('/gaugestatus',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
  var b=document.getElementById('priceBadge');
  if(b){b.className='badge '+d.badgeClass;b.innerText=d.badgeLabel;}
  var g=document.querySelector('.priceGauge');
  if(g){g.classList.remove('pg-good','pg-mid','pg-bad');g.classList.add(d.pgClass);var z=g.querySelector('.pg-zone');if(z)z.innerText=d.pgZoneLabel;}
}).catch(function(){});}
setInterval(updatePriceBadge,10000);
})();</script>)JS";

  server.sendContent(html);
  html = "";

  html += "<script src='/widget-engine.js'></script>";
  html += "<script>var homeData = " + kioskLayoutJson(homeLayout, HOME_WIDGET_KEYS, HOME_WIDGET_LABELS, HOME_WIDGET_COUNT) + ";</script>";
  html += "<script>var hwChartPoints = " + buildChartPointsJson() + ";";
  html += "WidgetGridEngine.createChartCrosshair('priceChartSvg', 'hwChartWrap', 'hwChartTooltip', function(){ return hwChartPoints; });</script>";

  server.sendContent(html);
  html = "";
  html += R"JS(<script>
var kwArrangeMode = false;
var kwController = null;

function kwGrid(){ return { cols: 8, rows: 13 }; }
function kwGetEl(i){ return document.querySelector('.kw-canvas .kw[data-idx="' + i + '"]'); }

function kwApplyLayout(i){
  var el = kwGetEl(i);
  var item = homeData[i];
  if (el) {
    el.style.gridColumn = item.colStart + '/span ' + item.colSpan;
    el.style.gridRow = item.rowStart + '/span ' + item.rowSpan;
  }
}

function kwSaveOne(i){
  var item = homeData[i];
  var body = new URLSearchParams();
  body.set('target', 'home');
  body.set('index', i);
  body.set('colStart', item.colStart);
  body.set('colSpan', item.colSpan);
  body.set('rowStart', item.rowStart);
  body.set('rowSpan', item.rowSpan);
  body.set('visible', item.visible ? '1' : '0');
  // keepalive: true - der Request darf ueberleben, auch wenn der Nutzer direkt
  // nach dem Ziehen zu einer anderen Seite navigiert (z.B. zur Konto-Seite, um
  // ein Firmware-Update zu starten) - sonst kann der Browser die noch laufende
  // Speicher-Anfrage abbrechen, bevor sie den Server erreicht, und die neue
  // Position geht scheinbar "nach dem Update" verloren (tatsaechlich wurde sie
  // nie gespeichert).
  return fetch('/savekiosklayoutajax', { method: 'POST', keepalive: true, headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(r){ return r.json(); })
    .then(function(d){ return !!d.ok; })
    .catch(function(){ return false; });
}

function kwCommit(indexes){
  var allOk = true;
  var chain = Promise.resolve();
  indexes.forEach(function(i){
    chain = chain.then(function(){ return kwSaveOne(i); }).then(function(ok){ if (!ok) allOk = false; });
  });
  chain.then(function(){
    if (typeof showToast === 'function') showToast(allOk ? 'Anordnung gespeichert' : 'Fehler beim Speichern', allOk ? 'ok' : 'err');
  });
}

function kwToggleArrange(){
  kwArrangeMode = !kwArrangeMode;
  document.body.classList.toggle('kw-arrange-mode', kwArrangeMode);
  var btn = document.getElementById('kwArrangeBtn');
  if (btn) btn.textContent = kwArrangeMode ? 'Fertig' : 'Anordnen';
  var resetBtn = document.getElementById('kwResetBtn');
  if (resetBtn) resetBtn.style.display = kwArrangeMode ? '' : 'none';
  if (kwArrangeMode) {
    // Waehrend des Anordnens darf die Seite nicht per meta-refresh neu laden
    // und dabei eine laufende Geste abbrechen.
    var meta = document.querySelector("meta[http-equiv='refresh' i]");
    if (meta) meta.remove();
  }
}

function kwResetLayout(){
  if (!confirm('Anordnung der Startseite auf Standard zuruecksetzen?')) return;
  var body = new URLSearchParams();
  body.set('target', 'home');
  fetch('/resetkiosklayout', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(){ location.reload(); });
}

(function(){
  kwController = WidgetGridEngine.createController({
    getEl: kwGetEl,
    getItems: function(){ return homeData; },
    getGrid: kwGrid,
    cellSize: function(){
      var canvas = document.querySelector('.kw-canvas');
      var r = canvas.getBoundingClientRect();
      var g = kwGrid();
      return { w: r.width / g.cols, h: r.height / g.rows };
    },
    applyLayout: kwApplyLayout,
    onCommit: kwCommit
  });
  document.querySelectorAll('.kw-canvas .kw').forEach(function(el){
    var idx = parseInt(el.getAttribute('data-idx'), 10);
    el.addEventListener('pointerdown', function(e){
      if (!kwArrangeMode) return;
      if (e.target.closest('.kw-resize')) return;
      kwController.startDrag(e, idx);
    });
    var handle = el.querySelector('.kw-resize');
    if (handle) {
      handle.addEventListener('pointerdown', function(e){
        if (!kwArrangeMode) return;
        kwController.startResize(e, idx);
      });
    }
  });
})();
</script>)JS";

  html += htmlFooter();

  // Letzter Inhalts-Chunk, dann ein leerer Chunk zum Abschluss der Chunked
  // Transfer Encoding (siehe WebServer::sendContent() - ein Aufruf mit
  // Laenge 0 sendet den abschliessenden "0\r\n\r\n"-Marker). sendHeader()
  // waere hier zu spaet, die Kopfzeilen wurden bereits ganz am Anfang der
  // Funktion mit dem ersten server.send() verschickt.
  server.sendContent(html);
  server.sendContent("");
}

// -----------------------------------------------------------------------------
// Konto / Sicherheit
// -----------------------------------------------------------------------------

void handleAccountPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(17500);

  html += htmlHeader("Konto");
  html += "<section class='hero'><h1>Konto &amp; Sicherheit</h1><p>Admin-Login, Setup-WLAN-Passwort, allgemeine Geraeteeinstellungen und Firmware-Updates.</p></section>";
  html += navTabs("/account");

  html += "<section class='card'><div class='panelTitle'><h2>Allgemeine Einstellungen</h2><span class='badge warnb'>Update alle ";
  html += String(apiUpdateMinutes);
  html += " Min</span></div>";
  html += "<form action='/save' method='post'>";
  html += "<input type='hidden' name='redirectTo' value='/account'>";
  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Name des Webinterfaces</label><input name='webName' maxlength='32' value='";
  html += htmlEscape(webInterfaceName);
  html += "'></div>";
  html += "<div class='field'><label>API-Update alle Minuten</label><input name='apiMinutes' type='number' min='1' max='60' value='";
  html += String(apiUpdateMinutes);
  html += "'></div>";
  html += "<div class='field'><label>Display-Refresh alle Sekunden</label><input name='dispRefresh' type='number' min='1' max='300' value='";
  html += String(displayRefreshSeconds);
  html += "'></div>";
  // Akzentfarbe (iOS-Systemfarben)
  html += "<div class='field' style='grid-column:1/-1'><label>Akzentfarbe</label>";
  html += "<div style='display:flex;gap:10px;flex-wrap:wrap;margin-top:6px'>";
  const char* accKeys[8] = {"blue","green","orange","red","pink","purple","teal","indigo"};
  const char* accHexes[8] = {"#007AFF","#34C759","#FF9500","#FF3B30","#FF2D55","#AF52DE","#5AC8FA","#5856D6"};
  const char* accNames[8] = {"Blau","Gruen","Orange","Rot","Pink","Lila","Tuerkis","Indigo"};
  for (int i = 0; i < 8; i++) {
    bool active = (accentColor == accKeys[i]);
    html += "<label style='display:flex;flex-direction:column;align-items:center;gap:4px;cursor:pointer;font-size:11px;color:var(--muted);font-weight:500'>";
    html += "<input type='radio' name='accent' value='" + String(accKeys[i]) + "' style='display:none'" + (active ? " checked" : "") + " onchange='this.form.submit()'>";
    html += "<span style='width:32px;height:32px;border-radius:50%;background:" + String(accHexes[i]) + ";display:inline-block;box-shadow:0 1px 3px rgba(0,0,0,.15);border:3px solid " + (active ? "var(--text)" : "transparent") + ";transition:border-color .15s var(--ease)'></span>";
    html += String(accNames[i]);
    html += "</label>";
  }
  html += "</div></div>";
  html += "<div class='field' style='grid-column:1/-1'><label>Erscheinungsbild</label>";
  html += "<div style='display:flex;gap:10px;margin-top:6px'>";
  html += "<label style='flex:1;padding:12px 16px;border-radius:12px;border:2px solid " + String(appearanceMode == "solid" ? "var(--accent)" : "var(--line)") + ";cursor:pointer;background:var(--card)'>";
  html += "<input type='radio' name='appear' value='solid' style='display:none'" + String(appearanceMode == "solid" ? " checked" : "") + " onchange='this.form.submit()'>";
  html += "<div style='font-weight:600'>Solid</div><div style='font-size:12px;color:var(--muted)'>Klare weisse Karten (Standard)</div>";
  html += "</label>";
  html += "<label style='flex:1;padding:12px 16px;border-radius:12px;border:2px solid " + String(appearanceMode == "glass" ? "var(--accent)" : "var(--line)") + ";cursor:pointer;background:linear-gradient(135deg,#e0eafc,#f5e6ff)'>";
  html += "<input type='radio' name='appear' value='glass' style='display:none'" + String(appearanceMode == "glass" ? " checked" : "") + " onchange='this.form.submit()'>";
  html += "<div style='font-weight:600'>Glass</div><div style='font-size:12px;color:var(--muted)'>Frosted-Glas mit Farbverlauf</div>";
  html += "</label>";
  html += "</div></div>";
  html += "</div>";
  html += "<div class='actions'><button type='submit'>Einstellungen speichern</button></div>";
  html += "</form></section>";

  html += "<section class='card'><div class='panelTitle'><h2>Setup-WLAN-Passwort</h2><span class='badge ";
  html += setupApActive ? "warnb'>Aktiv" : "okb'>Inaktiv";
  html += "</span></div>";
  html += "<p class='small'>Passwort fuer das Setup-WLAN '" + htmlEscape(String(AP_SSID)) + "' (Ersteinrichtung).</p>";
  html += "<form action='/saveaccount' method='post'>";
  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Neues Setup-WLAN-Passwort (min. 8 Zeichen, leer = unveraendert)</label><input name='apPass' type='password' maxlength='64'></div>";
  html += "</div>";
  html += "<div class='actions'><button type='submit' name='formType' value='appass'>Setup-Passwort speichern</button></div>";
  html += "</form></section>";

  html += "<section class='card'><div class='panelTitle'><h2>Firmware-Update ueber GitHub</h2><span class='badge ";
  html += (githubRepo.length() == 0) ? "errb'>Nicht eingerichtet" : "okb'>Eingerichtet";
  html += "</span></div>";
  html += "<p class='small'>Aktuell installierte Version: <b>" + String(FIRMWARE_VERSION) + "</b>. Ein neues Release wird erkannt, wenn im Repository ein Release mit hoeherem Tag (z.B. 'v1.1.0') und einer angehaengten .bin-Datei veroeffentlicht ist.</p>";
  html += "<form action='/saveaccount' method='post'><div class='formGrid'>";
  html += "<div class='field'><label>GitHub-Repository (besitzer/repo)</label><input name='ghRepo' maxlength='100' placeholder='dein-name/dynamic-price-clock' value='" + htmlEscape(githubRepo) + "'></div>";
  html += "<div class='field'><label>Zugriffstoken (optional, nur fuer private Repos)</label><input name='ghToken' type='password' maxlength='100' placeholder='Leer lassen bei oeffentlichem Repo'></div>";
  html += "</div>";
  html += "<div class='actions'><button type='submit' name='formType' value='github'>GitHub-Einstellungen speichern</button></div>";
  html += "</form>";
  html += "<div class='formSection' style='margin-top:16px'>";
  html += "<div class='panelTitle'><h4 style='margin:0'>Update pruefen</h4><span id='ghUpdateBadge' class='badge'>Noch nicht geprueft</span></div>";
  html += "<p id='ghUpdateMsg' class='small'>Klicke auf \"Auf Updates pruefen\", um die neueste Version im hinterlegten Repository abzufragen.</p>";
  html += "<div id='ghProgressWrap' style='display:none;margin:10px 0'>";
  html += "<div style='display:flex;gap:8px;align-items:center;margin-bottom:8px'>";
  html += "<span id='ghPhaseIcon' style='font-size:18px'>&#8987;</span>";
  html += "<span id='ghPhaseLbl' style='font-size:13px;font-weight:600;color:var(--text)'>Verbinde mit GitHub...</span>";
  html += "</div>";
  html += "<div style='background:var(--panel2);border-radius:12px;height:12px;overflow:hidden'><div id='ghProgressBar' style='background:var(--accent);height:100%;width:0%;transition:width .4s;border-radius:12px'></div></div>";
  html += "<div style='display:flex;justify-content:space-between;margin-top:5px'><p id='ghProgressText' class='small' style='margin:0'></p><p id='ghSpeedText' class='small' style='margin:0;color:var(--muted)'></p></div>";
  html += "<p id='ghProgressSub' class='small' style='margin-top:4px;color:var(--muted)'></p>";
  html += "</div>";
  html += "<div class='actions'><button type='button' class='secondary' onclick='checkGhUpdate()'>Auf Updates pruefen</button><button type='button' id='ghUpdateBtn' style='display:none' onclick='startGhUpdate()'>Jetzt aktualisieren</button></div>";
  html += "</div>";
  html += "<div class='formSection' style='margin-top:16px;border-top:1px solid var(--surface-border);padding-top:16px'>";
  html += "<div class='panelTitle'><h4 style='margin:0'>Firmware manuell hochladen</h4><span id='upBadge' class='badge'>Bereit</span></div>";
  html += "<p class='small'>Lade eine .bin-Datei direkt hoch, z.B. fuer ein Downgrade auf eine aeltere Version. Nach dem Upload startet das Geraet automatisch neu.</p>";
  html += "<form id='upForm' onsubmit='return startUpload(event)'>";
  html += "<div class='field'><label>Firmware-Datei (.bin)</label><input type='file' name='firmware' id='upFile' accept='.bin' required></div>";
  html += "<div id='upProgressWrap' style='display:none;margin:10px 0'>";
  html += "<div style='background:var(--surface-border);border-radius:999px;height:10px;overflow:hidden'><div id='upProgressBar' style='background:var(--accent);height:100%;width:0%;transition:width .3s'></div></div>";
  html += "<p id='upProgressText' class='small' style='margin-top:6px'></p>";
  html += "</div>";
  html += "<p id='upMsg' class='small'></p>";
  html += "<div class='actions'><button type='submit' id='upSubmit'>Datei hochladen &amp; installieren</button></div>";
  html += "</form>";
  html += "</div></section>";

  html += "<section class='card'><div class='panelTitle'><h2>Anker Solarbank (PV-Erzeugung)</h2><span class='badge ";
  html += ankerConfigured ? "okb'>Eingerichtet" : "errb'>Nicht eingerichtet";
  html += "</span></div>";
  html += "<p class='small'>Fuer die Energiefluss-Anzeige (Tablet-Modus 2) werden PV-Erzeugung, Batterie und Hausverbrauch ueber deinen Anker-Account abgefragt. Es handelt sich um eine inoffizielle Schnittstelle - Anker kann diese jederzeit aendern.</p>";
  html += "<form action='/saveaccount' method='post'><div class='formGrid'>";
  html += "<div class='field'><label>Anker-Konto E-Mail</label><input name='ankerEmail' type='email' maxlength='80' value='" + htmlEscape(ankerEmail) + "'></div>";
  html += "<div class='field'><label>Anker-Konto Passwort</label><input name='ankerPassword' type='password' maxlength='80' placeholder='Leer lassen = unveraendert'></div>";
  html += "</div>";
  html += "<div class='actions'><button type='submit' name='formType' value='anker'>Anker-Zugangsdaten speichern</button></div>";
  html += "</form>";
  if (ankerConfigured) {
    html += "<div class='formSection' style='margin-top:16px'>";
    html += "<div class='panelTitle'><h4 style='margin:0'>Live-Status</h4><span class='badge ";
    html += (ankerLastError.length() == 0 && ankerPvW >= 0) ? "okb'>Verbunden" : "errb'>Fehler";
    html += "</span></div>";
    if (ankerLastError.length() > 0) {
      html += "<p class='small' style='color:var(--errb-text)'>" + htmlEscape(ankerLastError) + "</p>";
    } else if (ankerPvW >= 0) {
      html += "<p class='small'>PV: " + String(ankerPvW, 0) + " W &middot; Batterie: " + String(ankerBatteryW, 0) + " W &middot; Ausgang: " + String(ankerOutputW, 0) + " W &middot; Hausverbrauch: " + String(ankerHomeLoadW, 0) + " W</p>";
    } else {
      html += "<p class='small'>Noch keine Daten abgefragt - erster Poll erfolgt automatisch innerhalb von 60s.</p>";
    }
    if (ankerLastRawJson.length() > 0) {
      html += "<details style='margin-top:8px'><summary class='small' style='cursor:pointer'>Rohdaten der letzten Anker-Antwort (Debug)</summary>";
      html += "<pre style='white-space:pre-wrap;word-break:break-all;font-size:11px;background:var(--overlay-faint);border-radius:10px;padding:8px;margin-top:6px'>" + htmlEscape(ankerLastRawJson) + "</pre>";
      html += "</details>";
    }
    html += "</div>";
  }
  html += "</section>";

  html += "<script src='/account-update.js'></script>";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void handleSaveAccount() {
  if (!checkAuth()) return;

  String formType = server.hasArg("formType") ? server.arg("formType") : "";
  bool saveOk = true;

  if (formType == "appass") {
    String newApPass = server.hasArg("apPass") ? server.arg("apPass") : "";

    if (newApPass.length() > 0) {
      if (newApPass.length() < 8) {
        lastError = "Setup-WLAN-Passwort zu kurz (min. 8 Zeichen)";
        saveOk = false;
      } else {
        apPassword = newApPass;
        prefs.putString("apPass", apPassword);
        lastError = "";

        if (setupApActive) {
          WiFi.softAPdisconnect(true);
          setupApActive = false;
          delay(300);
          startSetupPortal(setupApPermanent);
        }
      }
    }
  } else if (formType == "tibberca") {
    String newCa = server.hasArg("tibberCa") ? server.arg("tibberCa") : "";
    newCa.trim();

    if (newCa.indexOf("BEGIN CERTIFICATE") < 0) {
      lastError = "Kein gueltiges PEM-Zertifikat (BEGIN CERTIFICATE fehlt)";
      saveOk = false;
    } else {
      tibberRootCaPem = newCa;
      prefs.putString("tibberCa", tibberRootCaPem);
      lastError = "";
    }
  } else if (formType == "tibbercaclear") {
    tibberRootCaPem = "";
    prefs.putString("tibberCa", "");
  } else if (formType == "github") {
    String newRepo = server.hasArg("ghRepo") ? server.arg("ghRepo") : "";
    newRepo.trim();
    if (newRepo.length() > 100) newRepo = newRepo.substring(0, 100);
    githubRepo = newRepo;
    prefs.putString("ghRepo", githubRepo);

    String newToken = server.hasArg("ghToken") ? server.arg("ghToken") : "";
    if (newToken.length() > 0) {
      if (newToken.length() > 100) newToken = newToken.substring(0, 100);
      githubToken = newToken;
      prefs.putString("ghToken", githubToken);
    }
  } else if (formType == "anker") {
    String newEmail = server.hasArg("ankerEmail") ? server.arg("ankerEmail") : "";
    newEmail.trim();
    if (newEmail.length() > 80) newEmail = newEmail.substring(0, 80);
    bool emailChanged = (newEmail != ankerEmail);
    ankerEmail = newEmail;
    prefs.putString("ankerEmail", ankerEmail);

    String newPass = server.hasArg("ankerPassword") ? server.arg("ankerPassword") : "";
    bool passChanged = false;
    if (newPass.length() > 0) {
      if (newPass.length() > 80) newPass = newPass.substring(0, 80);
      ankerPassword = newPass;
      prefs.putString("ankerPass", ankerPassword);
      passChanged = true;
    }

    ankerConfigured = (ankerEmail.length() > 0 && ankerPassword.length() > 0);
    if (emailChanged || passChanged) {
      // Zugangsdaten geaendert -> alten Token/Site verwerfen, naechster Poll loggt neu ein
      ankerAuthToken = "";
      ankerUserId = "";
      ankerGtoken = "";
      ankerSiteId = "";
      prefs.putString("ankerSite", "");
      ankerPvW = -1; ankerBatteryW = -1; ankerOutputW = -1; ankerHomeLoadW = -1;
      ankerLastError = "";
    }
  } else if (formType == "priceprovider") {
    String newProvider = server.hasArg("priceProvider") ? server.arg("priceProvider") : "tibber";
    Serial.print("DEBUG priceprovider-Formular empfangen. Rohwert 'priceProvider': '");
    Serial.print(newProvider);
    Serial.println("'");

    if (newProvider != "tibber" && newProvider != "awattar_de" && newProvider != "awattar_at") newProvider = "tibber";
    priceProvider = newProvider;
    prefs.putString("priceProv", priceProvider);

    Serial.print("DEBUG priceProvider nach Speichern (RAM): '");
    Serial.print(priceProvider);
    Serial.print("' / aus Preferences zurueckgelesen: '");
    Serial.print(prefs.getString("priceProv", "?"));
    Serial.println("'");

    float newSurchg = server.hasArg("priceSurchg") ? server.arg("priceSurchg").toFloat() : 0.0;
    if (newSurchg < 0) newSurchg = 0;
    if (newSurchg > 100) newSurchg = 100;
    priceSurchargeCt = newSurchg;
    prefs.putFloat("priceSurchg", priceSurchargeCt);

    float newVat = server.hasArg("priceVat") ? server.arg("priceVat").toFloat() : 0.0;
    if (newVat < 0) newVat = 0;
    if (newVat > 50) newVat = 50;
    priceVatPercent = newVat;
    prefs.putFloat("priceVat", priceVatPercent);

    // Preis-Refresh NICHT synchron hier ausloesen (blockierender Netzwerk-Call
    // wuerde die HTTP-Antwort verzoegern/riskieren) - stattdessen den naechsten
    // loop()-Tick sofort dazu zwingen, die neue Quelle abzufragen.
    lastUpdate = 0;
  }

  bool acctBelongsToProviderPage = (formType == "priceprovider" || formType == "tibberca" || formType == "tibbercaclear");
  String acctRedirectTarget = acctBelongsToProviderPage ? "/anbieter" : "/account";
  server.sendHeader("Location", saveOk ? (acctRedirectTarget + "?saved=1") : (acctRedirectTarget + "?saved=0"));
  server.send(303);
}

// -----------------------------------------------------------------------------
// Anbieter-Seite (Strompreis-Quelle + Tibber-Zugang)
// -----------------------------------------------------------------------------

void handleProviderPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(14500);

  html += htmlHeader("Anbieter");
  html += "<section class='hero'><h1>Anbieter</h1><p>Strompreis-Quelle waehlen und Zugangsdaten hinterlegen.</p></section>";
  html += navTabs("/anbieter");

  html += "<section class='card'><div class='panelTitle'><h2>Strompreis-Quelle</h2><div style='display:flex;gap:6px'><span class='badge ";
  html += (priceProvider == "tibber") ? "okb'>Tibber" : "okb'>aWATTar";
  html += "</span><span id='providerSaveState' class='badge warnb'>Bereit</span></div></div>";
  html += "<p class='small'>Waehle, woher die Strompreise geladen werden. Aenderungen werden automatisch gespeichert. Tibber braucht einen Zugangstoken (Abschnitt unten). aWATTar (Deutschland/Oesterreich) ist frei ohne Anmeldung nutzbar, liefert aber nur den Boersenpreis ohne Netzentgelte/Steuern - diese koennen als Aufschlag ergaenzt werden.</p>";
  html += "<form id='providerForm' action='/saveaccount' method='post'>";
  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Anbieter</label><select name='priceProvider'>";
  html += "<option value='tibber'"; if (priceProvider == "tibber") html += " selected"; html += ">Tibber</option>";
  html += "<option value='awattar_de'"; if (priceProvider == "awattar_de") html += " selected"; html += ">aWATTar Deutschland</option>";
  html += "<option value='awattar_at'"; if (priceProvider == "awattar_at") html += " selected"; html += ">aWATTar Oesterreich</option>";
  html += "</select></div>";
  html += "<div class='field'><label>Aufschlag Netzentgelte/Steuern (ct/kWh)</label><input name='priceSurchg' type='number' step='0.01' min='0' max='100' value='" + String(priceSurchargeCt) + "'></div>";
  html += "<div class='field'><label>Mehrwertsteuer (%)</label><input name='priceVat' type='number' step='0.1' min='0' max='50' value='" + String(priceVatPercent) + "'></div>";
  html += "</div>";
  html += "<p class='small'>Nur bei aWATTar wirksam: Endpreis = (Boersenpreis/1000 + Aufschlag/100) &times; (1 + MwSt/100) EUR/kWh.</p>";
  html += "<div class='actions'><button type='submit' name='formType' value='priceprovider'>Strompreis-Quelle speichern</button><button type='button' class='secondary' onclick='saveProviderNow()'>Direkt speichern</button></div>";
  html += "</form></section>";

  html += "<section class='card'><div class='panelTitle'><h2>Tibber-Zugang</h2><div style='display:flex;gap:6px'><span class='badge ";
  html += (tibberToken.length() == 0) ? "errb'>Kein Token" : "okb'>Token hinterlegt";
  html += "</span><span id='tibberSaveState' class='badge warnb'>Bereit</span></div></div>";
  html += "<p class='small'>Nur relevant, wenn oben Tibber als Anbieter ausgewaehlt ist. Aenderungen werden automatisch gespeichert. TLS-Zertifikatspruefung fuer die Tibber-API findest du auf der Konto-Seite.</p>";
  html += "<form id='tibberForm' action='/save' method='post'>";
  html += "<input type='hidden' name='redirectTo' value='/anbieter'>";
  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Tibber Token (leer = unveraendert)</label><input name='token' type='password' placeholder='Neuen Token eingeben'></div>";
  html += "<div class='field'><label>Home</label><select name='homeId'>";

  for (int i = 0; i < homeCount; i++) {
    html += "<option value='";
    html += htmlEscape(homeIds[i]);
    html += "'";

    if (homeIds[i] == selectedHomeId) {
      html += " selected";
    }

    html += ">";
    html += htmlEscape(homeNames[i]);
    html += "</option>";
  }

  html += "</select></div>";
  html += "<div class='field'><label>Grundgebühr (EUR/Monat)</label><input name='baseFee' type='number' step='0.01' min='0' max='100' value='" + String(tibberBaseFeeEur, 2) + "' title='Wird zu den Energiekosten addiert, damit die Monatskosten-Anzeige dem echten Rechnungsbetrag entspricht. Die Tibber-API liefert nur die reinen Energiekosten ohne Grundgebühr.'></div>";
  html += "</div>";
  html += "<div class='actions'><button type='submit'>Tibber-Zugang speichern</button><button type='button' class='secondary' onclick='saveTibberNow()'>Direkt speichern</button></div>";
  html += "</form></section>";

  html += "<section class='card'><div class='panelTitle'><h2>TLS-Zertifikatspruefung Tibber-API</h2><span class='badge ";
  html += (tibberRootCaPem.length() == 0) ? "errb'>Ungeprueft" : "okb'>Geprueft";
  html += "</span></div>";
  if (tibberRootCaPem.length() == 0) {
    html += "<p class='err'>Kein Root-CA-Zertifikat hinterlegt. Die Verbindung zur Tibber-API laeuft aktuell OHNE Zertifikatspruefung (setInsecure). Das ist ein Sicherheitsrisiko (Man-in-the-Middle).</p>";
  } else {
    html += "<p class='ok'>Root-CA-Zertifikat hinterlegt. TLS-Verbindungen zur Tibber-API werden geprueft.</p>";
  }
  html += "<p class='small'>So ermittelst du das aktuelle Zertifikat (auf einem PC, NICHT auf dem ESP32):<br>";
  html += "<code>openssl s_client -connect api.tibber.com:443 -showcerts &lt;/dev/null | openssl x509 -outform PEM</code><br>";
  html += "Das letzte Zertifikat der ausgegebenen Kette (Root-CA) hier komplett inkl. BEGIN/END-Zeilen einfuegen.</p>";
  html += "<form action='/saveaccount' method='post'>";
  html += "<div class='field'><label>Root-CA PEM</label><textarea name='tibberCa' rows='10' placeholder='-----BEGIN CERTIFICATE-----'>" + htmlEscape(tibberRootCaPem) + "</textarea></div>";
  html += "<div class='actions'><button type='submit' name='formType' value='tibberca'>Zertifikat speichern</button>";
  if (tibberRootCaPem.length() > 0) {
    html += "<button type='submit' name='formType' value='tibbercaclear' class='danger'>Zertifikat entfernen</button>";
  }
  html += "</div></form></section>";

  html += R"JS(
<script>
let providerSaveTimer=null;
function providerState(t,c){const s=document.getElementById('providerSaveState');if(s){s.className='badge '+c;s.innerText=t;}}
function saveProviderNow(){
  const form=document.getElementById('providerForm');
  if(!form){providerState('Kein Formular','errb');return;}
  const body=new URLSearchParams(new FormData(form)).toString();
  providerState('Speichere...','warnb');
  fetch('/saveproviderajax',{method:'POST',cache:'no-store',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:body})
    .then(r=>r.text())
    .then(t=>{if(t.indexOf('OK')>=0){providerState('Gespeichert','okb');}else{providerState('Fehler','errb');}})
    .catch(()=>{providerState('Fehler','errb');});
}
function scheduleProviderSave(){clearTimeout(providerSaveTimer);providerState('Änderungen...','warnb');providerSaveTimer=setTimeout(saveProviderNow,400);}

let tibberSaveTimer=null;
function tibberState(t,c){const s=document.getElementById('tibberSaveState');if(s){s.className='badge '+c;s.innerText=t;}}
function saveTibberNow(){
  const form=document.getElementById('tibberForm');
  if(!form){tibberState('Kein Formular','errb');return;}
  const body=new URLSearchParams(new FormData(form)).toString();
  tibberState('Speichere...','warnb');
  fetch('/savetibberajax',{method:'POST',cache:'no-store',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:body})
    .then(r=>r.text())
    .then(t=>{if(t.indexOf('OK')>=0){tibberState('Gespeichert','okb');}else{tibberState('Fehler','errb');}})
    .catch(()=>{tibberState('Fehler','errb');});
}
function scheduleTibberSave(){clearTimeout(tibberSaveTimer);tibberState('Änderungen...','warnb');tibberSaveTimer=setTimeout(saveTibberNow,400);}

document.addEventListener('DOMContentLoaded',()=>{
  const pForm=document.getElementById('providerForm');
  if(pForm){
    pForm.querySelectorAll('input,select').forEach(el=>{
      el.addEventListener('input',scheduleProviderSave);
      el.addEventListener('change',saveProviderNow);
    });
    pForm.addEventListener('submit',()=>{providerState('Speichere...','warnb');});
  }
  const tForm=document.getElementById('tibberForm');
  if(tForm){
    tForm.querySelectorAll('input,select').forEach(el=>{
      el.addEventListener('input',scheduleTibberSave);
      el.addEventListener('change',saveTibberNow);
    });
    tForm.addEventListener('submit',()=>{tibberState('Speichere...','warnb');});
  }
});
</script>
)JS";

  html += htmlFooter();

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void handleSaveProviderAjax() {
  if (!checkAuth()) return;

  String newProvider = server.hasArg("priceProvider") ? server.arg("priceProvider") : "tibber";
  if (newProvider != "tibber" && newProvider != "awattar_de" && newProvider != "awattar_at") newProvider = "tibber";
  priceProvider = newProvider;
  prefs.putString("priceProv", priceProvider);

  float newSurchg = server.hasArg("priceSurchg") ? server.arg("priceSurchg").toFloat() : 0.0;
  if (newSurchg < 0) newSurchg = 0;
  if (newSurchg > 100) newSurchg = 100;
  priceSurchargeCt = newSurchg;
  prefs.putFloat("priceSurchg", priceSurchargeCt);

  float newVat = server.hasArg("priceVat") ? server.arg("priceVat").toFloat() : 0.0;
  if (newVat < 0) newVat = 0;
  if (newVat > 50) newVat = 50;
  priceVatPercent = newVat;
  prefs.putFloat("priceVat", priceVatPercent);

  // Preis-Refresh NICHT synchron hier ausloesen, siehe Kommentar in
  // handleSaveAccount() - stattdessen den naechsten loop()-Tick dazu zwingen.
  lastUpdate = 0;

  server.send(200, "text/plain", "OK provider=" + priceProvider);
}

void handleSaveTibberAjax() {
  if (!checkAuth()) return;

  String newToken = server.hasArg("token") ? server.arg("token") : "";
  newToken.trim();

  if (newToken.length() > 0) {
    tibberToken = newToken;
    prefs.putString("token", tibberToken);
  }

  if (server.hasArg("homeId")) {
    selectedHomeId = server.arg("homeId");
    prefs.putString("homeId", selectedHomeId);
  }

  if (server.hasArg("baseFee")) {
    float newFee = server.arg("baseFee").toFloat();
    if (newFee < 0) newFee = 0;
    if (newFee > 100) newFee = 100;
    tibberBaseFeeEur = newFee;
    prefs.putFloat("baseFee", tibberBaseFeeEur);
  }

  server.send(200, "text/plain", "OK");
}

// -----------------------------------------------------------------------------
// WLAN-Seite
// -----------------------------------------------------------------------------

int wifiRssiToQuality(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  int q = 2 * (rssi + 100);
  if (q < 0) q = 0;
  if (q > 100) q = 100;
  return q;
}

String wifiQualityLabel(int quality) {
  if (quality >= 80) return "Sehr gut";
  if (quality >= 60) return "Gut";
  if (quality >= 40) return "Mittel";
  if (quality >= 20) return "Schwach";
  return "Sehr schwach";
}

String jsonEscapeValue(String s) {
  String out = "";

  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s.charAt(i);

    if (c == 92) {
      out += (char)92;
      out += (char)92;
    } else if (c == 34) {
      out += (char)92;
      out += (char)34;
    } else if (c == 10) {
      out += (char)92;
      out += 'n';
    } else if (c == 13) {
      // ignore
    } else if (c < 32) {
      // ignore
    } else {
      out += (char)c;
    }
  }

  return out;
}

void handleWifiStatusJson() {
  if (!checkAuth()) return;

  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;
  int quality = WiFi.status() == WL_CONNECTED ? wifiRssiToQuality(rssi) : 0;

  String json = "{";
  json += "\"connected\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"status\":\"" + jsonEscapeValue(wifiStatusToText(WiFi.status())) + "\"";
  json += ",\"savedSsid\":\"" + jsonEscapeValue(wifiSsid) + "\"";
  json += ",\"activeSsid\":\"" + jsonEscapeValue(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("")) + "\"";
  json += ",\"ip\":\"" + jsonEscapeValue(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"rssi\":" + String(rssi);
  json += ",\"quality\":" + String(quality);
  json += ",\"channel\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0);
  json += ",\"mac\":\"" + jsonEscapeValue(getStaMacText()) + "\"";
  json += ",\"chip\":\"" + jsonEscapeValue(getChipInfoText()) + "\"";
  json += ",\"wifiBand\":" + String(wifiBandPreference);
  json += ",\"setupActive\":";
  json += setupApActive ? "true" : "false";
  json += ",\"setupSsid\":\"" + jsonEscapeValue(setupApSsid) + "\"";
  json += ",\"setupIp\":\"" + jsonEscapeValue(setupApActive ? WiFi.softAPIP().toString() : String("")) + "\"";
  json += ",\"macRotate\":";
  json += wifiMacRotationEnabled ? "true" : "false";
  json += ",\"staticIpOn\":";
  json += wifiStaticIpEnabled ? "true" : "false";
  json += ",\"auto5thIp\":";
  json += wifiAutoFifthLastIpEnabled ? "true" : "false";
  json += ",\"staticIp\":\"" + jsonEscapeValue(wifiStaticIpText) + "\"";
  json += ",\"gateway\":\"" + jsonEscapeValue(wifiGatewayText) + "\"";
  json += ",\"subnet\":\"" + jsonEscapeValue(wifiSubnetText) + "\"";
  json += ",\"dns1\":\"" + jsonEscapeValue(wifiDns1Text) + "\"";
  json += ",\"dns2\":\"" + jsonEscapeValue(wifiDns2Text) + "\"";
  json += ",\"lastError\":\"" + jsonEscapeValue(lastError) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleWifiScanJson() {
  if (!checkAuth()) return;

  if (!setupApActive && WiFi.status() != WL_CONNECTED) {
    startSetupPortal(true);
  }

  if (setupApActive) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.setSleep(false);

  int n = WiFi.scanNetworks(false, true);
  String json = "{\"networks\":[";

  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";

    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    int quality = wifiRssiToQuality(rssi);

    json += "{";
    json += "\"ssid\":\"" + jsonEscapeValue(ssid) + "\"";
    json += ",\"bssid\":\"" + jsonEscapeValue(WiFi.BSSIDstr(i)) + "\"";
    json += ",\"rssi\":" + String(rssi);
    json += ",\"quality\":" + String(quality);
    json += ",\"channel\":" + String(WiFi.channel(i));
    json += ",\"auth\":" + String((int)WiFi.encryptionType(i));
    json += "}";
  }

  json += "]}";
  WiFi.scanDelete();

  server.send(200, "application/json", json);
}

void handleSaveWifiAjax() {
  if (!checkAuth()) return;

  String newSsid = "";

  if (server.hasArg("ssid")) {
    newSsid = server.arg("ssid");
    newSsid.trim();
  }

  if (newSsid.length() > 0) {
    wifiSsid = newSsid;
    prefs.putString("wifiSsid", wifiSsid);
  }

  if (server.hasArg("pass")) {
    wifiPassword = server.arg("pass");
    prefs.putString("wifiPass", wifiPassword);
  }

  if (server.hasArg("setupSsid")) {
    setupApSsid = server.arg("setupSsid");
    setupApSsid.trim();
    if (setupApSsid.length() == 0) setupApSsid = DEFAULT_WIFI_SETUP_AP_SSID;
    if (setupApSsid.length() > 32) setupApSsid = setupApSsid.substring(0, 32);
    prefs.putString("setupSsid", setupApSsid);

    if (setupApActive) {
      WiFi.softAPdisconnect(true);
      setupApActive = false;
      delay(300);
      startSetupPortal(true);
    }
  }

  if (server.hasArg("macRotate")) {
    wifiMacRotationEnabled = server.arg("macRotate") == "1";
    prefs.putBool("macRotate", wifiMacRotationEnabled);
  }

  if (server.hasArg("wifiBand")) {
    wifiBandPreference = server.arg("wifiBand").toInt();
    if (wifiBandPreference < 0) wifiBandPreference = 0;
    if (wifiBandPreference > 2) wifiBandPreference = 2;
    prefs.putInt("wifiBand", wifiBandPreference);
  }

  if (server.hasArg("staticIpOn")) {
    wifiStaticIpEnabled = server.arg("staticIpOn") == "1";
    prefs.putBool("staticIpOn", wifiStaticIpEnabled);
  }

  if (server.hasArg("auto5thIp")) {
    wifiAutoFifthLastIpEnabled = server.arg("auto5thIp") == "1";
    prefs.putBool("auto5thIp", wifiAutoFifthLastIpEnabled);
  }

  if (server.hasArg("staticIp")) {
    wifiStaticIpText = server.arg("staticIp");
    wifiStaticIpText.trim();
    prefs.putString("staticIp", wifiStaticIpText);
  }

  if (server.hasArg("gateway")) {
    wifiGatewayText = server.arg("gateway");
    wifiGatewayText.trim();
    prefs.putString("gateway", wifiGatewayText);
  }

  if (server.hasArg("subnet")) {
    wifiSubnetText = server.arg("subnet");
    wifiSubnetText.trim();
    if (wifiSubnetText.length() == 0) wifiSubnetText = "255.255.255.0";
    prefs.putString("subnet", wifiSubnetText);
  }

  if (server.hasArg("dns1")) {
    wifiDns1Text = server.arg("dns1");
    wifiDns1Text.trim();
    prefs.putString("dns1", wifiDns1Text);
  }

  if (server.hasArg("dns2")) {
    wifiDns2Text = server.arg("dns2");
    wifiDns2Text.trim();
    prefs.putString("dns2", wifiDns2Text);
  }

  if (wifiAutoFifthLastIpEnabled) wifiStaticIpEnabled = false;

  startSetupPortal(true);
  pendingWifiReconnect = true;
  pendingWifiReconnectAt = millis() + 1500;
  lastError = "WLAN gespeichert, Verbindung startet gleich";
  showWifiBootStatus("WLAN gespeichert", wifiSsid, "Verbinde gleich");

  String response = "{\"ok\":true,\"message\":\"gespeichert\"}";
  server.send(200, "application/json", response);
}

void handleConnectWifiAjax() {
  if (!checkAuth()) return;

  startSetupPortal(true);
  pendingWifiReconnect = true;
  pendingWifiReconnectAt = millis() + 1000;
  lastError = "WLAN Verbindung manuell gestartet";
  showWifiBootStatus("Verbinde neu", wifiSsid, "Setup bleibt aktiv");

  server.send(200, "application/json", "{\"ok\":true,\"message\":\"connect_started\"}");
}

void handlePinoutPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(28000);

  html += htmlHeader("Pinout");
  html += "<section class='hero'><h1>Pinout / Verdrahtung</h1><p>GPIO-Belegung fuer ESP32-C5, zwei GC9A01 TFTs, MAX7219 Matrix und WS2812B/WS2818 Tagesring.</p></section>";
  html += navTabs("/pinout");

  html += "<section class='card'><div class='panelTitle'><h2>Schaltplan</h2><span class='badge okb'>8 GPIOs belegt</span></div>";
  html += buildPinoutSvg();
  html += "<p class='small' style='margin-top:10px'>Schematische Uebersicht, keine exakte Silkscreen-Abbildung deines konkreten ESP32-C5-Boards. Pruefe die GPIO-Nummern immer zusaetzlich gegen die Beschriftung auf deinem Board.</p>";
  html += "</section>";

  html += "<section class='card'><div class='panelTitle'><h2>Wichtigste Pins</h2><span class='badge okb'>6 Verbindungen</span></div>";
  html += "<div class='gridCards'>";
  html += "<div class='metric'><div class='label'>SPI-Takt (SCLK) / SPI-Daten (MOSI)</div><div class='value' style='font-size:22px'>GPIO" + String(TFT_SCLK_PIN) + " / GPIO" + String(TFT_MOSI_PIN) + "</div><div class='sub'>Gemeinsamer Datenbus fuer beide Displays UND die LED-Matrix</div></div>";
  html += "<div class='metric'><div class='label'>Display Data/Command (DC) / Reset (RST)</div><div class='value' style='font-size:22px'>GPIO" + String(TFT_DC_PIN) + " / GPIO" + String(TFT_RST_PIN) + "</div><div class='sub'>Gemeinsam fuer beide Displays, fest im Code</div></div>";
  html += "<div class='metric'><div class='label'>Display 1 Chip-Select</div><div class='value'>GPIO" + String(TFT1_CS_PIN) + "</div><div class='sub'>Waehlt Display 1 auf dem SPI-Bus aus, fest im Code</div></div>";
  html += "<div class='metric'><div class='label'>Display 2 Chip-Select</div><div class='value'>GPIO" + String(TFT2_CS_PIN) + "</div><div class='sub'>Waehlt Display 2 auf dem SPI-Bus aus, fest im Code</div></div>";
  html += "<div class='metric'><div class='label'>Tagesring Datenleitung (DIN)</div><div class='value'>GPIO" + String(LED_RING_PIN) + "</div><div class='sub'>WS2812B/WS2818, 60 LEDs</div></div>";
  html += "<div class='metric'><div class='label'>LED-Matrix Daten / Takt / Chip-Select</div><div class='value' style='font-size:20px'>GPIO" + String(MATRIX_DIN_PIN) + " / GPIO" + String(MATRIX_CLK_PIN) + " / GPIO" + String(MATRIX_CS_PIN) + "</div><div class='sub'>MAX7219 Daisy-Chain, teilt sich Takt/Daten mit den Displays</div></div>";
  html += "</div>";
  html += "<p class='small' style='margin-top:14px'>Gemeinsame Masse fuer ESP32-C5, Matrix, TFTs und LED-Ring verwenden. Niemals 5V auf den 3V3-Pin geben.</p></section>";

  if (server.hasArg("pinsChanged")) {
    html += "<section class='card' style='border-color:rgba(180,83,9,.4)'><div class='panelTitle'><h2>Neustart erforderlich</h2><span class='badge warnb'>Neue GPIOs noch nicht aktiv</span></div>";
    html += "<p class='small'>Die neuen GPIO-Zuordnungen sind gespeichert, wirken sich aber erst nach einem Neustart aus.</p>";
    html += "<form action='/restartdevice' method='post'><div class='actions'><button type='submit' class='danger'>Jetzt neu starten</button></div></form></section>";
  }

  html += "<section class='card'><div class='panelTitle'><h2>GPIO-Zuordnung aendern</h2><span class='badge warnb'>Fortgeschritten</span></div>";
  html += "<p class='small'>Hier aenderst du, welchen GPIO die Firmware fuer eine Funktion verwendet - das aendert nicht die Verkabelung selbst. Du musst das jeweilige Kabel danach passend an den neuen Pin umstecken, sonst funktioniert diese Funktion nicht mehr. Ein Neustart ist nach dem Speichern noetig, damit die Aenderung wirkt. Die WLAN-/Web-Verwaltung bleibt davon unberuehrt, auch wenn ein Display oder der Tagesring wegen einer falschen Zuordnung nicht mehr reagiert.</p>";
  html += "<p class='small'><b>Hinweis zur Liste:</b> Die Auswahl umfasst GPIO0-27. Welche Pins auf deinem konkreten ESP32-C5-Board tatsaechlich herausgefuehrt bzw. fuer internen Flash reserviert sind, kann je nach Modul/Board variieren - pruefe bei Unsicherheit das Datenblatt oder die Beschriftung deines Boards. GPIO0 ist bei den meisten ESP32-Chips ein Boot-Strapping-Pin und wird deshalb markiert.</p>";
  html += "<form action='/savepins' method='post'><div class='formGrid'>";
  addGpioSelect(html, "tftSclkPin", "SPI-Takt (SCLK) - fuer beide Displays und die Matrix", "Gemeinsamer Taktleitung des SPI-Busses. Aendern betrifft Display 1, Display 2 und die LED-Matrix gleichzeitig.", tftSclkPin);
  addGpioSelect(html, "tftMosiPin", "SPI-Daten (MOSI) - fuer beide Displays und die Matrix", "Gemeinsame Datenleitung des SPI-Busses. Aendern betrifft Display 1, Display 2 und die LED-Matrix gleichzeitig.", tftMosiPin);
  addGpioSelect(html, "ledRingPin", "Tagesring Datenleitung (DIN)", "Steuert die WS2812B/WS2818 LED-Kette des Tagesrings.", ledRingPinVar);
  addGpioSelect(html, "matrixCsPin", "LED-Matrix Chip-Select (CS)", "Waehlt die MAX7219-Matrixkette auf dem SPI-Bus aus.", matrixCsPinVar);
  html += "</div>";
  html += "<div class='actions'><button type='submit'>GPIOs speichern</button></div></form>";
  html += "<div class='formSection' style='margin-top:16px'><h4>Fest im Code (nicht per Web aenderbar)</h4><p class='small'>Diese 4 Pins stecken in den Display-Objekten, die schon vor dem eigentlichen Programmstart angelegt werden. Sie per Web aenderbar zu machen wuerde einen groesseren, riskanten Firmware-Umbau erfordern. Aendere sie stattdessen direkt im Sketch (Konstanten TFT_DC_PIN, TFT_RST_PIN, TFT1_CS_PIN, TFT2_CS_PIN) und flashe neu.</p>";
  html += "<div class='gridCards'>";
  html += "<div class='metric'><div class='label'>Display Data/Command (DC)</div><div class='value'>GPIO" + String(TFT_DC_PIN) + "</div></div>";
  html += "<div class='metric'><div class='label'>Display Reset (RST)</div><div class='value'>GPIO" + String(TFT_RST_PIN) + "</div></div>";
  html += "<div class='metric'><div class='label'>Display 1 Chip-Select</div><div class='value'>GPIO" + String(TFT1_CS_PIN) + "</div></div>";
  html += "<div class='metric'><div class='label'>Display 2 Chip-Select</div><div class='value'>GPIO" + String(TFT2_CS_PIN) + "</div></div>";
  html += "</div></div>";
  html += "</section>";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

// -----------------------------------------------------------------------------
// Tablet-/Kiosk-Modus
// -----------------------------------------------------------------------------

void handleKioskPage() {
  if (!checkAuth()) return;

  String statusText, statusColor;
  getKioskPriceStatus(statusText, statusColor);

  // Ein groesseres html.reserve() allein loest Truncation bei fragmentiertem
  // Heap NICHT zuverlaessig (siehe ausfuehrlicher Kommentar in handleRoot() -
  // live nachgewiesen, dass ein einzelner grosser reserve()-Aufruf bei
  // fragmentiertem Heap komplett fehlschlagen und die Seite dadurch sogar
  // NOCH frueher abgeschnitten werden kann als mit einem kleineren reserve().
  // Die Antwort wird deshalb jetzt in mehreren kleinen Chunks per
  // server.sendContent() gesendet (HTTP Chunked Transfer Encoding) - jeder
  // einzelne Chunk braucht nur ein kleines, viel eher verfuegbares
  // zusammenhaengendes Speicherstueck statt eines einzigen Blocks fuer die
  // komplette Seite.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String html;
  html.reserve(6000);

  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>";
  html += htmlEscape(webInterfaceName + " - Tablet-Modus");
  html += "</title>";
  html += "<script>(function(){try{if(localStorage.getItem('theme')==='dark'){document.documentElement.setAttribute('data-theme','dark');}}catch(e){}})();</script>";
  html += "<link rel='stylesheet' href='/style.css?v=" ASSET_VERSION "'>";
  html += "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>";
  // Zonen-Farben fuer animierten Hintergrund
  String zoneColor1 = "#003d1a"; String zoneColor2 = "#001a0d"; // günstig: grün
  if (metricCurrent15 >= 0) {
    int nc = euroToCentRounded(metricCurrent15);
    if (nc >= ledRedCent) { zoneColor1 = "#3d0a00"; zoneColor2 = "#1a0400"; }
    else if (nc >= ledYellowCent) { zoneColor1 = "#3d2200"; zoneColor2 = "#1a0f00"; }
  }
  html += "<style>";
  // Animierter Hintergrund: langsam rotierendes Gradient + Zone-Ambient-Glow
  html += "@keyframes bgRotate{0%{background-position:0% 50%}50%{background-position:100% 50%}100%{background-position:0% 50%}}";
  html += "@keyframes bgPulse{0%,100%{opacity:.7}50%{opacity:1}}";
  html += "html,body{height:100%;overflow:hidden;margin:0}";
  html += "body{padding:0!important;display:flex;align-items:center;justify-content:center;background:#000;position:relative}";
  // Schicht 1: tiefer Hintergrundverlauf
  html += "body::before{content:'';position:fixed;inset:0;background:radial-gradient(ellipse 80% 60% at 50% 0%," + zoneColor1 + ",transparent 70%),radial-gradient(ellipse 60% 80% at 20% 100%," + zoneColor2 + ",transparent 70%),#000;animation:bgPulse 8s ease-in-out infinite;z-index:0}";
  // Schicht 2: bewegter Farb-Orb
  html += "body::after{content:'';position:fixed;width:60vmax;height:60vmax;left:50%;top:50%;transform:translate(-50%,-50%);background:radial-gradient(circle," + zoneColor1 + " 0%,transparent 70%);animation:bgRotate 20s ease infinite,bgPulse 12s ease-in-out infinite 4s;background-size:200% 200%;border-radius:50%;z-index:0;filter:blur(40px);opacity:.5}";
  html += ".kiosk-wrap{position:relative;z-index:1;display:flex;flex-direction:column;align-items:center;max-height:100vh;padding:clamp(8px,2.2vh,20px);box-sizing:border-box;text-align:center;overflow:hidden}";
  // Die px-Obergrenze ist bewusst grosszuegig (bis knapp ueber 4K) statt am
  // urspruenglich anvisierten Tablet-Format orientiert: min() greift auf
  // kleineren Displays ohnehin ueber vw/vh, auf einem grossen TV soll die
  // Kiosk-Flaeche aber wirklich den Bildschirm ausfuellen statt zentriert
  // klein zu bleiben.
  html += ".kiosk-canvas{position:relative;display:grid;grid-template-columns:repeat(" + String(KIOSK_GRID_COLS_PORTRAIT) + ",1fr);grid-template-rows:repeat(" + String(KIOSK_GRID_ROWS_PORTRAIT) + ",1fr);gap:clamp(4px,1vh,10px);width:min(97vw,2160px);height:min(94vh,3840px);box-sizing:border-box;margin:0 auto}";
  // Widget-Grundklasse: Glassmorphism
  html += ".kw{position:relative;container-type:inline-size;overflow:hidden;box-sizing:border-box;display:flex;flex-direction:column;align-items:center;justify-content:center;min-width:0;min-height:0;border-radius:clamp(12px,2vh,24px);background:rgba(255,255,255,.07);backdrop-filter:blur(24px) saturate(160%);-webkit-backdrop-filter:blur(24px) saturate(160%);border:1px solid rgba(255,255,255,.12);transition:box-shadow .2s ease}";
  html += ".kw.wg-dragging,.kw.wg-resizing{box-shadow:0 20px 44px rgba(0,0,0,.5);z-index:50}";
  html += ".kiosk-arrange-mode .kw{cursor:grab;user-select:none}";
  html += ".kiosk-arrange-mode .kw.wg-dragging{cursor:grabbing}";
  html += ".kw-resize{display:none;position:absolute;right:6px;bottom:6px;width:18px;height:18px;border-radius:50%;background:#fff;border:2px solid rgba(0,0,0,.4);cursor:nwse-resize;z-index:5}";
  html += ".kiosk-arrange-mode .kw-resize{display:block}";
  // Text-Inhalte sind ab hier bewusst mit cqi (Container-Query-Inline-Size,
  // d.h. relativ zur Breite des jeweiligen .kw-Widgets) statt vh skaliert:
  // vh haengt nur von der Bildschirmhoehe ab, nicht von der tatsaechlichen
  // Widget-Groesse - seit Widgets im Anordnen-Modus frei in der Groesse
  // veraenderbar sind, muss der Inhalt stattdessen zur eigenen Box skalieren,
  // sonst bleibt Text beim Verkleinern/Vergroessern eines einzelnen Widgets
  // unveraendert. SVGs (Gauge/Diagramm) brauchen das nicht, die skalieren
  // durch ihr viewBox bereits automatisch mit der Containergroesse.
  html += ".kiosk-time{font-size:clamp(16px,16cqi,56px);font-weight:700;line-height:1;letter-spacing:-1px;color:#fff;font-variant-numeric:tabular-nums}";
  html += ".kiosk-date{font-size:clamp(8px,4cqi,15px);color:rgba(255,255,255,.65);margin-top:4px;text-transform:capitalize;font-weight:500}";
  html += ".kw-gauge svg{width:100%;height:100%;background:transparent;border:0;margin:0}";
  html += ".kw-gauge .priceGauge{max-width:100%;padding:clamp(6px,3cqi,14px)}";
  html += ".kw-gauge .pg-value{color:#fff}";
  html += ".kw-gauge .pg-label,.kw-gauge .pg-time,.kw-gauge .pg-unit,.kw-gauge .pg-scale{color:rgba(255,255,255,.6)}";
  html += ".kw-gauge .pg-track{background:rgba(255,255,255,.15)}";
  html += ".kiosk-live-power{font-size:clamp(16px,12cqi,42px);font-weight:700;color:#fff;letter-spacing:-0.5px}";
  html += ".kiosk-live-power:empty{display:none}";
  html += ".kiosk-live-power.bar{display:flex;flex-direction:column;justify-content:center;align-items:stretch;gap:clamp(3px,2cqi,8px);width:100%;padding:clamp(3px,2cqi,10px) clamp(6px,3cqi,16px);box-sizing:border-box}";
  html += ".kiosk-live-power.bar .klpLbl{font-size:clamp(8px,4cqi,12px);color:rgba(255,255,255,.55);text-transform:uppercase;letter-spacing:.4px;font-weight:600;line-height:1;text-align:center}";
  html += ".kiosk-live-power.bar .klpVal{font-size:clamp(14px,10cqi,36px);font-weight:700;line-height:1;font-variant-numeric:tabular-nums;text-align:center;color:#fff;letter-spacing:-0.5px}";
  html += ".kiosk-live-power.bar .klpTrack{position:relative;width:100%;height:clamp(6px,3cqi,14px);border-radius:999px;overflow:hidden;background:rgba(255,255,255,.15)}";
  html += ".kiosk-live-power.bar .klpFill{position:absolute;top:0;left:0;bottom:0;border-radius:999px;min-width:6px;transition:width .3s var(--ease),background .3s var(--ease)}";
  html += ".kiosk-live-power.bar .klpFill.zc{background:linear-gradient(90deg,#22c55e,#4ade80)}";
  html += ".kiosk-live-power.bar .klpFill.zm{background:linear-gradient(90deg,#facc15,#fb923c)}";
  html += ".kiosk-live-power.bar .klpFill.ze{background:linear-gradient(90deg,#fb923c,#fb7185)}";
  html += ".kiosk-live-power.bar .klpScale{display:flex;justify-content:space-between;font-size:clamp(7px,3cqi,10px);color:rgba(255,255,255,.4);font-weight:600;line-height:1;padding:0 4px}";
  html += ".kiosk-live-power.bar.klp-mini .klpLbl,.kiosk-live-power.bar.klp-mini .klpScale{display:none}";
  html += ".kiosk-live-power.bar.klp-small .klpScale{display:none}";
  html += ".kiosk-status{font-size:clamp(12px,10cqi,28px);font-weight:700;padding:clamp(4px,3cqi,10px) clamp(10px,6cqi,26px);border-radius:999px;letter-spacing:-0.3px}";
  html += ".kw-chart{touch-action:none;cursor:crosshair}";
  html += ".kiosk-chart{position:relative;flex:1;min-height:0;width:100%}";
  html += ".kiosk-chart svg{width:100%;height:100%;display:block;background:transparent!important;border:0!important;border-radius:0!important;margin:0!important;box-shadow:none!important}";
  html += ".kiosk-crosshair-line{stroke:rgba(255,255,255,.5);stroke-width:1;stroke-dasharray:4,4;opacity:0;pointer-events:none}";
  html += ".kiosk-crosshair-dot{fill:#fff;stroke:rgba(0,0,0,.4);stroke-width:2;opacity:0;pointer-events:none}";
  html += ".kiosk-tooltip{position:absolute;transform:translate(-50%,-115%);background:rgba(30,30,40,.9);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,.15);border-radius:12px;padding:6px 12px;font-size:13px;font-weight:600;white-space:nowrap;pointer-events:none;opacity:0;color:#fff;box-shadow:0 8px 24px rgba(0,0,0,.4)}";
  html += ".kiosk-chart-hint{color:rgba(255,255,255,.4);font-size:clamp(9px,3cqi,12px);margin:clamp(6px,2cqi,14px) 0 0;flex:0 0 auto}";
  html += ".kiosk-meta{color:rgba(255,255,255,.65);font-size:clamp(8px,4cqi,14px);display:flex;flex-wrap:wrap;gap:clamp(4px,2cqi,10px);justify-content:center;align-items:center;height:100%}";
  html += ".kiosk-meta span{padding:clamp(3px,2cqi,7px) clamp(8px,4cqi,16px);border-radius:999px;background:rgba(255,255,255,.1);border:1px solid rgba(255,255,255,.12);color:#fff}";
  html += ".kiosk-meta span:empty{display:none}";
  html += ".kiosk-topbar{position:fixed;top:14px;right:14px;display:flex;gap:8px;opacity:.25;transition:opacity .2s var(--ease);z-index:10}";
  html += ".kiosk-topbar:hover{opacity:1}";
  html += ".kiosk-hint{font-size:clamp(9px,1.3vh,12px);color:rgba(255,255,255,.4);margin-top:clamp(4px,1vh,14px);max-width:520px}";
  html += ".actions{margin-top:clamp(6px,2vh,16px)!important;justify-content:center!important}";
  html += kioskWidgetCss(kioskPortrait);
  html += "@media (orientation:landscape){";
  html += ".kiosk-canvas{grid-template-columns:repeat(" + String(KIOSK_GRID_COLS_LANDSCAPE) + ",1fr);grid-template-rows:repeat(" + String(KIOSK_GRID_ROWS_LANDSCAPE) + ",1fr);width:min(97vw,3840px);height:min(94vh,2160px)}";
  html += kioskWidgetCss(kioskLandscape);
  html += "}";
  html += "</style>";
  html += "</head><body>";

  server.sendContent(html);
  html = "";

  html += "<div class='kiosk-topbar'>";
  html += "<button class='secondary' type='button' id='kioskArrangeBtn' onclick='kioskToggleArrange()'>Anordnen</button>";
  html += "<button class='secondary' type='button' onclick='enterKioskFullscreen()'>Vollbild</button>";
  if (ankerConfigured) {
    html += "<a href='/kiosk2'><button class='secondary' type='button'>Energie</button></a>";
  }
  html += "<a href='/'><button class='secondary' type='button'>Dashboard</button></a>";
  html += "</div>";
  html += "<div class='kiosk-wrap'>";
  html += "<div class='kiosk-canvas' id='kioskCanvas'>";

  html += "<div class='kw kw-clock' data-idx='0'><div class='kiosk-time' id='kioskTime'>--:--</div><div class='kiosk-date' id='kioskDate'></div><span class='kw-resize'></span></div>";

  html += "<div class='kw kw-gauge' id='kioskGaugeWrap' data-idx='1'>";
  // Kiosk-Klasse auf priceRing per JS setzen nach dem Render
  html += buildPriceGaugeSvg();
  html += "<script>var _r=document.querySelector('#kioskGaugeWrap .priceRing');if(_r)_r.classList.add('kiosk');</script>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  html += "<div class='kw kw-status kiosk-status' id='kioskStatus' data-idx='2' style='color:" + statusColor + "'>" + statusText + "<span class='kw-resize'></span></div>";

  bool livePowerHave = (livePowerW >= 0 && millis() - livePowerUpdatedAtMs < 60000);
  if (kioskLivePowerStyle == "bar") {
    float pct = livePowerHave ? (livePowerW / 1000.0f / livePowerMaxKw * 100.0f) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    html += "<div class='kw kw-livepower kiosk-live-power bar' id='kioskLivePower' data-idx='3'>";
    if (livePowerHave) {
      float kwv = livePowerW / 1000.0f;
      String fillZ = "zc";
      if (kwv >= livePowerYellowKw) fillZ = "ze";
      else if (kwv >= livePowerGreenKw) fillZ = "zm";
      html += "<div class='klpLbl'>&#9889; Aktueller Verbrauch</div>";
      html += "<div class='klpVal'>" + formatLivePowerValue() + "</div>";
      html += "<div class='klpTrack'><div class='klpFill " + fillZ + "' style='width:" + String(pct, 1) + "%'></div></div>";
      html += "<div class='klpScale'>";
      for (int i = 0; i <= 4; i++) {
        float v = livePowerMaxKw * i / 4.0f;
        String s = (v == (int)v) ? String((int)v) : String(v, 1);
        s.replace(".", ",");
        html += "<span>" + s + (i == 4 ? " kW" : "") + "</span>";
      }
      html += "</div>";
    }
    html += "<span class='kw-resize'></span>";
    html += "</div>";
  } else {
    String livePowerText = livePowerHave ? ("&#9889; " + formatLivePowerValue()) : "";
    html += "<div class='kw kw-livepower kiosk-live-power' id='kioskLivePower' data-idx='3'>" + livePowerText + "<span class='kw-resize'></span></div>";
  }

  server.sendContent(html);
  html = "";

  // Zonen-Farbe fuer Chart: Linie + Fill passend zum Hintergrund-Glow
  int nc2 = (metricCurrent15 >= 0) ? euroToCentRounded(metricCurrent15) : -1;
  String chartLine = "#4ade80"; String chartFill = "#22c55e";
  if (nc2 >= ledRedCent) { chartLine = "#ff6b6b"; chartFill = "#ff3b30"; }
  else if (nc2 >= ledYellowCent) { chartLine = "#ffb347"; chartFill = "#ff9500"; }

  html += "<div class='kw kw-chart' data-idx='4'>";
  html += "<div class='kiosk-chart' id='kioskChartWrap'>";
  html += buildSvgChart(chartLine, chartFill);
  html += "<div class='kiosk-tooltip' id='kioskTooltip'></div>";
  html += "</div>";
  html += "<p class='small kiosk-chart-hint'>Mit Finger oder Maus über das Diagramm fahren, um Preise zu sehen.</p>";
  html += "<span class='kw-resize'></span>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  String lowText = "--";
  if (metricLow15Day >= 0) {
    lowText = priceToCentText(metricLow15Day) + " ct um " + formatTimeOnly(metricLow15DayTime);
  }

  String avgText = "--";
  if (metricDayAvg >= 0) {
    avgText = priceToCentText(metricDayAvg) + " ct";
  }

  String monthCostText = "";
  String monthEstimateText = "";
  if (tibberMonthCost >= 0) {
    monthCostText = "Bisher: " + euroCostText(tibberMonthCost) + " " + htmlEscape(tibberMonthCurrency);
    float projected = estimateFullMonthCost();
    if (projected >= 0) {
      monthEstimateText = "Prognose: " + euroCostText(projected) + " " + htmlEscape(tibberMonthCurrency);
    }
  }

  html += "<div class='kw kw-meta' data-idx='5'><div class='kiosk-meta'>";
  html += "<span id='kioskLowText'>Tief heute: " + lowText + "</span>";
  html += "<span id='kioskAvgText'>Tagesdurchschnitt: " + avgText + "</span>";
  html += "<span id='kioskMonthCost'>" + monthCostText + "</span>";
  html += "<span id='kioskMonthEstimate'>" + monthEstimateText + "</span>";
  html += "<span id='kioskStandText'>Stand: " + getCurrentIsoPrefix().substring(11) + " Uhr</span>";
  html += "</div><span class='kw-resize'></span></div>";

  html += "</div>"; // kiosk-canvas

  html += "<p class='kiosk-hint' id='kioskWakeHint'></p>";

  html += "</div>";

  server.sendContent(html);
  html = "";

  html += "<script>var kioskChartPoints = " + buildChartPointsJson() + ";</script>";

  html += "<script src='/widget-engine.js'></script>";
  html += "<script>var kioskArrangeData = {";
  html += "portrait:" + kioskLayoutJson(kioskPortrait) + ",";
  html += "landscape:" + kioskLayoutJson(kioskLandscape);
  html += "};</script>";

  server.sendContent(html);
  html = "";

  html += R"JS(<script>
var kioskArrangeMode = false;
var kioskArrangeController = null;
var KIOSK_ARRANGE_GRID = { portrait: { cols: 6, rows: 12 }, landscape: { cols: 12, rows: 8 } };

function kioskArrangeOrientation(){
  return window.matchMedia('(orientation: landscape)').matches ? 'landscape' : 'portrait';
}
function kioskArrangeGrid(){ return KIOSK_ARRANGE_GRID[kioskArrangeOrientation()]; }
function kioskArrangeItems(){ return kioskArrangeData[kioskArrangeOrientation()]; }
function kioskArrangeEl(i){ return document.querySelector('#kioskCanvas .kw[data-idx="' + i + '"]'); }

function kioskArrangeApplyLayout(i){
  var el = kioskArrangeEl(i);
  var item = kioskArrangeItems()[i];
  if (el) {
    el.style.gridColumn = item.colStart + '/span ' + item.colSpan;
    el.style.gridRow = item.rowStart + '/span ' + item.rowSpan;
  }
}

function kioskArrangeSaveOne(i){
  var item = kioskArrangeItems()[i];
  var body = new URLSearchParams();
  body.set('target', 'k1');
  body.set('orientation', kioskArrangeOrientation());
  body.set('index', i);
  body.set('colStart', item.colStart);
  body.set('colSpan', item.colSpan);
  body.set('rowStart', item.rowStart);
  body.set('rowSpan', item.rowSpan);
  body.set('visible', item.visible ? '1' : '0');
  // keepalive: true - der Request darf ueberleben, auch wenn der Nutzer direkt
  // nach dem Ziehen zu einer anderen Seite navigiert (z.B. zur Konto-Seite, um
  // ein Firmware-Update zu starten) - sonst kann der Browser die noch laufende
  // Speicher-Anfrage abbrechen, bevor sie den Server erreicht, und die neue
  // Position geht scheinbar "nach dem Update" verloren (tatsaechlich wurde sie
  // nie gespeichert).
  return fetch('/savekiosklayoutajax', { method: 'POST', keepalive: true, headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(r){ return r.json(); }).then(function(d){ return !!d.ok; }).catch(function(){ return false; });
}

function kioskArrangeCommit(indexes){
  var chain = Promise.resolve();
  indexes.forEach(function(i){ chain = chain.then(function(){ return kioskArrangeSaveOne(i); }); });
}

function kioskInitArrangeController(){
  kioskArrangeController = WidgetGridEngine.createController({
    getEl: kioskArrangeEl,
    getItems: kioskArrangeItems,
    getGrid: kioskArrangeGrid,
    cellSize: function(){
      var canvas = document.getElementById('kioskCanvas');
      var r = canvas.getBoundingClientRect();
      var g = kioskArrangeGrid();
      return { w: r.width / g.cols, h: r.height / g.rows };
    },
    applyLayout: kioskArrangeApplyLayout,
    onCommit: kioskArrangeCommit
  });
}

function kioskToggleArrange(){
  kioskArrangeMode = !kioskArrangeMode;
  document.body.classList.toggle('kiosk-arrange-mode', kioskArrangeMode);
  var btn = document.getElementById('kioskArrangeBtn');
  if (btn) btn.textContent = kioskArrangeMode ? 'Fertig' : 'Anordnen';
}

(function(){
  kioskInitArrangeController();
  document.querySelectorAll('#kioskCanvas .kw').forEach(function(el){
    var idx = parseInt(el.getAttribute('data-idx'), 10);
    el.addEventListener('pointerdown', function(e){
      if (!kioskArrangeMode) return;
      if (e.target.closest('.kw-resize')) return;
      kioskArrangeController.startDrag(e, idx);
    });
    var handle = el.querySelector('.kw-resize');
    if (handle) {
      handle.addEventListener('pointerdown', function(e){
        if (!kioskArrangeMode) return;
        kioskArrangeController.startResize(e, idx);
      });
    }
  });
})();
</script>)JS";

  server.sendContent(html);
  html = "";

  html += R"JS(
<script>
function enterKioskFullscreen(){
  var el = document.documentElement;
  if (el.requestFullscreen) el.requestFullscreen();
}

function updateKioskClock(){
  var timeEl = document.getElementById('kioskTime');
  var dateEl = document.getElementById('kioskDate');
  if (!timeEl && !dateEl) return;
  var d = new Date();
  if (timeEl) timeEl.innerText = d.toLocaleTimeString('de-DE', {hour:'2-digit', minute:'2-digit'});
  if (dateEl) dateEl.innerText = d.toLocaleDateString('de-DE', {weekday:'long', day:'2-digit', month:'long', year:'numeric'});
}
updateKioskClock();
setInterval(updateKioskClock, 1000);

var kioskChartWrapEl = document.getElementById('kioskChartWrap');
var kioskGaugeWrapEl = document.getElementById('kioskGaugeWrap');
var kioskCrosshair = WidgetGridEngine.createChartCrosshair('priceChartSvg', 'kioskChartWrap', 'kioskTooltip', function(){ return kioskChartPoints; });

// Statt eines vollen Seiten-Reloads (das den Vollbildmodus beenden wuerde,
// weil requestFullscreen() eine Nutzer-Geste braucht) werden die Preisdaten
// periodisch per fetch() nachgeladen und nur die betroffenen Elemente ersetzt.
function refreshKioskData(){
  fetch('/kioskdata').then(function(r){ return r.json(); }).then(function(data){
    var statusEl = document.getElementById('kioskStatus');
    if (statusEl) { statusEl.innerText = data.statusText; statusEl.style.color = data.statusColor; }

    if (kioskGaugeWrapEl) kioskGaugeWrapEl.innerHTML = data.gaugeSvg;

    if (kioskChartWrapEl) {
      kioskChartWrapEl.innerHTML = data.chartSvg + "<div class='kiosk-tooltip' id='kioskTooltip'></div>";
      kioskChartPoints = data.chartPoints;
      kioskCrosshair.reattach();
    }

    var lowEl = document.getElementById('kioskLowText');
    if (lowEl) lowEl.innerText = 'Tief heute: ' + data.lowText;
    var avgEl = document.getElementById('kioskAvgText');
    if (avgEl) avgEl.innerText = 'Tagesdurchschnitt: ' + data.avgText;
    var monthEl = document.getElementById('kioskMonthCost');
    if (monthEl) monthEl.innerText = data.monthCostText || '';
    var estEl = document.getElementById('kioskMonthEstimate');
    if (estEl) estEl.innerText = data.monthEstimateText || '';
    var standEl = document.getElementById('kioskStandText');
    if (standEl) standEl.innerText = 'Stand: ' + data.standText;
  }).catch(function(e){ /* naechster Versuch beim naechsten Intervall */ });
}
setInterval(refreshKioskData, 30000);

// Eigener, schnellerer Poller nur fuer den Tibber-Pulse-Verbrauch - der
// aendert sich alle paar Sekunden, waehrend Gauge/Diagramm/Preise nur alle
// 30s aktualisiert werden muessen.
function refreshLivePower(){
  var el = document.getElementById('kioskLivePower');
  if (!el) return;
  fetch('/livepower').then(function(r){ return r.json(); }).then(function(data){
    if (el.classList.contains('bar')) {
      if (!data.text) { el.innerHTML = ''; return; }
      var pct = (typeof data.pct === 'number' && data.pct >= 0) ? data.pct : 0;
      var zone = (typeof data.zone === 'string') ? data.zone : 'zc';
      var max = (typeof data.max === 'number' && data.max > 0) ? data.max : 10;
      var fmt = function(n){ var s = (n===Math.floor(n))?n.toString():n.toFixed(1); return s.replace('.', ','); };
      var scale = '';
      for (var i = 0; i <= 4; i++) {
        var v = max * i / 4;
        scale += '<span>' + fmt(v) + (i === 4 ? ' kW' : '') + '</span>';
      }
      el.innerHTML = "<div class='klpLbl'>⚡ Aktueller Verbrauch</div><div class='klpVal'>" + data.text + "</div><div class='klpTrack'><div class='klpFill " + zone + "' style='width:" + pct.toFixed(1) + "%'></div></div><div class='klpScale'>" + scale + "</div>";
      if (typeof klpApplySizeClass === 'function') klpApplySizeClass();
    } else {
      el.innerHTML = data.text || '';
    }
  }).catch(function(e){ /* naechster Versuch beim naechsten Intervall */ });
}
refreshLivePower();
setInterval(refreshLivePower, 2500);

// Groessen-Fallback: statt CSS-Container-Queries ordnen wir per JS eine Klasse
// zu, die Label/Skala je nach Widget-Hoehe ein-/ausblendet. Das laeuft auch auf
// aelteren TV-Browsern die weder @container noch cqh koennen.
function klpApplySizeClass(){
  var el = document.getElementById('kioskLivePower');
  if (!el || !el.classList.contains('bar')) return;
  var h = el.clientHeight || 0;
  el.classList.remove('klp-mini','klp-small');
  if (h < 60) el.classList.add('klp-mini');
  else if (h < 95) el.classList.add('klp-small');
}
klpApplySizeClass();
window.addEventListener('resize', klpApplySizeClass);
if (typeof ResizeObserver !== 'undefined') {
  var _klpEl = document.getElementById('kioskLivePower');
  if (_klpEl) new ResizeObserver(klpApplySizeClass).observe(_klpEl);
}

let kioskWakeLock = null;
async function requestKioskWakeLock(){
  var hint = document.getElementById('kioskWakeHint');
  if (!('wakeLock' in navigator)) {
    if (hint) hint.innerText = 'Bildschirm-wach-halten wird von diesem Browser hier nicht unterstuetzt (funktioniert meist nur ueber HTTPS). Bitte zusaetzlich die Standby-/Bildschirmschoner-Zeit im Tablet selbst deaktivieren.';
    return;
  }
  try {
    kioskWakeLock = await navigator.wakeLock.request('screen');
    if (hint) hint.innerText = 'Bildschirm-wach-halten aktiv.';
    kioskWakeLock.addEventListener('release', () => { if (hint) hint.innerText = 'Bildschirm-wach-halten wurde beendet.'; });
  } catch (e) {
    if (hint) hint.innerText = 'Bildschirm-wach-halten fehlgeschlagen (' + e.message + '). Bitte zusaetzlich die Standby-Zeit im Tablet deaktivieren.';
  }
}
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') requestKioskWakeLock();
});
requestKioskWakeLock();
</script>
)JS";

  html += "</body></html>";

  server.sendContent(html);
  server.sendContent("");
}

// Zweite Kiosk-Seite: vereinfachter Energiefluss (PV -> Batterie/Haus -> Netz)
// auf Basis der Anker-Solarbank-Daten. Gleicher Glass-/Apple-Look wie die
// Preis-Kiosk-Seite, aber eigenes, festes Layout statt des Grid-Editors.
void handleKiosk2Page() {
  if (!checkAuth()) return;

  // Siehe ausfuehrlichen Kommentar in handleRoot()/handleKioskPage(): ein
  // groesseres reserve() allein loest Truncation bei fragmentiertem Heap
  // NICHT zuverlaessig. Die Antwort wird deshalb in mehreren kleinen Chunks
  // per server.sendContent() gesendet (HTTP Chunked Transfer Encoding).
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String html;
  html.reserve(6000);

  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>" + htmlEscape(webInterfaceName + " - Energiefluss") + "</title>";
  html += "<script>(function(){try{if(localStorage.getItem('theme')==='dark'){document.documentElement.setAttribute('data-theme','dark');}}catch(e){}})();</script>";
  html += "<link rel='stylesheet' href='/style.css?v=" ASSET_VERSION "'>";
  html += "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>";
  html += "<style>";
  html += "html,body{height:100%;overflow:hidden;margin:0}";
  html += "body{padding:0!important;display:flex;align-items:center;justify-content:center;background:#000;position:relative}";
  html += "@keyframes efPulse{0%,100%{opacity:.7}50%{opacity:1}}";
  html += "body::before{content:'';position:fixed;inset:0;background:radial-gradient(ellipse 80% 60% at 50% 0%,#00301f,transparent 70%),radial-gradient(ellipse 60% 80% at 20% 100%,#001a12,transparent 70%),#000;animation:efPulse 8s ease-in-out infinite;z-index:0}";
  html += ".kiosk-wrap{position:relative;z-index:1;display:flex;flex-direction:column;align-items:center;max-height:100vh;padding:clamp(8px,2.2vh,20px);box-sizing:border-box;text-align:center;overflow:hidden}";
  html += ".kiosk-canvas{position:relative;display:grid;grid-template-columns:repeat(" + String(KIOSK_GRID_COLS_PORTRAIT) + ",1fr);grid-template-rows:repeat(" + String(KIOSK_GRID_ROWS_PORTRAIT) + ",1fr);gap:clamp(4px,1vh,10px);width:min(97vw,2160px);height:min(94vh,3840px);box-sizing:border-box;margin:0 auto}";
  html += ".kw{position:relative;container-type:inline-size;overflow:hidden;box-sizing:border-box;display:flex;flex-direction:column;align-items:center;justify-content:center;min-width:0;min-height:0;border-radius:clamp(12px,2vh,24px);background:rgba(255,255,255,.07);backdrop-filter:blur(24px) saturate(160%);-webkit-backdrop-filter:blur(24px) saturate(160%);border:1px solid rgba(255,255,255,.12);padding:clamp(6px,1.4vh,14px);gap:4px;transition:box-shadow .2s ease}";
  html += ".kw.wg-dragging,.kw.wg-resizing{box-shadow:0 20px 44px rgba(0,0,0,.5);z-index:50}";
  html += ".kiosk-arrange-mode .kw{cursor:grab;user-select:none}";
  html += ".kiosk-arrange-mode .kw.wg-dragging{cursor:grabbing}";
  html += ".kw-resize{display:none;position:absolute;right:6px;bottom:6px;width:18px;height:18px;border-radius:50%;background:#fff;border:2px solid rgba(0,0,0,.4);cursor:nwse-resize;z-index:5}";
  html += ".kiosk-arrange-mode .kw-resize{display:block}";
  html += "#efGaugeCard,#efChartCard{width:100%;height:100%}";
  // Siehe Kommentar in handleKioskPage(): cqi statt vh, damit Text/Icons mit
  // der tatsaechlichen Widget-Groesse (nicht der Bildschirmhoehe) skalieren.
  html += ".kiosk-time{font-size:clamp(16px,16cqi,56px);font-weight:700;line-height:1;letter-spacing:-1px;color:#fff;font-variant-numeric:tabular-nums}";
  html += ".kiosk-date{font-size:clamp(8px,4cqi,15px);color:rgba(255,255,255,.65);margin-top:4px;text-transform:capitalize;font-weight:500}";
  html += ".kw-pricegauge .priceRing{max-width:100%;padding:0}";
  html += ".kw-pricegauge svg{background:transparent!important;border:0!important;border-radius:0!important;margin:0!important;box-shadow:none!important}";
  html += ".kw-pricechart{padding:clamp(6px,3cqi,12px);touch-action:none;cursor:crosshair}";
  html += ".kw-pricechart svg{width:100%;height:100%;display:block;background:transparent!important;border:0!important;border-radius:0!important;margin:0!important;box-shadow:none!important}";
  html += "#efChartCard{position:relative}";
  html += ".kiosk-crosshair-line{stroke:rgba(255,255,255,.5);stroke-width:1;stroke-dasharray:4,4;opacity:0;pointer-events:none}";
  html += ".kiosk-crosshair-dot{fill:#fff;stroke:rgba(0,0,0,.4);stroke-width:2;opacity:0;pointer-events:none}";
  html += ".kiosk-tooltip{position:absolute;transform:translate(-50%,-115%);background:rgba(30,30,40,.9);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,.15);border-radius:12px;padding:6px 12px;font-size:13px;font-weight:600;white-space:nowrap;pointer-events:none;opacity:0;color:#fff;box-shadow:0 8px 24px rgba(0,0,0,.4)}";
  // Hub-Diagramm statt vier lose verschiebbarer Einzelkarten: Haus in der
  // Mitte als Anker, PV/Batterie/Netz als Satelliten-Knoten drumherum, durch
  // Speichen verbunden, die nur bei tatsaechlichem Leistungsfluss farbig
  // aufleuchten und einen animierten Punkt in Fliessrichtung zeigen - macht
  // auf einen Blick sichtbar WER an WEN liefert, statt vier Zahlen einzeln
  // lesen und im Kopf verknuepfen zu muessen.
  html += ".kw-energyflow svg{width:100%;height:100%}";
  html += ".ef-node-circle{fill:rgba(255,255,255,.07);stroke:rgba(255,255,255,.4);stroke-width:1.5}";
  html += ".ef-node-house{stroke:rgba(255,255,255,.65)}";
  html += ".ef-node-label{font-size:9.5px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;fill:rgba(255,255,255,.5)}";
  html += ".ef-node-value{font-size:17px;font-weight:700;fill:#fff;font-variant-numeric:tabular-nums}";
  html += ".ef-node-sub{font-size:9px;fill:rgba(255,255,255,.4)}";
  html += ".ef-house-value{font-size:20px;font-weight:700;fill:#fff;font-variant-numeric:tabular-nums}";
  html += ".ef-house-label{font-size:10px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;fill:rgba(255,255,255,.65)}";
  html += ".ef-soc-bg{fill:none;stroke:rgba(255,255,255,.12);stroke-width:4}";
  html += ".ef-soc-fill{fill:none;stroke:#0A84FF;stroke-width:4;stroke-linecap:round;transform-origin:80px 320px;transform:rotate(-90deg);transition:stroke-dasharray .4s ease}";
  html += ".ef-soc-value{font-size:14px;font-weight:700;fill:#0A84FF;font-variant-numeric:tabular-nums}";
  html += ".ef-line{fill:none;transition:stroke .3s ease}";
  html += ".ef-line.idle{stroke:rgba(255,255,255,.14);stroke-width:2;stroke-dasharray:4 5;opacity:.6}";
  html += ".ef-line.active-pv{stroke:#34C759;stroke-width:2.5}";
  html += ".ef-line.active-batt{stroke:#0A84FF;stroke-width:2.5}";
  html += ".ef-line.active-grid{stroke:#FF9500;stroke-width:2.5}";
  html += ".ef-dot{display:none}";
  html += ".ef-dot.show{display:block}";
  html += ".ef-dot-pv{fill:#34C759}";
  html += ".ef-dot-batt-charge,.ef-dot-batt-discharge{fill:#0A84FF}";
  html += ".ef-dot-grid-import,.ef-dot-grid-export{fill:#FF9500}";
  // Kennzahlen-Kacheln (Autarkie/Eigenverbrauch/Ertrag/CO2/Geld) - 3x2-Grid,
  // Groesse per cqi an die tatsaechliche Widget-Groesse gekoppelt (nicht an
  // die Bildschirmhoehe), da das Widget in Hoch- und Querformat sehr
  // unterschiedlich breit/hoch sein kann (siehe KIOSK2_*_DEFAULTS).
  html += ".kw-stats{padding:clamp(6px,2cqi,14px)}";
  html += ".ef-stats-grid{display:grid;grid-template-columns:repeat(3,1fr);grid-template-rows:repeat(2,1fr);gap:clamp(4px,2cqi,10px);width:100%;height:100%}";
  html += ".ef-stat-tile{display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;background:rgba(255,255,255,.05);border-radius:clamp(8px,1.5cqi,14px);padding:clamp(2px,1cqi,6px);overflow:hidden}";
  html += ".ef-stat-label{font-size:clamp(7px,2.6cqi,10px);color:rgba(255,255,255,.5);text-transform:uppercase;letter-spacing:.05em;font-weight:600;line-height:1.2}";
  html += ".ef-stat-value{font-size:clamp(11px,4.5cqi,20px);font-weight:700;color:#fff;margin-top:2px;font-variant-numeric:tabular-nums}";
  html += ".ef-error{color:rgba(255,255,255,.6);font-size:clamp(11px,1.6vh,14px);text-align:center;max-width:80vw;margin-top:10px}";
  html += ".kiosk-arrange-mode .ef-node{cursor:grab}";
  html += ".ef-node.ef-node-dragging{cursor:grabbing}";
  html += ".kiosk-topbar{position:fixed;top:14px;right:14px;display:flex;gap:8px;opacity:.25;transition:opacity .2s var(--ease);z-index:10}";
  html += ".kiosk-topbar:hover{opacity:1}";
  html += kioskWidgetCss(kiosk2Portrait, KIOSK2_WIDGET_KEYS, KIOSK2_WIDGET_COUNT);
  html += "@media (orientation:landscape){";
  html += ".kiosk-canvas{grid-template-columns:repeat(" + String(KIOSK_GRID_COLS_LANDSCAPE) + ",1fr);grid-template-rows:repeat(" + String(KIOSK_GRID_ROWS_LANDSCAPE) + ",1fr);width:min(97vw,3840px);height:min(94vh,2160px)}";
  html += kioskWidgetCss(kiosk2Landscape, KIOSK2_WIDGET_KEYS, KIOSK2_WIDGET_COUNT);
  html += "}";
  html += "</style>";
  html += "</head><body>";

  server.sendContent(html);
  html = "";

  html += "<div class='kiosk-topbar'>";
  html += "<button class='secondary' type='button' id='kioskArrangeBtn' onclick='kioskToggleArrange()'>Anordnen</button>";
  html += "<a href='/kiosk'><button class='secondary' type='button'>Preise</button></a>";
  html += "<a href='/'><button class='secondary' type='button'>Dashboard</button></a>";
  html += "</div>";

  // Zonen-Farbe fuer das Diagramm, identisch zur Logik in handleKioskPage()/handleKioskData()
  int nc2 = (metricCurrent15 >= 0) ? euroToCentRounded(metricCurrent15) : -1;
  String chartLine = "#4ade80"; String chartFill = "#22c55e";
  if (nc2 >= ledRedCent) { chartLine = "#ff6b6b"; chartFill = "#ff3b30"; }
  else if (nc2 >= ledYellowCent) { chartLine = "#ffb347"; chartFill = "#ff9500"; }

  html += "<div class='kiosk-wrap'>";
  html += "<div class='kiosk-canvas' id='kioskCanvas'>";
  html += "<div class='kw kw-clock' data-idx='0'><div class='kiosk-time' id='kioskTime'>--:--</div><div class='kiosk-date' id='kioskDate'></div><span class='kw-resize'></span></div>";
  html += "<div class='kw kw-pricegauge' data-idx='1'><div id='efGaugeCard'>" + buildPriceGaugeSvg() + "</div><span class='kw-resize'></span></div>";
  html += "<div class='kw kw-pricechart' data-idx='2'><div id='efChartCard'>" + buildSvgChart(chartLine, chartFill) + "<div class='kiosk-tooltip' id='efTooltip'></div></div><span class='kw-resize'></span></div>";

  server.sendContent(html);
  html = "";

  html += "<div class='kw kw-energyflow' data-idx='3'>" + buildEnergyFlowSvg() + "<span class='kw-resize'></span></div>";
  html += "<div class='kw kw-stats' data-idx='4'>" + buildEnergyStatsHtml() + "<span class='kw-resize'></span></div>";
  html += "</div>";
  html += "<script>var _r=document.querySelector('#efGaugeCard .priceRing');if(_r)_r.classList.add('kiosk');</script>";
  if (!ankerConfigured) {
    html += "<p class='ef-error'>Keine Anker-Solarbank eingerichtet. Zugangsdaten auf der Konto-Seite hinterlegen.</p>";
  }
  html += "<p class='ef-error' id='efErr' style='display:none'></p>";
  html += "</div>";

  server.sendContent(html);
  html = "";

  html += "<script src='/widget-engine.js'></script>";
  html += "<script>var kioskChartPoints = " + buildChartPointsJson() + ";</script>";

  server.sendContent(html);
  html = "";

  html += R"JS(
<script>
function efFmt(w){
  if (w === null || isNaN(w)) return '-- W';
  var a = Math.abs(w);
  if (a >= 1000) return (w/1000).toFixed(2).replace('.', ',') + ' kW';
  return Math.round(w) + ' W';
}
// Aktualisiert das Hub-Diagramm (siehe buildEnergyFlowSvg()): Werte in den
// Knoten, Batterie-Ladering und welche Speiche in welche Richtung gerade
// farbig/animiert ist. Wichtig: SVG-Elemente brauchen setAttribute('class',...)
// statt .className = ... - .className liefert bei SVG-Elementen ein
// SVGAnimatedString-Objekt zurueck, eine simple String-Zuweisung wuerde dort
// stillschweigend nichts bewirken.
function efApply(d){
  var errEl = document.getElementById('efErr');
  if (!d.ok) {
    if (errEl) { errEl.style.display = ''; errEl.innerText = d.error || 'Verbindung zur Anker-Cloud fehlgeschlagen.'; }
    return;
  }
  if (errEl) errEl.style.display = 'none';

  var pvVal = document.getElementById('efPvVal'); if (pvVal) pvVal.textContent = efFmt(d.pv);
  var battVal = document.getElementById('efBattVal'); if (battVal) battVal.textContent = efFmt(d.batt);
  var houseVal = document.getElementById('efHouseVal'); if (houseVal) houseVal.textContent = efFmt(d.house);
  var gridVal = document.getElementById('efGridVal'); if (gridVal) gridVal.textContent = efFmt(d.grid);

  var gridSub = document.getElementById('efGridSub');
  if (gridSub) gridSub.textContent = d.grid > 5 ? 'Bezug' : (d.grid < -5 ? 'Einspeisung' : '');

  // Ladering: Umfang bei r=50 ist 2*PI*50 = 314.16 - Anteil davon je nach %
  // als "gefuellt gefuellt-rest" Dasharray, Rest unsichtbar bis zum Ringende.
  var socVal = document.getElementById('efSocVal');
  var socRing = document.getElementById('efSocRing');
  var socCirc = 2 * Math.PI * 50;
  if (typeof d.soc === 'number' && d.soc >= 0) {
    if (socVal) socVal.textContent = Math.round(d.soc) + '%';
    if (socRing) {
      var filled = socCirc * (Math.max(0, Math.min(100, d.soc)) / 100);
      socRing.setAttribute('stroke-dasharray', filled.toFixed(1) + ' ' + (socCirc - filled).toFixed(1));
    }
  } else {
    if (socVal) socVal.textContent = '';
    if (socRing) socRing.setAttribute('stroke-dasharray', '0 ' + socCirc.toFixed(1));
  }

  // PV-Speiche: liefert immer Richtung Haus, nie umgekehrt.
  var pvActive = d.pv > 5;
  var lnPv = document.getElementById('lnPv');
  if (lnPv) lnPv.setAttribute('class', 'ef-line ' + (pvActive ? 'active-pv' : 'idle'));
  var dotPv = document.getElementById('dotPv');
  if (dotPv) dotPv.setAttribute('class', 'ef-dot ef-dot-pv' + (pvActive ? ' show' : ''));

  // Batterie-Speiche: positiv=laedt (Fluss Haus->Batterie), negativ=entlaedt
  // und versorgt damit das Haus (Fluss Batterie->Haus).
  var battCharging = d.batt > 5, battDischarging = d.batt < -5;
  var lnBatt = document.getElementById('lnBatt');
  if (lnBatt) lnBatt.setAttribute('class', 'ef-line ' + ((battCharging || battDischarging) ? 'active-batt' : 'idle'));
  var dotBattCharge = document.getElementById('dotBattCharge');
  if (dotBattCharge) dotBattCharge.setAttribute('class', 'ef-dot ef-dot-batt-charge' + (battCharging ? ' show' : ''));
  var dotBattDischarge = document.getElementById('dotBattDischarge');
  if (dotBattDischarge) dotBattDischarge.setAttribute('class', 'ef-dot ef-dot-batt-discharge' + (battDischarging ? ' show' : ''));

  // Netz-Speiche: positiv=Bezug (Fluss Netz->Haus), negativ=Einspeisung
  // (Fluss Haus->Netz).
  var gridImport = d.grid > 5, gridExport = d.grid < -5;
  var lnGrid = document.getElementById('lnGrid');
  if (lnGrid) lnGrid.setAttribute('class', 'ef-line ' + ((gridImport || gridExport) ? 'active-grid' : 'idle'));
  var dotGridImport = document.getElementById('dotGridImport');
  if (dotGridImport) dotGridImport.setAttribute('class', 'ef-dot ef-dot-grid-import' + (gridImport ? ' show' : ''));
  var dotGridExport = document.getElementById('dotGridExport');
  if (dotGridExport) dotGridExport.setAttribute('class', 'ef-dot ef-dot-grid-export' + (gridExport ? ' show' : ''));

  // Kennzahlen-Widget (siehe buildEnergyStatsHtml()). Autarkie/Eigenverbrauch
  // sind Momentanwerte aus denselben pv/house/grid-Rohdaten wie das
  // Hub-Diagramm - bewusst nicht serverseitig berechnet (siehe Kommentar in
  // handleAnkerData()), um doppelte Rechenlogik zu vermeiden.
  var autarkyEl = document.getElementById('statAutarky');
  if (autarkyEl) {
    if (d.house > 1) {
      var selfCovered = d.house - Math.max(0, d.grid);
      var autarky = Math.max(0, Math.min(100, selfCovered / d.house * 100));
      autarkyEl.textContent = Math.round(autarky) + '%';
    } else {
      autarkyEl.textContent = '--';
    }
  }
  var selfConsEl = document.getElementById('statSelfCons');
  if (selfConsEl) {
    if (d.pv > 1) {
      var exported = Math.max(0, -d.grid);
      var selfConsumed = Math.max(0, d.pv - exported);
      var selfCons = Math.max(0, Math.min(100, selfConsumed / d.pv * 100));
      selfConsEl.textContent = Math.round(selfCons) + '%';
    } else {
      selfConsEl.textContent = '--';
    }
  }
  var pvTodayEl = document.getElementById('statPvToday');
  if (pvTodayEl) pvTodayEl.textContent = (typeof d.pvYieldTodayKwh === 'number') ? d.pvYieldTodayKwh.toFixed(1).replace('.', ',') + ' kWh' : '--';
  var yieldTotalEl = document.getElementById('statYieldTotal');
  if (yieldTotalEl) yieldTotalEl.textContent = (typeof d.yieldTotalKwh === 'number' && d.yieldTotalKwh >= 0) ? Math.round(d.yieldTotalKwh) + ' kWh' : '--';
  var co2El = document.getElementById('statCo2');
  if (co2El) co2El.textContent = (typeof d.co2SavedKg === 'number' && d.co2SavedKg >= 0) ? Math.round(d.co2SavedKg) + ' kg' : '--';
  var moneyEl = document.getElementById('statMoney');
  if (moneyEl) moneyEl.textContent = (typeof d.moneySavedEur === 'number' && d.moneySavedEur >= 0) ? Math.round(d.moneySavedEur) + ' €' : '--';
}
function efPoll(){
  fetch('/ankerdata').then(function(r){ return r.json(); }).then(efApply).catch(function(e){});
}
efPoll();
setInterval(efPoll, 15000);

// Preis-Gauge + Diagramm regelmaessig aus /kioskdata aktualisieren (gleicher
// Endpunkt wie Kiosk-Seite 1), ohne die ganze Seite neu zu laden.
function efPriceRefresh(){
  fetch('/kioskdata').then(function(r){ return r.json(); }).then(function(d){
    var gc = document.getElementById('efGaugeCard');
    if (gc && d.gaugeSvg != null) {
      gc.innerHTML = d.gaugeSvg;
      var r2 = gc.querySelector('.priceRing');
      if (r2) r2.classList.add('kiosk');
    }
    var cc = document.getElementById('efChartCard');
    if (cc && d.chartSvg != null) {
      cc.innerHTML = d.chartSvg + "<div class='kiosk-tooltip' id='efTooltip'></div>";
      if (d.chartPoints != null) kioskChartPoints = d.chartPoints;
      efCrosshair.reattach();
    }
  }).catch(function(e){});
}
setInterval(efPriceRefresh, 30000);

var efCrosshair = WidgetGridEngine.createChartCrosshair('priceChartSvg', 'efChartCard', 'efTooltip', function(){ return kioskChartPoints; });

function updateKioskClock(){
  var timeEl = document.getElementById('kioskTime');
  var dateEl = document.getElementById('kioskDate');
  if (!timeEl && !dateEl) return;
  var d = new Date();
  if (timeEl) timeEl.innerText = d.toLocaleTimeString('de-DE', {hour:'2-digit', minute:'2-digit'});
  if (dateEl) dateEl.innerText = d.toLocaleDateString('de-DE', {weekday:'long', day:'2-digit', month:'long', year:'numeric'});
}
updateKioskClock();
setInterval(updateKioskClock, 1000);
</script>
)JS";

  server.sendContent(html);
  html = "";

  html += "<script>var kioskArrangeData = {";
  html += "portrait:" + kioskLayoutJson(kiosk2Portrait, KIOSK2_WIDGET_KEYS, KIOSK2_WIDGET_LABELS, KIOSK2_WIDGET_COUNT) + ",";
  html += "landscape:" + kioskLayoutJson(kiosk2Landscape, KIOSK2_WIDGET_KEYS, KIOSK2_WIDGET_LABELS, KIOSK2_WIDGET_COUNT);
  html += "};</script>";

  server.sendContent(html);
  html = "";

  html += R"JS(<script>
var kioskArrangeMode = false;
var kioskArrangeController = null;
var KIOSK_ARRANGE_GRID = { portrait: { cols: 6, rows: 12 }, landscape: { cols: 12, rows: 8 } };

function kioskArrangeOrientation(){
  return window.matchMedia('(orientation: landscape)').matches ? 'landscape' : 'portrait';
}
function kioskArrangeGrid(){ return KIOSK_ARRANGE_GRID[kioskArrangeOrientation()]; }
function kioskArrangeItems(){ return kioskArrangeData[kioskArrangeOrientation()]; }
function kioskArrangeEl(i){ return document.querySelector('#kioskCanvas .kw[data-idx="' + i + '"]'); }

function kioskArrangeApplyLayout(i){
  var el = kioskArrangeEl(i);
  var item = kioskArrangeItems()[i];
  if (el) {
    el.style.gridColumn = item.colStart + '/span ' + item.colSpan;
    el.style.gridRow = item.rowStart + '/span ' + item.rowSpan;
  }
}

function kioskArrangeSaveOne(i){
  var item = kioskArrangeItems()[i];
  var body = new URLSearchParams();
  body.set('target', 'k2');
  body.set('orientation', kioskArrangeOrientation());
  body.set('index', i);
  body.set('colStart', item.colStart);
  body.set('colSpan', item.colSpan);
  body.set('rowStart', item.rowStart);
  body.set('rowSpan', item.rowSpan);
  body.set('visible', item.visible ? '1' : '0');
  // keepalive: true - der Request darf ueberleben, auch wenn der Nutzer direkt
  // nach dem Ziehen zu einer anderen Seite navigiert (z.B. zur Konto-Seite, um
  // ein Firmware-Update zu starten) - sonst kann der Browser die noch laufende
  // Speicher-Anfrage abbrechen, bevor sie den Server erreicht, und die neue
  // Position geht scheinbar "nach dem Update" verloren (tatsaechlich wurde sie
  // nie gespeichert).
  return fetch('/savekiosklayoutajax', { method: 'POST', keepalive: true, headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(r){ return r.json(); }).then(function(d){ return !!d.ok; }).catch(function(){ return false; });
}

function kioskArrangeCommit(indexes){
  var chain = Promise.resolve();
  indexes.forEach(function(i){ chain = chain.then(function(){ return kioskArrangeSaveOne(i); }); });
}

function kioskInitArrangeController(){
  kioskArrangeController = WidgetGridEngine.createController({
    getEl: kioskArrangeEl,
    getItems: kioskArrangeItems,
    getGrid: kioskArrangeGrid,
    cellSize: function(){
      var canvas = document.getElementById('kioskCanvas');
      var r = canvas.getBoundingClientRect();
      var g = kioskArrangeGrid();
      return { w: r.width / g.cols, h: r.height / g.rows };
    },
    applyLayout: kioskArrangeApplyLayout,
    onCommit: kioskArrangeCommit
  });
}

function kioskToggleArrange(){
  kioskArrangeMode = !kioskArrangeMode;
  document.body.classList.toggle('kiosk-arrange-mode', kioskArrangeMode);
  var btn = document.getElementById('kioskArrangeBtn');
  if (btn) btn.textContent = kioskArrangeMode ? 'Fertig' : 'Anordnen';
}

(function(){
  kioskInitArrangeController();
  document.querySelectorAll('#kioskCanvas .kw').forEach(function(el){
    var idx = parseInt(el.getAttribute('data-idx'), 10);
    el.addEventListener('pointerdown', function(e){
      if (!kioskArrangeMode) return;
      if (e.target.closest('.kw-resize')) return;
      // Einzelne Hub-Diagramm-Knoten haben ihr eigenes Drag-Handling (siehe
      // unten) - ohne diese Ausnahme wuerde ein Ziehen an einem Knoten
      // zusaetzlich das ganze Energiefluss-Widget verschieben.
      if (e.target.closest('.ef-node')) return;
      kioskArrangeController.startDrag(e, idx);
    });
    var handle = el.querySelector('.kw-resize');
    if (handle) {
      handle.addEventListener('pointerdown', function(e){
        if (!kioskArrangeMode) return;
        kioskArrangeController.startResize(e, idx);
      });
    }
  });
})();

// Energiefluss-Hub-Diagramm: PV/Batterie/Netz/Haus einzeln frei verschiebbar
// im Anordnen-Modus, Verbindungslinien werden nach jeder Verschiebung neu
// berechnet. efLineBetween() MUSS mit der C++-Entsprechung efLinePath()
// (siehe buildEnergyFlowSvg()) synchron bleiben - beide docken die Linie am
// Kreisrand (nicht am Zentrum) an, damit sie nicht sichtbar hineinragt.
var EF_NODE_RADIUS = [42, 50, 42, 56];
var EF_CANVAS_W = 640, EF_CANVAS_H = 360;
var efNodePositions = [null, null, null, null];

function efLineBetween(x1, y1, r1, x2, y2, r2) {
  var dx = x2 - x1, dy = y2 - y1;
  var dist = Math.sqrt(dx * dx + dy * dy) || 0.01;
  var ux = dx / dist, uy = dy / dist;
  return { x1: x1 + ux * r1, y1: y1 + uy * r1, x2: x2 - ux * r2, y2: y2 - uy * r2 };
}
function efPathStr(p) {
  return 'M ' + p.x1.toFixed(1) + ' ' + p.y1.toFixed(1) + ' L ' + p.x2.toFixed(1) + ' ' + p.y2.toFixed(1);
}
function efReadNodePositions() {
  document.querySelectorAll('.ef-node').forEach(function(g){
    var idx = parseInt(g.getAttribute('data-node'), 10);
    var m = /translate\(([-\d.]+),\s*([-\d.]+)\)/.exec(g.getAttribute('transform') || '');
    if (m) efNodePositions[idx] = { x: parseFloat(m[1]), y: parseFloat(m[2]) };
  });
}
function efRecalcLines() {
  if (!efNodePositions[0] || !efNodePositions[1] || !efNodePositions[2] || !efNodePositions[3]) return;
  var pv = efNodePositions[0], batt = efNodePositions[1], grid = efNodePositions[2], house = efNodePositions[3];

  var pvLine = efLineBetween(pv.x, pv.y, EF_NODE_RADIUS[0], house.x, house.y, EF_NODE_RADIUS[3]);
  var battLine = efLineBetween(batt.x, batt.y, EF_NODE_RADIUS[1], house.x, house.y, EF_NODE_RADIUS[3]);
  var gridLine = efLineBetween(grid.x, grid.y, EF_NODE_RADIUS[2], house.x, house.y, EF_NODE_RADIUS[3]);

  var pvPath = efPathStr(pvLine);
  var battPath = efPathStr(battLine);
  var battPathRev = 'M ' + battLine.x2.toFixed(1) + ' ' + battLine.y2.toFixed(1) + ' L ' + battLine.x1.toFixed(1) + ' ' + battLine.y1.toFixed(1);
  var gridPath = efPathStr(gridLine);
  var gridPathRev = 'M ' + gridLine.x2.toFixed(1) + ' ' + gridLine.y2.toFixed(1) + ' L ' + gridLine.x1.toFixed(1) + ' ' + gridLine.y1.toFixed(1);

  var lnPv = document.getElementById('lnPv'); if (lnPv) lnPv.setAttribute('d', pvPath);
  var lnBatt = document.getElementById('lnBatt'); if (lnBatt) lnBatt.setAttribute('d', battPath);
  var lnGrid = document.getElementById('lnGrid'); if (lnGrid) lnGrid.setAttribute('d', gridPath);

  var dotDefs = [
    ['dotPv', pvPath], ['dotBattCharge', battPathRev], ['dotBattDischarge', battPath],
    ['dotGridImport', gridPath], ['dotGridExport', gridPathRev]
  ];
  dotDefs.forEach(function(pair){
    var dotEl = document.getElementById(pair[0]);
    if (!dotEl) return;
    var am = dotEl.querySelector('animateMotion');
    if (am) am.setAttribute('path', pair[1]);
  });
}
function efNodeToSvgPoint(svg, clientX, clientY) {
  var pt = svg.createSVGPoint();
  pt.x = clientX; pt.y = clientY;
  var ctm = svg.getScreenCTM();
  if (!ctm) return { x: 0, y: 0 };
  var p = pt.matrixTransform(ctm.inverse());
  return { x: p.x, y: p.y };
}
(function(){
  efReadNodePositions();
  var svg = document.querySelector('.kw-energyflow svg');
  if (!svg) return;

  document.querySelectorAll('.ef-node').forEach(function(nodeEl){
    var idx = parseInt(nodeEl.getAttribute('data-node'), 10);
    nodeEl.addEventListener('pointerdown', function(e){
      if (!kioskArrangeMode) return;
      e.preventDefault();
      e.stopPropagation();
      nodeEl.classList.add('ef-node-dragging');
      var r = EF_NODE_RADIUS[idx];

      function onMove(ev){
        var p = efNodeToSvgPoint(svg, ev.clientX, ev.clientY);
        var x = Math.max(r, Math.min(EF_CANVAS_W - r, p.x));
        var y = Math.max(r, Math.min(EF_CANVAS_H - r, p.y));
        nodeEl.setAttribute('transform', 'translate(' + x.toFixed(1) + ',' + y.toFixed(1) + ')');
        efNodePositions[idx] = { x: x, y: y };
        efRecalcLines();
      }
      function onUp(){
        document.removeEventListener('pointermove', onMove);
        document.removeEventListener('pointerup', onUp);
        nodeEl.classList.remove('ef-node-dragging');
        var pos = efNodePositions[idx];
        if (pos) {
          var body = new URLSearchParams();
          body.set('idx', idx);
          body.set('x', pos.x.toFixed(1));
          body.set('y', pos.y.toFixed(1));
          fetch('/saveefnodepos', { method: 'POST', keepalive: true, headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() });
        }
      }
      document.addEventListener('pointermove', onMove);
      document.addEventListener('pointerup', onUp);
    });
  });
})();
</script>)JS";

  html += "</body></html>";

  server.sendContent(html);
  server.sendContent("");
}

// JSON fuer die Energiefluss-Kiosk-Seite (siehe handleKiosk2Page): liefert
// PV-Erzeugung, Batterieleistung, Hausverbrauch und daraus abgeleiteten
// Netzbezug (Hausverbrauch - Solarbank-Ausgang, floor 0 falls negativ).
void handleAnkerData() {
  if (!checkAuth()) return;

  // Aktueller Preis (wie auf Kiosk-Seite 1) wird unabhaengig vom
  // Anker-Status mitgeliefert, damit die Energiefluss-Seite denselben
  // Preis anzeigen kann, ohne einen zweiten Endpunkt abzufragen.
  String zoneText, zoneColor;
  getKioskPriceStatus(zoneText, zoneColor);
  int nowCent = (quarterCount > 0 && metricCurrent15 >= 0) ? euroToCentRounded(metricCurrent15) : -1;

  String json = "{";
  json += "\"priceCent\":" + String(nowCent) + ",";
  json += "\"priceZone\":\"" + jsonEscapeValue(zoneText) + "\",";
  json += "\"priceColor\":\"" + jsonEscapeValue(zoneColor) + "\",";
  if (!ankerConfigured) {
    json += "\"ok\":false,\"error\":\"Keine Anker-Zugangsdaten hinterlegt\"";
  } else if (ankerLastError.length() > 0) {
    // ankerLastError wird bei jedem Poll entweder auf "" gesetzt (Erfolg) oder
    // auf die aktuelle Fehlermeldung (siehe updateAnkerSolarData()/ankerAuthedPost())
    // - spiegelt also immer den Status des LETZTEN Polls wider. Die fruehere
    // Zusatzbedingung "&& ankerPvW < 0" maskierte jeden Poll-Fehler nach dem
    // ersten erfolgreichen Poll, weil ankerPvW seinen letzten guten Wert
    // dauerhaft behaelt und nie zurueckgesetzt wird - /kiosk2 haette dann
    // veraltete Werte als "ok" angezeigt, obwohl die Verbindung gerade steht.
    json += "\"ok\":false,\"error\":\"" + jsonEscapeValue(ankerLastError) + "\"";
  } else {
    float grid = ankerHomeLoadW - ankerOutputW;
    json += "\"ok\":true,";
    json += "\"pv\":" + String(ankerPvW, 1) + ",";
    json += "\"batt\":" + String(ankerBatteryW, 1) + ",";
    json += "\"soc\":" + String(ankerBatterySoc) + ",";
    json += "\"house\":" + String(ankerHomeLoadW, 1) + ",";
    json += "\"grid\":" + String(grid, 1) + ",";
    // Autarkie/Eigenverbrauch werden bewusst NICHT hier serverseitig
    // berechnet, sondern aus denselben pv/batt/house/grid-Rohwerten
    // clientseitig in efApply() - vermeidet doppelte Rechenlogik in C++ UND
    // JS, die sonst leicht auseinanderlaufen kann.
    json += "\"yieldTotalKwh\":" + String(ankerTotalYieldKwh, 1) + ",";
    json += "\"co2SavedKg\":" + String(ankerCo2SavedKg, 1) + ",";
    json += "\"moneySavedEur\":" + String(ankerMoneySavedEur, 1) + ",";
    json += "\"pvYieldTodayKwh\":" + String(pvYieldTodayKwh, 2);
  }
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

// Liefert die Preisdaten fuer den Tablet-Modus als JSON, damit die Seite sie
// periodisch per fetch() nachladen kann, statt komplett neu zu laden (das
// wuerde den Vollbildmodus des Tablets beenden).
void handleKioskData() {
  if (!checkAuth()) return;

  String statusText, statusColor;
  getKioskPriceStatus(statusText, statusColor);

  String lowText = "--";
  if (metricLow15Day >= 0) {
    lowText = priceToCentText(metricLow15Day) + " ct um " + formatTimeOnly(metricLow15DayTime);
  }

  String avgText = "--";
  if (metricDayAvg >= 0) {
    avgText = priceToCentText(metricDayAvg) + " ct";
  }

  String standText = getCurrentIsoPrefix().substring(11) + " Uhr";

  String monthCostText = "";
  String monthEstimateText = "";
  if (tibberMonthCost >= 0) {
    monthCostText = "Bisher: " + euroCostText(tibberMonthCost) + " " + tibberMonthCurrency;
    float projected = estimateFullMonthCost();
    if (projected >= 0) {
      monthEstimateText = "Prognose: " + euroCostText(projected) + " " + tibberMonthCurrency;
    }
  }

  String json;
  json.reserve(6000);
  json += "{";
  json += "\"statusText\":\"" + jsonEscapeValue(statusText) + "\",";
  json += "\"statusColor\":\"" + jsonEscapeValue(statusColor) + "\",";
  json += "\"gaugeSvg\":\"" + jsonEscapeValue(buildPriceGaugeSvg()) + "\",";
  int nc3 = (metricCurrent15 >= 0) ? euroToCentRounded(metricCurrent15) : -1;
  String cLine = "#4ade80"; String cFill = "#22c55e";
  if (nc3 >= ledRedCent) { cLine = "#ff6b6b"; cFill = "#ff3b30"; }
  else if (nc3 >= ledYellowCent) { cLine = "#ffb347"; cFill = "#ff9500"; }
  json += "\"chartSvg\":\"" + jsonEscapeValue(buildSvgChart(cLine, cFill)) + "\",";
  json += "\"chartPoints\":" + buildChartPointsJson() + ",";
  json += "\"lowText\":\"" + jsonEscapeValue(lowText) + "\",";
  json += "\"avgText\":\"" + jsonEscapeValue(avgText) + "\",";
  json += "\"standText\":\"" + jsonEscapeValue(standText) + "\",";
  json += "\"monthCostText\":\"" + jsonEscapeValue(monthCostText) + "\",";
  json += "\"monthEstimateText\":\"" + jsonEscapeValue(monthEstimateText) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleGaugeStatus() {
  if (!checkAuth()) return;
  int nowCent = (metricCurrent15 >= 0) ? euroToCentRounded(metricCurrent15) : -1;
  String zone = "ok", label = "Guenstig";
  if (nowCent < 0) { zone = ""; label = "Keine Daten"; }
  else if (nowCent >= ledRedCent) { zone = "err"; label = "Jetzt teuer"; }
  else if (nowCent >= ledYellowCent) { zone = "warn"; label = "Jetzt mittel"; }
  String pgClass = "pg-good", pgLabel = "Guenstig";
  if (nowCent >= ledRedCent) { pgClass = "pg-bad"; pgLabel = "Teuer"; }
  else if (nowCent >= ledYellowCent) { pgClass = "pg-mid"; pgLabel = "Mittel"; }
  String json = "{";
  json += "\"badgeClass\":\"" + zone + "b\",";
  json += "\"badgeLabel\":\"" + label + "\",";
  json += "\"pgClass\":\"" + pgClass + "\",";
  json += "\"pgZoneLabel\":\"" + pgLabel + "\",";
  json += "\"yellow\":" + String(ledYellowCent) + ",";
  json += "\"red\":" + String(ledRedCent);
  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// Leichtgewichtiger Endpunkt nur fuer den aktuellen Tibber-Pulse-Verbrauch,
// damit Start- und Tablet-Seite ihn alle paar Sekunden abfragen koennen,
// ohne wie /kioskdata jedes Mal Gauge- und Diagramm-SVG neu zu erzeugen.
void handleLivePower() {
  if (!checkAuth()) return;

  String text = "";
  float pct = -1;
  String zone = "zc";
  if (livePowerW >= 0 && millis() - livePowerUpdatedAtMs < 60000) {
    text = "⚡ " + formatLivePowerValue();
    float kw = livePowerW / 1000.0f;
    pct = kw / livePowerMaxKw * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (kw >= livePowerYellowKw) zone = "ze";
    else if (kw >= livePowerGreenKw) zone = "zm";
  }

  // "value" liefert den Verbrauchswert OHNE das Blitz-Icon (im Unterschied
  // zu "text") - wird von der Startseite gebraucht, die Icon und Wert in
  // getrennten Elementen (pg-label/pg-value) anzeigt statt in einer Zeile.
  // wsEvent/wsEventAgeMs sind reine Diagnosefelder (siehe tibberWsLastEvent) -
  // additiv, das bestehende Frontend ignoriert unbekannte JSON-Felder.
  unsigned long wsAge = tibberWsLastEventMs > 0 ? (millis() - tibberWsLastEventMs) / 1000 : 0;
  String rtFlag = tibberRealTimeEnabledKnown ? (tibberRealTimeEnabled ? "true" : "false") : "unbekannt";
  String json = "{\"text\":\"" + jsonEscapeValue(text) + "\",\"value\":\"" + jsonEscapeValue(formatLivePowerValue()) + "\",\"pct\":" + String(pct, 1) + ",\"zone\":\"" + zone + "\",\"max\":" + String(livePowerMaxKw, 1) + ",\"wsEvent\":\"" + jsonEscapeValue(tibberWsLastEvent) + "\",\"wsEventAgeSec\":" + String(wsAge) + ",\"realTimeConsumptionEnabled\":\"" + rtFlag + "\"}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

// Speichert Position/Groesse/Sichtbarkeit genau eines Kiosk-Widgets - wird
// nach jedem Ziehen/Skalieren im Editor per AJAX aufgerufen (wie saveLayoutNow()
// beim Display-Layout-Editor), statt das ganze Formular auf einmal abzuschicken.
void handleSaveKioskLayoutAjax() {
  if (!checkAuth()) return;

  int index = server.hasArg("index") ? server.arg("index").toInt() : -1;
  bool landscape = server.hasArg("orientation") && server.arg("orientation") == "landscape";
  String target = server.hasArg("target") ? server.arg("target") : "";
  bool isK2 = target == "k2";
  bool isHome = target == "home";
  int count = isHome ? HOME_WIDGET_COUNT : (isK2 ? KIOSK2_WIDGET_COUNT : KIOSK_WIDGET_COUNT);

  if (index < 0 || index >= count) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Ungueltiger Index\"}");
    return;
  }

  // Die Startseite hat kein Hoch-/Querformat - immer ihr eigenes, einzelnes Grid.
  uint8_t cols = isHome ? HOME_GRID_COLS : (landscape ? KIOSK_GRID_COLS_LANDSCAPE : KIOSK_GRID_COLS_PORTRAIT);
  uint8_t rows = isHome ? HOME_GRID_ROWS : (landscape ? KIOSK_GRID_ROWS_LANDSCAPE : KIOSK_GRID_ROWS_PORTRAIT);

  KioskWidgetLayout item;
  item.colStart = server.hasArg("colStart") ? server.arg("colStart").toInt() : 1;
  item.colSpan  = server.hasArg("colSpan")  ? server.arg("colSpan").toInt()  : 1;
  item.rowStart = server.hasArg("rowStart") ? server.arg("rowStart").toInt() : 1;
  item.rowSpan  = server.hasArg("rowSpan")  ? server.arg("rowSpan").toInt()  : 1;
  item.visible  = server.hasArg("visible")  ? server.arg("visible") == "1"  : true;

  // Clamp auf Grid-Grenzen
  if (item.colStart < 1) item.colStart = 1;
  if (item.rowStart < 1) item.rowStart = 1;
  if (item.colSpan < 1) item.colSpan = 1;
  if (item.rowSpan < 1) item.rowSpan = 1;
  if (item.colStart > cols) item.colStart = cols;
  if (item.rowStart > rows) item.rowStart = rows;
  if (item.colStart + item.colSpan - 1 > cols) item.colSpan = cols - item.colStart + 1;
  if (item.rowStart + item.rowSpan - 1 > rows) item.rowSpan = rows - item.rowStart + 1;

  if (isHome) {
    homeLayout[index] = item;
    saveHomeLayoutToPrefs();
  } else if (isK2) {
    if (landscape) { kiosk2Landscape[index] = item; } else { kiosk2Portrait[index] = item; }
    saveKiosk2Widget(landscape, index, item);
  } else {
    if (landscape) { kioskLandscape[index] = item; } else { kioskPortrait[index] = item; }
    saveKioskWidget(landscape, index, item);
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleResetKioskLayout() {
  if (!checkAuth()) return;

  String orientation = server.hasArg("orientation") ? server.arg("orientation") : "";
  bool doPortrait = (orientation == "portrait" || orientation.length() == 0);
  bool doLandscape = (orientation == "landscape" || orientation.length() == 0);
  String target = server.hasArg("target") ? server.arg("target") : "";
  bool isK2 = target == "k2";
  bool isHome = target == "home";

  if (isHome) {
    for (int i = 0; i < HOME_WIDGET_COUNT; i++) {
      homeLayout[i] = HOME_DEFAULTS[i];
    }
    saveHomeLayoutToPrefs();
  } else if (isK2) {
    for (int i = 0; i < KIOSK2_WIDGET_COUNT; i++) {
      if (doPortrait) {
        kiosk2Portrait[i] = KIOSK2_PORTRAIT_DEFAULTS[i];
        saveKiosk2Widget(false, i, kiosk2Portrait[i]);
      }
      if (doLandscape) {
        kiosk2Landscape[i] = KIOSK2_LANDSCAPE_DEFAULTS[i];
        saveKiosk2Widget(true, i, kiosk2Landscape[i]);
      }
    }
    // Hub-Diagramm-Knotenpositionen sind unabhaengig von Hoch-/Querformat
    // (ein einziges viewBox-Koordinatensystem) - bei jedem Kiosk-2-Reset mit
    // zurueckgesetzt, unabhaengig davon, ob nur eine Ausrichtung gewaehlt war.
    for (int i = 0; i < 4; i++) {
      efNodePos[i] = EF_NODE_DEFAULTS[i];
    }
    saveEfNodePosToPrefs();
  } else {
    for (int i = 0; i < KIOSK_WIDGET_COUNT; i++) {
      if (doPortrait) {
        kioskPortrait[i] = KIOSK_PORTRAIT_DEFAULTS[i];
        saveKioskWidget(false, i, kioskPortrait[i]);
      }
      if (doLandscape) {
        kioskLandscape[i] = KIOSK_LANDSCAPE_DEFAULTS[i];
        saveKioskWidget(true, i, kioskLandscape[i]);
      }
    }
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

// Figma-artiger Drag/Resize-Editor fuer das Kiosk-Widget-Layout - siehe
// KioskWidgetLayout weiter oben. Hoch- und Querformat werden getrennt
// bearbeitet (Umschalter oben), aber ohne Neuladen der Seite. Jede
// Ziehen-/Skalieren-Aktion speichert sofort per AJAX (siehe
// handleSaveKioskLayoutAjax), analog zum saveLayoutNow()-Muster des
// Display-Layout-Editors.
void handleKioskLayoutPage() {
  if (!checkAuth()) return;

  // Siehe ausfuehrlichen Kommentar in handleRoot()/handleKioskPage(): ein
  // groesseres reserve() allein loest Truncation bei fragmentiertem Heap
  // NICHT zuverlaessig. Die Antwort wird deshalb in mehreren kleinen Chunks
  // per server.sendContent() gesendet (HTTP Chunked Transfer Encoding).
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  String html;
  html.reserve(6000);

  html += htmlHeader("Kiosk-Layout");
  html += "<section class='hero'><h1>Kiosk-Layout</h1><p>Anordnung des Tablet-Modus frei per Ziehen/Skalieren gestalten - getrennt fuer Hoch- und Querformat.</p></section>";
  html += navTabs("/kiosklayout");

  html += R"CSS(<style>
.kl-shell{display:flex;gap:16px;flex-wrap:wrap;margin-top:14px}
.kl-canvas-wrap{flex:1;min-width:280px;display:flex;justify-content:center}
.kl-canvas{position:relative;width:min(360px,90vw);aspect-ratio:9/16;display:grid;grid-template-columns:repeat(6,1fr);grid-template-rows:repeat(12,1fr);gap:2px;padding:6px;background:var(--overlay-faint);border:1px solid var(--line);border-radius:12px;overflow:hidden;touch-action:none;box-sizing:border-box}
.kl-canvas.landscape{width:min(560px,90vw);aspect-ratio:16/9;grid-template-columns:repeat(12,1fr);grid-template-rows:repeat(8,1fr)}
.kl-cell{background:rgba(96,165,250,.06);border-radius:2px;pointer-events:none}
.kl-item{position:relative;border:2px dashed var(--accent2);background:rgba(96,165,250,.14);border-radius:6px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;padding:4px;cursor:move;box-sizing:border-box;user-select:none;overflow:visible;text-align:center;min-width:0;min-height:0;z-index:2;transition:box-shadow .2s var(--ease)}
.kl-item.selected{border-style:solid;background:rgba(96,165,250,.24)}
.kl-item.kl-hidden{opacity:.32;border-style:dotted}
.kl-item.wg-dragging,.kl-item.wg-resizing{cursor:grabbing;box-shadow:0 14px 34px rgba(0,0,0,.38);z-index:50;transition:none}
.kl-item-caption{font-size:9px;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.4px;pointer-events:none;line-height:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:100%}
.kl-item-preview{font-size:12px;font-weight:800;color:var(--text);pointer-events:none;line-height:1.1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:100%}
.kl-resize{position:absolute;right:-7px;bottom:-7px;width:16px;height:16px;background:var(--accent2);border:2px solid var(--panel);border-radius:50%;cursor:nwse-resize}
.kl-layers{width:230px;border:0;border-radius:14px;padding:12px;background:var(--panel2)}
.kl-layer-row{display:flex;align-items:center;gap:8px;padding:8px;border-radius:8px;cursor:pointer}
.kl-layer-row:hover{background:var(--overlay-hover)}
.kl-layer-row.selected{background:var(--accent-tint-bg)}
.kl-layer-row span{flex:1;font-size:13px;font-weight:700}
.kl-layer-row button{padding:2px 10px;min-height:30px;font-size:15px}
</style>)CSS";

  server.sendContent(html);
  html = "";

  html += "<section class='card'>";
  html += "<div class='panelTitle'><h2>Anordnung</h2><div style='display:flex;gap:8px;flex-wrap:wrap'>";
  html += "<button type='button' id='klTabK1' onclick=\"klSwitchPage('k1')\">Kiosk 1: Preise</button>";
  html += "<button type='button' class='secondary' id='klTabK2' onclick=\"klSwitchPage('k2')\">Kiosk 2: Energie</button>";
  html += "<span style='width:1px;background:var(--line);align-self:stretch;margin:0 2px'></span>";
  html += "<button type='button' id='klTabPortrait' onclick=\"klSwitchOrientation('portrait')\">Hochformat</button>";
  html += "<button type='button' class='secondary' id='klTabLandscape' onclick=\"klSwitchOrientation('landscape')\">Querformat</button>";
  html += "</div></div>";
  html += "<p class='small'>Elemente ziehen zum Verschieben, Punkt unten rechts zum Skalieren. Auge-Symbol = ein-/ausblenden. Aenderungen werden automatisch gespeichert.</p>";

  html += "<div class='kl-shell'>";
  html += "<div class='kl-canvas-wrap'><div class='kl-canvas' id='klCanvas'></div></div>";
  html += "<div class='kl-layers' id='klLayers'></div>";
  html += "</div>";

  html += "<div class='actions'><button type='button' class='secondary' onclick='klReset()'>Diese Ausrichtung zuruecksetzen</button><a href='/kiosk' target='_blank'><button type='button' class='secondary'>Preis-Modus oeffnen</button></a><a href='/kiosk2' target='_blank'><button type='button' class='secondary'>Energie-Modus oeffnen</button></a><span id='klSaveState' class='badge warnb'>Bereit</span></div>";
  html += "</section>";

  server.sendContent(html);
  html = "";

  // Preis-Schwellen fuer Guenstig/Mittel/Teuer
  html += "<section class='card'>";
  html += "<div class='panelTitle'><h2>Preis-Schwellen</h2></div>";
  html += "<p class='small'>Ab welchem Preis gilt der Strom als &quot;Mittel&quot; bzw. &quot;Teuer&quot;? Wirkt sich auf die Farbanzeige in Kiosk, Uebersicht und LED-Ring aus.</p>";
  html += "<form action='/save' method='post'><input type='hidden' name='redirectTo' value='/kiosklayout'>";
  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Balken-Start (ct/kWh)</label><input name='gaugeMin' type='number' min='0' max='200' value='" + String(gaugeMinCent) + "' title='Linker Rand des Preis-Balkens.'></div>";
  html += "<div class='field'><label>Balken-Ende (ct/kWh)</label><input name='gaugeMax' type='number' min='1' max='200' value='" + String(gaugeMaxCent) + "' title='Rechter Rand des Preis-Balkens.'></div>";
  html += "<div class='field'><label>Schwelle Mittel (ab ct/kWh)</label><input name='ledYellow' type='number' min='0' max='200' value='" + String(ledYellowCent) + "' title='Unter diesem Wert = Guenstig (gruen). Ab diesem Wert = Mittel (gelb).'></div>";
  html += "<div class='field'><label>Schwelle Teuer (ab ct/kWh)</label><input name='ledRed' type='number' min='0' max='200' value='" + String(ledRedCent) + "' title='Ab diesem Wert = Teuer (rot).'></div>";
  html += "</div>";
  // Visuelles Feedback
  html += "<div style='margin:12px 0;padding:12px 16px;border-radius:14px;background:var(--panel2);font-size:13px;display:flex;gap:16px;flex-wrap:wrap'>";
  html += "<span style='color:#8e8e93;font-weight:600'>Skala: " + String(gaugeMinCent) + " &ndash; " + String(gaugeMaxCent) + " ct</span>";
  html += "<span style='color:#34C759;font-weight:600'>&#9632; Guenstig: &lt; " + String(ledYellowCent) + " ct</span>";
  html += "<span style='color:#FF9500;font-weight:600'>&#9632; Mittel: " + String(ledYellowCent) + " &ndash; " + String(ledRedCent - 1) + " ct</span>";
  html += "<span style='color:#FF3B30;font-weight:600'>&#9632; Teuer: &ge; " + String(ledRedCent) + " ct</span>";
  html += "</div>";
  html += "<div class='actions'><button type='submit'>Speichern</button></div>";
  html += "</form></section>";
  // Nach dem Speichern der Schwellen: Kiosk-Gauge sofort aktualisieren
  if (server.hasArg("saved")) {
    html += "<script>if(typeof refreshKioskData==='function'){refreshKioskData();}</script>";
  }

  server.sendContent(html);
  html = "";

  // Live-Verbrauch-Einstellungen speziell fuer Kiosk und Modern-Balken.
  html += "<section class='card'>";
  html += "<div class='panelTitle'><h2>Live-Verbrauch-Anzeige</h2></div>";
  html += "<p class='small'>Wie der aktuelle Verbrauch im Tablet-Modus dargestellt wird und wo die Farbschwellen des Balkens liegen. Der Balken wird auch auf der Uebersichts-Seite (Modern-Design) verwendet. In der &quot;Bar&quot;-Variante zeigt der Kiosk nur Wert und einen farbigen Balken &ndash; genug fuer eine kompakte Widget-Groesse.</p>";
  html += "<form action='/save' method='post'><input type='hidden' name='redirectTo' value='/kiosklayout'>";
  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Kiosk-Stil</label><select name='klpStyle'>";
  html += "<option value='text'";
  if (kioskLivePowerStyle == "text") html += " selected";
  html += ">Text (kompakt, z.B. &quot;&#9889; 1.23 kW&quot;)</option>";
  html += "<option value='bar'";
  if (kioskLivePowerStyle == "bar") html += " selected";
  html += ">Bar (Wert + Farbbalken 0-max)</option>";
  html += "</select></div>";
  html += "<div class='field'><label>Balken Maximum (kW)</label><input name='lpMax' type='number' step='0.5' min='1' max='50' value='" + String(livePowerMaxKw, 1) + "' title='Endwert der Skala und des Farbverlaufs.'></div>";
  html += "<div class='field'><label>Gruene Zone bis (kW)</label><input name='lpGreen' type='number' step='0.1' min='0.1' max='50' value='" + String(livePowerGreenKw, 1) + "' title='Bis zu diesem Wert wird der Balken gruen (niedriger Verbrauch).'></div>";
  html += "<div class='field'><label>Gelbe Zone bis (kW)</label><input name='lpYellow' type='number' step='0.1' min='0.1' max='50' value='" + String(livePowerYellowKw, 1) + "' title='Bis zu diesem Wert wird der Balken gelb, darueber rot.'></div>";
  html += "</div>";
  html += "<div class='actions'><button type='submit'>Speichern</button></div>";
  html += "</form>";
  html += "</section>";

  server.sendContent(html);
  html = "";

  html += "<script src='/widget-engine.js'></script>";
  html += "<script>var klData = {";
  html += "k1:{portrait:" + kioskLayoutJson(kioskPortrait) + ",landscape:" + kioskLayoutJson(kioskLandscape) + "},";
  html += "k2:{portrait:" + kioskLayoutJson(kiosk2Portrait, KIOSK2_WIDGET_KEYS, KIOSK2_WIDGET_LABELS, KIOSK2_WIDGET_COUNT) + ",landscape:" + kioskLayoutJson(kiosk2Landscape, KIOSK2_WIDGET_KEYS, KIOSK2_WIDGET_LABELS, KIOSK2_WIDGET_COUNT) + "}";
  html += "};</script>";

  server.sendContent(html);
  html = "";

  html += R"JS(<script>
var klPage = 'k1';
var klOrientation = 'portrait';
var klSelected = 0;
var klController = null;
var KL_GRID = {
  portrait:  { cols: 6,  rows: 12 },
  landscape: { cols: 12, rows: 8  }
};
function klGrid(){ return KL_GRID[klOrientation]; }

function klRenderCanvas(){
  var canvas = document.getElementById('klCanvas');
  canvas.innerHTML = '';
  canvas.className = 'kl-canvas' + (klOrientation === 'landscape' ? ' landscape' : '');
  var g = klGrid();
  // Grid-Zellen als visueller Hintergrund
  for (var r = 0; r < g.rows; r++) {
    for (var c = 0; c < g.cols; c++) {
      var cell = document.createElement('div');
      cell.className = 'kl-cell';
      cell.style.gridColumn = (c+1) + '/span 1';
      cell.style.gridRow = (r+1) + '/span 1';
      canvas.appendChild(cell);
    }
  }
  var items = klData[klPage][klOrientation];
  items.forEach(function(item, i){
    var el = document.createElement('div');
    el.className = 'kl-item' + (i === klSelected ? ' selected' : '') + (!item.visible ? ' kl-hidden' : '');
    el.setAttribute('data-idx', i);
    el.style.gridColumn = item.colStart + '/span ' + item.colSpan;
    el.style.gridRow = item.rowStart + '/span ' + item.rowSpan;
    var caption = document.createElement('span');
    caption.className = 'kl-item-caption';
    caption.textContent = item.label;
    el.appendChild(caption);
    if (item.preview) {
      var val = document.createElement('span');
      val.className = 'kl-item-preview';
      val.innerHTML = item.preview;
      el.appendChild(val);
    }
    var handle = document.createElement('span');
    handle.className = 'kl-resize';
    handle.addEventListener('pointerdown', function(e){ klController.startResize(e, i); });
    el.appendChild(handle);
    el.addEventListener('pointerdown', function(e){ klSelected = i; klController.startDrag(e, i); });
    canvas.appendChild(el);
  });
  klController = WidgetGridEngine.createController({
    getEl: function(i){ return canvas.querySelector('.kl-item[data-idx="' + i + '"]'); },
    getItems: function(){ return klData[klPage][klOrientation]; },
    getGrid: klGrid,
    cellSize: function(){
      var r = canvas.getBoundingClientRect();
      var gr = klGrid();
      return { w: r.width / gr.cols, h: r.height / gr.rows };
    },
    applyLayout: function(i){
      var el = canvas.querySelector('.kl-item[data-idx="' + i + '"]');
      var item = klData[klPage][klOrientation][i];
      if (el) {
        el.style.gridColumn = item.colStart + '/span ' + item.colSpan;
        el.style.gridRow = item.rowStart + '/span ' + item.rowSpan;
      }
    },
    onCommit: klCommit
  });
  klRenderLayers();
}
</script>)JS";

  server.sendContent(html);
  html = "";

  html += R"JS(<script>
function klRenderLayers(){
  var wrap = document.getElementById('klLayers');
  wrap.innerHTML = '';
  klData[klPage][klOrientation].forEach(function(item, i){
    var row = document.createElement('div');
    row.className = 'kl-layer-row' + (i === klSelected ? ' selected' : '');
    row.addEventListener('click', function(){ klSelected = i; klRenderCanvas(); });
    var label = document.createElement('span');
    label.textContent = item.label;
    var vis = document.createElement('button');
    vis.type = 'button';
    vis.className = 'secondary';
    vis.textContent = item.visible ? '●' : '○';
    vis.title = item.visible ? 'Ausblenden' : 'Einblenden';
    // klCommit() ruft am Ende nur klRenderLayers() auf, nicht klRenderCanvas() -
    // ohne den direkten klRenderCanvas()-Aufruf hier wuerde das Canvas-Element
    // (kl-hidden-Klasse fuer Transparenz/Rahmen) erst nach Abschluss des
    // Speicher-Requests optisch aktualisiert, statt sofort beim Klick.
    vis.addEventListener('click', function(e){ e.stopPropagation(); item.visible = !item.visible; klRenderCanvas(); klCommit([i]); });
    row.appendChild(label);
    row.appendChild(vis);
    wrap.appendChild(row);
  });
}

// Speichert ein einzelnes Widget; Rueckgabewert ist ein Promise<boolean>.
function klSaveOne(i){
  var item = klData[klPage][klOrientation][i];
  var body = new URLSearchParams();
  body.set('target', klPage);
  body.set('orientation', klOrientation);
  body.set('index', i);
  body.set('colStart', item.colStart);
  body.set('colSpan', item.colSpan);
  body.set('rowStart', item.rowStart);
  body.set('rowSpan', item.rowSpan);
  body.set('visible', item.visible ? '1' : '0');
  // keepalive: true - der Request darf ueberleben, auch wenn der Nutzer direkt
  // nach dem Ziehen zu einer anderen Seite navigiert (z.B. zur Konto-Seite, um
  // ein Firmware-Update zu starten) - sonst kann der Browser die noch laufende
  // Speicher-Anfrage abbrechen, bevor sie den Server erreicht, und die neue
  // Position geht scheinbar "nach dem Update" verloren (tatsaechlich wurde sie
  // nie gespeichert).
  return fetch('/savekiosklayoutajax', { method: 'POST', keepalive: true, headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(r){ return r.json(); })
    .then(function(d){ return !!d.ok; })
    .catch(function(){ return false; });
}

// Wird einmal pro Geste (Drag/Resize/Sichtbarkeits-Umschalten) mit ALLEN
// veraenderten Indizes aufgerufen - inkl. per Kollisions-Schubs verschobener
// Nachbarn. Speichert nacheinander (nicht parallel), da der ESP32-WebServer
// einzelne Requests sequentiell abarbeitet.
function klCommit(indexes){
  var state = document.getElementById('klSaveState');
  if (state) { state.className = 'badge warnb'; state.textContent = 'Speichere...'; }
  var allOk = true;
  var chain = Promise.resolve();
  indexes.forEach(function(i){
    chain = chain.then(function(){ return klSaveOne(i); }).then(function(ok){ if (!ok) allOk = false; });
  });
  chain.then(function(){
    if (state) { state.className = allOk ? 'badge okb' : 'badge errb'; state.textContent = allOk ? 'Gespeichert' : 'Fehler'; }
    klRenderLayers();
  });
}
</script>)JS";

  server.sendContent(html);
  html = "";

  html += R"JS(<script>
function klSwitchOrientation(o){
  klOrientation = o;
  klSelected = 0;
  document.getElementById('klTabPortrait').className = o === 'portrait' ? '' : 'secondary';
  document.getElementById('klTabLandscape').className = o === 'landscape' ? '' : 'secondary';
  klRenderCanvas();
}

function klSwitchPage(p){
  klPage = p;
  klSelected = 0;
  document.getElementById('klTabK1').className = p === 'k1' ? '' : 'secondary';
  document.getElementById('klTabK2').className = p === 'k2' ? '' : 'secondary';
  klRenderCanvas();
}

function klReset(){
  var name = klOrientation === 'landscape' ? 'Querformat' : 'Hochformat';
  var pageName = klPage === 'k2' ? 'Kiosk 2 (Energie)' : 'Kiosk 1 (Preise)';
  if (!confirm(pageName + ' - ' + name + ' auf Standard zuruecksetzen?')) return;
  var body = new URLSearchParams();
  body.set('target', klPage);
  body.set('orientation', klOrientation);
  fetch('/resetkiosklayout', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(){ location.reload(); });
}

klRenderCanvas();
</script>)JS";

  html += htmlFooter();
  server.sendContent(html);
  server.sendContent("");
}

void handleWifiPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(26000);
  html += htmlHeader("WLAN");
  html += "<section class='hero'><h1>WLAN</h1><p>WLAN-Verwaltung mit Scan, Auswahl und Live-Status.</p></section>";
  html += navTabs("/wifi");

  html += R"JS(
<style>
.wifi-dashboard{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}.wifi-panel{border:1px solid var(--line);background:var(--overlay-faint);border-radius:18px;padding:14px}.wifi-panel h3{margin:0 0 8px}.wifi-status-line{font-size:13px;color:var(--muted);margin-top:6px}.wifi-signal{display:flex;align-items:flex-end;gap:5px;height:34px;margin:10px 0}.wifi-signal span{display:block;width:14px;border-radius:5px 5px 2px 2px;background:rgba(148,163,184,.3);border:1px solid var(--surface-border)}.wifi-signal span:nth-child(1){height:9px}.wifi-signal span:nth-child(2){height:14px}.wifi-signal span:nth-child(3){height:19px}.wifi-signal span:nth-child(4){height:25px}.wifi-signal span:nth-child(5){height:31px}.wifi-signal span.on{background:linear-gradient(180deg,var(--accent),var(--ok));border-color:var(--accent)}.wifi-list{display:grid;gap:8px;margin-top:12px}.wifi-row{display:grid;grid-template-columns:1fr auto auto;gap:10px;align-items:center;border:1px solid var(--line);border-radius:14px;padding:10px;background:var(--overlay-faint);cursor:pointer}.wifi-row:hover{background:var(--accent-tint-bg)}.wifi-row.selected{border-color:var(--accent);background:var(--accent-tint-bg)}.wifi-row .ssid{font-weight:900}.wifi-row .meta{font-size:12px;color:var(--muted)}.wifi-form{display:grid;grid-template-columns:1fr 1fr;gap:12px}.wifi-form .wide{grid-column:1/3}.mesh-note{border:1px solid rgba(245,158,11,.35);background:rgba(245,158,11,.08);border-radius:16px;padding:12px;margin-top:12px}@media(max-width:760px){.wifi-form{grid-template-columns:1fr}.wifi-form .wide{grid-column:1}.wifi-row{grid-template-columns:1fr}.wifi-row button{width:100%}}
.wifi-scanning{display:flex;flex-direction:column;align-items:center;gap:14px;padding:26px 10px}
.wifi-radar{position:relative;width:64px;height:64px;border-radius:50%;border:3px solid var(--surface-border)}
.wifi-radar:before{content:'';position:absolute;inset:-3px;border-radius:50%;border:3px solid transparent;border-top-color:var(--accent);border-right-color:var(--accent);animation:wifiRadarSpin .9s linear infinite}
.wifi-radar:after{content:'';position:absolute;inset:14px;border-radius:50%;background:var(--accent);opacity:.35;animation:wifiRadarPulse 1.4s ease-out infinite}
@keyframes wifiRadarSpin{to{transform:rotate(360deg)}}
@keyframes wifiRadarPulse{0%{transform:scale(.4);opacity:.55}100%{transform:scale(2.4);opacity:0}}
.wifi-scan-btn.scanning{opacity:.7;pointer-events:none}
</style>
)JS";

  html += "<section class='card'>";
  html += "<div class='panelTitle'><h2>Live Status</h2><span id='wifiStateBadge' class='badge warnb'>Lade...</span></div>";
  html += "<div class='wifi-dashboard'>";
  html += "<div class='wifi-panel'><h3><span id='wifiDot' class='status-dot warn'></span>Verbindung</h3><div id='wifiStatusText' class='value'>--</div><div id='wifiIpText' class='wifi-status-line'>IP: --</div><div id='wifiMacText' class='wifi-status-line'>MAC: --</div></div>";
  html += "<div class='wifi-panel'><h3>Signal</h3><div id='wifiBars' class='wifi-signal'></div><div id='wifiQualityText' class='wifi-status-line'>--</div></div>";
  html += "<div class='wifi-panel'><h3>Setup-WLAN</h3><div id='setupText' class='value'>--</div><div id='setupIpText' class='wifi-status-line'>--</div></div>";
  html += "</div>";
  html += "<div class='actions'><button type='button' onclick='refreshStatus()'>Status aktualisieren</button><button class='secondary' type='button' onclick='startConnect()'>Neu verbinden</button></div>";
  html += "</section>";

  html += "<section class='card'>";
  html += "<div class='panelTitle'><h2>WLAN auswählen</h2><button id='wifiScanBtn' class='secondary wifi-scan-btn' type='button' onclick='scanWifi()'>WLAN scannen</button></div>";
  html += "<p class='small'>Netzwerk unten anklicken, Passwort eintragen, speichern - fertig.</p>";
  html += "<div id='wifiList' class='wifi-list'><p class='small'>Noch kein Scan.</p></div>";

  html += "<div class='wifi-form' style='margin-top:14px'>";
  html += "<div class='field wide'><label>SSID</label><input id='wifiSsid' maxlength='32' placeholder='WLAN Name'></div>";
  html += "<div class='field wide'><label>Passwort</label><input id='wifiPass' type='password' maxlength='64' placeholder='WLAN Passwort'></div>";
  html += "</div>";
  html += "<div class='actions'><button type='button' onclick='saveWifi()'>Speichern und verbinden</button><button class='secondary' type='button' onclick='togglePass()'>Passwort anzeigen</button></div>";
  html += "<div id='wifiMessage' class='wifi-status-line'>Bereit.</div>";

  html += "<details style='margin-top:16px'><summary><h3 style='display:inline'>Erweiterte Einstellungen</h3></summary>";
  html += "<div class='wifi-form' style='margin-top:12px'>";
  html += "<div class='field wide'><label>Name des Setup-WLANs</label><input id='setupSsid' maxlength='32' placeholder='Tibber Strompreis'></div>";
  html += "<div class='field'><div class='toggleRow'><label title='Aendert bei jeder Verbindung die WLAN-MAC-Adresse, z.B. gegen Geraete-Tracking im Netzwerk.'>MAC-Rotation</label><label class='toggle'><input type='checkbox' id='macRotate'><span class='toggleSlider'></span></label></div><div class='small' style='margin-top:6px'>Neue MAC-Adresse bei jedem Verbindungsaufbau (Datenschutz).</div></div>";
  html += "<div class='field'><label>WLAN-Band ESP32-C5</label><select id='wifiBand' title='Bei Verbindungsproblemen ein festes Frequenzband erzwingen.'><option value='0'>Auto 2,4 + 5 GHz</option><option value='1'>Nur 2,4 GHz</option><option value='2'>Nur 5 GHz</option></select><div class='small' style='margin-top:6px'>Bei Verbindungsproblemen ein festes Band waehlen statt Auto.</div></div>";
  html += "<div class='field'><div class='toggleRow'><label title='Vergibt automatisch die fuenftletzte IP-Adresse im Subnetz.'>Automatische IP</label><label class='toggle'><input type='checkbox' id='auto5thIp'><span class='toggleSlider'></span></label></div><div class='small' style='margin-top:6px'>Feste, vorhersehbare Adresse ohne manuelle IP-Konfiguration.</div></div>";
  html += "<div class='field'><div class='toggleRow'><label>Feste IP manuell</label><label class='toggle'><input type='checkbox' id='staticIpOn'><span class='toggleSlider'></span></label></div></div>";
  html += "<div class='field'><label>IP-Adresse</label><input id='staticIp' placeholder='z.B. 192.168.178.250'></div>";
  html += "<div class='field'><label>Gateway</label><input id='gateway' placeholder='z.B. 192.168.178.1'></div>";
  html += "<div class='field'><label>Subnetz</label><input id='subnet' placeholder='255.255.255.0'></div>";
  html += "<div class='field'><label>DNS 1</label><input id='dns1' placeholder='leer = Gateway'></div>";
  html += "<div class='field'><label>DNS 2</label><input id='dns2' placeholder='1.1.1.1'></div>";
  html += "</div>";
  html += "<div class='mesh-note'><b>Mesh-Hinweis:</b><br>Bei mehreren gleichen WLAN-Namen kann der ESP32 den falschen Repeater wählen. Am stabilsten ist ein eigenes 2,4-GHz-WLAN/Gast-WLAN fuer den ESP32 mit WPA2/CCMP.</div>";
  html += "</details>";
  html += "</section>";

  html += R"JS(
<script>
let selectedSsid='';
function $(id){return document.getElementById(id);}
function setMsg(t){$('wifiMessage').innerText=t;}
function bars(q){let n=0;if(q>=80)n=5;else if(q>=60)n=4;else if(q>=40)n=3;else if(q>=20)n=2;else if(q>0)n=1;let h='';for(let i=1;i<=5;i++){h+='<span'+(i<=n?' class="on"':'')+'></span>';}return h;}
function authText(a){if(a===0)return 'offen';if(a===1)return 'WEP';if(a===2)return 'WPA';if(a===3)return 'WPA2';if(a===4)return 'WPA/WPA2';if(a===5)return 'WPA2 Enterprise';if(a===6)return 'WPA3';if(a===7)return 'WPA2/WPA3';return 'Auth '+a;}
async function refreshStatus(){try{const r=await fetch('/wifistatusjson',{cache:'no-store'});const s=await r.json();$('wifiStateBadge').className='badge '+(s.connected?'okb':'errb');$('wifiStateBadge').innerText=s.connected?'Verbunden':'Nicht verbunden';$('wifiDot').className='status-dot pulse '+(s.connected?'ok':'warn');$('wifiStatusText').innerText=s.connected?(s.activeSsid||s.savedSsid):s.status;$('wifiIpText').innerText='IP: '+(s.ip||'--')+' | Kanal: '+(s.channel||'--');$('wifiMacText').innerText='MAC: '+(s.mac||'--')+' | Chip: '+(s.chip||'--');$('wifiBars').innerHTML=bars(s.quality||0);$('wifiQualityText').innerText=(s.quality||0)+'% | RSSI '+(s.rssi||'--')+' dBm';$('setupText').innerText=s.setupSsid||'--';$('setupIpText').innerText=(s.setupActive?'aktiv ':'aus ')+(s.setupIp||'');$('wifiSsid').value=$('wifiSsid').value||s.savedSsid||'';$('setupSsid').value=$('setupSsid').value||s.setupSsid||'Tibber Strompreis';$('macRotate').checked=!!s.macRotate;$('wifiBand').value=String(s.wifiBand||0);$('staticIpOn').checked=!!s.staticIpOn;$('auto5thIp').checked=!!s.auto5thIp;$('staticIp').value=$('staticIp').value||s.staticIp||'';$('gateway').value=$('gateway').value||s.gateway||'';$('subnet').value=$('subnet').value||s.subnet||'255.255.255.0';$('dns1').value=$('dns1').value||s.dns1||'';$('dns2').value=$('dns2').value||s.dns2||'';}catch(e){setMsg('Status Fehler: '+e);}}
async function scanWifi(){const btn=$('wifiScanBtn');if(btn){btn.classList.add('scanning');btn.innerText='Scanne...';}setMsg('Scanne WLANs...');$('wifiList').innerHTML='<div class="wifi-scanning"><div class="wifi-radar"></div><p class="small">Suche nach WLANs in der Umgebung...</p></div>';try{const r=await fetch('/wifiscanjson',{cache:'no-store'});const data=await r.json();const list=data.networks||[];if(!list.length){$('wifiList').innerHTML='<p class="small">Keine WLANs gefunden.</p>';setMsg('Keine WLANs gefunden.');return;}list.sort((a,b)=>b.rssi-a.rssi);$('wifiList').innerHTML='';list.forEach((n,idx)=>{const row=document.createElement('div');row.className='wifi-row';row.innerHTML='<div><div class="ssid">'+(n.ssid||'<versteckt>')+'</div><div class="meta">BSSID '+n.bssid+' | '+authText(n.auth)+'</div></div><div class="meta">'+n.rssi+' dBm / '+n.quality+'%</div><div class="meta">Kanal '+n.channel+'</div>';row.onclick=()=>{document.querySelectorAll('.wifi-row').forEach(x=>x.classList.remove('selected'));row.classList.add('selected');selectedSsid=n.ssid;$('wifiSsid').value=n.ssid;setMsg('Ausgewählt: '+n.ssid);};$('wifiList').appendChild(row);});setMsg(list.length+' WLANs gefunden.');}catch(e){$('wifiList').innerHTML='<p class="small">Scan fehlgeschlagen.</p>';setMsg('Scan Fehler: '+e);}finally{if(btn){btn.classList.remove('scanning');btn.innerText='WLAN scannen';}}}
async function saveWifi(){const ssid=$('wifiSsid').value.trim();const pass=$('wifiPass').value;const setup=$('setupSsid').value.trim()||'Tibber Strompreis';if(!ssid){setMsg('Bitte SSID auswählen oder eingeben.');return;}const body=new URLSearchParams();body.set('ssid',ssid);body.set('pass',pass);body.set('setupSsid',setup);body.set('macRotate',$('macRotate').checked?'1':'0');body.set('wifiBand',$('wifiBand').value);body.set('staticIpOn',$('staticIpOn').checked?'1':'0');body.set('auto5thIp',$('auto5thIp').checked?'1':'0');body.set('staticIp',$('staticIp').value.trim());body.set('gateway',$('gateway').value.trim());body.set('subnet',$('subnet').value.trim());body.set('dns1',$('dns1').value.trim());body.set('dns2',$('dns2').value.trim());setMsg('Speichere und starte Verbindung...');try{const r=await fetch('/savewifiajax',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:body.toString()});const j=await r.json();setMsg(j.ok?'Gespeichert. Verbindung startet...':'Fehler beim Speichern.');showToast(j.ok?'Gespeichert, WLAN-Verbindung startet':'Fehler beim Speichern',j.ok?'ok':'err');setTimeout(refreshStatus,1000);}catch(e){setMsg('Speicher Fehler: '+e);showToast('Speicher Fehler','err');}}
async function startConnect(){setMsg('Verbindung wird gestartet...');try{await fetch('/connectwifiajax',{method:'POST'});setMsg('Verbindungsversuch gestartet.');showToast('Verbindungsversuch gestartet','info');setTimeout(refreshStatus,1000);}catch(e){setMsg('Connect Fehler: '+e);showToast('Verbindungsfehler','err');}}
function togglePass(){const p=$('wifiPass');p.type=p.type==='password'?'text':'password';}
document.addEventListener('DOMContentLoaded',()=>{refreshStatus();scanWifi();setInterval(refreshStatus,5000);});
</script>
)JS";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void handleDisplaysPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(68000);

  html += htmlHeader("Displays");
  html += "<section class='hero'><h1>Displays</h1><p>Anzeigemodi, feste Overlays und der freie Layout-Editor fuer Display 1 und Display 2 - an einem Ort.</p></section>";
  html += navTabs("/displays");

  html += "<div class='dpTabs'>";
  html += "<button type='button' id='tabBtn-overlays' class='dpTab active' onclick=\"switchDpTab('overlays')\">Overlays</button>";
  html += "<button type='button' id='tabBtn-layout' class='dpTab' onclick=\"switchDpTab('layout')\">Layout-Editor</button>";
  html += "</div>";
  html += "<style>";
  html += ".dpTabs{display:inline-flex;padding:4px;background:var(--panel2);border-radius:12px;margin:14px 0 16px;gap:2px}";
  html += ".dpTab{background:transparent!important;color:var(--muted)!important;border:0!important;box-shadow:none!important;padding:8px 20px!important;min-height:36px!important;border-radius:10px!important;font-weight:600!important;font-size:14px!important;transition:background .15s var(--ease),color .15s var(--ease)!important}";
  html += ".dpTab.active{background:var(--card)!important;color:var(--text)!important;box-shadow:0 1px 2px var(--shadow-soft)!important}";
  html += ".dpTab:hover:not(.active){color:var(--text)!important}";
  html += "</style>";

  html += "<div id='tabOverlays'>";

  html += "<section class='card'><h2>Display-Modi</h2>";
  html += "<form action='/savedisplays' method='post'>";
  html += "<div class='twoCol'>";

  for (int d = 1; d <= 2; d++) {
    bool enabled = (d == 1) ? display1Enabled : display2Enabled;
    int mode = (d == 1) ? display1Mode : display2Mode;
    String prefix = "d" + String(d);

    html += "<div class='metric'>";
    html += "<div class='label'>Display ";
    html += String(d);
    html += "</div>";
    html += "<div class='value' style='font-size:28px'>";
    html += enabled ? "Ein" : "Aus";
    html += "</div>";

    html += "<div class='field'><label>Aktiv</label><select name='";
    html += prefix;
    html += "Enabled'>";
    html += "<option value='1'";
    if (enabled) html += " selected";
    html += ">Ein</option><option value='0'";
    if (!enabled) html += " selected";
    html += ">Aus</option></select></div>";

    html += "<div class='field'><label>Anzeigemodus</label><select name='";
    html += prefix;
    html += "Mode'>";

    const char* labels[] = {"Layout aus Editor", "Display aus", "Nur Uhrzeit", "Nur Datum", "Datum + Uhrzeit"};
    for (int m = 0; m <= 4; m++) {
      html += "<option value='";
      html += String(m);
      html += "'";
      if (mode == m) html += " selected";
      html += ">";
      html += labels[m];
      html += "</option>";
    }

    html += "</select></div>";
    html += "<p class='small'>Uhrzeit: ";
    html += getDisplayTimeText();
    html += " · Datum: ";
    html += getDisplayDateText();
    html += "</p>";
    html += "</div>";
  }

  html += "</div>";

  html += "<section class='formSection'><h3>Preisbalken am Bildschirmrand</h3><p class='small'>Schmaler Farbbalken am Rand von Display 1, der den aktuellen Preis anzeigt.</p><div class='formGrid'>";
  html += "<div class='field'><label>Preisbalken anzeigen</label><select name='priceBarOn'>";
  html += "<option value='1'";
  if (displayPriceBarEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!displayPriceBarEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "<div class='field'><label>Preistext unter dem Balken anzeigen</label><select name='priceBarText'>";
  html += "<option value='1'";
  if (displayPriceBarTextEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!displayPriceBarTextEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "<div class='field'><label>Skala Minimalpreis (ct/kWh)</label><input name='priceBarMin' type='number' min='0' max='200' value='" + String(displayPriceBarMinCent) + "'></div>";
  html += "<div class='field'><label>Skala Maximalpreis (ct/kWh)</label><input name='priceBarMax' type='number' min='1' max='300' value='" + String(displayPriceBarMaxCent) + "'></div>";
  html += "<div class='field'><label>Balkenbreite (Pixel)</label><input name='priceBarW' type='number' min='4' max='24' value='" + String(displayPriceBarWidth) + "'></div>";
  html += "</div></section>";

  html += "<section class='formSection'><h3>Tages-Liniendiagramm</h3><p class='small'>Kleines Preisverlaufs-Diagramm, das zusaetzlich auf einem oder beiden Displays eingeblendet werden kann.</p><div class='formGrid'>";
  html += "<div class='field'><label>Auf Display 1 anzeigen</label><select name='d1DayChart'>";
  html += "<option value='1'";
  if (display1DayChartEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!display1DayChartEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "<div class='field'><label>Auf Display 2 anzeigen</label><select name='d2DayChart'>";
  html += "<option value='1'";
  if (display2DayChartEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!display2DayChartEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "<div class='field'><label>Position X (Pixel)</label><input id='dayChartX' name='dayChartX' type='number' min='0' max='239' value='" + String(displayDayChartX) + "'></div>";
  html += "<div class='field'><label>Position Y (Pixel)</label><input id='dayChartY' name='dayChartY' type='number' min='0' max='239' value='" + String(displayDayChartY) + "'></div>";
  html += "<div class='field'><label>Breite (Pixel)</label><input id='dayChartW' name='dayChartW' type='number' min='60' max='240' value='" + String(displayDayChartWidth) + "'></div>";
  html += "<div class='field'><label>Höhe (Pixel)</label><input id='dayChartH' name='dayChartH' type='number' min='40' max='150' value='" + String(displayDayChartHeight) + "'></div>";
  html += "</div>";
  html += "<div class='formSection'><h4>Diagramm per Drag &amp; Drop verschieben</h4>";
  html += "<div id='chartDragPreview' style='position:relative;width:240px;height:240px;border-radius:50%;overflow:hidden;background:#020617;border:2px solid #334155;margin:10px auto;touch-action:none;'>";
  html += "<div style='position:absolute;inset:8px;border-radius:50%;border:1px dashed rgba(255,255,255,.18);'></div>";
  html += "<div id='chartDragBox' style='position:absolute;left:" + String(displayDayChartX) + "px;top:" + String(displayDayChartY) + "px;width:" + String(displayDayChartWidth) + "px;height:" + String(displayDayChartHeight) + "px;border:2px solid #fff;background:rgba(34,197,94,.18);box-sizing:border-box;cursor:move;border-radius:8px;box-shadow:0 0 12px rgba(255,255,255,.35);'>";
  html += "<span style='position:absolute;left:6px;top:4px;color:#fff;font-size:11px;font-weight:800;'>Diagramm</span></div></div>";
  html += "<p class='small'>Ziehe das Diagramm in der runden Vorschau. X/Y werden automatisch gespeichert. Breite und Höhe kannst du oben ueber die Felder einstellen.</p></div>";
  html += "</section>";

  html += "<section class='formSection'><h3>Preis-Uhr auf Display 2 (24h-Ring)</h3><p class='small'>Ringfoermige Anzeige, die die guenstigen und teuren Stunden des Tages farbig um das Display herum darstellt.</p><div class='formGrid'>";
  html += "<div class='field'><label>Preis-Uhr aktivieren</label><select name='d2ClockRing'>";
  html += "<option value='1'";
  if (display2CheapClockRingEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!display2CheapClockRingEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "<div class='field'><label>Ringbreite (Pixel)</label><input name='d2ClockRingW' type='number' min='6' max='28' value='" + String(display2CheapClockRingWidth) + "'></div>";
  html += "<div class='field'><label>Preistext unter der Uhr anzeigen</label><select name='d2ClockText'>";
  html += "<option value='1'";
  if (display2CheapClockRingTextEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!display2CheapClockRingTextEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "<div class='field'><label>Uhrzeit-Beschriftung am Ring anzeigen</label><select name='d2ClockLabels'>";
  html += "<option value='1'";
  if (display2CheapClockRingLabelsEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!display2CheapClockRingLabelsEnabled) html += " selected";
  html += ">Aus</option></select></div>";
  html += "</div>";
  html += "<h4>Ringfarben</h4><div class='formGrid'>";
  addColorSelect(html, "d2RingCheap", "Farbe günstige Stunden", display2RingCheapColorId);
  addColorSelect(html, "d2RingMid", "Farbe mittlere Stunden", display2RingMidColorId);
  addColorSelect(html, "d2RingHigh", "Farbe teure Stunden", display2RingHighColorId);
  addColorSelect(html, "d2RingBest", "Farbe guenstigster Block", display2RingBestColorId);
  addColorSelect(html, "d2RingNow", "Farbe aktuelle Stunde", display2RingCurrentColorId);
  html += "</div></section>";

  html += "<div class='actions'><button type='submit'>Display-Einstellungen speichern</button><span id='displayAutoSaveState' class='badge warnb'>Bereit</span></div>";
  html += R"JS(
<script>
let displayAutoSaveTimer=null;
let displayAutoSaveBusy=false;
let displayAutoSaveAgain=false;
let chartDragActive=false;
let chartDragOffX=0;
let chartDragOffY=0;

function displayAutoState(text, cls){
  const el=document.getElementById('displayAutoSaveState');
  if(!el)return;
  el.className='badge '+cls;
  el.innerText=text;
}

function displayForm(){
  return document.querySelector("form[action='/savedisplays']") || document.querySelector('form');
}

function saveDisplaysNow(){
  const form=displayForm();
  if(!form){displayAutoState('Kein Formular','errb');return;}
  if(displayAutoSaveBusy){displayAutoSaveAgain=true;return;}
  displayAutoSaveBusy=true;
  displayAutoState('Speichere...','warnb');
  fetch('/savedisplaychartajax',{
    method:'POST',cache:'no-store',credentials:'same-origin',
    headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
    body:new URLSearchParams(new FormData(form)).toString()
  }).then(()=>{
    displayAutoState('Gespeichert','okb');
  }).catch(()=>{
    displayAutoState('Fehler','errb');
  }).finally(()=>{
    displayAutoSaveBusy=false;
    if(displayAutoSaveAgain){displayAutoSaveAgain=false;setTimeout(saveDisplaysNow,150);}
  });
}

function scheduleDisplayAutoSave(){
  clearTimeout(displayAutoSaveTimer);
  displayAutoState('Änderungen...','warnb');
  displayAutoSaveTimer=setTimeout(saveDisplaysNow,450);
}

function clampDisplayChart(){
  const x=document.getElementById('dayChartX');
  const y=document.getElementById('dayChartY');
  const w=document.getElementById('dayChartW');
  const h=document.getElementById('dayChartH');
  if(!x||!y||!w||!h)return;
  let vx=parseInt(x.value||0),vy=parseInt(y.value||0),vw=parseInt(w.value||204),vh=parseInt(h.value||76);
  if(vw<60)vw=60;if(vw>240)vw=240;if(vh<40)vh=40;if(vh>150)vh=150;
  if(vx<0)vx=0;if(vy<0)vy=0;if(vx+vw>240)vx=240-vw;if(vy+vh>240)vy=240-vh;
  x.value=vx;y.value=vy;w.value=vw;h.value=vh;
}

function updateChartDragBox(){
  clampDisplayChart();
  const box=document.getElementById('chartDragBox');
  const x=document.getElementById('dayChartX');
  const y=document.getElementById('dayChartY');
  const w=document.getElementById('dayChartW');
  const h=document.getElementById('dayChartH');
  if(!box||!x||!y||!w||!h)return;
  box.style.left=x.value+'px';box.style.top=y.value+'px';box.style.width=w.value+'px';box.style.height=h.value+'px';
}

function chartPoint(e){
  if(e.touches&&e.touches.length)return {x:e.touches[0].clientX,y:e.touches[0].clientY};
  return {x:e.clientX,y:e.clientY};
}

function chartDragStart(e){
  const box=document.getElementById('chartDragBox');
  if(!box)return;
  const p=chartPoint(e);const r=box.getBoundingClientRect();
  chartDragOffX=p.x-r.left;chartDragOffY=p.y-r.top;chartDragActive=true;
  if(e.preventDefault)e.preventDefault();
}

function chartDragMove(e){
  if(!chartDragActive)return;
  const prev=document.getElementById('chartDragPreview');
  const x=document.getElementById('dayChartX');
  const y=document.getElementById('dayChartY');
  const w=document.getElementById('dayChartW');
  const h=document.getElementById('dayChartH');
  if(!prev||!x||!y||!w||!h)return;
  const p=chartPoint(e);const r=prev.getBoundingClientRect();
  let px=Math.round((p.x-r.left-chartDragOffX)/4)*4;
  let py=Math.round((p.y-r.top-chartDragOffY)/4)*4;
  const vw=parseInt(w.value||204),vh=parseInt(h.value||76);
  if(px<0)px=0;if(py<0)py=0;if(px+vw>240)px=240-vw;if(py+vh>240)py=240-vh;
  x.value=px;y.value=py;
  updateChartDragBox();
  scheduleDisplayAutoSave();
  if(e.preventDefault)e.preventDefault();
}

function chartDragEnd(){chartDragActive=false;}

document.addEventListener('DOMContentLoaded',()=>{
  const form=displayForm();
  if(form){
    form.querySelectorAll('input,select').forEach(el=>{
      el.addEventListener('input',()=>{updateChartDragBox();scheduleDisplayAutoSave();});
      el.addEventListener('change',()=>{updateChartDragBox();scheduleDisplayAutoSave();});
    });
  }
  const box=document.getElementById('chartDragBox');
  if(box){
    box.addEventListener('mousedown',chartDragStart);
    box.addEventListener('touchstart',chartDragStart,{passive:false});
    document.addEventListener('mousemove',chartDragMove);
    document.addEventListener('touchmove',chartDragMove,{passive:false});
    document.addEventListener('mouseup',chartDragEnd);
    document.addEventListener('touchend',chartDragEnd);
  }
  updateChartDragBox();
});
</script>
)JS";
  html += "</form></section>";
  html += "</div>";

  html += "<div id='tabLayout' style='display:none'>";
  html += R"JS(
<style>
.layout-js-wrap{display:grid;gap:18px}
.layout-actions{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}
.layout-box{border:0;background:var(--panel2);border-radius:16px;padding:16px}
.layout-box h3{margin:0 0 8px;font-size:15px;font-weight:600}.layout-box p{margin:0 0 12px;color:var(--muted);font-size:13px}.layout-box button{width:100%}
.palette{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:12px 0 16px;max-height:130px;overflow-y:auto;padding:12px;border:0;border-radius:14px;background:var(--panel2)}
.palette-hint{width:100%;color:var(--muted);font-size:12px;margin-bottom:2px}
.palette-group{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.palette-group-label{color:#64748b;font-size:10px;text-transform:uppercase;letter-spacing:.05em;margin-right:1px}
.palette-chip{background:rgba(96,165,250,.14);border:1px solid rgba(96,165,250,.35);color:var(--text);font-size:11px;font-weight:700;padding:6px 10px;border-radius:999px;cursor:grab;user-select:none;-webkit-user-select:none;touch-action:none}
.palette-chip:active{cursor:grabbing}
.palette-ghost{position:fixed;z-index:999999;pointer-events:none;background:linear-gradient(135deg,var(--accent2),#7c3aed);color:#fff;padding:7px 13px;border-radius:10px;font-size:12px;font-weight:700;box-shadow:0 10px 26px rgba(0,0,0,.45)}
.editor-shell{display:grid;grid-template-columns:190px 1fr 300px;gap:14px;align-items:start}
.layer-panel{border:0;background:var(--panel2);border-radius:16px;padding:12px}
.panel-label{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.05em;margin-bottom:8px}
.layer-list{display:grid;gap:4px}
.layer-row{display:grid;grid-template-columns:16px 18px 1fr 20px;gap:6px;align-items:center;padding:7px 6px;border-radius:10px;cursor:pointer;font-size:12px;border:1px solid transparent;transition:background .15s var(--ease),border-color .15s var(--ease)}
.layer-row:hover{background:var(--overlay-hover)}
.layer-row.selected{background:var(--accent-tint-bg);border-color:var(--accent-tint-border)}
.layer-handle{cursor:grab;color:#64748b;touch-action:none;text-align:center}
.layer-num{color:#64748b;font-weight:800;text-align:center}
.layer-label{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.layer-vis{appearance:none;background:transparent;border:0;box-shadow:none;color:var(--ok);width:auto;min-height:auto;padding:0;font-size:13px;cursor:pointer}
.layer-vis.off{color:#64748b}
.layer-drop-indicator{height:3px;background:var(--accent);border-radius:2px;margin:2px 4px}
.canvas-panel{display:flex;flex-direction:column;align-items:center}
.layout-preview{position:relative;width:240px;height:240px;background-color:#000;background-image:linear-gradient(rgba(255,255,255,.10) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.10) 1px,transparent 1px),linear-gradient(rgba(94,234,212,.18) 1px,transparent 1px),linear-gradient(90deg,rgba(94,234,212,.18) 1px,transparent 1px);background-size:12px 12px,12px 12px,40px 40px,40px 40px;border:2px solid #3b4664;margin:0 auto 8px;border-radius:50%;overflow:hidden;touch-action:none;box-shadow:inset 0 0 24px rgba(96,165,250,.16)}
.layout-preview:after{content:'240 x 240 rund';position:absolute;right:6px;bottom:4px;color:#64748b;font-size:10px;pointer-events:none}
.layout-item{position:absolute;color:#fff;border:1px dashed rgba(255,255,255,.42);padding:3px;cursor:grab;user-select:none;-webkit-user-select:none;white-space:nowrap;touch-action:none;background:rgba(96,165,250,.18);border-radius:5px;z-index:20;pointer-events:auto}
.layout-item:before{content:'';position:absolute;inset:-6px}
.layout-item.dragging{cursor:grabbing;background:rgba(94,234,212,.35);border-color:rgba(94,234,212,1);box-shadow:0 0 0 2px rgba(94,234,212,.35)}
.layout-item.selected{outline:2px solid var(--accent);outline-offset:1px;z-index:35}
.layout-item-num{position:absolute;left:-7px;top:-9px;background:#0b1224;border:1px solid rgba(255,255,255,.4);color:#9aa8c7;font-size:9px;font-weight:800;border-radius:999px;width:15px;height:15px;display:flex;align-items:center;justify-content:center;line-height:1;z-index:1}
.layout-item-text{pointer-events:none}
.resize-handle{position:absolute;right:-6px;bottom:-6px;width:13px;height:13px;background:var(--accent);border:2px solid #0b1224;border-radius:50%;cursor:nwse-resize;z-index:40;touch-action:none}
@keyframes locatePulse{0%{box-shadow:0 0 0 0 rgba(94,234,212,.85)}70%{box-shadow:0 0 0 16px rgba(94,234,212,0)}100%{box-shadow:0 0 0 0 rgba(94,234,212,0)}}
.layout-item.locate-pulse{animation:locatePulse 1.1s ease-out 2;z-index:30}
.props-panel{border:0;background:var(--panel2);border-radius:16px;padding:14px;position:sticky;top:92px;min-height:120px}
.props-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:12px}
.props-head strong{font-size:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.card-head-btns{display:flex;gap:4px;align-items:center;flex:none}
.icon-btn{appearance:none;background:transparent;border:1px solid var(--line);box-shadow:none;width:28px;height:28px;min-height:28px;padding:0;border-radius:8px;color:var(--muted);font-size:13px;display:inline-flex;align-items:center;justify-content:center;cursor:pointer;transition:background .15s var(--ease),color .15s var(--ease)}
.icon-btn:hover{background:var(--overlay-hover);color:var(--text)}
.layout-fields{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px}.layout-fields .wide{grid-column:1/3}.layout-fields label:not(.toggle){display:block;color:var(--muted);font-size:12px;margin-bottom:4px}
.layout-nudge{display:grid;grid-template-columns:repeat(3,42px);gap:6px;justify-content:center;margin-top:10px;padding:12px;border:0;border-radius:14px;background:var(--card)}
.layout-nudge button{padding:8px 0;border-radius:10px;box-shadow:none;background:var(--btn-muted);margin:0}.layout-nudge .blank{visibility:hidden}
.layout-floating{position:fixed;right:18px;bottom:18px;z-index:99999;background:var(--float-bg);border:1px solid var(--float-border);border-radius:18px;padding:10px;box-shadow:0 14px 40px var(--shadow-float);display:flex;gap:8px;align-items:center;backdrop-filter:blur(14px)}
@media(max-width:1080px){.editor-shell{grid-template-columns:170px 1fr 260px}}
@media(max-width:900px){.editor-shell{grid-template-columns:1fr}.layer-panel{order:2}.layer-list{display:flex;overflow-x:auto;gap:6px;-webkit-overflow-scrolling:touch}.layer-row{grid-template-columns:14px 16px auto 18px;flex:none;min-width:160px}.canvas-panel{order:1}.props-panel{order:3;position:relative;top:auto}}
@media(max-width:760px){.layout-floating{left:10px;right:10px;bottom:10px;justify-content:center}.layout-fields{grid-template-columns:1fr}.layout-fields .wide{grid-column:1}}
</style>
)JS";

  html += "<section class='card'><div class='panelTitle'><h2>Aktionen</h2><span id='saveState' class='badge warnb'>Bereit</span></div>";
  html += "<div class='layout-actions'>";
  html += "<div class='layout-box'><h3>Speichern</h3><p>Speichert per AJAX und aktualisiert die Displays direkt.</p><button class='btnSuccess' type='button' onclick='saveLayoutNow()'>Jetzt speichern</button></div>";
  html += "<div class='layout-box'><h3>Export</h3><p>Layout als JSON sichern.</p><form action='/exportlayout' method='get'><button class='btnMuted' type='submit'>Layout exportieren</button></form></div>";
  html += "<div class='layout-box dangerZone'><h3>Zuruecksetzen</h3><p>Standardlayout wiederherstellen.</p><form action='/resetlayout' method='post'><button class='danger' type='submit'>Layout zuruecksetzen</button></form></div>";
  html += "</div></section>";

  html += "<section class='card'><h2>Presets</h2><div class='presetGrid'>";
  html += "<form action='/presetlayout' method='post'><input type='hidden' name='preset' value='price'><button type='submit'>Preset: Preis gross</button></form>";
  html += "<form action='/presetlayout' method='post'><input type='hidden' name='preset' value='low2'><button type='submit'>Preset: Tiefpreis + 2. Tiefpreis</button></form>";
  html += "<form action='/presetlayout' method='post'><input type='hidden' name='preset' value='compact'><button type='submit'>Preset: Kompakt</button></form>";
  html += "</div></section>";

  html += "<section class='card'><h2>Layout importieren</h2><form action='/importlayout' method='post'>";
  html += "<textarea name='json' rows='5' placeholder='Exportiertes Layout-JSON hier einfuegen'></textarea>";
  html += "<div class='actions'><button type='submit'>Layout importieren</button></div></form></section>";

  html += "<form id='layoutForm' action='/savelayout' method='post'>";
  for (int d = 1; d <= 2; d++) {
    LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;
    for (int i = 0; i < LAYOUT_ITEMS; i++) {
      String id = "d" + String(d) + "e" + String(i);
      html += "<input type='hidden' id='" + id + "key' name='" + id + "key' value='" + htmlEscape(layout[i].key) + "'>";
      html += "<input type='hidden' id='" + id + "txt' name='" + id + "txt' value='" + htmlEscape(layout[i].customText) + "'>";
      html += "<input type='hidden' id='" + id + "pre' name='" + id + "pre' value='" + htmlEscape(layout[i].prefix) + "'>";
      html += "<input type='hidden' id='" + id + "suf' name='" + id + "suf' value='" + htmlEscape(layout[i].suffix) + "'>";
      html += "<input type='hidden' id='" + id + "x' name='" + id + "x' value='" + String(layout[i].x) + "'>";
      html += "<input type='hidden' id='" + id + "y' name='" + id + "y' value='" + String(layout[i].y) + "'>";
      html += "<input type='hidden' id='" + id + "s' name='" + id + "s' value='" + String(layout[i].size) + "'>";
      html += "<input type='hidden' id='" + id + "a' name='" + id + "a' value='" + String(layout[i].autoScale ? 1 : 0) + "'>";
      html += "<input type='hidden' id='" + id + "al' name='" + id + "al' value='" + String(layout[i].align) + "'>";
      html += "<input type='hidden' id='" + id + "v' name='" + id + "v' value='" + String(layout[i].visible ? 1 : 0) + "'>";
    }
  }
  html += "</form>";

  html += "<div id='layoutApp' class='layout-js-wrap'></div>";
  html += "<div class='layout-floating'><button class='btnSuccess' type='button' onclick='saveLayoutNow()'>Speichern</button><span id='floatSaveState' class='badge warnb'>Bereit</span></div>";
  html += "<script src='/layout-editor.js'></script>";


  html += "</div>";

  html += R"JS(
<script>
function switchDpTab(name){
  var isOverlays = (name==='overlays');
  var ov=document.getElementById('tabOverlays');
  var la=document.getElementById('tabLayout');
  if(ov)ov.style.display=isOverlays?'':'none';
  if(la)la.style.display=isOverlays?'none':'';
  var b1=document.getElementById('tabBtn-overlays');
  var b2=document.getElementById('tabBtn-layout');
  if(b1)b1.className='dpTab'+(isOverlays?' active':'');
  if(b2)b2.className='dpTab'+(isOverlays?'':' active');
  try{localStorage.setItem('dpTab',name);}catch(e){}
  if(!isOverlays){
    if(typeof initLayoutApp==='function'){initLayoutApp();}
    else{
      var app=document.getElementById('layoutApp');
      if(app&&!app.dataset.err){app.dataset.err='1';app.innerHTML='<div style="padding:16px;background:var(--errb-bg);color:var(--errb-text);border-radius:12px;font-size:13px"><b>initLayoutApp fehlt!</b> Das Layout-Editor-Script wurde nicht ausgefuehrt - vermutlich Syntax-Fehler oder Browser-Cache. Bitte hart neu laden (Strg+Shift+R).</div>';}
    }
  }
}
(function(){
  var t='overlays';
  try{t=localStorage.getItem('dpTab')||'overlays';}catch(e){}
  switchDpTab(t);
})();
</script>
)JS";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void handleSaveDisplays() {
  if (!checkAuth()) return;

  if (server.hasArg("d1Enabled")) {
    display1Enabled = server.arg("d1Enabled") == "1";
    prefs.putBool("d1Enabled", display1Enabled);
  }

  if (server.hasArg("d2Enabled")) {
    display2Enabled = server.arg("d2Enabled") == "1";
    prefs.putBool("d2Enabled", display2Enabled);
  }

  if (server.hasArg("d1Mode")) {
    display1Mode = server.arg("d1Mode").toInt();
    if (display1Mode < 0) display1Mode = 0;
    if (display1Mode > 4) display1Mode = 4;
    prefs.putInt("d1Mode", display1Mode);
  }

  if (server.hasArg("d2Mode")) {
    display2Mode = server.arg("d2Mode").toInt();
    if (display2Mode < 0) display2Mode = 0;
    if (display2Mode > 4) display2Mode = 4;
    prefs.putInt("d2Mode", display2Mode);
  }

  if (server.hasArg("priceBarOn")) {
    displayPriceBarEnabled = server.arg("priceBarOn") == "1";
    prefs.putBool("priceBarOn", displayPriceBarEnabled);
  }

  if (server.hasArg("priceBarMin")) {
    displayPriceBarMinCent = server.arg("priceBarMin").toInt();
    if (displayPriceBarMinCent < 0) displayPriceBarMinCent = 0;
    if (displayPriceBarMinCent > 200) displayPriceBarMinCent = 200;
    prefs.putInt("priceBarMin", displayPriceBarMinCent);
  }

  if (server.hasArg("priceBarMax")) {
    displayPriceBarMaxCent = server.arg("priceBarMax").toInt();
    if (displayPriceBarMaxCent <= displayPriceBarMinCent) displayPriceBarMaxCent = displayPriceBarMinCent + 10;
    if (displayPriceBarMaxCent > 300) displayPriceBarMaxCent = 300;
    prefs.putInt("priceBarMax", displayPriceBarMaxCent);
  }

  if (server.hasArg("priceBarW")) {
    displayPriceBarWidth = server.arg("priceBarW").toInt();
    if (displayPriceBarWidth < 4) displayPriceBarWidth = 4;
    if (displayPriceBarWidth > 24) displayPriceBarWidth = 24;
    prefs.putInt("priceBarW", displayPriceBarWidth);
  }

  if (server.hasArg("priceBarText")) {
    displayPriceBarTextEnabled = server.arg("priceBarText") == "1";
    prefs.putBool("priceBarText", displayPriceBarTextEnabled);
  }

  if (server.hasArg("d1DayChart")) {
    display1DayChartEnabled = server.arg("d1DayChart") == "1";
    prefs.putBool("d1DayChart", display1DayChartEnabled);
  }

  if (server.hasArg("d2DayChart")) {
    display2DayChartEnabled = server.arg("d2DayChart") == "1";
    prefs.putBool("d2DayChart", display2DayChartEnabled);
  }

  if (server.hasArg("dayChartX")) {
    displayDayChartX = server.arg("dayChartX").toInt();
    if (displayDayChartX < 0) displayDayChartX = 0;
    if (displayDayChartX > SCREEN_WIDTH - 40) displayDayChartX = SCREEN_WIDTH - 40;
    prefs.putInt("dayChartX", displayDayChartX);
  }

  if (server.hasArg("dayChartY")) {
    displayDayChartY = server.arg("dayChartY").toInt();
    if (displayDayChartY < 0) displayDayChartY = 0;
    if (displayDayChartY > SCREEN_HEIGHT - 30) displayDayChartY = SCREEN_HEIGHT - 30;
    prefs.putInt("dayChartY", displayDayChartY);
  }

  if (server.hasArg("dayChartW")) {
    displayDayChartWidth = server.arg("dayChartW").toInt();
    if (displayDayChartWidth < 60) displayDayChartWidth = 60;
    if (displayDayChartWidth > SCREEN_WIDTH) displayDayChartWidth = SCREEN_WIDTH;
    prefs.putInt("dayChartW", displayDayChartWidth);
  }

  if (server.hasArg("dayChartH")) {
    displayDayChartHeight = server.arg("dayChartH").toInt();
    if (displayDayChartHeight < 40) displayDayChartHeight = 40;
    if (displayDayChartHeight > 150) displayDayChartHeight = 150;
    prefs.putInt("dayChartH", displayDayChartHeight);
  }

  if (displayDayChartX + displayDayChartWidth > SCREEN_WIDTH) {
    displayDayChartX = SCREEN_WIDTH - displayDayChartWidth;
    prefs.putInt("dayChartX", displayDayChartX);
  }
  if (displayDayChartY + displayDayChartHeight > SCREEN_HEIGHT) {
    displayDayChartY = SCREEN_HEIGHT - displayDayChartHeight;
    prefs.putInt("dayChartY", displayDayChartY);
  }

  if (server.hasArg("d2ClockRing")) {
    display2CheapClockRingEnabled = server.arg("d2ClockRing") == "1";
    prefs.putBool("d2ClockRing", display2CheapClockRingEnabled);
  }

  if (server.hasArg("d2ClockRingW")) {
    display2CheapClockRingWidth = server.arg("d2ClockRingW").toInt();
    if (display2CheapClockRingWidth < 6) display2CheapClockRingWidth = 6;
    if (display2CheapClockRingWidth > 28) display2CheapClockRingWidth = 28;
    prefs.putInt("d2ClockRingW", display2CheapClockRingWidth);
  }

  if (server.hasArg("d2ClockText")) {
    display2CheapClockRingTextEnabled = server.arg("d2ClockText") == "1";
    prefs.putBool("d2ClockText", display2CheapClockRingTextEnabled);
  }

  if (server.hasArg("d2ClockLabels")) {
    display2CheapClockRingLabelsEnabled = server.arg("d2ClockLabels") == "1";
    prefs.putBool("d2ClockLabels", display2CheapClockRingLabelsEnabled);
  }

  if (server.hasArg("d2RingCheap")) {
    display2RingCheapColorId = server.arg("d2RingCheap").toInt();
    if (display2RingCheapColorId < 0) display2RingCheapColorId = 0;
    if (display2RingCheapColorId > 10) display2RingCheapColorId = 10;
    prefs.putInt("d2RingCheap", display2RingCheapColorId);
  }

  if (server.hasArg("d2RingMid")) {
    display2RingMidColorId = server.arg("d2RingMid").toInt();
    if (display2RingMidColorId < 0) display2RingMidColorId = 0;
    if (display2RingMidColorId > 10) display2RingMidColorId = 10;
    prefs.putInt("d2RingMid", display2RingMidColorId);
  }

  if (server.hasArg("d2RingHigh")) {
    display2RingHighColorId = server.arg("d2RingHigh").toInt();
    if (display2RingHighColorId < 0) display2RingHighColorId = 0;
    if (display2RingHighColorId > 10) display2RingHighColorId = 10;
    prefs.putInt("d2RingHigh", display2RingHighColorId);
  }

  if (server.hasArg("d2RingBest")) {
    display2RingBestColorId = server.arg("d2RingBest").toInt();
    if (display2RingBestColorId < 0) display2RingBestColorId = 0;
    if (display2RingBestColorId > 10) display2RingBestColorId = 10;
    prefs.putInt("d2RingBest", display2RingBestColorId);
  }

  if (server.hasArg("d2RingNow")) {
    display2RingCurrentColorId = server.arg("d2RingNow").toInt();
    if (display2RingCurrentColorId < 0) display2RingCurrentColorId = 0;
    if (display2RingCurrentColorId > 10) display2RingCurrentColorId = 10;
    prefs.putInt("d2RingNow", display2RingCurrentColorId);
  }

  showLayoutDisplays();
  server.sendHeader("Location", "/displays?saved=1");
  server.send(303);
}

void addColorSelect(String &html, const char* name, const char* label, int selected) {
  html += "<div class='field'><label>";
  html += label;
  html += "</label><select name='";
  html += name;
  html += "'>";

  const char* names[] = {"Gruen", "Gelb", "Rot", "Blau", "Weiss", "Cyan", "Violett", "Orange", "Pink", "Grau", "Aus"};

  for (int i = 0; i <= 10; i++) {
    html += "<option value='";
    html += String(i);
    html += "'";
    if (selected == i) html += " selected";
    html += ">";
    html += names[i];
    html += "</option>";
  }

  html += "</select></div>";
}

String gpioOwnerLabel(int gpio) {
  if (gpio == tftSclkPin) return "SPI-Takt (SCLK)";
  if (gpio == tftMosiPin) return "SPI-Daten (MOSI)";
  if (gpio == TFT_DC_PIN) return "Display Data/Command";
  if (gpio == TFT_RST_PIN) return "Display Reset";
  if (gpio == TFT1_CS_PIN) return "Display 1 Chip-Select";
  if (gpio == TFT2_CS_PIN) return "Display 2 Chip-Select";
  if (gpio == ledRingPinVar) return "Tagesring";
  if (gpio == matrixCsPinVar) return "LED-Matrix Chip-Select";
  return "";
}

void addGpioSelect(String &html, String fieldName, String label, String hint, int currentValue) {
  html += "<div class='field'><label title='" + hint + "'>" + label + "</label><select name='" + fieldName + "' title='" + hint + "'>";

  for (int g = 0; g <= 27; g++) {
    html += "<option value='" + String(g) + "'";
    if (g == currentValue) html += " selected";
    html += ">GPIO" + String(g);

    if (g == 0) {
      html += " - meist Boot-Strapping-Pin, eher vermeiden";
    } else {
      String owner = gpioOwnerLabel(g);
      if (owner.length() > 0 && g != currentValue) {
        html += " - aktuell belegt: " + owner;
      }
    }

    html += "</option>";
  }

  html += "</select></div>";
}

void handleSaveDisplayChartAjax() {
  if (!checkAuth()) return;

  if (server.hasArg("d1DayChart")) {
    display1DayChartEnabled = server.arg("d1DayChart") == "1";
    prefs.putBool("d1DayChart", display1DayChartEnabled);
  }

  if (server.hasArg("d2DayChart")) {
    display2DayChartEnabled = server.arg("d2DayChart") == "1";
    prefs.putBool("d2DayChart", display2DayChartEnabled);
  }

  if (server.hasArg("dayChartX")) {
    displayDayChartX = server.arg("dayChartX").toInt();
    if (displayDayChartX < 0) displayDayChartX = 0;
    if (displayDayChartX > SCREEN_WIDTH - 40) displayDayChartX = SCREEN_WIDTH - 40;
  }

  if (server.hasArg("dayChartY")) {
    displayDayChartY = server.arg("dayChartY").toInt();
    if (displayDayChartY < 0) displayDayChartY = 0;
    if (displayDayChartY > SCREEN_HEIGHT - 30) displayDayChartY = SCREEN_HEIGHT - 30;
  }

  if (server.hasArg("dayChartW")) {
    displayDayChartWidth = server.arg("dayChartW").toInt();
    if (displayDayChartWidth < 60) displayDayChartWidth = 60;
    if (displayDayChartWidth > SCREEN_WIDTH) displayDayChartWidth = SCREEN_WIDTH;
  }

  if (server.hasArg("dayChartH")) {
    displayDayChartHeight = server.arg("dayChartH").toInt();
    if (displayDayChartHeight < 40) displayDayChartHeight = 40;
    if (displayDayChartHeight > 150) displayDayChartHeight = 150;
  }

  if (displayDayChartX + displayDayChartWidth > SCREEN_WIDTH) {
    displayDayChartX = SCREEN_WIDTH - displayDayChartWidth;
  }
  if (displayDayChartY + displayDayChartHeight > SCREEN_HEIGHT) {
    displayDayChartY = SCREEN_HEIGHT - displayDayChartHeight;
  }

  prefs.putInt("dayChartX", displayDayChartX);
  prefs.putInt("dayChartY", displayDayChartY);
  prefs.putInt("dayChartW", displayDayChartWidth);
  prefs.putInt("dayChartH", displayDayChartHeight);

  showLayoutDisplays();

  String response = "OK";
  response += " x=" + String(displayDayChartX);
  response += " y=" + String(displayDayChartY);
  response += " w=" + String(displayDayChartWidth);
  response += " h=" + String(displayDayChartHeight);
  server.send(200, "text/plain", response);
}

void handleLedRingPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(12000);

  html += htmlHeader("WS2812B/WS2818 Tagesring");
  html += "<section class='hero'><h1>WS2812B/WS2818 Tagesring</h1><p>60 LEDs zeigen den heutigen Strompreisverlauf als Tageskreis.</p></section>";
  html += navTabs("/ring");

  html += "<section class='card'><h2>Status</h2>";
  html += "<div class='gridCards'>";
  html += "<div class='metric'><div class='label'>Tagesring</div><div class='value'>";
  html += ledRingEnabled ? "Ein" : "Aus";
  html += "</div><div class='sub'>Pin GPIO";
  html += String(LED_RING_PIN);
  html += ", ";
  html += String(ledActiveCount);
  html += " aktiv / ";
  html += String(LED_RING_COUNT);
  html += " physisch</div></div>";
  html += "<div class='metric'><div class='label'>Helligkeit</div><div class='value'>";
  html += String(ledBrightness);
  html += "</div><div class='sub'>0 bis 255</div></div>";
  html += "<div class='metric'><div class='label'>Schwellen</div><div class='value' style='font-size:26px'>";
  html += String(ledYellowCent);
  html += "/";
  html += String(ledRedCent);
  html += " ct</div><div class='sub'>Gelb / Rot</div></div>";
  html += "</div></section>";

  html += "<section class='card'><div class='panelTitle'><h2>Einstellungen</h2><span id='ringSaveState' class='badge warnb'>Bereit</span></div>";
  html += "<form id='ringForm' action='/savering' method='post'>";

  html += "<h3>Betrieb</h3><div class='formGrid'>";
  html += "<div class='field'><label>Tagesring aktiv</label><select name='ledOn'>";
  html += "<option value='1'";
  if (ledRingEnabled) html += " selected";
  html += ">Ein</option><option value='0'";
  if (!ledRingEnabled) html += " selected";
  html += ">Aus</option></select></div>";

  html += "<div class='field'><label>Helligkeit (0-255)</label><input name='ledBright' type='number' min='0' max='255' value='";
  html += String(ledBrightness);
  html += "'></div>";

  html += "<div class='field'><label>Aktualisierung alle Sekunden</label><input name='ledRefresh' type='number' min='1' max='300' value='";
  html += String(ledRefreshSeconds);
  html += "'></div>";

  html += "<div class='field'><label>Anzahl aktiver LEDs</label><select name='ledCount'>";
  html += "<option value='60'";
  if (ledActiveCount == 60) html += " selected";
  html += ">60 LEDs Tagesring</option>";
  html += "<option value='24'";
  if (ledActiveCount == 24) html += " selected";
  html += ">24 LEDs Tagesring</option>";
  html += "</select></div>";
  html += "</div>";

  html += "<h3 style='margin-top:18px'>Preis-Schwellen</h3><p class='small'>Ab diesen Preisen wechselt die LED-Farbe von günstig auf mittel bzw. von mittel auf teuer.</p><div class='formGrid'>";
  html += "<div class='field'><label>Gelb ab (ct/kWh)</label><input name='ledYellow' type='number' min='0' max='200' value='";
  html += String(ledYellowCent);
  html += "'></div>";

  html += "<div class='field'><label>Rot ab (ct/kWh)</label><input name='ledRed' type='number' min='0' max='200' value='";
  html += String(ledRedCent);
  html += "'></div>";
  html += "</div>";

  html += "<h3 style='margin-top:18px'>Farben</h3><p class='small'>Günstig/Mittel/Teuer sind die drei Preis-Schwellen von oben. Aktueller Zeitraum und günstigster Block überschreiben diese Preisfarbe zusätzlich an der jeweiligen LED.</p><div class='formGrid'>";
  addColorSelect(html, "ledCheapColor", "Farbe günstig", ledCheapColorId);
  addColorSelect(html, "ledMidColor", "Farbe mittel", ledMidColorId);
  addColorSelect(html, "ledHighColor", "Farbe teuer", ledHighColorId);
  addColorSelect(html, "ledCurrentColor", "Farbe aktueller Zeitraum (Überschreibung)", ledCurrentColorId);
  addColorSelect(html, "ledLowColor", "Farbe günstigster Block (Überschreibung)", ledLowBlockColorId);
  html += "</div>";

  html += "<div class='actions'><button type='submit'>Tagesring speichern</button><button type='button' class='secondary' onclick='saveRingNow()'>Direkt speichern</button></div></form></section>";

  html += R"JS(
<script>
let ringSaveTimer=null;
function ringState(t,c){const s=document.getElementById('ringSaveState');if(s){s.className='badge '+c;s.innerText=t;}}
function saveRingNow(){
  const form=document.getElementById('ringForm');
  if(!form){ringState('Kein Formular','errb');return;}
  const body=new URLSearchParams(new FormData(form)).toString();
  ringState('Speichere...','warnb');
  fetch('/saveringajax',{method:'POST',cache:'no-store',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:body})
    .then(r=>r.text())
    .then(t=>{if(t.indexOf('OK')>=0){ringState('Gespeichert','okb');}else{ringState('Fehler','errb');}})
    .catch(()=>{ringState('Fehler','errb');});
}
function scheduleRingSave(){clearTimeout(ringSaveTimer);ringState('Änderungen...','warnb');ringSaveTimer=setTimeout(saveRingNow,400);}
document.addEventListener('DOMContentLoaded',()=>{
  const form=document.getElementById('ringForm');
  if(!form)return;
  form.querySelectorAll('input,select').forEach(el=>{
    el.addEventListener('input',scheduleRingSave);
    el.addEventListener('change',saveRingNow);
  });
  form.addEventListener('submit',()=>{ringState('Speichere...','warnb');});
});
</script>
)JS";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void saveLedRingFromRequest() {
  if (server.hasArg("ledOn")) {
    ledRingEnabled = server.arg("ledOn") == "1";
    prefs.putBool("ledOn", ledRingEnabled);
  }

  if (server.hasArg("ledBright")) {
    ledBrightness = server.arg("ledBright").toInt();
    if (ledBrightness < 0) ledBrightness = 0;
    if (ledBrightness > 255) ledBrightness = 255;
    prefs.putInt("ledBright", ledBrightness);
    ledRing.setBrightness(ledBrightness);
  }

  if (server.hasArg("ledRefresh")) {
    ledRefreshSeconds = server.arg("ledRefresh").toInt();
    if (ledRefreshSeconds < 1) ledRefreshSeconds = 1;
    if (ledRefreshSeconds > 300) ledRefreshSeconds = 300;
    prefs.putInt("ledRefresh", ledRefreshSeconds);
    ledRefreshInterval = (unsigned long)ledRefreshSeconds * 1000UL;
  }

  if (server.hasArg("ledCount")) {
    ledActiveCount = server.arg("ledCount").toInt();
    if (ledActiveCount != 24 && ledActiveCount != 60) ledActiveCount = LED_RING_COUNT;
    if (ledActiveCount > LED_RING_COUNT) ledActiveCount = LED_RING_COUNT;
    prefs.putInt("ledCount", ledActiveCount);
  }

  if (server.hasArg("ledYellow")) {
    ledYellowCent = server.arg("ledYellow").toInt();
    if (ledYellowCent < 0) ledYellowCent = 0;
    if (ledYellowCent > 200) ledYellowCent = 200;
    prefs.putInt("ledYellow", ledYellowCent);
  }

  if (server.hasArg("ledRed")) {
    ledRedCent = server.arg("ledRed").toInt();
    if (ledRedCent < 0) ledRedCent = 0;
    if (ledRedCent > 200) ledRedCent = 200;
  }

  if (ledRedCent < ledYellowCent) ledRedCent = ledYellowCent + 1;
  if (ledRedCent > 200) ledRedCent = 200;
  prefs.putInt("ledRed", ledRedCent);

  if (server.hasArg("ledCheapColor")) {
    ledCheapColorId = server.arg("ledCheapColor").toInt();
    if (ledCheapColorId < 0) ledCheapColorId = 0;
    if (ledCheapColorId > 10) ledCheapColorId = 10;
    prefs.putInt("ledCheapCol", ledCheapColorId);
  }

  if (server.hasArg("ledMidColor")) {
    ledMidColorId = server.arg("ledMidColor").toInt();
    if (ledMidColorId < 0) ledMidColorId = 0;
    if (ledMidColorId > 10) ledMidColorId = 10;
    prefs.putInt("ledMidCol", ledMidColorId);
  }

  if (server.hasArg("ledHighColor")) {
    ledHighColorId = server.arg("ledHighColor").toInt();
    if (ledHighColorId < 0) ledHighColorId = 0;
    if (ledHighColorId > 10) ledHighColorId = 10;
    prefs.putInt("ledHighCol", ledHighColorId);
  }

  if (server.hasArg("ledCurrentColor")) {
    ledCurrentColorId = server.arg("ledCurrentColor").toInt();
    if (ledCurrentColorId < 0) ledCurrentColorId = 0;
    if (ledCurrentColorId > 10) ledCurrentColorId = 10;
    prefs.putInt("ledCurCol", ledCurrentColorId);
  }

  if (server.hasArg("ledLowColor")) {
    ledLowBlockColorId = server.arg("ledLowColor").toInt();
    if (ledLowBlockColorId < 0) ledLowBlockColorId = 0;
    if (ledLowBlockColorId > 10) ledLowBlockColorId = 10;
    prefs.putInt("ledLowCol", ledLowBlockColorId);
  }

  if (!ledRingEnabled) {
    clearLedRing();
  } else {
    updateLedRing();
  }
}

void handleSaveLedRing() {
  if (!checkAuth()) return;

  saveLedRingFromRequest();

  server.sendHeader("Location", "/ring?saved=1");
  server.send(303);
}

void handleSaveLedRingAjax() {
  if (!checkAuth()) return;

  if (server.args() <= 0) {
    server.send(400, "text/plain", "NO_ARGS");
    return;
  }

  saveLedRingFromRequest();

  String response = "OK ledOn=";
  response += ledRingEnabled ? "1" : "0";
  response += " bright=";
  response += String(ledBrightness);
  server.send(200, "text/plain", response);
}

void handleMatrixPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(16000);

  html += htmlHeader("Matrix");
  html += "<section class='hero'><h1>8x8 Matrix</h1><p>Bis zu 4 MAX7219 LED-Matrixmodule in Daisy-Chain, jedes einzeln konfigurierbar.</p></section>";
  html += navTabs("/matrix");

  html += "<section class='card'><h2>Status</h2><div class='gridCards'>";
  html += "<div class='metric'><div class='label'>Daisy-Chain</div><div class='value'>" + String(MATRIX_DEVICE_COUNT) + " Module</div><div class='sub'>DIN GPIO" + String(MATRIX_DIN_PIN) + ", CLK GPIO" + String(MATRIX_CLK_PIN) + ", CS GPIO" + String(MATRIX_CS_PIN) + "</div></div>";
  html += "<div class='metric'><div class='label'>Refresh</div><div class='value'>" + String(matrixRefreshSeconds) + " s</div><div class='sub'>gemeinsam fuer alle Matrixmodule</div></div>";
  html += "</div></section>";

  html += "<section class='card'><div class='panelTitle'><h2>Einstellungen</h2><span id='matrixSaveState' class='badge warnb'>Bereit</span></div>";
  html += "<form id='matrixForm' action='/savematrix' method='post'>";

  html += "<div class='formGrid'>";
  html += "<div class='field'><label>Refresh alle Sekunden</label><input name='matrixRefresh' type='number' min='1' max='300' value='" + String(matrixRefreshSeconds) + "'></div>";
  html += "</div>";

  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    html += "<section class='formSection'><h3>Matrix " + String(module + 1) + "</h3><div class='formGrid'>";

    html += "<div class='field'><label>Aktiv</label><select name='matrixOn" + String(module) + "'>";
    html += "<option value='1'";
    if (matrixEnabled[module]) html += " selected";
    html += ">Ein</option><option value='0'";
    if (!matrixEnabled[module]) html += " selected";
    html += ">Aus</option></select></div>";

    html += "<div class='field'><label>Angezeigter Wert</label><select name='matrixMode" + String(module) + "'>";
    for (int m = 0; m <= 8; m++) {
      html += "<option value='" + String(m) + "'";
      if (matrixMode[module] == m) html += " selected";
      html += ">" + htmlEscape(matrixModeLabel(m)) + "</option>";
    }
    html += "</select></div>";

    html += "<div class='field'><label>Helligkeit (0-15)</label><input name='matrixBright" + String(module) + "' type='number' min='0' max='15' value='" + String(matrixBrightness[module]) + "'></div>";
    html += "<div class='field'><label>Live-Vorschau (nicht editierbar)</label><input readonly value='" + htmlEscape(matrixValuePreviewForModule(module)) + "'></div>";

    html += "</div></section>";
  }

  html += "<p class='small'>Verdrahtung: Matrix 1 DIN an ESP32, danach Matrix 1 DOUT an Matrix 2 DIN usw. CLK und CS/LOAD parallel durchverbinden. Preise werden auf 2 Ziffern begrenzt.</p>";
  html += "<div class='actions'><button type='submit'>Matrix speichern</button><button type='button' class='secondary' onclick='saveMatrixNow()'>Direkt speichern</button></div></form></section>";

  html += R"JS(
<script>
let matrixSaveTimer = null;
let matrixSaveBusy = false;
let matrixSaveAgain = false;

function matrixState(text, cls) {
  const el = document.getElementById('matrixSaveState');
  if (!el) return;
  el.className = 'badge ' + cls;
  el.innerText = text;
}

function saveMatrixNow() {
  const form = document.getElementById('matrixForm');
  if (!form) { matrixState('Kein Formular', 'errb'); return; }
  if (matrixSaveBusy) { matrixSaveAgain = true; return; }
  matrixSaveBusy = true;
  matrixState('Speichere...', 'warnb');

  fetch('/savematrixajax', {
    method: 'POST', cache: 'no-store', credentials: 'same-origin',
    headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
    body: new URLSearchParams(new FormData(form)).toString()
  })
  .then(r => r.text())
  .then(t => { if (t.indexOf('OK') >= 0) matrixState('Gespeichert', 'okb'); else matrixState('Fehler', 'errb'); })
  .catch(() => matrixState('Fehler', 'errb'))
  .finally(() => {
    matrixSaveBusy = false;
    if (matrixSaveAgain) { matrixSaveAgain = false; setTimeout(saveMatrixNow, 120); }
  });
}

function scheduleMatrixSave() {
  clearTimeout(matrixSaveTimer);
  matrixState('Änderungen...', 'warnb');
  matrixSaveTimer = setTimeout(saveMatrixNow, 400);
}

document.addEventListener('DOMContentLoaded', () => {
  const form = document.getElementById('matrixForm');
  if (!form) return;
  form.querySelectorAll('input,select').forEach(el => {
    el.addEventListener('input', scheduleMatrixSave);
    el.addEventListener('change', scheduleMatrixSave);
  });
  form.addEventListener('submit', () => { matrixState('Speichere...', 'warnb'); });
});
</script>
)JS";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void saveMatrixFromRequest() {
  for (int module = 0; module < MATRIX_DEVICE_COUNT; module++) {
    String onName = "matrixOn" + String(module);
    String modeName = "matrixMode" + String(module);
    String brightName = "matrixBright" + String(module);

    if (server.hasArg(onName)) {
      matrixEnabled[module] = server.arg(onName) == "1";
      prefs.putBool(onName.c_str(), matrixEnabled[module]);
    }

    if (server.hasArg(modeName)) {
      matrixMode[module] = server.arg(modeName).toInt();
      if (matrixMode[module] < 0) matrixMode[module] = 0;
      if (matrixMode[module] > 8) matrixMode[module] = 8;
      prefs.putInt(modeName.c_str(), matrixMode[module]);
    }

    if (server.hasArg(brightName)) {
      matrixBrightness[module] = server.arg(brightName).toInt();
      if (matrixBrightness[module] < 0) matrixBrightness[module] = 0;
      if (matrixBrightness[module] > 15) matrixBrightness[module] = 15;
      prefs.putInt(brightName.c_str(), matrixBrightness[module]);
      max7219WriteDevice(module, 0x0A, matrixBrightness[module] & 0x0F);
    }
  }

  if (server.hasArg("matrixRefresh")) {
    matrixRefreshSeconds = server.arg("matrixRefresh").toInt();
    if (matrixRefreshSeconds < 1) matrixRefreshSeconds = 1;
    if (matrixRefreshSeconds > 300) matrixRefreshSeconds = 300;
    prefs.putInt("matrixRefresh", matrixRefreshSeconds);
    matrixRefreshInterval = (unsigned long)matrixRefreshSeconds * 1000UL;
  }

  updatePriceMatrix();
}

void handleSaveMatrix() {
  if (!checkAuth()) return;

  saveMatrixFromRequest();

  server.sendHeader("Location", "/matrix?saved=1");
  server.send(303);
}

void handleSaveMatrixAjax() {
  if (!checkAuth()) return;

  if (server.args() <= 0) {
    server.send(400, "text/plain", "NO_ARGS");
    return;
  }

  saveMatrixFromRequest();

  String response = "OK refresh=" + String(matrixRefreshSeconds);
  server.send(200, "text/plain", response);
}

// -----------------------------------------------------------------------------
// Layout Editor
// -----------------------------------------------------------------------------


void saveLayoutFromRequest() {
  for (int d = 1; d <= 2; d++) {
    LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;

    for (int i = 0; i < LAYOUT_ITEMS; i++) {
      String id = "d" + String(d) + "e" + String(i);

      if (server.hasArg(id + "key")) layout[i].key = server.arg(id + "key");
      if (server.hasArg(id + "txt")) layout[i].customText = server.arg(id + "txt");
      if (server.hasArg(id + "pre")) layout[i].prefix = server.arg(id + "pre");
      if (server.hasArg(id + "suf")) layout[i].suffix = server.arg(id + "suf");

      if (server.hasArg(id + "x")) {
        layout[i].x = server.arg(id + "x").toInt();
      }

      if (server.hasArg(id + "y")) {
        layout[i].y = server.arg(id + "y").toInt();
      }

      if (server.hasArg(id + "s")) layout[i].size = server.arg(id + "s").toInt();
      if (server.hasArg(id + "v")) layout[i].visible = server.arg(id + "v") == "1";
      if (server.hasArg(id + "a")) layout[i].autoScale = server.arg(id + "a") == "1";
      if (server.hasArg(id + "al")) layout[i].align = server.arg(id + "al").toInt();

      if (layout[i].x < 0) layout[i].x = 0;
      if (layout[i].x > SCREEN_WIDTH - 1) layout[i].x = SCREEN_WIDTH - 1;
      if (layout[i].y < 0) layout[i].y = 0;
      if (layout[i].y > SCREEN_HEIGHT - 1) layout[i].y = SCREEN_HEIGHT - 1;
      if (layout[i].size < 1) layout[i].size = 1;
      if (layout[i].size > 4) layout[i].size = 4;
      if (layout[i].align < 0) layout[i].align = 0;
      if (layout[i].align > 2) layout[i].align = 2;
      if (layout[i].customText.length() > 32) layout[i].customText = layout[i].customText.substring(0, 32);
      if (layout[i].prefix.length() > 12) layout[i].prefix = layout[i].prefix.substring(0, 12);
      if (layout[i].suffix.length() > 12) layout[i].suffix = layout[i].suffix.substring(0, 12);

      saveLayoutItem(d, i, layout[i]);
    }
  }
}

void handleSaveLayout() {
  if (!checkAuth()) return;

  saveLayoutFromRequest();
  showLayoutDisplays();

  server.sendHeader("Location", "/layout?saved=1");
  server.send(303);
}

void handleSaveLayoutAjax() {
  if (!checkAuth()) return;

  int argCount = server.args();

  if (argCount <= 0) {
    server.send(400, "text/plain", "NO_ARGS");
    return;
  }

  saveLayoutFromRequest();

  lastDisplayRefresh = millis();
  showLayoutDisplays();

  String response = "OK args=" + String(argCount);
  server.send(200, "text/plain", response);
}

void handleResetLayout() {
  if (!checkAuth()) return;

  for (int d = 1; d <= 2; d++) {
    for (int i = 0; i < LAYOUT_ITEMS; i++) {
      String prefix = "d" + String(d) + "e" + String(i);

      prefs.remove((prefix + "key").c_str());
      prefs.remove((prefix + "txt").c_str());
      prefs.remove((prefix + "pre").c_str());
      prefs.remove((prefix + "suf").c_str());
      prefs.remove((prefix + "x").c_str());
      prefs.remove((prefix + "y").c_str());
      prefs.remove((prefix + "s").c_str());
      prefs.remove((prefix + "v").c_str());
      prefs.remove((prefix + "a").c_str());
      prefs.remove((prefix + "al").c_str());
    }
  }

  loadLayoutDefaults();
  showLayoutDisplays();

  server.sendHeader("Location", "/layout?saved=1");
  server.send(303);
}

void handlePresetLayout() {
  if (!checkAuth()) return;

  String preset = server.hasArg("preset") ? server.arg("preset") : "";
  applyLayoutPreset(preset);

  for (int d = 1; d <= 2; d++) {
    LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;
    for (int i = 0; i < LAYOUT_ITEMS; i++) {
      saveLayoutItem(d, i, layout[i]);
    }
  }

  showLayoutDisplays();
  server.sendHeader("Location", "/layout?saved=1");
  server.send(303);
}

void applyLayoutPreset(String preset) {
  loadLayoutDefaults();

  if (preset == "price") {
    d1Layout[0] = {"customText", "Aktueller Preis", "", "", 120, 36, 2, true, true, 1};
    d1Layout[1] = {"current15", "", "", "", 120, 86, 4, true, true, 1};
    d1Layout[2] = {"customText", "ct/kWh", "", "", 120, 138, 2, true, true, 1};
    d1Layout[3] = {"ip", "", "", "", 120, 182, 1, true, true, 1};
    d1Layout[4] = {"time", "", "", "", 120, 202, 2, true, true, 1};
  }

  if (preset == "low2") {
    d2Layout[0] = {"customText", "Beste Zeiten", "", "", 120, 34, 2, true, true, 1};
    d2Layout[1] = {"low60DayFullRange", "", "1. ", "", 120, 86, 1, true, true, 1};
    d2Layout[2] = {"secondLow60DayFullRange", "", "2. ", "", 120, 114, 1, true, true, 1};
    d2Layout[3] = {"dayAvg", "", "AVG Tag ", "", 120, 146, 1, true, true, 1};
    d2Layout[4] = {"low15DayFull", "", "15m ", "", 120, 174, 1, false, true, 1};
  }

  if (preset == "compact") {
    d1Layout[0] = {"current15", "", "Jetzt ", "", 120, 52, 3, true, true, 1};
    d1Layout[1] = {"current60", "", "AVG60 ", "", 120, 104, 1, true, true, 1};
    d1Layout[2] = {"dayAvg", "", "AVG Tag ", "", 120, 128, 1, true, true, 1};
    d1Layout[3] = {"time", "", "", "", 120, 176, 2, true, true, 1};

    d2Layout[0] = {"low60DayFullRange", "", "1. ", "", 120, 58, 1, true, true, 1};
    d2Layout[1] = {"secondLow60DayFullRange", "", "2. ", "", 120, 92, 1, true, true, 1};
    d2Layout[2] = {"low15DayFull", "", "15m ", "", 120, 126, 1, true, true, 1};
    d2Layout[3] = {"ip", "", "", "", 120, 176, 1, true, true, 1};
  }
}

void handleExportLayout() {
  if (!checkAuth()) return;

  DynamicJsonDocument doc(8192);

  for (int d = 1; d <= 2; d++) {
    JsonArray arr = doc.createNestedArray(d == 1 ? "d1" : "d2");
    LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;

    for (int i = 0; i < LAYOUT_ITEMS; i++) {
      JsonObject o = arr.createNestedObject();
      o["key"] = layout[i].key;
      o["txt"] = layout[i].customText;
      o["pre"] = layout[i].prefix;
      o["suf"] = layout[i].suffix;
      o["x"] = layout[i].x;
      o["y"] = layout[i].y;
      o["s"] = layout[i].size;
      o["v"] = layout[i].visible;
      o["a"] = layout[i].autoScale;
      o["al"] = layout[i].align;
    }
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleImportLayout() {
  if (!checkAuth()) return;

  if (!server.hasArg("json")) {
    server.sendHeader("Location", "/layout?saved=0");
    server.send(303);
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, server.arg("json"));

  if (!err) {
    for (int d = 1; d <= 2; d++) {
      JsonArray arr = doc[d == 1 ? "d1" : "d2"];
      LayoutItem* layout = (d == 1) ? d1Layout : d2Layout;

      for (int i = 0; i < LAYOUT_ITEMS && i < (int)arr.size(); i++) {
        JsonObject o = arr[i];
        if (o["key"].is<const char*>()) layout[i].key = String(o["key"].as<const char*>());
        if (o["txt"].is<const char*>()) layout[i].customText = String(o["txt"].as<const char*>());
        if (o["pre"].is<const char*>()) layout[i].prefix = String(o["pre"].as<const char*>());
        if (o["suf"].is<const char*>()) layout[i].suffix = String(o["suf"].as<const char*>());
        layout[i].x = o["x"] | layout[i].x;
        layout[i].y = o["y"] | layout[i].y;
        layout[i].size = o["s"] | layout[i].size;
        layout[i].visible = o["v"] | layout[i].visible;
        layout[i].autoScale = o["a"] | layout[i].autoScale;
        layout[i].align = o["al"] | layout[i].align;

        if (layout[i].x < 0) layout[i].x = 0;
        if (layout[i].x > SCREEN_WIDTH - 1) layout[i].x = SCREEN_WIDTH - 1;
        if (layout[i].y < 0) layout[i].y = 0;
        if (layout[i].y > SCREEN_HEIGHT - 1) layout[i].y = SCREEN_HEIGHT - 1;
        if (layout[i].size < 1) layout[i].size = 1;
        if (layout[i].size > 4) layout[i].size = 4;
        if (layout[i].align < 0) layout[i].align = 0;
        if (layout[i].align > 2) layout[i].align = 2;

        saveLayoutItem(d, i, layout[i]);
      }
    }
  }

  showLayoutDisplays();
  server.sendHeader("Location", err ? "/layout?saved=0" : "/layout?saved=1");
  server.send(303);
}
// -----------------------------------------------------------------------------
// Pinout-Schaubild
// -----------------------------------------------------------------------------

String pinoutPinSvg(bool leftSide, int y, String title, String sub, String color) {
  int padX = leftSide ? 246 : 440;
  int lineX1 = leftSide ? 246 : 454;
  int lineX2 = leftSide ? 120 : 580;
  int textX = leftSide ? 115 : 585;
  String anchor = leftSide ? "end" : "start";

  String s;
  s += "<line x1='" + String(lineX1) + "' y1='" + String(y) + "' x2='" + String(lineX2) + "' y2='" + String(y) + "' stroke='" + color + "' stroke-width='1.5'/>";
  s += "<rect x='" + String(padX) + "' y='" + String(y - 4) + "' width='14' height='8' rx='2' fill='" + color + "'/>";
  s += "<text x='" + String(textX) + "' y='" + String(y - 6) + "' fill='" + color + "' font-size='12' font-weight='700' text-anchor='" + anchor + "'>" + title + "</text>";
  s += "<text x='" + String(textX) + "' y='" + String(y + 10) + "' fill='#9aa8c7' font-size='10' text-anchor='" + anchor + "'>" + sub + "</text>";
  return s;
}

String buildPinoutSvg() {
  String blue = "#60a5fa";
  String teal = "#5eead4";
  String violet = "#a78bfa";
  String grey = "#94a3b8";

  String svg;
  svg.reserve(4500);

  svg += "<svg viewBox='0 0 700 400' xmlns='http://www.w3.org/2000/svg' style='font-family:Inter,system-ui,sans-serif'>";
  svg += "<rect x='0' y='0' width='700' height='400' fill='#0b1224' rx='16'/>";
  svg += "<rect x='260' y='30' width='180' height='340' rx='18' fill='#141c30' stroke='#26324f' stroke-width='2'/>";
  svg += "<rect x='280' y='60' width='140' height='110' rx='6' fill='#333c52' stroke='#4b566e' stroke-width='1.5'/>";
  svg += "<text x='350' y='110' fill='#e6ecff' font-size='14' font-weight='800' text-anchor='middle'>ESP32-C5</text>";
  svg += "<text x='350' y='128' fill='#9aa8c7' font-size='11' text-anchor='middle'>WROOM-1</text>";
  svg += "<rect x='320' y='362' width='60' height='16' rx='3' fill='#5b6685'/>";
  svg += "<text x='350' y='392' fill='#64748b' font-size='10' text-anchor='middle'>USB-C</text>";

  svg += pinoutPinSvg(true, 70, "3V3", "Versorgung TFT/Matrix/LEDs", grey);
  svg += pinoutPinSvg(true, 130, "GND", "Gemeinsame Masse", grey);
  svg += pinoutPinSvg(true, 190, "GPIO" + String(TFT_SCLK_PIN), "TFT SCLK + Matrix CLK", blue);
  svg += pinoutPinSvg(true, 250, "GPIO" + String(TFT_MOSI_PIN), "TFT MOSI + Matrix DIN", blue);
  svg += pinoutPinSvg(true, 310, "GPIO" + String(TFT_DC_PIN), "TFT DC (beide Displays)", blue);

  svg += pinoutPinSvg(false, 70, "GPIO" + String(TFT_RST_PIN), "TFT RST (beide Displays)", blue);
  svg += pinoutPinSvg(false, 130, "GPIO" + String(TFT1_CS_PIN), "TFT 1 Chip-Select", blue);
  svg += pinoutPinSvg(false, 190, "GPIO" + String(MATRIX_CS_PIN), "Matrix Chip-Select", violet);
  svg += pinoutPinSvg(false, 250, "GPIO" + String(TFT2_CS_PIN), "TFT 2 Chip-Select", blue);
  svg += pinoutPinSvg(false, 310, "GPIO" + String(LED_RING_PIN), "LED-Ring DIN", teal);

  svg += "</svg>";
  return svg;
}

// -----------------------------------------------------------------------------
// Energiefluss-Hub-Diagramm (Kiosk 2)
// -----------------------------------------------------------------------------

// Berechnet den Verbindungspfad zwischen zwei Knoten von Kreisrand zu
// Kreisrand (nicht Zentrum-zu-Zentrum), damit die Linie exakt andockt statt
// hineinzuragen. Gleiche Formel wie clientseitig in efLineBetween() (siehe
// Kiosk-2-JS) - beide MUESSEN bei Aenderungen synchron bleiben, da die
// clientseitige Funktion die Linien nach jedem Verschieben eines Knotens neu
// berechnet, waehrend diese hier nur die Erstauslieferung der Seite bedient.
static String efLinePath(int idxA, int idxB) {
  float x1 = efNodePos[idxA].x, y1 = efNodePos[idxA].y, r1 = EF_NODE_RADIUS[idxA];
  float x2 = efNodePos[idxB].x, y2 = efNodePos[idxB].y, r2 = EF_NODE_RADIUS[idxB];
  float dx = x2 - x1, dy = y2 - y1;
  float dist = sqrt(dx * dx + dy * dy);
  if (dist < 0.01f) dist = 0.01f;
  float ux = dx / dist, uy = dy / dist;
  float ax = x1 + ux * r1, ay = y1 + uy * r1;
  float bx = x2 - ux * r2, by = y2 - uy * r2;
  return "M " + String(ax, 1) + " " + String(ay, 1) + " L " + String(bx, 1) + " " + String(by, 1);
}

// Haus (Index 3) in der Mitte als Anker, PV/Batterie/Netz (0/1/2) als
// Satelliten-Knoten drumherum. Jeder Knoten ist ein <g transform=
// 'translate(x,y)'> mit relativ zu (0,0) positionierten Kindelementen -
// verschieben im Anordnen-Modus aendert dadurch nur EIN transform-Attribut
// pro Knoten (siehe efNodeDragMove() im Kiosk-2-JS), statt jedes Kind-
// Element einzeln neu positionieren zu muessen. Nur die Werte selbst
// (efPvVal etc.) werden hier mit Platzhaltern ("--") vorgerendert - der
// eigentliche Zustand (welche Speiche aktiv ist, in welche Richtung,
// Batterie-Ladering) wird rein clientseitig von efApply() anhand der
// /ankerdata-Antwort gesetzt. Alle Koordinaten beziehen sich auf viewBox
// 0 0 EF_CANVAS_W EF_CANVAS_H.
String buildEnergyFlowSvg() {
  String svg = "<svg viewBox='0 0 " + String(EF_CANVAS_W) + " " + String(EF_CANVAS_H) + "' role='img' aria-label='Energiefluss zwischen Solaranlage, Batterie, Haus und Netz'>";

  // Speichen (idle-Zustand als Start, efApply() setzt die Klasse aktiv um).
  String pvPath = efLinePath(0, 3), battPath = efLinePath(1, 3), gridPath = efLinePath(2, 3);
  String battPathRev = efLinePath(3, 1), gridPathRev = efLinePath(3, 2);

  svg += "<path id='lnPv' class='ef-line idle' d='" + pvPath + "'/>";
  svg += "<path id='lnBatt' class='ef-line idle' d='" + battPath + "'/>";
  svg += "<path id='lnGrid' class='ef-line idle' d='" + gridPath + "'/>";

  // Fliessende Punkte je Speiche/Richtung - per Default verborgen
  // (.ef-dot{display:none}), efApply() blendet je nach Fliessrichtung genau
  // einen davon pro Speiche ein.
  svg += "<circle id='dotPv' class='ef-dot ef-dot-pv' r='4'><animateMotion dur='1.6s' repeatCount='indefinite' path='" + pvPath + "'/></circle>";
  svg += "<circle id='dotBattCharge' class='ef-dot ef-dot-batt-charge' r='4'><animateMotion dur='1.6s' repeatCount='indefinite' path='" + battPathRev + "'/></circle>";
  svg += "<circle id='dotBattDischarge' class='ef-dot ef-dot-batt-discharge' r='4'><animateMotion dur='1.6s' repeatCount='indefinite' path='" + battPath + "'/></circle>";
  svg += "<circle id='dotGridImport' class='ef-dot ef-dot-grid-import' r='4'><animateMotion dur='1.6s' repeatCount='indefinite' path='" + gridPath + "'/></circle>";
  svg += "<circle id='dotGridExport' class='ef-dot ef-dot-grid-export' r='4'><animateMotion dur='1.6s' repeatCount='indefinite' path='" + gridPathRev + "'/></circle>";

  // PV-Knoten (Index 0)
  svg += "<g class='ef-node' data-node='0' transform='translate(" + String(efNodePos[0].x, 1) + "," + String(efNodePos[0].y, 1) + ")'>";
  svg += "<circle class='ef-node-circle' r='42'/>";
  svg += "<text y='-10' text-anchor='middle' class='ef-node-label'>PV</text>";
  svg += "<text y='10' text-anchor='middle' class='ef-node-value' id='efPvVal'>-- W</text></g>";

  // Batterie-Knoten (Index 1) mit Ladestand-Ring
  float socCirc = 2.0f * PI * EF_NODE_RADIUS[1];
  svg += "<g class='ef-node' data-node='1' transform='translate(" + String(efNodePos[1].x, 1) + "," + String(efNodePos[1].y, 1) + ")'>";
  svg += "<circle class='ef-soc-bg' r='" + String(EF_NODE_RADIUS[1], 0) + "'/>";
  svg += "<circle class='ef-soc-fill' id='efSocRing' r='" + String(EF_NODE_RADIUS[1], 0) + "' stroke-dasharray='0 " + String(socCirc, 1) + "'/>";
  svg += "<circle class='ef-node-circle' r='42'/>";
  svg += "<text y='-16' text-anchor='middle' class='ef-node-label'>Batterie</text>";
  svg += "<text y='1' text-anchor='middle' class='ef-node-value' id='efBattVal'>-- W</text>";
  svg += "<text y='16' text-anchor='middle' class='ef-soc-value' id='efSocVal'></text></g>";

  // Netz-Knoten (Index 2)
  svg += "<g class='ef-node' data-node='2' transform='translate(" + String(efNodePos[2].x, 1) + "," + String(efNodePos[2].y, 1) + ")'>";
  svg += "<circle class='ef-node-circle' r='42'/>";
  svg += "<text y='-16' text-anchor='middle' class='ef-node-label'>Netz</text>";
  svg += "<text y='2' text-anchor='middle' class='ef-node-value' id='efGridVal'>-- W</text>";
  svg += "<text y='17' text-anchor='middle' class='ef-node-sub' id='efGridSub'></text></g>";

  // Haus-Knoten (Index 3, Mitte/Anker)
  svg += "<g class='ef-node' data-node='3' transform='translate(" + String(efNodePos[3].x, 1) + "," + String(efNodePos[3].y, 1) + ")'>";
  svg += "<circle class='ef-node-circle ef-node-house' r='56'/>";
  svg += "<text y='-12' text-anchor='middle' class='ef-house-label'>Haus</text>";
  svg += "<text y='11' text-anchor='middle' class='ef-house-value' id='efHouseVal'>-- W</text></g>";

  svg += "</svg>";
  return svg;
}

// Kompaktes Kennzahlen-Widget neben dem Hub-Diagramm: Autarkie/Eigenverbrauch
// (momentane Verhaeltniszahlen, clientseitig aus denselben pv/house/grid-
// Rohwerten berechnet, die auch das Hub-Diagramm bekommt - siehe efApply())
// sowie Tagesertrag/Gesamtertrag/CO2/Geld gespart (direkt aus /ankerdata).
String buildEnergyStatsHtml() {
  String html = "<div class='ef-stats-grid'>";
  html += "<div class='ef-stat-tile'><div class='ef-stat-label'>Autarkie</div><div class='ef-stat-value' id='statAutarky'>--</div></div>";
  html += "<div class='ef-stat-tile'><div class='ef-stat-label'>Eigenverbrauch</div><div class='ef-stat-value' id='statSelfCons'>--</div></div>";
  html += "<div class='ef-stat-tile'><div class='ef-stat-label'>PV heute</div><div class='ef-stat-value' id='statPvToday'>--</div></div>";
  html += "<div class='ef-stat-tile'><div class='ef-stat-label'>Ertrag gesamt</div><div class='ef-stat-value' id='statYieldTotal'>--</div></div>";
  html += "<div class='ef-stat-tile'><div class='ef-stat-label'>CO2 gespart</div><div class='ef-stat-value' id='statCo2'>--</div></div>";
  html += "<div class='ef-stat-tile'><div class='ef-stat-label'>Geld gespart</div><div class='ef-stat-value' id='statMoney'>--</div></div>";
  html += "</div>";
  return html;
}

// -----------------------------------------------------------------------------
// Preis-Gauge
// -----------------------------------------------------------------------------

String buildPriceGaugeSvg() {
  if (quarterCount == 0 || metricCurrent15 < 0) {
    return "<p class='small'>Noch keine Preisdaten fuer die Anzeige geladen.</p>";
  }

  int nowCent = euroToCentRounded(metricCurrent15);
  int minCent = gaugeMinCent;
  int maxCent = gaugeMaxCent;
  if (maxCent <= minCent) maxCent = minCent + 1;

  float f = (float)(nowCent - minCent) / (float)(maxCent - minCent);
  if (f < 0) f = 0;
  if (f > 1) f = 1;

  String zoneLabel = "Guenstig";
  String ringColor = "#34C759";
  String pgClass = "pg-good";
  if (nowCent >= ledRedCent) { zoneLabel = "Teuer"; ringColor = "#FF3B30"; pgClass = "pg-bad"; }
  else if (nowCent >= ledYellowCent) { zoneLabel = "Mittel"; ringColor = "#FF9500"; pgClass = "pg-mid"; }

  String timeFrom = formatTimeOnly(currentStartsAt);
  String timeTo = formatTimeOnly(addMinutesToIsoTime(currentStartsAt, 15));

  // 270°-Tachobogen: Luecke unten (45° je Seite), Start unten-links, Ende unten-rechts
  // r=90, circumference=565.5, arcLen=424.1 (75% = 270°)
  // rotate(-225) bringt Startpunkt auf 7:30-Uhr-Position (unten-links)
  float circ = 2.0f * 3.14159f * 90.0f;  // 565.49
  float arcLen = circ * 0.75f;             // 424.12 = 270°
  float progressLen = arcLen * f;

  String html;
  html.reserve(1600);
  html += "<div class='priceRing " + pgClass + "'>";
  if (timeFrom.length() > 0 && timeTo.length() > 0) {
    html += "<div class='pr-time'>" + timeFrom + " &ndash; " + timeTo + " Uhr</div>";
  }
  html += "<svg viewBox='0 0 240 250' xmlns='http://www.w3.org/2000/svg'>";
  // Hintergrundbogen (270°)
  html += "<circle cx='120' cy='115' r='90' fill='none' stroke='var(--pr-track)' stroke-width='18' stroke-linecap='round'";
  html += " stroke-dasharray='" + String(arcLen, 1) + " " + String(circ, 1) + "'";
  html += " transform='rotate(-225 120 115)'/>";
  // Fortschrittsbogen in Zonenfarbe
  html += "<circle cx='120' cy='115' r='90' fill='none' stroke='" + ringColor + "' stroke-width='18' stroke-linecap='round'";
  html += " stroke-dasharray='" + String(progressLen, 1) + " " + String(circ, 1) + "'";
  html += " transform='rotate(-225 120 115)'/>";
  // Preis in der Mitte
  html += "<text x='120' y='103' fill='var(--text)' font-size='56' font-weight='700' text-anchor='middle' font-family='-apple-system,system-ui,sans-serif' letter-spacing='-3'>" + String(nowCent) + "</text>";
  html += "<text x='120' y='125' fill='var(--muted)' font-size='13' font-weight='500' text-anchor='middle'>ct/kWh</text>";
  html += "<text x='120' y='147' fill='" + ringColor + "' font-size='14' font-weight='600' text-anchor='middle'>" + zoneLabel + "</text>";
  // Min/Max Beschriftung an den Bogenenden (unten-links / unten-rechts)
  html += "<text x='22' y='222' fill='var(--muted)' font-size='11' font-weight='500'>" + String(minCent) + " ct</text>";
  html += "<text x='218' y='222' fill='var(--muted)' font-size='11' font-weight='500' text-anchor='end'>" + String(maxCent) + " ct</text>";
  html += "</svg>";
  html += "</div>";
  return html;
}

void getKioskPriceStatus(String &statusText, String &statusColor) {
  statusText = "Keine Daten";
  statusColor = "var(--muted)";

  if (quarterCount > 0 && metricCurrent15 >= 0) {
    int nowCent = euroToCentRounded(metricCurrent15);
    if (nowCent >= ledRedCent) {
      statusText = "Teuer";
      statusColor = "#fb7185";
    } else if (nowCent >= ledYellowCent) {
      statusText = "Mittel";
      statusColor = "#facc15";
    } else {
      statusText = "Günstig";
      statusColor = "#4ade80";
    }
  }
}

// Erzeugt die Positions-/Groessen-/Sichtbarkeits-CSS-Regeln fuer alle
// Kiosk-Widgets aus einem Layout-Array (Prozentwerte relativ zum
// .kiosk-canvas-Container). Wird einmal fuer Hochformat (Basis-Styles) und
// einmal fuer Querformat (innerhalb der Media-Query) aufgerufen.
String kioskWidgetCss(KioskWidgetLayout arr[], const char* const keys[], int count) {
  String css;
  css.reserve(700);

  for (int i = 0; i < count; i++) {
    css += ".kw-" + String(keys[i]) + "{";
    if (!arr[i].visible) {
      css += "display:none}";
      continue;
    }
    css += "grid-column:" + String(arr[i].colStart) + "/span " + String(arr[i].colSpan) + ";";
    css += "grid-row:" + String(arr[i].rowStart) + "/span " + String(arr[i].rowSpan) + "}";
  }

  return css;
}

// Serialisiert ein Layout-Array fuer den Kiosk-Layout-Editor (JS-Objekt beim
// Seitenaufbau), damit der Editor Hoch- und Querformat clientseitig ohne
// Neuladen umschalten kann. keys/labels/count optional, damit dieselbe
// Funktion auch fuer das Kiosk2-Widget-Set (Energiefluss) genutzt werden kann.
String kioskLayoutJson(KioskWidgetLayout arr[], const char* const keys[], const char* const labels[], int count) {
  String json = "[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    String preview = "";
    String key = String(keys[i]);
    if (key == "clock") {
      preview = getDisplayTimeText();
    } else if (key == "gauge" || key == "pricegauge") {
      preview = (metricCurrent15 >= 0) ? (priceToCentText(metricCurrent15) + " ct/kWh") : "-- ct/kWh";
    } else if (key == "status") {
      String st, sc; getKioskPriceStatus(st, sc);
      preview = st;
    } else if (key == "livepower") {
      String p = formatLivePowerValue();
      preview = (p.length() > 0) ? ("&#9889; " + p) : "&#9889; -- W";
    } else if (key == "chart" || key == "pricechart") {
      preview = "&#128200; Preisverlauf (" + String(quarterCount) + " Slots)";
    } else if (key == "meta") {
      String low = (metricLow15Day >= 0) ? priceToCentText(metricLow15Day) : "--";
      String avg = (metricDayAvg >= 0) ? priceToCentText(metricDayAvg) : "--";
      preview = "Tief " + low + " &middot; Schnitt " + avg;
    } else if (key == "energyflow") {
      preview = (ankerPvW >= 0) ? ("&#9728;&#65039; " + String((int)ankerPvW) + " W &middot; &#127968; " + String((int)ankerHomeLoadW) + " W") : "&#9728;&#65039; -- W";
    } else if (key == "stats") {
      preview = (ankerTotalYieldKwh >= 0) ? ("Ertrag " + String((int)ankerTotalYieldKwh) + " kWh") : "Kennzahlen";
    }
    json += "{";
    json += "\"key\":\"" + key + "\",";
    json += "\"label\":\"" + String(labels[i]) + "\",";
    json += "\"preview\":\"" + jsonEscapeValue(preview) + "\",";
    json += "\"colStart\":" + String(arr[i].colStart) + ",";
    json += "\"colSpan\":" + String(arr[i].colSpan) + ",";
    json += "\"rowStart\":" + String(arr[i].rowStart) + ",";
    json += "\"rowSpan\":" + String(arr[i].rowSpan) + ",";
    json += "\"visible\":" + String(arr[i].visible ? "true" : "false");
    json += "}";
  }
  json += "]";
  return json;
}

// -----------------------------------------------------------------------------
// Diagramm
// -----------------------------------------------------------------------------

// Liefert dieselben x/y-Koordinaten, die buildSvgChart() intern fuer die
// Preislinie berechnet, als JSON-Array - fuer das interaktive "Linie
// abfahren" im Tablet-Modus (Fadenkreuz per Touch/Maus ueber dem Chart).
// Dupliziert bewusst die Geometrie-Berechnung statt buildSvgChart() zu
// veraendern, um den bestehenden Chart auf anderen Seiten nicht anzufassen.
String buildChartPointsJson() {
  if (quarterCount == 0) return "[]";

  float minP = quarterPrices[0];
  float maxP = quarterPrices[0];

  for (int i = 1; i < quarterCount; i++) {
    if (quarterPrices[i] < minP) minP = quarterPrices[i];
    if (quarterPrices[i] > maxP) maxP = quarterPrices[i];
  }

  if (maxP <= minP) maxP = minP + 0.01;

  int left = 55;
  int top = 25;
  int chartW = 760 - left - 20;
  int chartH = 320 - top - 55;

  String json;
  json.reserve((size_t)quarterCount * 40 + 8);
  json += "[";

  for (int i = 0; i < quarterCount; i++) {
    float norm = (quarterPrices[i] - minP) / (maxP - minP);
    int x = left + ((chartW * i) / max(1, quarterCount - 1));
    int y = top + chartH - int(norm * chartH);

    if (i > 0) json += ",";
    json += "{\"x\":" + String(x) + ",\"y\":" + String(y) + ",\"p\":" + String(euroToCentRounded(quarterPrices[i])) + ",\"t\":\"" + formatTimeOnly(quarterTimes[i]) + "\"}";
  }

  json += "]";
  return json;
}

String buildSvgChart(String lineColor, String fillColor) {
  if (quarterCount == 0) {
    return "<p>Keine Diagrammdaten</p>";
  }
  // Default-Farben wenn keine uebergeben: Akzentfarbe des Themes statt
  // hartcodiertem Weiss, damit das Diagramm im Light-Theme nicht unsichtbar ist
  if (lineColor.length() == 0) lineColor = "var(--accent)";
  if (fillColor.length() == 0) fillColor = "var(--accent)";

  float minP = quarterPrices[0];
  float maxP = quarterPrices[0];

  for (int i = 1; i < quarterCount; i++) {
    if (quarterPrices[i] < minP) minP = quarterPrices[i];
    if (quarterPrices[i] > maxP) maxP = quarterPrices[i];
  }

  if (maxP <= minP) maxP = minP + 0.01;

  int left = 55;
  int top = 25;
  int chartW = 760 - left - 20;
  int chartH = 320 - top - 55;

  int xs[MAX_QUARTERS];
  int ys[MAX_QUARTERS];

  for (int i = 0; i < quarterCount; i++) {
    float norm = (quarterPrices[i] - minP) / (maxP - minP);
    xs[i] = left + ((chartW * i) / max(1, quarterCount - 1));
    ys[i] = top + chartH - int(norm * chartH);
  }

  String today0 = quarterTimes[0].length() >= 10 ? quarterTimes[0].substring(0, 10) : "";
  int tomorrowIndex = -1;

  for (int i = 1; i < quarterCount; i++) {
    if (quarterTimes[i].length() >= 10 && quarterTimes[i].substring(0, 10) != today0) {
      tomorrowIndex = i;
      break;
    }
  }

  String nowPrefix = getCurrentIsoPrefix();
  int nowIndex = -1;

  if (nowPrefix.length() >= 16) {
    for (int i = 0; i < quarterCount; i++) {
      if (quarterTimes[i].length() < 16) continue;
      if (quarterTimes[i].substring(0, 16).compareTo(nowPrefix) <= 0) {
        nowIndex = i;
      } else {
        break;
      }
    }
  }

  String svg;
  svg.reserve(13000);

  // Apple-Weather-Style Chart: transparent, keine Box, dünne Gridlines,
  // satter Farbverlauf-Fill, glatte dicke Linie, weisse Marker
  svg += "<svg id='priceChartSvg' viewBox='0 0 760 320' xmlns='http://www.w3.org/2000/svg' style='font-family:-apple-system,Inter,system-ui,sans-serif'>";
  svg += "<defs>";
  svg += "<linearGradient id='chartFill' x1='0' y1='0' x2='0' y2='1'>";
  svg += "<stop offset='0%' stop-color='" + fillColor + "' stop-opacity='1'/>";
  svg += "<stop offset='100%' stop-color='" + fillColor + "' stop-opacity='0'/>";
  svg += "</linearGradient>";
  svg += "<filter id='chartGlow'><feGaussianBlur stdDeviation='3' result='b'/><feMerge><feMergeNode in='b'/><feMergeNode in='SourceGraphic'/></feMerge></filter>";
  svg += "</defs>";

  // Dezente horizontale Gridlinien
  for (int t = 0; t <= 4; t++) {
    float value = minP + ((maxP - minP) * t / 4.0);
    int y = top + chartH - ((chartH * t) / 4);
    svg += "<line x1='" + String(left) + "' y1='" + String(y) + "' x2='" + String(left + chartW) + "' y2='" + String(y) + "' stroke='var(--line)' stroke-width='1'/>";
    svg += "<text x='3' y='" + String(y + 4) + "' fill='var(--muted)' font-size='11' font-weight='500'>" + String(euroToCentRounded(value)) + "</text>";
  }
  // Zeitstempel unten
  for (int i = 0; i < quarterCount; i += 12) {
    svg += "<text x='" + String(xs[i]) + "' y='315' fill='var(--muted)' font-size='10' font-weight='500' text-anchor='middle'>" + formatTimeOnly(quarterTimes[i]) + "</text>";
  }

  String linePoints;
  for (int i = 0; i < quarterCount; i++) {
    linePoints += String(xs[i]) + "," + String(ys[i]) + " ";
  }
  String areaPoints = String(xs[0]) + "," + String(top + chartH) + " " + linePoints + String(xs[quarterCount - 1]) + "," + String(top + chartH);

  // Farbiger Fill
  svg += "<polygon points='" + areaPoints + "' fill='url(#chartFill)' stroke='none'/>";

  // Durchschnittslinie — dezent gestrichelt
  if (metricDayAvg >= 0) {
    float normAvg = (metricDayAvg - minP) / (maxP - minP);
    if (normAvg < 0) normAvg = 0;
    if (normAvg > 1) normAvg = 1;
    int yAvg = top + chartH - int(normAvg * chartH);
    svg += "<line x1='" + String(left) + "' y1='" + String(yAvg) + "' x2='" + String(left + chartW) + "' y2='" + String(yAvg) + "' stroke='var(--muted)' stroke-width='1' stroke-dasharray='6,4' opacity='.5'/>";
    svg += "<text x='" + String(left + chartW - 4) + "' y='" + String(yAvg - 5) + "' fill='var(--muted)' font-size='10' font-weight='600' text-anchor='end'>&#8960; " + String(euroToCentRounded(metricDayAvg)) + " ct</text>";
  }

  // Hauptlinie — glatt, in uebergebener Farbe
  svg += "<polyline fill='none' stroke='" + lineColor + "' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round' points='" + linePoints + "'/>";

  // Morgen-Trenner
  if (tomorrowIndex > 0) {
    svg += "<line x1='" + String(xs[tomorrowIndex]) + "' y1='" + String(top) + "' x2='" + String(xs[tomorrowIndex]) + "' y2='" + String(top + chartH) + "' stroke='var(--line)' stroke-width='1' stroke-dasharray='3,4'/>";
    svg += "<text x='" + String(xs[tomorrowIndex] + 5) + "' y='" + String(top + 13) + "' fill='var(--muted)' font-size='10' font-weight='600'>Morgen</text>";
  }

  // Tiefster 60-Min-Block — goldener Marker
  if (metricLow60Day >= 0) {
    int lowIndex = 0;
    for (int i = 0; i < quarterCount; i++) {
      if (quarterTimes[i] == metricLow60DayTime) { lowIndex = i; break; }
    }
    svg += "<circle cx='" + String(xs[lowIndex]) + "' cy='" + String(ys[lowIndex]) + "' r='4' fill='#FFD60A' opacity='.9'/>";
    int lx = xs[lowIndex] + 8; if (lx > left + chartW - 65) lx = xs[lowIndex] - 70;
    int ly = ys[lowIndex] - 8; if (ly < top + 12) ly = ys[lowIndex] + 18;
    svg += "<text x='" + String(lx) + "' y='" + String(ly) + "' fill='#FFD60A' font-size='11' font-weight='600'>" + String(euroToCentRounded(metricLow60Day)) + " ct</text>";
  }

  // Jetzt-Marker — grosse weisse Kugel mit Zone-Farbe
  if (nowIndex >= 0) {
    int nowCent = euroToCentRounded(quarterPrices[nowIndex]);
    String nowColor = "#34C759";
    if (nowCent >= ledRedCent) nowColor = "#FF3B30";
    else if (nowCent >= ledYellowCent) nowColor = "#FF9500";
    svg += "<circle cx='" + String(xs[nowIndex]) + "' cy='" + String(ys[nowIndex]) + "' r='8' fill='var(--card)' filter='url(#chartGlow)'/>";
    svg += "<circle cx='" + String(xs[nowIndex]) + "' cy='" + String(ys[nowIndex]) + "' r='5' fill='" + nowColor + "'/>";
    int lx = xs[nowIndex] + 12; if (lx > left + chartW - 65) lx = xs[nowIndex] - 72;
    int ly = ys[nowIndex] - 12; if (ly < top + 16) ly = ys[nowIndex] + 22;
    svg += "<text x='" + String(lx) + "' y='" + String(ly) + "' fill='" + nowColor + "' font-size='12' font-weight='700'>Jetzt " + String(nowCent) + " ct</text>";
  }

  svg += "</svg>";

  return svg;
}

// -----------------------------------------------------------------------------
// Web Actions
// -----------------------------------------------------------------------------

void handleSave() {
  if (!checkAuth()) return;

  if (server.hasArg("webName")) {
    webInterfaceName = server.arg("webName");
    webInterfaceName.trim();
    if (webInterfaceName.length() == 0) webInterfaceName = "Dynamic Price Clock";
    if (webInterfaceName.length() > 32) webInterfaceName = webInterfaceName.substring(0, 32);
    prefs.putString("webName", webInterfaceName);
  }

  if (server.hasArg("gaugeMin") || server.hasArg("gaugeMax")) {
    if (server.hasArg("gaugeMin")) { int v=server.arg("gaugeMin").toInt(); gaugeMinCent=(v<0)?0:(v>200?200:v); }
    if (server.hasArg("gaugeMax")) { int v=server.arg("gaugeMax").toInt(); gaugeMaxCent=(v<1)?1:(v>200?200:v); }
    if (gaugeMaxCent <= gaugeMinCent) {
      gaugeMaxCent = gaugeMinCent + 1;
      // gaugeMinCent=200 (das erlaubte Maximum) wuerde hier gaugeMaxCent auf
      // 201 setzen - ausserhalb des dokumentierten 1-200-Bereichs. Erneut
      // klemmen und im seltenen Fall stattdessen gaugeMinCent nachgeben.
      if (gaugeMaxCent > 200) { gaugeMaxCent = 200; gaugeMinCent = 199; }
    }
    prefs.putInt("gaugeMin", gaugeMinCent);
    prefs.putInt("gaugeMax", gaugeMaxCent);
  }

  if (server.hasArg("ledYellow") || server.hasArg("ledRed")) {
    if (server.hasArg("ledYellow")) {
      ledYellowCent = server.arg("ledYellow").toInt();
      if (ledYellowCent < 0) ledYellowCent = 0;
      if (ledYellowCent > 200) ledYellowCent = 200;
    }
    if (server.hasArg("ledRed")) {
      ledRedCent = server.arg("ledRed").toInt();
      if (ledRedCent < 0) ledRedCent = 0;
      if (ledRedCent > 200) ledRedCent = 200;
    }
    if (ledRedCent <= ledYellowCent) ledRedCent = ledYellowCent + 1;
    if (ledRedCent > 200) ledRedCent = 200;
    prefs.putInt("ledYellow", ledYellowCent);
    prefs.putInt("ledRed", ledRedCent);
  }

  if (server.hasArg("accent")) {
    String a = server.arg("accent");
    if (a == "blue" || a == "green" || a == "orange" || a == "red" || a == "pink" || a == "purple" || a == "teal" || a == "indigo") {
      accentColor = a;
      prefs.putString("accent", accentColor);
    }
  }

  if (server.hasArg("appear")) {
    String m = server.arg("appear");
    if (m != "glass") m = "solid";
    appearanceMode = m;
    prefs.putString("appear", appearanceMode);
  }

  if (server.hasArg("klpStyle")) {
    String s = server.arg("klpStyle");
    if (s != "bar") s = "text";
    kioskLivePowerStyle = s;
    prefs.putString("klpStyle", kioskLivePowerStyle);
  }

  if (server.hasArg("lpMax") || server.hasArg("lpGreen") || server.hasArg("lpYellow")) {
    float mx = server.hasArg("lpMax") ? server.arg("lpMax").toFloat() : livePowerMaxKw;
    float gr = server.hasArg("lpGreen") ? server.arg("lpGreen").toFloat() : livePowerGreenKw;
    float ye = server.hasArg("lpYellow") ? server.arg("lpYellow").toFloat() : livePowerYellowKw;
    if (mx < 1.0f) mx = 10.0f;
    if (mx > 50.0f) mx = 50.0f;
    if (gr < 0.1f) gr = 0.1f;
    if (gr >= mx) gr = mx * 0.2f;
    if (ye <= gr) ye = gr + 0.1f;
    if (ye >= mx) ye = mx * 0.5f;
    livePowerMaxKw = mx;
    livePowerGreenKw = gr;
    livePowerYellowKw = ye;
    prefs.putFloat("lpMax", livePowerMaxKw);
    prefs.putFloat("lpGreen", livePowerGreenKw);
    prefs.putFloat("lpYellow", livePowerYellowKw);
  }

  if (server.hasArg("token")) {
    String newToken = server.arg("token");
    newToken.trim();

    if (newToken.length() > 0) {
      tibberToken = newToken;
      prefs.putString("token", tibberToken);
    }
  }

  if (server.hasArg("homeId")) {
    selectedHomeId = server.arg("homeId");
    prefs.putString("homeId", selectedHomeId);
  }

  if (server.hasArg("baseFee")) {
    float newFee = server.arg("baseFee").toFloat();
    if (newFee < 0) newFee = 0;
    if (newFee > 100) newFee = 100;
    tibberBaseFeeEur = newFee;
    prefs.putFloat("baseFee", tibberBaseFeeEur);
  }

  if (server.hasArg("apiMinutes")) {
    apiUpdateMinutes = server.arg("apiMinutes").toInt();
    if (apiUpdateMinutes < 1) apiUpdateMinutes = 1;
    if (apiUpdateMinutes > 60) apiUpdateMinutes = 60;
    prefs.putInt("apiMinutes", apiUpdateMinutes);
    updateInterval = (unsigned long)apiUpdateMinutes * 60UL * 1000UL;
  }

  if (server.hasArg("dispRefresh")) {
    displayRefreshSeconds = server.arg("dispRefresh").toInt();
    if (displayRefreshSeconds < 1) displayRefreshSeconds = 1;
    if (displayRefreshSeconds > 300) displayRefreshSeconds = 300;
    prefs.putInt("dispRefresh", displayRefreshSeconds);
    displayRefreshInterval = (unsigned long)displayRefreshSeconds * 1000UL;
  }

  if (!apMode) {
    updatePrices();
  }

  String requestedRedirect = server.hasArg("redirectTo") ? server.arg("redirectTo") : "";
  bool validRedirect = (requestedRedirect == "/anbieter" || requestedRedirect == "/account" || requestedRedirect == "/kiosklayout");
  String redirectTarget = validRedirect ? requestedRedirect : "/";
  server.sendHeader("Location", redirectTarget + "?saved=1");
  server.send(303);
}

void handleSavePins() {
  if (!checkAuth()) return;

  int newSclk = server.hasArg("tftSclkPin") ? server.arg("tftSclkPin").toInt() : tftSclkPin;
  int newMosi = server.hasArg("tftMosiPin") ? server.arg("tftMosiPin").toInt() : tftMosiPin;
  int newLed = server.hasArg("ledRingPin") ? server.arg("ledRingPin").toInt() : ledRingPinVar;
  int newMatrixCs = server.hasArg("matrixCsPin") ? server.arg("matrixCsPin").toInt() : matrixCsPinVar;

  int editablePins[4] = {newSclk, newMosi, newLed, newMatrixCs};
  int fixedPins[4] = {TFT_DC_PIN, TFT_RST_PIN, TFT1_CS_PIN, TFT2_CS_PIN};
  bool valid = true;

  for (int i = 0; i < 4; i++) {
    if (editablePins[i] < 0 || editablePins[i] > 48) valid = false;
  }

  for (int i = 0; i < 4 && valid; i++) {
    for (int j = i + 1; j < 4; j++) {
      if (editablePins[i] == editablePins[j]) valid = false;
    }
    for (int k = 0; k < 4; k++) {
      if (editablePins[i] == fixedPins[k]) valid = false;
    }
  }

  if (valid) {
    tftSclkPin = newSclk;
    tftMosiPin = newMosi;
    ledRingPinVar = newLed;
    matrixCsPinVar = newMatrixCs;
    prefs.putInt("tftSclkPin", tftSclkPin);
    prefs.putInt("tftMosiPin", tftMosiPin);
    prefs.putInt("ledRingPin", ledRingPinVar);
    prefs.putInt("matrixCsPin", matrixCsPinVar);
  }

  server.sendHeader("Location", valid ? "/pinout?saved=1&pinsChanged=1" : "/pinout?saved=0");
  server.send(303);
}

void handleRestartDevice() {
  if (!checkAuth()) return;

  String html;
  html += htmlHeader("Neustart");
  html += "<section class='hero'><h1>Neustart</h1><p>Das Geraet startet jetzt neu, um die neuen GPIO-Zuordnungen zu uebernehmen. Diese Seite laedt sich in ein paar Sekunden von selbst nicht neu - bitte die Startseite manuell erneut aufrufen.</p></section>";
  html += htmlFooter();

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);

  delay(1500);
  ESP.restart();
}

void handleCheckGithubUpdate() {
  if (!checkAuth()) return;

  // Waehrend ein Update laeuft keine weitere HTTPS-Sitzung zu api.github.com
  // eroeffnen - konkurriert sonst um denselben knappen Heap wie der
  // eigentliche Download (siehe Kommentar in loop()).
  if (ota.owner != OtaOwner::None) {
    String json = "{\"ok\":false,\"currentVersion\":\"" + jsEscape(String(FIRMWARE_VERSION)) + "\",\"latestVersion\":\"\",\"updateAvailable\":false,\"downloadUrl\":\"\",\"error\":\"Ein Update laeuft bereits\"}";
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(200, "application/json", json);
    return;
  }

  bool ok = otaCheckLatest(/*withRetryAndDiag=*/true);
  bool updateAvailable = ok && ota.latestVersion.length() > 0 && ota.latestVersion != String(FIRMWARE_VERSION);

  String json = "{";
  json += "\"ok\":" + String(ok ? "true" : "false") + ",";
  json += "\"currentVersion\":\"" + jsEscape(String(FIRMWARE_VERSION)) + "\",";
  json += "\"latestVersion\":\"" + jsEscape(ota.latestVersion) + "\",";
  json += "\"updateAvailable\":" + String(updateAvailable ? "true" : "false") + ",";
  json += "\"downloadUrl\":\"" + jsEscape(ota.latestUrl) + "\",";
  json += "\"error\":\"" + jsEscape(ota.checkError) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleVersionCheck() {
  if (!checkAuth()) return;

  // Siehe Kommentar in handleCheckGithubUpdate() - dieser Endpunkt wird
  // zusaetzlich automatisch alle 10 Minuten von JEDER geladenen Seite
  // aufgerufen, koennte also auch unbemerkt waehrend eines laufenden Updates
  // feuern, wenn dieser Schutz fehlen wuerde.
  if (ota.owner != OtaOwner::None) {
    String json = "{\"ok\":false,\"currentVersion\":\"" + jsEscape(String(FIRMWARE_VERSION)) + "\",\"latestVersion\":\"\",\"updateAvailable\":false,\"error\":\"Ein Update laeuft bereits\"}";
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(200, "application/json", json);
    return;
  }

  bool ok = otaCheckLatest(/*withRetryAndDiag=*/false);
  bool updateAvailable = ok && ota.latestVersion.length() > 0 && ota.latestVersion != String(FIRMWARE_VERSION);

  String json = "{";
  json += "\"ok\":" + String(ok ? "true" : "false") + ",";
  json += "\"currentVersion\":\"" + jsEscape(String(FIRMWARE_VERSION)) + "\",";
  json += "\"latestVersion\":\"" + jsEscape(ota.latestVersion) + "\",";
  json += "\"updateAvailable\":" + String(updateAvailable ? "true" : "false") + ",";
  json += "\"error\":\"" + jsEscape(ota.checkError) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleGithubUpdate() {
  if (!checkAuth()) return;

  if (ota.owner != OtaOwner::None) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Update laeuft bereits\"}");
    return;
  }

  // Absichtlich KEINE clientseitig uebergebene URL mehr akzeptieren (frueher
  // server.arg("url") bevorzugt) - ein Client koennte damit eine beliebige
  // URL zum Herunterladen/Flashen vorgeben, nur grob per Teilstring-Pruefung
  // auf "github" gefiltert. Stattdessen immer die servereigene, bereits
  // verifizierte ota.latestUrl verwenden (bei Bedarf erst per
  // otaCheckLatest() aktualisieren).
  if (ota.latestUrl.length() == 0) {
    otaCheckLatest(/*withRetryAndDiag=*/true);
  }
  String url = ota.latestUrl;

  if (url.length() == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Keine Update-URL - zuerst auf Updates pruefen\"}");
    return;
  }

  String rejection = otaTryAcquire(OtaOwner::Github);
  if (rejection.length() > 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"" + jsEscape(rejection) + "\"}");
    return;
  }

  otaPendingUrl = url;

  // Eigener Task, damit der Webserver waehrend des Downloads erreichbar
  // bleibt und /otaprogress den Fortschritt live zurueckgeben kann.
  // Prioritaet 5 damit TLS nicht durch andere Tasks unterbrochen wird (max=25)
  xTaskCreate(otaGithubTask, "otaGithubTask", 16384, NULL, 5, NULL);

  server.send(200, "application/json", "{\"ok\":true,\"started\":true}");
}

void handleOtaProgress() {
  if (!checkAuth()) return;

  int percent = -1;
  if (ota.bytesTotal > 0) {
    percent = (int)(((long)ota.bytesWritten * 100L) / ota.bytesTotal);
  }

  bool running = (ota.owner != OtaOwner::None);
  bool done = (ota.phase == OtaPhase::Failed || ota.phase == OtaPhase::Rebooting);
  unsigned long heartbeatAge = (ota.heartbeatMs > 0) ? (millis() - ota.heartbeatMs) / 1000 : 0;

  String json = "{";
  json += "\"running\":" + String(running ? "true" : "false") + ",";
  json += "\"done\":" + String(done ? "true" : "false") + ",";
  json += "\"success\":" + String(ota.success ? "true" : "false") + ",";
  json += "\"bytesWritten\":" + String(ota.bytesWritten) + ",";
  json += "\"bytesTotal\":" + String(ota.bytesTotal) + ",";
  json += "\"percent\":" + String(percent) + ",";
  json += "\"heartbeatAge\":" + String(heartbeatAge) + ",";
  json += "\"diag\":\"" + jsEscape(ota.diag) + "\",";
  json += "\"error\":\"" + jsEscape(ota.error) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleUploadFirmwareData() {
  if (!checkAuth()) return;

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Denselben Sperren-Mechanismus wie der GitHub-OTA-Pfad nutzen (inkl.
    // Heap-Schwellenwert, siehe otaTryAcquire()), damit beide Wege sich
    // nicht gegenseitig ins Gehege kommen (sonst koennten zwei gleichzeitige
    // Update.begin()-Aufrufe dieselbe OTA-Partition beschreiben).
    otaUploadPreflightRejection = otaTryAcquire(OtaOwner::ManualUpload);
    if (otaUploadPreflightRejection.length() > 0) return; // Sperre nicht erhalten - ab hier nichts an ota.* schreiben
    otaSetPhase(OtaOwner::ManualUpload, OtaPhase::Uploading);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaFail(OtaOwner::ManualUpload, "Update.begin fehlgeschlagen: " + String(Update.errorString()));
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (ota.owner != OtaOwner::ManualUpload || ota.error.length() > 0) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaFail(OtaOwner::ManualUpload, "Update.write fehlgeschlagen: " + String(Update.errorString()));
    } else {
      otaProgress(OtaOwner::ManualUpload, ota.bytesWritten + upload.currentSize, ota.bytesWritten + upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (ota.owner != OtaOwner::ManualUpload) return;
    otaSetPhase(OtaOwner::ManualUpload, OtaPhase::Flashing);
    if (ota.error.length() == 0 && Update.end(true)) {
      otaFinishSuccess(OtaOwner::ManualUpload);
    } else {
      otaFail(OtaOwner::ManualUpload, ota.error.length() > 0 ? ota.error : ("Update.end fehlgeschlagen: " + String(Update.errorString())));
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    // Update.abort() nur aufrufen, wenn DIESER Upload die Sperre tatsaechlich
    // haelt - sonst wuerde ein von vornherein abgewiesener Upload (weil z.B.
    // gerade ein GitHub-Update lief) in dessen laufende Update-Sitzung
    // hineinfunken. otaFail() prueft die Eigentuemerschaft selbst und ist
    // deshalb als einziger der beiden Aufrufe hier unconditional sicher -
    // der historische Bug (abgebrochener, abgewiesener Upload ueberschreibt
    // den Fehlertext eines fremden laufenden Updates) ist dadurch strukturell
    // unmoeglich, nicht nur per Konvention vermieden.
    if (ota.owner == OtaOwner::ManualUpload) {
      Update.abort();
    }
    otaFail(OtaOwner::ManualUpload, "Upload abgebrochen");
  }
}

void handleUploadFirmware() {
  if (!checkAuth()) return;

  bool ok = (otaUploadPreflightRejection.length() == 0) && ota.success;
  String err = (otaUploadPreflightRejection.length() > 0) ? otaUploadPreflightRejection : ota.error;

  String json = "{\"ok\":" + String(ok ? "true" : "false");
  if (!ok) {
    json += ",\"error\":\"" + jsEscape(err) + "\"";
  }
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);

  if (ok) {
    delay(500);
    ESP.restart();
  }
}

void handleResetWifi() {
  if (!checkAuth()) return;

  prefs.remove("wifiSsid");
  prefs.remove("wifiPass");

  wifiSsid = String(DEFAULT_WIFI_SSID);
  wifiPassword = String(DEFAULT_WIFI_PASSWORD);

  String html;
  html += htmlHeader("WLAN zurueckgesetzt");
  html += "<h1>WLAN zurueckgesetzt</h1>";
  html += "<p>Gespeicherte WLAN-Daten wurden geloescht. Geraet startet neu in den Setup-Modus.</p>";
  html += htmlFooter();

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);

  delay(1500);
  ESP.restart();
}

void handleRefresh() {
  if (!checkAuth()) return;

  if (!apMode) {
    updatePrices();
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleJson() {
  if (!checkAuth()) return;

  String json;
  json.reserve(8000);

  json += "{";

  json += "\"webInterfaceName\":\"";
  json += jsonEscapeValue(webInterfaceName);
  json += "\",";
  json += "\"todayTomorrowMode\":true,";
  json += "\"current15\":\"" + priceToCentText(metricCurrent15) + "\",";
  json += "\"current60\":\"" + priceToCentText(metricCurrent60) + "\",";
  json += "\"dayAvg\":\"" + priceToCentText(metricDayAvg) + "\",";
  json += "\"low15Day\":\"" + priceToCentText(metricLow15Day) + "\",";
  json += "\"low15DayTime\":\"" + formatTimeOnly(metricLow15DayTime) + "\",";
  json += "\"low15DayFull\":\"" + getLayoutValue("low15DayFull") + "\",";
  json += "\"low60Day\":\"" + priceToCentText(metricLow60Day) + "\",";
  json += "\"low60DayTime\":\"" + formatTimeOnly(metricLow60DayTime) + "\",";
  json += "\"low60DayEndTime\":\"" + addMinutesToIsoTime(metricLow60DayTime, 60) + "\",";
  json += "\"low60DayTimeRange\":\"" + getLayoutValue("low60DayTimeRange") + "\",";
  json += "\"low60DayFull\":\"" + getLayoutValue("low60DayFull") + "\",";
  json += "\"low60DayFullRange\":\"" + getLayoutValue("low60DayFullRange") + "\",";
  json += "\"secondLow60Day\":\"" + priceToCentText(metricSecondLow60Day) + "\",";
  json += "\"secondLow60DayTime\":\"" + formatTimeOnly(metricSecondLow60DayTime) + "\",";
  json += "\"secondLow60DayEndTime\":\"" + addMinutesToIsoTime(metricSecondLow60DayTime, 60) + "\",";
  json += "\"secondLow60DayTimeRange\":\"" + getLayoutValue("secondLow60DayTimeRange") + "\",";
  json += "\"secondLow60DayFullRange\":\"" + getLayoutValue("secondLow60DayFullRange") + "\",";
  json += "\"currentStartsAt\":\"" + currentStartsAt + "\",";
  json += "\"espNow\":\"" + getCurrentIsoPrefix() + "\",";
  json += "\"todayDate\":\"" + getLocalDatePrefix() + "\",";
  json += "\"quarterCount\":" + String(quarterCount) + ",";
  json += "\"apiUpdateMinutes\":" + String(apiUpdateMinutes) + ",";
  json += "\"displayRefreshSeconds\":" + String(displayRefreshSeconds) + ",";
  json += "\"ledRingEnabled\":" + String(ledRingEnabled ? "true" : "false") + ",";
  json += "\"ledBrightness\":" + String(ledBrightness) + ",";
  json += "\"ledRefreshSeconds\":" + String(ledRefreshSeconds) + ",";
  json += "\"ledActiveCount\":" + String(ledActiveCount) + ",";
  json += "\"ledYellowCent\":" + String(ledYellowCent) + ",";
  json += "\"ledRedCent\":" + String(ledRedCent) + ",";
  json += "\"selectedHomeId\":\"" + jsonEscapeValue(selectedHomeId) + "\",";
  json += "\"homeCount\":" + String(homeCount) + ",";
  json += "\"wifiSsid\":\"" + jsonEscapeValue(wifiSsid) + "\",";
  json += "\"apMode\":" + String(apMode ? "true" : "false") + ",";
  json += "\"tlsVerified\":" + String(tibberRootCaPem.length() > 0 ? "true" : "false") + ",";

  json += "\"ip\":\"";
  if (apMode) {
    json += WiFi.softAPIP().toString();
  } else {
    json += WiFi.localIP().toString();
  }
  json += "\",";

  json += "\"error\":\"" + jsonEscapeValue(lastError) + "\"";

  json += "}";

  server.send(200, "application/json", json);
}

// -----------------------------------------------------------------------------
// HTML Helpers
// -----------------------------------------------------------------------------

void handleFaviconSvg() {
  String color = "#4ade80";

  if (quarterCount > 0 && metricCurrent15 >= 0) {
    int nowCent = euroToCentRounded(metricCurrent15);
    if (nowCent >= ledRedCent) {
      color = "#fb7185";
    } else if (nowCent >= ledYellowCent) {
      color = "#facc15";
    }
  }

  String svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>";
  svg += "<circle cx='16' cy='16' r='14' fill='" + color + "' stroke='#0b1224' stroke-width='2.5'/>";
  svg += "</svg>";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "image/svg+xml", svg);
}

void handleStyleCss() {
  String css = R"CSS(
:root{--bg1:#f2f2f7;--bg2:#f2f2f7;--radial1:transparent;--radial2:transparent;--panel:#ffffff;--panel2:#f2f2f7;--card:#ffffff;--line:rgba(60,60,67,.13);--text:#000000;--muted:#8e8e93;--accent:#007AFF;--accent2:#007AFF;--pink:#FF2D55;--purple:#AF52DE;--orange:#FF9500;--danger:#FF3B30;--ok:#34C759;--warn:#FF9500;--radius:20px;--ease:cubic-bezier(.2,.8,.2,1);--surface:#ffffff;--surface-nav:rgba(255,255,255,.92);--surface-border:rgba(60,60,67,.13);--overlay-hover:rgba(120,120,128,.08);--overlay-hover-strong:rgba(120,120,128,.16);--overlay-faint:rgba(120,120,128,.04);--overlay-row:rgba(120,120,128,.04);--metric-bg1:#ffffff;--metric-bg2:#ffffff;--input-bg:#ffffff;--badge-bg:rgba(120,120,128,.12);--toggle-off:#e5e5ea;--divider:rgba(60,60,67,.13);--shadow-soft:rgba(0,0,0,.05);--shadow-hover:rgba(0,0,0,.08);--shadow-float:rgba(0,0,0,.15);--btn-muted:#8e8e93;--accent-tint-bg:rgba(0,122,255,.12);--accent-tint-border:rgba(0,122,255,.35);--float-bg:rgba(255,255,255,.98);--float-border:rgba(60,60,67,.13);--okb-bg:rgba(52,199,89,.15);--okb-text:#248A3D;--errb-bg:rgba(255,59,48,.14);--errb-text:#D70015;--warnb-bg:rgba(255,149,0,.16);--warnb-text:#B25000;--infob-bg:rgba(0,122,255,.12);--infob-text:#0040DD;--purpleb-bg:rgba(175,82,222,.14);--purpleb-text:#8944AB;--alert-ok-bg:rgba(52,199,89,.10);--alert-ok-border:rgba(52,199,89,.28);--alert-err-bg:rgba(255,59,48,.09);--alert-err-border:rgba(255,59,48,.28);}
:root[data-theme="dark"]{--bg1:#000000;--bg2:#000000;--radial1:transparent;--radial2:transparent;--panel:#1c1c1e;--panel2:#2c2c2e;--card:#1c1c1e;--line:rgba(84,84,88,.65);--text:#ffffff;--muted:#8e8e93;--accent:#0A84FF;--accent2:#0A84FF;--pink:#FF375F;--purple:#BF5AF2;--orange:#FF9F0A;--danger:#FF453A;--ok:#30D158;--warn:#FF9F0A;--surface:#1c1c1e;--surface-nav:rgba(28,28,30,.94);--surface-border:rgba(84,84,88,.65);--overlay-hover:rgba(120,120,128,.18);--overlay-hover-strong:rgba(120,120,128,.36);--overlay-faint:rgba(120,120,128,.08);--overlay-row:rgba(255,255,255,.03);--metric-bg1:#1c1c1e;--metric-bg2:#1c1c1e;--input-bg:#1c1c1e;--badge-bg:rgba(120,120,128,.24);--toggle-off:#39393d;--divider:rgba(84,84,88,.65);--shadow-soft:rgba(0,0,0,.3);--shadow-hover:rgba(0,0,0,.4);--shadow-float:rgba(0,0,0,.6);--btn-muted:#8e8e93;--accent-tint-bg:rgba(10,132,255,.18);--accent-tint-border:rgba(10,132,255,.4);--float-bg:rgba(44,44,46,.98);--float-border:rgba(84,84,88,.65);--okb-bg:rgba(48,209,88,.18);--okb-text:#7FEB9D;--errb-bg:rgba(255,69,58,.18);--errb-text:#FF6961;--warnb-bg:rgba(255,159,10,.18);--warnb-text:#FFC97A;--infob-bg:rgba(10,132,255,.20);--infob-text:#7FBFFF;--purpleb-bg:rgba(191,90,242,.18);--purpleb-text:#DAA5F5;--alert-ok-bg:rgba(48,209,88,.14);--alert-ok-border:rgba(48,209,88,.32);--alert-err-bg:rgba(255,69,58,.14);--alert-err-border:rgba(255,69,58,.32);}
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'SF Pro Display','Inter','Segoe UI',Roboto,sans-serif;background:var(--bg1);color:var(--text);padding:0;-webkit-tap-highlight-color:transparent;transition:background .2s var(--ease),color .2s var(--ease);font-weight:400;letter-spacing:-0.01em;position:relative}
/* SmartFin-Style: dezenter gruener Ambient-Blob oben rechts (wie im Screenshot) */
body::after{content:'';position:fixed;top:-20%;right:-10%;width:60vmax;height:60vmax;border-radius:50%;background:radial-gradient(circle,rgba(52,199,89,.18) 0%,transparent 65%);pointer-events:none;z-index:0}
:root[data-theme="dark"] body::after{background:radial-gradient(circle,rgba(48,209,88,.12) 0%,transparent 65%)}
.shell{position:relative;z-index:1}
h1,h2,h3{line-height:1.15;letter-spacing:-0.03em;font-weight:600}
.shell{width:min(1120px,calc(100% - 24px));margin:0 auto;padding:18px 0 96px}
.hero{background:var(--card);border:0;border-radius:22px;padding:20px 22px;margin:14px 0 16px;box-shadow:0 1px 2px var(--shadow-soft)}
.hero h1{margin:0;font-size:clamp(22px,4vw,32px);font-weight:700;letter-spacing:-0.04em}
.hero p{margin:6px 0 0;color:var(--muted);font-size:14px;font-weight:500}
.nav{position:sticky;top:0;z-index:10;background:var(--surface-nav);backdrop-filter:blur(20px) saturate(180%);-webkit-backdrop-filter:blur(20px) saturate(180%);border:1px solid var(--surface-border);border-radius:18px;padding:8px;margin:0 0 16px;display:flex;gap:6px;align-items:center;overflow-x:auto;scroll-snap-type:x proximity;-webkit-overflow-scrolling:touch;scrollbar-width:none}
.nav::-webkit-scrollbar{display:none}
.nav a{color:var(--text);text-decoration:none;flex:none;scroll-snap-align:start}
.nav .badge{flex:none;margin-right:2px}
.navDivider{width:1px;align-self:stretch;background:var(--divider);margin:6px 4px;flex:none}
.navbtn{appearance:none;display:inline-flex;align-items:center;gap:10px;border:none;background:transparent;color:var(--text);padding:9px 14px;min-height:40px;border-radius:12px;font-weight:500;font-size:14px;cursor:pointer;white-space:nowrap;transition:background .15s var(--ease),color .15s var(--ease);letter-spacing:-0.01em}
.navbtn svg{flex:none;width:18px;height:18px;color:var(--muted)}
.navbtn:hover{background:var(--overlay-hover)}
.navbtn.active{background:var(--accent-tint-bg);color:var(--accent)}
.navbtn.active svg{color:var(--accent)}
.themeToggle{appearance:none;display:inline-flex;align-items:center;justify-content:center;gap:8px;border:1px solid var(--surface-border);background:transparent;color:var(--muted);padding:9px 15px;min-height:40px;border-radius:999px;font-weight:700;font-size:13px;cursor:pointer;white-space:nowrap;transition:background .15s var(--ease),color .15s var(--ease)}
.themeToggle:hover{background:var(--overlay-hover);color:var(--text)}
.themeToggle svg{flex:none;width:16px;height:16px}
.themeToggle .iconSun{display:none}
:root[data-theme="dark"] .themeToggle .iconSun{display:inline-flex}
:root[data-theme="dark"] .themeToggle .iconMoon{display:none}
.navFooter{margin-top:auto;padding-top:10px;display:flex;flex-direction:column;align-items:center;gap:2px;color:#9ca3af;font-size:9px;line-height:1.3;text-align:center;opacity:.5;font-weight:400;letter-spacing:.2px}
.navFooter a{color:inherit;text-decoration:none;transition:opacity .15s var(--ease)}
.navFooter a:hover{opacity:1;text-decoration:underline}
button,.btn{appearance:none;border:none;background:var(--accent);color:#fff;padding:11px 20px;min-height:44px;border-radius:14px;font-weight:600;font-size:15px;cursor:pointer;box-shadow:none;transition:transform .15s var(--ease),opacity .15s var(--ease),background .15s var(--ease);white-space:nowrap;letter-spacing:-0.01em}
button:hover{opacity:.86}
button:active{transform:scale(.97);opacity:.75}
button:focus-visible,a:focus-visible,input:focus-visible,select:focus-visible,textarea:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
button:disabled{opacity:.4;cursor:not-allowed;transform:none}
button.secondary{background:var(--overlay-hover);color:var(--accent);border:none;box-shadow:none;font-weight:600}
button.secondary:hover{background:var(--overlay-hover-strong);opacity:1}
button.danger{background:var(--danger);color:#fff}
.toggle{position:relative;display:inline-block;width:46px;height:26px;flex:none;cursor:pointer}
.toggle input{position:absolute;opacity:0;width:0;height:0}
.toggleSlider{position:absolute;inset:0;background:var(--toggle-off);border:1px solid var(--line);border-radius:999px;transition:background .2s var(--ease),border-color .2s var(--ease)}
.toggleSlider:before{content:'';position:absolute;left:2px;top:1px;width:22px;height:22px;background:#fff;border-radius:50%;transition:transform .2s var(--ease);box-shadow:0 2px 4px rgba(0,0,0,.15)}
.toggle input:checked+.toggleSlider{background:var(--ok);border-color:transparent}
.toggle input:checked+.toggleSlider:before{transform:translateX(20px)}
.toggle input:focus-visible+.toggleSlider{outline:2px solid var(--accent);outline-offset:2px}
.toggleRow{display:flex;align-items:center;justify-content:space-between;gap:10px}
.toggleRow label:first-child{color:var(--muted);font-size:13px}
.status-dot{display:inline-block;width:10px;height:10px;border-radius:50%;background-color:#ef4444;margin-right:6px;position:relative}
.status-dot.ok{background-color:#22c55e}
.status-dot.warn{background-color:#f59e0b}
.status-dot.pulse:after{content:'';position:absolute;inset:-4px;border-radius:50%;background-color:inherit;opacity:.55;animation:statusPulse 1.8s ease-out infinite}
@keyframes statusPulse{0%{transform:scale(.5);opacity:.55}100%{transform:scale(2.4);opacity:0}}
.gridCards{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;margin:14px 0}
.metric{background:var(--card);border:0;border-radius:16px;padding:16px 18px;text-align:left;transition:transform .15s var(--ease),box-shadow .15s var(--ease);box-shadow:0 1px 2px var(--shadow-soft)}
.metric:hover{transform:translateY(-1px);box-shadow:0 4px 12px var(--shadow-hover)}
.metric .label{color:var(--muted);font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:0.4px;margin-bottom:6px}
.metric .value{font-size:clamp(22px,3.5vw,32px);font-weight:700;letter-spacing:-0.04em;color:var(--text);font-variant-numeric:tabular-nums}
.metric .sub{color:var(--muted);font-size:12px;font-weight:500;margin-top:4px}
.card{background:var(--card);border:0;border-radius:22px;padding:20px 22px;margin:14px 0;box-shadow:0 1px 2px var(--shadow-soft)}
.card h2{margin:0 0 14px;font-size:18px;font-weight:600;letter-spacing:-0.02em;text-align:left}
.twoCol{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.formGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;text-align:left}
.field label:not(.toggle){display:block;color:var(--muted);font-size:13px;margin:0 0 6px}
input,select,textarea{width:100%;min-height:44px;background:var(--input-bg);color:var(--text);border:1px solid var(--line);font-size:16px;padding:11px 14px;border-radius:12px;outline:none;transition:border-color .15s var(--ease),box-shadow .15s var(--ease);font-family:inherit;letter-spacing:-0.01em}
textarea{min-height:90px}
input:focus,select:focus,textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-tint-bg)}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px;justify-content:flex-start}
.small{color:var(--muted);font-size:13px}
.err{color:var(--errb-text);background:var(--alert-err-bg);border:1px solid var(--alert-err-border);padding:10px 12px;border-radius:14px}
.ok{color:var(--okb-text);background:var(--alert-ok-bg);border:1px solid var(--alert-ok-border);padding:10px 12px;border-radius:14px}
.badge{display:inline-flex;align-items:center;gap:6px;border-radius:999px;padding:5px 11px;font-size:12px;font-weight:600;border:0;background:var(--badge-bg);color:var(--text);letter-spacing:-0.01em;transition:background .2s var(--ease),color .2s var(--ease)}
.badge.okb{background:var(--okb-bg);color:var(--okb-text)}.badge.errb{background:var(--errb-bg);color:var(--errb-text)}.badge.warnb{background:var(--warnb-bg);color:var(--warnb-text)}.badge.infob{background:var(--infob-bg);color:var(--infob-text)}.badge.purpleb{background:var(--purpleb-bg);color:var(--purpleb-text)}
table{width:100%;border-collapse:separate;border-spacing:0;margin-top:12px;font-size:13px;overflow:hidden;border:1px solid var(--line);border-radius:16px}
td,th{border-bottom:1px solid var(--line);padding:9px 10px;text-align:left}tr:last-child td{border-bottom:0}tr:nth-child(even){background:var(--overlay-row)}
svg{background:var(--overlay-faint);border:1px solid var(--line);border-radius:18px;margin-top:8px;width:100%;height:320px}
.gaugeWrap{display:flex;justify-content:center;margin:6px 0 4px}
.gaugeWrap svg{width:240px;max-width:100%;height:auto;background:transparent;border:0;margin:0}
/* Ring-Gauge (SmartFin-Style) */
.priceRing{width:100%;max-width:340px;margin:0 auto;text-align:center;--pr-track:rgba(0,0,0,.08)}
:root[data-theme="dark"] .priceRing{--pr-track:rgba(255,255,255,.12)}
.priceRing.kiosk{--pr-track:rgba(255,255,255,.15)}
.pr-time{font-size:clamp(11px,1.2vh,13px);color:var(--muted);font-weight:500;margin-bottom:4px;font-variant-numeric:tabular-nums}
/* .pr-time sitzt auf allen drei Seiten (Startseite, Kiosk 1, Kiosk 2) im
   Preis-Gauge-Widget, das per Drag/Resize seine Groesse aendern kann - die
   vh-Regel oben skaliert nur mit der Fensterhoehe, nicht mit der eigenen
   Widget-Breite. .kw traegt ueberall container-type:inline-size, daher reicht
   ein einziger, ueberall gueltiger cqi-Override statt drei Seiten-Kopien. */
.kw .pr-time{font-size:clamp(9px,2.8cqi,13px)}
.priceRing svg{width:100%;height:auto;max-width:280px}
/* Live-Power Bar bleibt mit pg-* Klassen */
.priceGauge{width:100%;max-width:420px;margin:0 auto;padding:8px 4px;text-align:center;font-variant-numeric:tabular-nums}
.priceGauge .pg-label{font-size:clamp(10px,1.4vh,12px);font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;line-height:1}
.priceGauge .pg-time{font-size:clamp(11px,1.4vh,13px);color:var(--muted);font-weight:500;margin-top:3px;font-variant-numeric:tabular-nums}
.priceGauge .pg-value{font-size:clamp(64px,14vh,140px);font-weight:800;line-height:1;letter-spacing:-0.05em;color:var(--text);margin:clamp(6px,1.4vh,14px) 0 clamp(2px,0.5vh,6px)}
.priceGauge .pg-unit{font-size:clamp(11px,1.4vh,14px);color:var(--muted);font-weight:500;margin-bottom:clamp(12px,2vh,20px)}
.priceGauge .pg-track{position:relative;height:clamp(6px,1vh,12px);border-radius:999px;background:var(--overlay-hover);margin:0 auto;max-width:88%;overflow:visible}
.priceGauge .pg-fill{position:absolute;left:0;top:0;bottom:0;border-radius:999px;background:linear-gradient(90deg,#34C759,#30D158);transition:width .3s var(--ease),background .3s var(--ease)}
.priceGauge.pg-mid .pg-fill{background:linear-gradient(90deg,#34C759,#FF9500)}
.priceGauge.pg-bad .pg-fill{background:linear-gradient(90deg,#FF9500,#FF3B30)}
.priceGauge .pg-marker{position:absolute;top:50%;width:clamp(14px,1.8vh,20px);height:clamp(14px,1.8vh,20px);border-radius:50%;background:#fff;transform:translate(-50%,-50%);box-shadow:0 0 0 3px var(--card),0 2px 4px rgba(0,0,0,.15);transition:left .3s var(--ease)}
.priceGauge .pg-scale{display:flex;justify-content:space-between;margin:8px auto 0;max-width:88%;font-size:clamp(10px,1.2vh,12px);font-weight:600;color:var(--muted)}
.priceGauge .pg-zone{color:#34C759}
.priceGauge.pg-mid .pg-zone{color:#FF9500}
.priceGauge.pg-bad .pg-zone{color:#FF3B30}
.live-power{margin:16px auto;padding:16px 22px;background:var(--panel2);border-radius:20px;max-width:420px;text-align:center}
.live-power:empty{display:none}
.live-power .pg-value{font-size:clamp(28px,6vh,52px);margin:6px 0}
.priceGauge .pg-fill.zc{background:linear-gradient(90deg,#34C759,#30D158)}
.priceGauge .pg-fill.zm{background:linear-gradient(90deg,#FF9500,#FF9F0A)}
.priceGauge .pg-fill.ze{background:linear-gradient(90deg,#FF9500,#FF3B30)}
a{color:var(--accent);text-decoration:none}
code{background:#0b1224;color:#e2e8f0;border:1px solid var(--line);border-radius:8px;padding:2px 6px;font-size:12px}
details summary{cursor:pointer;color:var(--text);list-style:none}details summary::-webkit-details-marker{display:none}details summary h2{display:inline-block!important}details summary::before{content:'▸ ';color:var(--accent);font-size:14px}details[open] summary::before{content:'▾ '}
.layoutPanel{background:var(--surface);border:1px solid var(--surface-border);border-radius:22px;padding:16px;margin:16px 0;text-align:left}
.layoutPanel h2{text-align:left;margin-top:0}
.presetGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.presetGrid form{margin:0}.presetGrid button{width:100%;min-height:48px}
.formSection{border:1px solid var(--line);border-radius:18px;background:var(--overlay-faint);padding:12px;margin-top:12px}
.panelTitle{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}.panelTitle h2{margin:0}
.btnMuted{background:var(--btn-muted)!important}.btnSuccess{background:linear-gradient(135deg,#22c55e,#16a34a)!important}
.dangerZone{border-color:rgba(220,38,38,.3);background:rgba(220,38,38,.05)}
.toastHost{position:fixed;left:50%;bottom:max(18px,env(safe-area-inset-bottom));transform:translateX(-50%);z-index:99999;display:flex;flex-direction:column-reverse;gap:8px;align-items:center;pointer-events:none;width:min(420px,92vw)}
.toast{pointer-events:auto;width:100%;padding:12px 16px;border-radius:14px;font-size:14px;font-weight:700;text-align:center;box-shadow:0 14px 34px rgba(15,23,42,.28);border:1px solid rgba(255,255,255,.14);opacity:0;transform:translateY(12px) scale(.97);transition:opacity .22s var(--ease),transform .22s var(--ease)}
.toast.show{opacity:1;transform:translateY(0) scale(1)}
.toast.ok{background:rgba(22,163,74,.96);color:#eafff1}
.toast.err{background:rgba(225,29,72,.96);color:#fff1f2}
.toast.info{background:rgba(37,99,235,.96);color:#eef4ff}
@keyframes fadeInUp{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:translateY(0)}}
.hero{animation:fadeInUp .5s var(--ease) both}
.card,.layoutPanel{animation:fadeInUp .5s var(--ease) both;animation-delay:.05s}
@media(prefers-reduced-motion:reduce){.hero,.card,.layoutPanel{animation:none}}
@media(hover:none){button:hover{filter:none}.metric:hover{border-color:var(--surface-border)}.navbtn:hover{background:transparent;color:var(--muted)}button.secondary:hover{background:transparent;border-color:var(--line)}}
@media(max-width:760px){.twoCol{grid-template-columns:1fr}.shell{width:min(100% - 16px,1120px)}.hero{padding:18px}.card{padding:14px}.nav{justify-content:flex-start}}
@media(min-width:901px){body{padding-left:244px}.nav{position:fixed;left:24px;top:24px;bottom:24px;width:196px;flex-direction:column;align-items:stretch;overflow-y:auto;overflow-x:hidden;padding:14px 10px;gap:4px}.nav .badge{margin:0 0 10px;justify-content:center}.navDivider{width:100%;height:1px;align-self:auto;margin:8px 0}.navbtn{width:100%;justify-content:flex-start}}
)CSS";

  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send(200, "text/css", css);
}

void handleAppJs() {
  String js = R"JS(
function toggleTheme() {
  var isDark = document.documentElement.getAttribute('data-theme') === 'dark';
  if (isDark) {
    document.documentElement.removeAttribute('data-theme');
    try { localStorage.setItem('theme', 'light'); } catch (e) {}
  } else {
    document.documentElement.setAttribute('data-theme', 'dark');
    try { localStorage.setItem('theme', 'dark'); } catch (e) {}
  }
}
(function(){
  var el = document.getElementById('livePowerBadge');
  if (!el) return;
  // WICHTIG: dieser Poller darf NICHT einfach data.text als kompletten
  // innerHTML-Ersatz einsetzen - der Server rendert beim ersten Laden die
  // volle Karte (Label + grosser Wert + Farbbalken + Skala, siehe
  // liveHomeText in handleRoot()), ein blosser Text-Dump wuerde das nach
  // 2.5s durch eine einzelne Zeile ersetzen und den Rest der Box leer
  // zuruecklassen. Stattdessen dieselbe Struktur wie serverseitig neu
  // aufbauen.
  function poll(){
    fetch('/livepower').then(function(r){ return r.json(); }).then(function(data){
      if (!data.text) { el.innerHTML = ''; return; }
      var pct = (typeof data.pct === 'number' && data.pct >= 0) ? data.pct : 0;
      var zone = (typeof data.zone === 'string') ? data.zone : 'zc';
      var max = (typeof data.max === 'number' && data.max > 0) ? data.max : 10;
      var maxStr = (max === Math.floor(max)) ? max.toFixed(0) : max.toFixed(1);
      el.innerHTML = "<div class='pg-label'>&#9889; Aktueller Verbrauch</div><div class='pg-value'>" + (data.value || '') + "</div><div class='pg-track'><div class='pg-fill " + zone + "' style='width:" + pct.toFixed(1) + "%'></div><div class='pg-marker' style='left:" + pct.toFixed(1) + "%'></div></div><div class='pg-scale'><span>0</span><span>" + maxStr + " kW</span></div>";
    }).catch(function(e){ /* naechster Versuch beim naechsten Intervall */ });
  }
  poll();
  setInterval(poll, 2500);
})();
function showToast(msg, type) {
  type = type || 'info';
  var host = document.getElementById('toastHost');
  if (!host) return;
  var t = document.createElement('div');
  t.className = 'toast ' + type;
  t.textContent = msg;
  host.appendChild(t);
  requestAnimationFrame(function(){ t.classList.add('show'); });
  setTimeout(function(){
    t.classList.remove('show');
    setTimeout(function(){ t.remove(); }, 250);
  }, 3200);
}
(function(){
  var p = new URLSearchParams(location.search);
  if (p.has('saved')) {
    var ok = p.get('saved') !== '0';
    showToast(ok ? 'Gespeichert' : 'Fehler beim Speichern', ok ? 'ok' : 'err');
    history.replaceState(null, '', location.pathname);
  }
})();
document.querySelectorAll('form').forEach(function(f){
  f.addEventListener('submit', function(){
    var btn = f.querySelector('button[type="submit"]');
    if (btn && !btn.disabled) {
      btn.dataset.origText = btn.innerText;
      btn.innerText = 'Speichere...';
      // WICHTIG: btn.disabled erst NACH diesem Tick setzen (setTimeout 0).
      // Bei submit-buttons mit name/value (z.B. name='formType' value='xyz')
      // wird ein waehrend des submit-Events deaktivierter Button aus den
      // gesendeten Formulardaten ausgeschlossen - der Server erhaelt dann
      // ein leeres formType und keine der Aktionen greift, ohne Fehlermeldung.
      setTimeout(function(){ btn.disabled = true; }, 0);
    }
  });
});
function fwBadge(cls, text, title) {
  var b = document.getElementById('fwVersionBadge');
  if (!b) return;
  b.className = 'badge' + (cls ? ' ' + cls : '');
  b.innerText = text;
  if (title) b.title = title;
}
function applyFwResult(j) {
  if (!j) return;
  if (!j.ok) {
    fwBadge('errb', 'v' + j.currentVersion + ' ⚠', 'Update-Check fehlgeschlagen: ' + (j.error || 'unbekannter Fehler') + ' - klicke zum erneuten Versuch');
  } else if (j.updateAvailable) {
    fwBadge('warnb', 'v' + j.currentVersion + ' → ' + j.latestVersion, 'Update verfügbar (' + j.latestVersion + ') - siehe Konto-Seite');
  } else {
    fwBadge('okb', 'v' + j.currentVersion, 'Firmware ist aktuell');
  }
}
(function(){
  // Seiten laden alle 60s per meta-refresh neu (siehe htmlHeader), daher den
  // zuletzt bekannten Check-Stand aus localStorage sofort anzeigen, statt bei
  // jedem Reload kurz auf den neutralen Standardzustand zurueckzufallen.
  try {
    var cached = localStorage.getItem('fwCheckResult');
    if (cached) applyFwResult(JSON.parse(cached));
  } catch (e) {}
})();
async function checkFirmwareVersion(force) {
  var last = 0;
  try { last = parseInt(localStorage.getItem('fwCheckAt') || '0', 10); } catch (e) {}
  if (!force && Date.now() - last < 10 * 60 * 1000) return;
  if (force) fwBadge('warnb', 'Prüfe...', 'Frage GitHub nach der neuesten Version...');
  try {
    const r = await fetch('/versioncheck', { cache: 'no-store' });
    const j = await r.json();
    applyFwResult(j);
    try {
      localStorage.setItem('fwCheckAt', String(Date.now()));
      localStorage.setItem('fwCheckResult', JSON.stringify(j));
    } catch (e) {}
  } catch (e) {
    fwBadge('errb', 'v?  ⚠', 'Verbindung zum Geraet fehlgeschlagen - klicke zum erneuten Versuch');
  }
}
checkFirmwareVersion(false);
setInterval(function(){ checkFirmwareVersion(false); }, 10 * 60 * 1000);
(function(){
  var b = document.getElementById('fwVersionBadge');
  if (b) {
    b.style.cursor = 'pointer';
    b.addEventListener('click', function(){ checkFirmwareVersion(true); });
  }
})();
)JS";

  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send(200, "application/javascript", js);
}

// Gemeinsame Drag/Resize/Auto-Reflow-Engine fuer alle Widget-Grids (Kiosk-
// Layout-Editor und spaeter die Startseite) - EINE Datei statt mehrfach
// inline eingebetteter Kopien, damit der Flash-Speicher nicht mehrfach
// belastet wird. Design: Widgets bleiben waehrend des normalen Betriebs
// stinknormale CSS-Grid-Items (grid-column/grid-row). Nur waehrend einer
// aktiven Drag/Resize-Geste weicht das jeweilige Element davon ab:
//  - Drag: bekommt zusaetzlich ein `transform: translate3d(...)` fuer die
//    Bewegung (rein visuell, loest kein Reflow der Nachbarn aus), die
//    tatsaechliche Grid-Position wird erst beim Loslassen uebernommen.
//  - Resize: wechselt kurzzeitig auf `position:absolute` mit expliziten
//    Pixel-Massen (statt eines `transform:scale`), damit der Browser das
//    Element wirklich neu layoutet und Container-Query-basierte Inhalte
//    (siehe .hw-* Widgets) waehrend des Ziehens live mitskalieren - ein
//    reines `scale()` wuerde nur das gerenderte Bild verzerren, keine echte
//    Container-Groessenaenderung ausloesen.
// Beim Loslassen: einfacher "Schubs"-Kollisions-Resolver verschiebt
// ueberlappende Nachbarn, die per FLIP-Technik (Rect vorher/nachher, dann
// inverses Transform auf 0 animiert) sanft an ihre neue Position gleiten.
void handleWidgetEngineJs() {
  String js = R"JS(
(function(global){

function wgClamp(v, lo, hi){ return Math.max(lo, Math.min(hi, v)); }

function wgRectsOverlap(a, b) {
  return a.colStart < b.colStart + b.colSpan &&
         a.colStart + a.colSpan > b.colStart &&
         a.rowStart < b.rowStart + b.rowSpan &&
         a.rowStart + a.rowSpan > b.rowStart;
}

// Verschiebt ueberlappende Nachbarn von `moving` aus dem Weg (max. 3 Durchlaeufe
// wegen Kettenreaktionen). Bevorzugt ein Verschieben nach unten; nur wenn das
// nicht in das Grid passt, wird stattdessen nach rechts verschoben. Mutiert
// `items` direkt, gibt die Indizes der tatsaechlich veraenderten Eintraege zurueck.
function wgResolveCollisions(items, movingIndex, cols, rows) {
  var changed = {};
  var moving = items[movingIndex];
  for (var pass = 0; pass < 3; pass++) {
    var any = false;
    for (var i = 0; i < items.length; i++) {
      if (i === movingIndex) continue;
      var it = items[i];
      if (!wgRectsOverlap(moving, it)) continue;
      var newRowStart = moving.rowStart + moving.rowSpan;
      if (newRowStart + it.rowSpan - 1 <= rows) {
        it.rowStart = newRowStart;
      } else {
        it.colStart = wgClamp(moving.colStart + moving.colSpan, 1, cols - it.colSpan + 1);
      }
      changed[i] = true;
      any = true;
    }
    if (!any) break;
  }
  return Object.keys(changed).map(Number);
}

// FLIP-Animation: `beforeRects` (vom Aufrufer VOR der Grid-Aenderung erfasst,
// je Index ein getBoundingClientRect()) wird gegen die aktuelle (neue)
// Position verglichen; die Differenz wird als Transform aufgebracht und dann
// auf 0 animiert, damit das Element sichtbar an seinen neuen Platz gleitet.
function wgFlipAnimate(getEl, indexes, beforeRects) {
  indexes.forEach(function(i){
    var el = getEl(i);
    var before = beforeRects[i];
    if (!el || !before) return;
    var after = el.getBoundingClientRect();
    var dx = before.left - after.left;
    var dy = before.top - after.top;
    if (!dx && !dy) return;
    el.style.transition = 'none';
    el.style.transform = 'translate3d(' + dx + 'px,' + dy + 'px,0)';
    requestAnimationFrame(function(){
      el.style.transition = 'transform .28s cubic-bezier(.22,.61,.36,1)';
      el.style.transform = '';
    });
  });
}

// Verdrahtet Drag+Resize fuer eine Grid-Instanz. opts:
//  getEl(i)          -> DOM-Element von Widget i (muss bereits im DOM stehen)
//  getItems()        -> lebendes Array {colStart,colSpan,rowStart,rowSpan,...}
//  getGrid()         -> {cols, rows}
//  cellSize()        -> {w,h} einer Zelle in px, jedes Mal frisch gemessen
//  applyLayout(i)    -> Aufrufer setzt grid-column/grid-row von Element i neu
//  onCommit(indexes) -> einmalig am Gestenende mit allen veraenderten Indizes
//                       (inkl. weggeschubster Nachbarn) - Aufrufer speichert
function wgCreateGridController(opts) {

  function finishGesture(i) {
    var items = opts.getItems();
    var grid = opts.getGrid();

    var beforeRects = {};
    items.forEach(function(_, idx){
      var e2 = opts.getEl(idx);
      if (e2) beforeRects[idx] = e2.getBoundingClientRect();
    });

    var shoved = wgResolveCollisions(items, i, grid.cols, grid.rows);
    var changed = [i].concat(shoved);
    changed.forEach(function(idx){ opts.applyLayout(idx); });

    // Nur die weggeschubsten Nachbarn brauchen die FLIP-Animation - das
    // gezogene/skalierte Element selbst kommt schon animiert von seiner
    // eigenen Drag/Resize-Geste an seiner finalen Position an.
    wgFlipAnimate(opts.getEl, shoved, beforeRects);

    if (opts.onCommit) opts.onCommit(changed);
  }

  function startDrag(e, i) {
    e.preventDefault();
    var el = opts.getEl(i);
    var item = opts.getItems()[i];
    var grid = opts.getGrid();
    var cell = opts.cellSize();
    var startColStart = item.colStart, startRowStart = item.rowStart;
    var startX = e.clientX, startY = e.clientY;
    var latestDx = 0, latestDy = 0, rafId = null;

    el.classList.add('wg-dragging');
    el.style.willChange = 'transform';

    function tick() {
      el.style.transform = 'translate3d(' + latestDx + 'px,' + latestDy + 'px,0)';
      rafId = null;
    }
    function onMove(ev) {
      latestDx = ev.clientX - startX;
      latestDy = ev.clientY - startY;
      if (rafId === null) rafId = requestAnimationFrame(tick);
      var dc = Math.round(latestDx / cell.w);
      var dr = Math.round(latestDy / cell.h);
      item.colStart = wgClamp(startColStart + dc, 1, grid.cols - item.colSpan + 1);
      item.rowStart = wgClamp(startRowStart + dr, 1, grid.rows - item.rowSpan + 1);
    }
    function onUp() {
      document.removeEventListener('pointermove', onMove);
      document.removeEventListener('pointerup', onUp);
      if (rafId !== null) cancelAnimationFrame(rafId);
      el.classList.remove('wg-dragging');
      el.style.willChange = '';
      el.style.transform = '';
      finishGesture(i);
    }
    document.addEventListener('pointermove', onMove);
    document.addEventListener('pointerup', onUp);
  }

  function startResize(e, i) {
    e.preventDefault();
    e.stopPropagation();
    var el = opts.getEl(i);
    var item = opts.getItems()[i];
    var grid = opts.getGrid();
    var cell = opts.cellSize();
    var rect = el.getBoundingClientRect();
    var canvasRect = el.offsetParent.getBoundingClientRect();
    var startColSpan = item.colSpan, startRowSpan = item.rowSpan;
    var startX = e.clientX, startY = e.clientY;
    var latestW = rect.width, latestH = rect.height, rafId = null;

    el.classList.add('wg-resizing');
    el.style.gridColumn = 'unset';
    el.style.gridRow = 'unset';
    el.style.position = 'absolute';
    el.style.left = (rect.left - canvasRect.left) + 'px';
    el.style.top = (rect.top - canvasRect.top) + 'px';
    el.style.width = rect.width + 'px';
    el.style.height = rect.height + 'px';
    el.style.willChange = 'width, height';

    function tick() {
      el.style.width = latestW + 'px';
      el.style.height = latestH + 'px';
      rafId = null;
    }
    function onMove(ev) {
      var dx = ev.clientX - startX, dy = ev.clientY - startY;
      latestW = Math.max(cell.w * 0.6, rect.width + dx);
      latestH = Math.max(cell.h * 0.6, rect.height + dy);
      if (rafId === null) rafId = requestAnimationFrame(tick);
      var dc = Math.round(dx / cell.w);
      var dr = Math.round(dy / cell.h);
      item.colSpan = wgClamp(startColSpan + dc, 1, grid.cols - item.colStart + 1);
      item.rowSpan = wgClamp(startRowSpan + dr, 1, grid.rows - item.rowStart + 1);
    }
    function onUp() {
      document.removeEventListener('pointermove', onMove);
      document.removeEventListener('pointerup', onUp);
      if (rafId !== null) cancelAnimationFrame(rafId);
      el.classList.remove('wg-resizing');
      el.style.position = '';
      el.style.left = '';
      el.style.top = '';
      el.style.width = '';
      el.style.height = '';
      el.style.willChange = '';
      finishGesture(i);
    }
    document.addEventListener('pointermove', onMove);
    document.addEventListener('pointerup', onUp);
  }

  return { startDrag: startDrag, startResize: startResize };
}

// Fadenkreuz zum "Abfahren" eines Preisdiagramms (buildSvgChart(), immer
// viewBox 0 0 760 320) mit Finger oder Maus - urspruenglich nur auf Kiosk-
// Seite 1 vorhanden, hier verallgemeinert fuer Wiederverwendung auf Kiosk 2
// und der Startseite. svgId/wrapId/tooltipId sind DOM-IDs, getPoints() muss
// das aktuelle Punkte-Array (aus buildChartPointsJson()) liefern - als
// Funktion statt fixem Array, da manche Seiten das Diagramm periodisch per
// AJAX austauschen und dabei neue Punktdaten laden. reattach() muss nach
// jedem so einem Austausch (innerHTML-Ersetzung des SVG) erneut aufgerufen
// werden, da das alte SVG-Element inkl. Linie/Punkt dabei verloren geht.
function wgCreateChartCrosshair(svgId, wrapId, tooltipId, getPoints) {
  var svg = null, vLine = null, dot = null, tooltip = null;
  var wrap = document.getElementById(wrapId);

  function attach() {
    svg = document.getElementById(svgId);
    // Manche Seiten ersetzen Diagramm-SVG UND Tooltip-Div gemeinsam per
    // innerHTML (siehe refreshKioskData()/efPriceRefresh()) - die urspruenglich
    // gefundene tooltip-Referenz waere danach ein verwaistes, nicht mehr im DOM
    // haengendes Element, deshalb hier bei jedem attach() neu nachschlagen.
    tooltip = document.getElementById(tooltipId);
    if (!svg) return;
    var svgNs = 'http://www.w3.org/2000/svg';
    vLine = document.createElementNS(svgNs, 'line');
    vLine.setAttribute('class', 'kiosk-crosshair-line');
    vLine.setAttribute('y1', '25');
    vLine.setAttribute('y2', '265');
    svg.appendChild(vLine);
    dot = document.createElementNS(svgNs, 'circle');
    dot.setAttribute('class', 'kiosk-crosshair-dot');
    dot.setAttribute('r', '6');
    svg.appendChild(dot);
  }

  // Das Diagramm-Widget kann frei in Groesse/Seitenverhaeltnis veraendert
  // werden, deshalb passt das SVG per Default preserveAspectRatio="xMidYMid
  // meet" oft mit Letterboxing rein - die tatsaechliche Inhaltsflaeche ist
  // dann kleiner als die Bounding-Box.
  function getSvgContentRect() {
    var rect = svg.getBoundingClientRect();
    var vb = svg.viewBox && svg.viewBox.baseVal;
    if (!vb || !vb.width || !vb.height || !rect.width || !rect.height) return rect;
    var scale = Math.min(rect.width / vb.width, rect.height / vb.height);
    var contentW = vb.width * scale;
    var contentH = vb.height * scale;
    return {
      left: rect.left + (rect.width - contentW) / 2,
      top: rect.top + (rect.height - contentH) / 2,
      width: contentW,
      height: contentH
    };
  }

  function nearestPoint(svgX) {
    var points = getPoints();
    if (!points || !points.length) return null;
    var best = points[0];
    var bestDist = Math.abs(best.x - svgX);
    for (var i = 1; i < points.length; i++) {
      var d = Math.abs(points[i].x - svgX);
      if (d < bestDist) { bestDist = d; best = points[i]; }
    }
    return best;
  }

  function showAt(clientX, clientY) {
    if (!svg || !vLine || !dot) return;
    var points = getPoints();
    if (!points || !points.length) return;
    var rect = getSvgContentRect();
    var svgX = (clientX - rect.left) / rect.width * 760;
    var pt = nearestPoint(svgX);
    if (!pt) return;

    vLine.setAttribute('x1', pt.x);
    vLine.setAttribute('x2', pt.x);
    vLine.style.opacity = '1';
    dot.setAttribute('cx', pt.x);
    dot.setAttribute('cy', pt.y);
    dot.style.opacity = '1';

    var dotClientX = rect.left + (pt.x / 760) * rect.width;
    var dotClientY = rect.top + (pt.y / 320) * rect.height;
    var wrapRect = wrap.getBoundingClientRect();
    tooltip.style.left = (dotClientX - wrapRect.left) + 'px';
    tooltip.style.top = (dotClientY - wrapRect.top) + 'px';
    tooltip.innerText = pt.p + ' ct um ' + pt.t;
    tooltip.style.opacity = '1';
  }

  function hide() {
    if (vLine) vLine.style.opacity = '0';
    if (dot) dot.style.opacity = '0';
    if (tooltip) tooltip.style.opacity = '0';
  }

  // WICHTIG: attach() muss VOR der Listener-Registrierung laufen - es setzt
  // `tooltip` erst (siehe Kommentar in attach()); wuerde die Pruefung unten
  // vor attach() stehen, waere `tooltip` immer noch null und die Listener
  // wuerden nie registriert.
  attach();
  if (wrap && tooltip) {
    wrap.addEventListener('pointermove', function(e){ showAt(e.clientX, e.clientY); });
    wrap.addEventListener('pointerdown', function(e){ showAt(e.clientX, e.clientY); });
    wrap.addEventListener('pointerleave', hide);
  }

  return { reattach: attach };
}

global.WidgetGridEngine = {
  clamp: wgClamp,
  rectsOverlap: wgRectsOverlap,
  resolveCollisions: wgResolveCollisions,
  flipAnimate: wgFlipAnimate,
  createController: wgCreateGridController,
  createChartCrosshair: wgCreateChartCrosshair
};

})(window);
)JS";

  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send(200, "application/javascript", js);
}

String htmlHeader(String title) {
  String html;
  if (title.length() == 0) title = webInterfaceName;

  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  // Kein Auto-Reload waehrend ein OTA-Update laeuft, sonst wird die
  // Fortschrittsanzeige (pollGhProgress) durch den Seiten-Neuladevorgang
  // mitten im Update abgebrochen.
  if (ota.owner == OtaOwner::None) {
    html += "<meta http-equiv='refresh' content='60'>";
  }
  html += "<title>";
  html += htmlEscape(webInterfaceName + " - " + title);
  html += "</title>";
  html += "<script>(function(){try{if(localStorage.getItem('theme')==='dark'){document.documentElement.setAttribute('data-theme','dark');}}catch(e){}})();</script>";
  html += "<link rel='stylesheet' href='/style.css?v=" ASSET_VERSION "'>";
  html += "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>";
  // Dynamische Akzentfarbe (iOS-Systemfarben) + optionaler Glass-Modus
  html += "<style>";
  String accHex = "#007AFF", accHexDk = "#0A84FF";
  if (accentColor == "green") { accHex = "#34C759"; accHexDk = "#30D158"; }
  else if (accentColor == "orange") { accHex = "#FF9500"; accHexDk = "#FF9F0A"; }
  else if (accentColor == "red") { accHex = "#FF3B30"; accHexDk = "#FF453A"; }
  else if (accentColor == "pink") { accHex = "#FF2D55"; accHexDk = "#FF375F"; }
  else if (accentColor == "purple") { accHex = "#AF52DE"; accHexDk = "#BF5AF2"; }
  else if (accentColor == "teal") { accHex = "#5AC8FA"; accHexDk = "#64D2FF"; }
  else if (accentColor == "indigo") { accHex = "#5856D6"; accHexDk = "#5E5CE6"; }
  html += ":root{--accent:" + accHex + ";--accent2:" + accHex + ";--accent-tint-bg:" + accHex + "1F;--accent-tint-border:" + accHex + "59}";
  html += ":root[data-theme='dark']{--accent:" + accHexDk + ";--accent2:" + accHexDk + ";--accent-tint-bg:" + accHexDk + "2E;--accent-tint-border:" + accHexDk + "66}";
  if (appearanceMode == "glass") {
    html += "body{background:linear-gradient(135deg,#e0eafc,#cfdef3,#f5e6ff)}";
    html += ":root[data-theme='dark'] body{background:linear-gradient(135deg,#1a1a2e,#16213e,#0f0c29)}";
    html += ".card,.hero,.nav{background:rgba(255,255,255,.55);backdrop-filter:blur(20px) saturate(180%);-webkit-backdrop-filter:blur(20px) saturate(180%);border:1px solid rgba(255,255,255,.35)}";
    html += ":root[data-theme='dark'] .card,:root[data-theme='dark'] .hero,:root[data-theme='dark'] .nav{background:rgba(28,28,30,.55);border:1px solid rgba(255,255,255,.1)}";
  }
  html += "</style>";
  html += "</head><body><main class='shell'>";

  return html;
}

String htmlFooter() {
  String html;
  html += "<div id='toastHost' class='toastHost'></div>";
  html += "<script src='/app.js?v=" ASSET_VERSION "'></script>";
  html += "</main></body></html>";
  return html;
}

String navTabsItem(String href, String label, String icon, String current) {
  String html = "<a href='" + href + "'><button class='navbtn";
  if (href == current) html += " active";
  html += "'>" + icon + "<span>" + label + "</span></button></a>";
  return html;
}

String navTabs(String current) {
  String iconHome = "<svg viewBox='0 0 24 24' width='18' height='18'><path d='M3 11l9-8 9 8M5 10v10h5v-6h4v6h5V10' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/></svg>";
  String iconWifi = "<svg viewBox='0 0 24 24' width='18' height='18'><path d='M2 8.5a16 16 0 0 1 20 0M5.5 12.5a11 11 0 0 1 13 0M9 16.5a6 6 0 0 1 6 0' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'/><circle cx='12' cy='20' r='1.3' fill='currentColor'/></svg>";
  String iconKey = "<svg viewBox='0 0 24 24' width='18' height='18'><circle cx='8' cy='8' r='4' fill='none' stroke='currentColor' stroke-width='2'/><path d='M11 11l9 9m-4-4l3-3m-6 2l2-2' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></svg>";
  String iconChip = "<svg viewBox='0 0 24 24' width='18' height='18'><rect x='6' y='6' width='12' height='12' rx='2' fill='none' stroke='currentColor' stroke-width='2'/><path d='M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></svg>";
  String iconMonitor = "<svg viewBox='0 0 24 24' width='18' height='18'><rect x='3' y='4' width='18' height='12' rx='2' fill='none' stroke='currentColor' stroke-width='2'/><path d='M8 20h8M12 16v4' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></svg>";
  String iconSun = "<svg viewBox='0 0 24 24' width='18' height='18'><circle cx='12' cy='12' r='4' fill='none' stroke='currentColor' stroke-width='2'/><path d='M12 2v3M12 19v3M2 12h3M19 12h3M5 5l2 2M17 17l2 2M19 5l-2 2M7 17l-2 2' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></svg>";
  String iconGrid = "<svg viewBox='0 0 24 24' width='18' height='18'><rect x='3' y='3' width='4' height='4' fill='currentColor'/><rect x='10' y='3' width='4' height='4' fill='currentColor'/><rect x='17' y='3' width='4' height='4' fill='currentColor'/><rect x='3' y='10' width='4' height='4' fill='currentColor'/><rect x='10' y='10' width='4' height='4' fill='currentColor'/><rect x='17' y='10' width='4' height='4' fill='currentColor'/><rect x='3' y='17' width='4' height='4' fill='currentColor'/><rect x='10' y='17' width='4' height='4' fill='currentColor'/><rect x='17' y='17' width='4' height='4' fill='currentColor'/></svg>";
  String iconPlug = "<svg viewBox='0 0 24 24' width='18' height='18'><path d='M9 2v4M15 2v4M7 6h10v4a5 5 0 0 1-5 5 5 5 0 0 1-5-5V6z' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/><path d='M12 15v4M9 21h6' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></svg>";
  String iconTablet = "<svg viewBox='0 0 24 24' width='18' height='18'><rect x='4' y='2' width='16' height='20' rx='2' fill='none' stroke='currentColor' stroke-width='2'/><circle cx='12' cy='18' r='1.2' fill='currentColor'/></svg>";

  String html;
  html += "<nav class='nav'>";
  html += "<span class='badge okb'>";
  html += htmlEscape(webInterfaceName);
  html += "</span>";
  html += "<span id='fwVersionBadge' class='badge' style='cursor:pointer' title='Firmware-Version - prueft alle 10 Minuten auf GitHub-Updates. Klicken fuer sofortigen Check.'>v";
  html += String(FIRMWARE_VERSION);
  html += "</span>";
  html += navTabsItem("/", "Übersicht", iconHome, current);
  html += "<span class='navDivider'></span>";
  html += navTabsItem("/wifi", "WLAN", iconWifi, current);
  html += navTabsItem("/account", "Konto", iconKey, current);
  html += navTabsItem("/anbieter", "Anbieter", iconPlug, current);
  html += navTabsItem("/pinout", "Pinout", iconChip, current);
  html += "<span class='navDivider'></span>";
  html += navTabsItem("/displays", "Displays", iconMonitor, current);
  html += navTabsItem("/ring", "Tagesring", iconSun, current);
  html += navTabsItem("/matrix", "Matrix", iconGrid, current);
  html += navTabsItem("/kiosklayout", "Kiosk", iconTablet, current);
  html += "<span class='navDivider'></span>";
  html += "<button type='button' class='themeToggle' onclick='toggleTheme()' title='Hell/Dunkel umschalten'>";
  html += "<svg class='iconSun' viewBox='0 0 24 24'><circle cx='12' cy='12' r='5' fill='currentColor'/><path d='M12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4' stroke='currentColor' stroke-width='2' stroke-linecap='round'/></svg>";
  html += "<svg class='iconMoon' viewBox='0 0 24 24'><path d='M20 14.5A8.5 8.5 0 1 1 9.5 4a7 7 0 0 0 10.5 10.5z' fill='currentColor'/></svg>";
  html += "<span>Theme</span>";
  html += "</button>";
  html += "<div class='navFooter'>";
  html += "<a href='https://github.com/Alphascrypt/dynamic-price-clock' target='_blank' rel='noopener'>GitHub-Repository</a>";
  html += "<span>Martin W.</span>";
  html += "</div>";
  html += "</nav>";
  return html;
}

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

String getCurrentIsoPrefix() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 3000)) {
    return "";
  }

  char buffer[17];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M", &timeinfo);
  return String(buffer);
}

String getLocalDatePrefix() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 3000)) {
    return "";
  }

  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
  return String(buffer);
}

String getDisplayDateText() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 3000)) {
    return "--.--.----";
  }

  char buffer[11];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y", &timeinfo);
  return String(buffer);
}

String getDisplayTimeText() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 3000)) {
    return "--:--";
  }

  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
  return String(buffer);
}

bool isTodaySlot(String isoTime) {
  String today = getLocalDatePrefix();

  // Ohne NTP-Sync ist "heute" fuer das Geraet unbekannt - vorher wurde hier
  // pauschal true zurueckgegeben, wodurch ALLE Slots (auch die von morgen)
  // faelschlich als "heutig" in Tagesstatistiken (Tagesdurchschnitt,
  // Tagestief 15/60 Min) einflossen. Sicherer Fallback: keinen Slot als
  // "heute" werten, bis die Uhrzeit synchronisiert ist - der bestehende
  // count==0-Zweig in calculateMetrics() faengt diesen Fall bereits mit
  // einer klaren Fehlermeldung ab, statt still falsche Werte zu zeigen.
  if (today.length() == 0) return false;
  if (isoTime.length() < 10) return false;

  return isoTime.substring(0, 10) == today;
}

int euroToCentRounded(float euroPrice) {
  return (int)round(euroPrice * 100.0);
}

String formatTimeOnly(String isoTime) {
  if (isoTime.length() >= 16) {
    return isoTime.substring(11, 16);
  }

  return "";
}

String addMinutesToIsoTime(String isoTime, int minutesToAdd) {
  if (isoTime.length() < 16) return "";

  int hour = isoTime.substring(11, 13).toInt();
  int minute = isoTime.substring(14, 16).toInt();

  minute += minutesToAdd;

  while (minute >= 60) {
    minute -= 60;
    hour++;
  }

  while (hour >= 24) {
    hour -= 24;
  }

  char buffer[6];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);

  return String(buffer);
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String jsEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("'", "\\'");
  s.replace("\"", "\\\"");
  s.replace("\n", "");
  s.replace("\r", "");
  return s;
}

void showMessage(Adafruit_GC9A01A &disp, bool ok, const char* msg) {
  Serial.println(msg);

  if (!ok) return;

  disp.fillScreen(DISPLAY_BLACK);
  disp.setTextColor(DISPLAY_WHITE);
  disp.setTextSize(1);
  disp.setCursor(0, 0);
  disp.println(msg);
}

void handleLayoutEditorJs() {
  if (!checkAuth()) return;
  String js = R"JS_LE(
const ITEMS = 8;
const OPTIONS = [
 ['customText','Text: Eigener Text / Ueberschrift'],
 ['labelCurrent15','Beschriftung: Aktueller 15-Minuten-Preis'],
 ['current15','Preis: Aktueller 15-Minuten-Preis jetzt'],
 ['labelCurrent60','Beschriftung: Aktueller 60-Minuten-Durchschnitt'],
 ['current60','Preis: Aktueller 60-Minuten-Durchschnitt jetzt'],
 ['labelDayAvg','Beschriftung: Tagesdurchschnitt'],
 ['dayAvg','Preis: Tagesdurchschnitt heute'],
 ['labelLow15','Beschriftung: Tiefster 15-Minuten-Preis'],
 ['low15Day','Preis: Tiefster 15-Minuten-Preis heute'],
 ['low15DayTime','Uhrzeit: Start tiefster 15-Minuten-Preis heute'],
 ['low15DayFull','Kombi: Tiefster 15-Minuten-Preis + Startzeit'],
 ['labelLow60','Beschriftung: Tiefster 60-Minuten-Block'],
 ['low60Day','Preis: Guenstigster 60-Minuten-Block heute'],
 ['low60DayTime','Uhrzeit: Start guenstigster 60-Minuten-Block'],
 ['low60DayEndTime','Uhrzeit: Ende guenstigster 60-Minuten-Block'],
 ['low60DayTimeRange','Zeitraum: Guenstigster 60-Minuten-Block'],
 ['low60DayFull','Kombi: Guenstigster 60-Minuten-Block + Startzeit'],
 ['low60DayFullRange','Kombi: Guenstigster 60-Minuten-Block + Zeitraum'],
 ['secondLow60Day','Preis: Zweitguenstigster 60-Minuten-Block heute'],
 ['secondLow60DayTime','Uhrzeit: Start zweitguenstigster 60-Minuten-Block'],
 ['secondLow60DayEndTime','Uhrzeit: Ende zweitguenstigster 60-Minuten-Block'],
 ['secondLow60DayTimeRange','Zeitraum: Zweitguenstigster 60-Minuten-Block'],
 ['secondLow60DayFull','Kombi: Zweitguenstigster 60-Minuten-Block + Startzeit'],
 ['secondLow60DayFullRange','Kombi: Zweitguenstigster 60-Minuten-Block + Zeitraum'],
 ['ip','System: IP-Adresse des ESP32'],
 ['time','System: Aktuelle Uhrzeit des ESP32'],
 ['lastUpdate','System: Startzeit aktueller Preis'],
 ['cpuLoad','System: CPU/Systemlast'],
 ['freeHeap','System: Freier Heap'],
 ['uptime','System: Uptime / Laufzeit'],
 ['error','System: Fehlermeldung / Status']
];
const SAMPLE = {customText:'Eigener Text',labelCurrent15:'Aktuell',current15:'24',labelCurrent60:'60 Min',current60:'25',labelDayAvg:'Durchschnitt',dayAvg:'23',labelLow15:'Tief 15',low15Day:'18',low15DayTime:'03:15',low15DayFull:'18 um 03:15',labelLow60:'Tief 60',low60Day:'19',low60DayTime:'03:15',low60DayEndTime:'04:15',low60DayTimeRange:'03:15-04:15',low60DayFull:'19 ab 03:15',low60DayFullRange:'19 03:15-04:15',secondLow60Day:'20',secondLow60DayTime:'05:00',secondLow60DayEndTime:'06:00',secondLow60DayTimeRange:'05:00-06:00',secondLow60DayFull:'20 ab 05:00',secondLow60DayFullRange:'20 05:00-06:00',ip:'192.168.178.55',time:'12:34',lastUpdate:'12:30',cpuLoad:'23%',freeHeap:'180 KB',uptime:'01:23',error:'Status OK'};
let saveTimer = null;
let dragEl = null;
let dragOffX = 0;
let dragOffY = 0;
let dragMoved = false;
let paletteDrag = null;
const FIELDS = ['key','txt','pre','suf','x','y','s','al','a','v'];
let selected = {1:0, 2:0};
function h(id){return document.getElementById(id);}
function hid(d,i,n){return h('d'+d+'e'+i+n);}
function getVal(d,i,n){const e=hid(d,i,n);return e?e.value:'';}
function setVal(d,i,n,v){const e=hid(d,i,n);if(e)e.value=v;}
function clamp(v,min,max){v=parseInt(v);if(isNaN(v))v=0;if(v<min)v=min;if(v>max)v=max;return v;}
function setState(t,c){const a=h('saveState');if(a){a.className='badge '+c;a.innerText=t;}const b=h('floatSaveState');if(b){b.className='badge '+c;b.innerText=t;}}
function optionLabel(key){const o=OPTIONS.find(x=>x[0]===key);return o?o[1]:key;}
function shortLabel(key){const o=OPTIONS.find(x=>x[0]===key);if(!o)return key;const ix=o[1].indexOf(':');return ix>0?o[1].substring(ix+1).trim():o[1];}
function groupOf(label){const ix=label.indexOf(':');return ix>0?label.substring(0,ix):'Sonstiges';}
function itemText(d,i){const key=getVal(d,i,'key');const base=(key==='customText')?(getVal(d,i,'txt')||'Eigener Text'):(SAMPLE[key]||optionLabel(key));return (getVal(d,i,'pre')||'')+base+(getVal(d,i,'suf')||'');}
function previewEl(d,i){return h('pv_d'+d+'_e'+i);}
function updateCountBadge(d){let count=0;for(let k=0;k<ITEMS;k++)if(getVal(d,k,'v')==='1')count++;const b=h('countBadge_d'+d);if(b)b.textContent=count+'/8 sichtbar';}
function refreshLayerRow(d,i){const row=h('layerrow_d'+d+'_e'+i);if(!row)return;const label=row.querySelector('.layer-label');if(label)label.textContent=shortLabel(getVal(d,i,'key'));const vis=getVal(d,i,'v')==='1';const visBtn=row.querySelector('.layer-vis');if(visBtn){visBtn.textContent=vis?'●':'○';visBtn.className='layer-vis'+(vis?'':' off');}updateCountBadge(d);}
function updatePreview(d,i){const el=previewEl(d,i);if(!el)return;let x=clamp(getVal(d,i,'x'),0,239);let y=clamp(getVal(d,i,'y'),0,239);let s=clamp(getVal(d,i,'s'),1,4);let al=String(getVal(d,i,'al'));let v=String(getVal(d,i,'v'));setVal(d,i,'x',x);setVal(d,i,'y',y);setVal(d,i,'s',s);el.style.left=x+'px';el.style.top=y+'px';el.style.fontSize=(s*10)+'px';el.style.display=(v==='1')?'block':'none';el.style.transform=(al==='1')?'translateX(-50%)':((al==='2')?'translateX(-100%)':'none');const txtSpan=el.querySelector('.layout-item-text');if(txtSpan)txtSpan.textContent=itemText(d,i);refreshLayerRow(d,i);if(Number(selected[d])===Number(i)){const title=h('propsTitle_d'+d);if(title)title.innerText=(Number(i)+1)+'. '+shortLabel(getVal(d,i,'key'));}}
function scheduleSave(){clearTimeout(saveTimer);setState('Aenderungen...','warnb');saveTimer=setTimeout(saveLayoutNow,400);}
function saveLayoutNow(){const form=h('layoutForm');if(!form){setState('Kein Formular','errb');return;}const body=new URLSearchParams(new FormData(form)).toString();if(body.length<5){setState('Keine Daten','errb');return;}setState('Speichere...','warnb');fetch('/savelayoutajax',{method:'POST',cache:'no-store',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body:body}).then(r=>r.text()).then(t=>{if(t.indexOf('OK')>=0){setState('Gespeichert','okb');}else{setState('Fehler','errb');}}).catch(()=>{form.submit();});}
function controlInput(d,i,n,type,label,min,max,hint){const wrap=document.createElement('div');const lbl=document.createElement('label');lbl.textContent=label;if(hint)lbl.title=hint;wrap.appendChild(lbl);const inp=document.createElement('input');inp.id='ui_d'+d+'_e'+i+'_'+n;inp.type=type;inp.value=getVal(d,i,n);inp.min=min||'';inp.max=max||'';if(hint)inp.title=hint;inp.addEventListener('input',()=>{setVal(d,i,n,inp.value);updatePreview(d,i);scheduleSave();});inp.addEventListener('change',()=>{setVal(d,i,n,inp.value);updatePreview(d,i);saveLayoutNow();});wrap.appendChild(inp);return wrap;}
function controlSelect(d,i,n,label,arr,hint){const wrap=document.createElement('div');const lbl=document.createElement('label');lbl.textContent=label;if(hint)lbl.title=hint;wrap.appendChild(lbl);const sel=document.createElement('select');sel.id='ui_d'+d+'_e'+i+'_'+n;if(hint)sel.title=hint;arr.forEach(o=>{const opt=document.createElement('option');opt.value=o[0];opt.textContent=o[1];if(String(getVal(d,i,n))===String(o[0]))opt.selected=true;sel.appendChild(opt);});sel.addEventListener('change',()=>{setVal(d,i,n,sel.value);updatePreview(d,i);saveLayoutNow();});wrap.appendChild(sel);return wrap;}
function controlToggle(d,i,n,label,hint){const wrap=document.createElement('div');wrap.className='toggleRow';const lbl=document.createElement('label');lbl.textContent=label;if(hint)lbl.title=hint;const toggle=document.createElement('label');toggle.className='toggle';const cb=document.createElement('input');cb.type='checkbox';cb.id='ui_d'+d+'_e'+i+'_'+n;cb.checked=getVal(d,i,n)==='1';if(hint)toggle.title=hint;cb.addEventListener('change',()=>{setVal(d,i,n,cb.checked?'1':'0');updatePreview(d,i);saveLayoutNow();});const slider=document.createElement('span');slider.className='toggleSlider';toggle.appendChild(cb);toggle.appendChild(slider);wrap.appendChild(lbl);wrap.appendChild(toggle);return wrap;}
function controlKeySelect(d,i){const wrap=document.createElement('div');wrap.className='wide';wrap.innerHTML='<label>Wert / Anzeigeinhalt</label>';const sel=document.createElement('select');sel.id='ui_d'+d+'_e'+i+'_key';const groups={};OPTIONS.forEach(o=>{const g=groupOf(o[1]);if(!groups[g]){groups[g]=document.createElement('optgroup');groups[g].label=g;sel.appendChild(groups[g]);}const opt=document.createElement('option');opt.value=o[0];opt.textContent=shortLabel(o[0]);if(String(getVal(d,i,'key'))===String(o[0]))opt.selected=true;groups[g].appendChild(opt);});sel.addEventListener('change',()=>{setVal(d,i,'key',sel.value);updatePreview(d,i);saveLayoutNow();});wrap.appendChild(sel);return wrap;}
function setControl(d,i,n,v){const el=h('ui_d'+d+'_e'+i+'_'+n);if(el){if(el.type==='checkbox'){el.checked=(String(v)==='1');}else{el.value=v;}el.dispatchEvent(new Event('change'));}else{setVal(d,i,n,v);updatePreview(d,i);}}
function resetElement(d,i){if(!confirm('Element '+(Number(i)+1)+' auf Display '+d+' wirklich zuruecksetzen?'))return;setState('Setze zurueck...','warnb');setControl(d,i,'key','customText');setControl(d,i,'txt','Text '+(Number(i)+1));setControl(d,i,'pre','');setControl(d,i,'suf','');setControl(d,i,'x',20);setControl(d,i,'y',20+Number(i)*24);setControl(d,i,'s',2);setControl(d,i,'al','0');setControl(d,i,'a','1');setControl(d,i,'v','0');}
function copyToOther(d,i){const od=(Number(d)===1)?2:1;FIELDS.forEach(n=>setControl(od,i,n,getVal(d,i,n)));showToast('Auf Display '+od+' kopiert','ok');}
function locateItem(d,i){const el=previewEl(d,i);if(!el)return;el.scrollIntoView({behavior:'smooth',block:'center'});el.classList.add('locate-pulse');setTimeout(()=>el.classList.remove('locate-pulse'),1200);}
function findFreeSlot(d){for(let i=0;i<ITEMS;i++){if(getVal(d,i,'v')!=='1')return i;}return -1;}
function addElementFromPalette(d,key,x,y){const free=findFreeSlot(d);if(free===-1){showToast('Alle 8 Plaetze auf Display '+d+' sind belegt.','err');return;}setControl(d,free,'key',key);if(key==='customText')setControl(d,free,'txt','Neuer Text');setControl(d,free,'x',x);setControl(d,free,'y',y);setControl(d,free,'v','1');selectSlot(d,free);showToast('Element hinzugefuegt','ok');}
function point(e){if(e.touches&&e.touches.length)return{x:e.touches[0].clientX,y:e.touches[0].clientY};return{x:e.clientX,y:e.clientY};}
function buildPalette(d){const wrap=h('palette_d'+d);if(!wrap)return;wrap.innerHTML='';const hint=document.createElement('div');hint.className='palette-hint';hint.textContent='Element auf die Vorschau ziehen, um es hinzuzufuegen:';wrap.appendChild(hint);const groups={};OPTIONS.forEach(o=>{const g=groupOf(o[1]);if(!groups[g]){groups[g]=document.createElement('div');groups[g].className='palette-group';const lbl=document.createElement('span');lbl.className='palette-group-label';lbl.textContent=g;groups[g].appendChild(lbl);wrap.appendChild(groups[g]);}const chip=document.createElement('span');chip.className='palette-chip';chip.textContent=shortLabel(o[0]);chip.addEventListener('pointerdown',e=>paletteDragStart(e,d,o[0],shortLabel(o[0])));groups[g].appendChild(chip);});}
function paletteDragStart(e,d,key,label){e.preventDefault();const ghost=document.createElement('div');ghost.className='palette-ghost';ghost.textContent=label;document.body.appendChild(ghost);paletteDrag={ghost:ghost,d:d,key:key};movePaletteGhost(e);document.addEventListener('pointermove',movePaletteGhost);document.addEventListener('pointerup',paletteDragEnd,{once:true});}
function movePaletteGhost(e){if(!paletteDrag)return;const p=point(e);paletteDrag.ghost.style.left=(p.x+12)+'px';paletteDrag.ghost.style.top=(p.y+12)+'px';}
function paletteDragEnd(e){if(!paletteDrag)return;document.removeEventListener('pointermove',movePaletteGhost);const p=point(e);const d=paletteDrag.d;const prev=h('preview_d'+d);if(prev){const r=prev.getBoundingClientRect();if(p.x>=r.left&&p.x<=r.right&&p.y>=r.top&&p.y<=r.bottom){const relX=clamp(Math.round((p.x-r.left)/r.width*240),0,239);const relY=clamp(Math.round((p.y-r.top)/r.height*240),0,239);addElementFromPalette(d,paletteDrag.key,relX,relY);}}paletteDrag.ghost.remove();paletteDrag=null;}
function moveBy(d,i,dx,dy){let x=clamp(getVal(d,i,'x'),0,239)+dx;let y=clamp(getVal(d,i,'y'),0,239)+dy;setVal(d,i,'x',clamp(x,0,239));setVal(d,i,'y',clamp(y,0,239));const ux=h('ui_d'+d+'_e'+i+'_x');const uy=h('ui_d'+d+'_e'+i+'_y');if(ux)ux.value=getVal(d,i,'x');if(uy)uy.value=getVal(d,i,'y');updatePreview(d,i);scheduleSave();}
function startResizeDrag(e,d,i){e.preventDefault();e.stopPropagation();const startP=point(e);const startSize=clamp(getVal(d,i,'s'),1,4);function onMove(ev){const p=point(ev);const delta=(p.x-startP.x)+(p.y-startP.y);const steps=Math.round(delta/18);const newSize=clamp(startSize+steps,1,4);setVal(d,i,'s',newSize);updatePreview(d,i);const el=h('ui_d'+d+'_e'+i+'_s');if(el)el.value=newSize;}function onUp(){document.removeEventListener('pointermove',onMove);document.removeEventListener('pointerup',onUp);saveLayoutNow();}document.addEventListener('pointermove',onMove);document.addEventListener('pointerup',onUp);}
function itemPointerDown(e,node,d,i){e.preventDefault();dragEl=node;dragMoved=false;selectSlot(d,i);dragEl.classList.add('dragging');const p=point(e);const r=node.getBoundingClientRect();dragOffX=p.x-r.left;dragOffY=p.y-r.top;document.body.style.userSelect='none';}
function onItemPointerMove(e){if(!dragEl)return;if(e.preventDefault)e.preventDefault();dragMoved=true;const p=point(e);const box=dragEl.parentElement.getBoundingClientRect();let px=Math.round((p.x-box.left-dragOffX)/4)*4;let py=Math.round((p.y-box.top-dragOffY)/4)*4;if(px<0)px=0;if(px>239)px=239;if(py<0)py=0;if(py>239)py=239;const d=dragEl.dataset.d;const i=dragEl.dataset.i;setVal(d,i,'x',clamp(Math.round(px),0,239));setVal(d,i,'y',clamp(Math.round(py),0,239));const ux=h('ui_d'+d+'_e'+i+'_x');const uy=h('ui_d'+d+'_e'+i+'_y');if(ux)ux.value=getVal(d,i,'x');if(uy)uy.value=getVal(d,i,'y');updatePreview(d,i);}
function onItemPointerUp(){if(!dragEl)return;dragEl.classList.remove('dragging');dragEl=null;document.body.style.userSelect='';if(dragMoved)saveLayoutNow();}
document.addEventListener('pointermove',onItemPointerMove);document.addEventListener('pointerup',onItemPointerUp);
function selectSlot(d,i){selected[d]=i;const prev=h('preview_d'+d);if(prev){prev.querySelectorAll('.layout-item.selected').forEach(el=>{el.classList.remove('selected');const hd=el.querySelector('.resize-handle');if(hd)hd.remove();});}const item=previewEl(d,i);if(item){item.classList.add('selected');const handle=document.createElement('span');handle.className='resize-handle';handle.addEventListener('pointerdown',e=>startResizeDrag(e,d,i));item.appendChild(handle);}const lyr=h('layers_d'+d);if(lyr){lyr.querySelectorAll('.layer-row.selected').forEach(el=>el.classList.remove('selected'));}const row=h('layerrow_d'+d+'_e'+i);if(row)row.classList.add('selected');renderProps(d,i);}
function renderProps(d,i){const wrap=h('props_d'+d);if(!wrap)return;wrap.innerHTML='';const head=document.createElement('div');head.className='props-head';head.innerHTML='<strong id="propsTitle_d'+d+'">'+(Number(i)+1)+'. '+shortLabel(getVal(d,i,'key'))+'</strong><div class="card-head-btns"><button type="button" class="icon-btn" title="Auf Vorschau zeigen">◎</button><button type="button" class="icon-btn" title="Auf anderes Display kopieren">⇄</button><button type="button" class="icon-btn" title="Element zuruecksetzen">↺</button></div>';wrap.appendChild(head);const btns=head.querySelectorAll('.icon-btn');btns[0].onclick=()=>locateItem(d,i);btns[1].onclick=()=>copyToOther(d,i);btns[2].onclick=()=>resetElement(d,i);const grid=document.createElement('div');grid.className='layout-fields';grid.appendChild(controlKeySelect(d,i));const txt=controlInput(d,i,'txt','text','Eigener Text','','','Nur relevant, wenn oben "Eigener Text" als Wert gewaehlt ist.');txt.className='wide';grid.appendChild(txt);grid.appendChild(controlInput(d,i,'pre','text','Text davor (Praefix)','','','Wird direkt vor dem Wert eingefuegt, z.B. "ab " vor einer Uhrzeit.'));grid.appendChild(controlInput(d,i,'suf','text','Text danach (Suffix)','','','Wird direkt nach dem Wert eingefuegt, z.B. " ct" hinter einem Preis.'));grid.appendChild(controlInput(d,i,'x','number','Position X (0-239 px)','0','239','Waagerechte Position auf dem 240x239 Pixel runden Display.'));grid.appendChild(controlInput(d,i,'y','number','Position Y (0-239 px)','0','239','Senkrechte Position auf dem 240x239 Pixel runden Display.'));grid.appendChild(controlInput(d,i,'s','number','Schriftgroesse (Stufe 1-4)','1','4','Stufe 1 = klein, Stufe 4 = sehr gross. Keine Pixelangabe.'));grid.appendChild(controlSelect(d,i,'al','Ausrichtung',[['0','Links'],['1','Zentriert'],['2','Rechts']],'Woran sich der Text an der X-Position ausrichtet.'));grid.appendChild(controlToggle(d,i,'a','Automatische Groesse','Passt die Schriftgroesse automatisch an die Textlaenge an.'));grid.appendChild(controlToggle(d,i,'v','Sichtbar','Element auf dem Display ein- oder ausblenden, ohne es zu loeschen.'));wrap.appendChild(grid);const nudge=document.createElement('div');nudge.className='layout-nudge';nudge.innerHTML='<span class="blank"></span><button type="button">&uarr;</button><span class="blank"></span><button type="button">&larr;</button><button type="button">&darr;</button><button type="button">&rarr;</button>';const b=nudge.querySelectorAll('button');b[0].onclick=()=>moveBy(d,i,0,-4);b[1].onclick=()=>moveBy(d,i,-4,0);b[2].onclick=()=>moveBy(d,i,0,4);b[3].onclick=()=>moveBy(d,i,4,0);wrap.appendChild(nudge);if(window.innerWidth<900){wrap.scrollIntoView({behavior:'smooth',block:'nearest'});}}
function renderLayerList(d){const wrap=h('layers_d'+d);if(!wrap)return;wrap.innerHTML='';for(let i=0;i<ITEMS;i++){const row=document.createElement('div');row.className='layer-row';row.id='layerrow_d'+d+'_e'+i;row.dataset.d=d;row.dataset.i=i;const vis=getVal(d,i,'v')==='1';row.innerHTML='<span class="layer-handle" title="Ziehen zum Sortieren">⋮⋮</span><span class="layer-num">'+(i+1)+'</span><span class="layer-label">'+shortLabel(getVal(d,i,'key'))+'</span><button type="button" class="layer-vis'+(vis?'':' off')+'">'+(vis?'●':'○')+'</button>';row.querySelector('.layer-label').addEventListener('click',()=>selectSlot(d,i));row.querySelector('.layer-num').addEventListener('click',()=>selectSlot(d,i));row.querySelector('.layer-vis').addEventListener('click',e=>{e.stopPropagation();toggleVisible(d,i);});row.querySelector('.layer-handle').addEventListener('pointerdown',e=>startReorderDrag(e,d,i));if(Number(selected[d])===i)row.classList.add('selected');wrap.appendChild(row);}updateCountBadge(d);}
function toggleVisible(d,i){const cur=getVal(d,i,'v');setControl(d,i,'v',cur==='1'?'0':'1');if(Number(selected[d])===Number(i))renderProps(d,i);}
function startReorderDrag(e,d,i){e.preventDefault();const wrap=h('layers_d'+d);if(!wrap)return;const rows=Array.from(wrap.querySelectorAll('.layer-row'));let targetIndex=Number(i);const indicator=document.createElement('div');indicator.className='layer-drop-indicator';function onMove(ev){const p=point(ev);let placed=false;for(const row of rows){const r=row.getBoundingClientRect();if(p.y>=r.top&&p.y<=r.bottom){const before=p.y<(r.top+r.height/2);row[before?'before':'after'](indicator);targetIndex=Number(row.dataset.i);if(!before)targetIndex+=1;placed=true;break;}}if(!placed&&indicator.parentElement!==wrap)wrap.appendChild(indicator);}function onUp(){document.removeEventListener('pointermove',onMove);document.removeEventListener('pointerup',onUp);indicator.remove();let to=targetIndex;if(to>Number(i))to-=1;to=clamp(to,0,ITEMS-1);moveSlot(d,Number(i),to);}wrap.appendChild(indicator);document.addEventListener('pointermove',onMove);document.addEventListener('pointerup',onUp);}
function moveSlot(d,from,to){if(from===to)return;const data=[];for(let k=0;k<ITEMS;k++){const o={};FIELDS.forEach(n=>o[n]=getVal(d,k,n));data.push(o);}const moved=data.splice(from,1)[0];data.splice(to,0,moved);data.forEach((o,k)=>FIELDS.forEach(n=>setVal(d,k,n,o[n])));selected[d]=to;renderLayerList(d);for(let k=0;k<ITEMS;k++)updatePreview(d,k);selectSlot(d,to);scheduleSave();}
function makeDisplay(d){const section=document.createElement('section');section.className='card layoutPanel';section.id='display-'+d;section.innerHTML='<div class="panelTitle"><h2>Display '+d+'</h2><span class="badge okb" id="countBadge_d'+d+'">8/8 sichtbar</span></div><div class="palette" id="palette_d'+d+'"></div><div class="editor-shell"><div class="layer-panel"><div class="panel-label">Ebenen</div><div class="layer-list" id="layers_d'+d+'"></div></div><div class="canvas-panel"><div class="layout-preview" id="preview_d'+d+'"></div><p class="small" style="text-align:center">Ziehen zum Verschieben, Punkt unten rechts zum Skalieren.</p></div><div class="props-panel" id="props_d'+d+'"></div></div>';document.getElementById('layoutApp').appendChild(section);buildPalette(d);const prev=h('preview_d'+d);for(let i=0;i<ITEMS;i++){const item=document.createElement('div');item.className='layout-item';item.id='pv_d'+d+'_e'+i;item.dataset.d=d;item.dataset.i=i;item.innerHTML='<span class="layout-item-num">'+(i+1)+'</span><span class="layout-item-text"></span>';item.addEventListener('pointerdown',e=>itemPointerDown(e,item,d,i));prev.appendChild(item);}renderLayerList(d);for(let i=0;i<ITEMS;i++)updatePreview(d,i);selectSlot(d,0);}
function initLayoutApp(){var app=document.getElementById('layoutApp');if(!app){console.error('layoutApp fehlt');return;}if(app.dataset.ready==='1')return;app.dataset.ready='1';var probe=document.createElement('div');probe.style.cssText='padding:12px 16px;background:var(--panel2);border-radius:12px;font-size:13px;color:var(--muted);margin-bottom:14px';probe.textContent='Layout-Editor initialisiert - Display 1 und 2 werden geladen...';app.appendChild(probe);try{makeDisplay(1);makeDisplay(2);probe.remove();}catch(e){probe.style.color='var(--errb-text)';probe.style.background='var(--errb-bg)';probe.textContent='Layout-Editor Fehler: '+e.message+' | Stack: '+(e.stack||'').slice(0,200);console.error(e);}}if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',initLayoutApp);}else{initLayoutApp();}
)JS_LE";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/javascript", js);
}

void handleAccountUpdateJs() {
  if (!checkAuth()) return;
  String js = R"ACJS(
function ghBadge(cls, text) {
  var b = document.getElementById('ghUpdateBadge');
  if (b) { b.className = 'badge ' + cls; b.innerText = text; }
}
async function checkGhUpdate() {
  var msg = document.getElementById('ghUpdateMsg');
  var btn = document.getElementById('ghUpdateBtn');
  ghBadge('warnb', 'Pruefe...');
  msg.innerText = '';
  btn.style.display = 'none';
  try {
    const r = await fetch('/checkgithubupdate', { cache: 'no-store' });
    const j = await r.json();
    if (!j.ok) {
      ghBadge('errb', 'Fehler');
      msg.innerText = j.error || 'Unbekannter Fehler beim Pruefen.';
      return;
    }
    if (j.updateAvailable) {
      ghBadge('warnb', 'Update verfuegbar');
      msg.innerText = 'Installiert: ' + j.currentVersion + '  ->  Neu: ' + j.latestVersion;
      window.ghUpdateUrl = j.downloadUrl;
      btn.style.display = '';
    } else {
      ghBadge('okb', 'Aktuell');
      msg.innerText = 'Du hast bereits die neueste Version (' + j.currentVersion + ').';
    }
  } catch (e) {
    ghBadge('errb', 'Fehler');
    msg.innerText = 'Verbindung fehlgeschlagen: ' + e;
  }
}
function formatBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(0) + ' KB';
  return (n / (1024 * 1024)).toFixed(1) + ' MB';
}
var _ghPollStart = 0;
var _ghLastBytes = 0;
var _ghLastBytesTime = 0;
var _ghTimeoutMs = 5 * 60 * 1000;
function ghSetPhase(icon, lbl, sub) {
  var i=document.getElementById('ghPhaseIcon');
  var l=document.getElementById('ghPhaseLbl');
  var s=document.getElementById('ghProgressSub');
  if(i)i.innerText=icon;
  if(l)l.innerText=lbl;
  if(s&&sub!=null)s.innerText=sub;
}
async function pollGhProgress() {
  var wrap = document.getElementById('ghProgressWrap');
  var bar = document.getElementById('ghProgressBar');
  var text = document.getElementById('ghProgressText');
  var speed = document.getElementById('ghSpeedText');
  var msg = document.getElementById('ghUpdateMsg');
  var now = Date.now();

  if (now - _ghPollStart > _ghTimeoutMs) {
    ghBadge('errb', 'Timeout');
    ghSetPhase('&#9888;', 'Timeout', 'Das Update hat zu lange gebraucht. Bitte Verbindung pruefen und erneut versuchen.');
    bar.style.background = 'var(--danger)';
    return;
  }

  try {
    const r = await fetch('/otaprogress', { cache: 'no-store' });
    const j = await r.json();

    var hbAge = (typeof j.heartbeatAge === 'number') ? j.heartbeatAge : 0;
    if (j.percent < 0 || j.bytesTotal === 0) {
      var waitSec = Math.round((now - _ghPollStart) / 1000);
      var hint = hbAge > 45 ? 'Keine Aktivitaet seit ' + hbAge + 's — ESP32 hängt möglicherweise. Bitte Gerät neu starten.' : 'GitHub leitet den Download um — das dauert 5-15 Sekunden.' + (j.diag ? ' (' + j.diag + ')' : '');
      ghSetPhase('&#8987;', 'Verbinde & lade herunter... (' + waitSec + 's)', hint);
      text.innerText = '';
      if(speed) speed.innerText = '';
    } else if (j.percent < 100) {
      ghSetPhase('&#8659;', 'Herunterladen...', '');
      bar.style.width = j.percent + '%';
      text.innerText = j.percent + '% (' + formatBytes(j.bytesWritten) + ' / ' + formatBytes(j.bytesTotal) + ')';
      if (speed && j.bytesWritten > _ghLastBytes && _ghLastBytesTime > 0) {
        var dt = (now - _ghLastBytesTime) / 1000;
        var bps = (j.bytesWritten - _ghLastBytes) / dt;
        speed.innerText = formatBytes(bps) + '/s';
      }
      _ghLastBytes = j.bytesWritten;
      _ghLastBytesTime = now;
    } else {
      ghSetPhase('&#9889;', 'Flashe Firmware...', 'Bitte Stromversorgung nicht trennen.');
      bar.style.width = '100%';
      text.innerText = 'Schreibe Flash...';
    }

    if (j.done) {
      if (j.success) {
        bar.style.width = '100%';
        ghBadge('okb', 'Fertig');
        ghSetPhase('&#10003;', 'Update erfolgreich!', 'Das Geraet startet neu — Seite wird in 8 Sekunden neu geladen.');
        msg.innerText = '';
        setTimeout(function(){ location.reload(); }, 8000);
      } else {
        ghBadge('errb', 'Fehler');
        ghSetPhase('&#10008;', 'Update fehlgeschlagen', j.error || 'Unbekannter Fehler.');
        wrap.style.display = 'none';
      }
      return;
    }

    setTimeout(pollGhProgress, 300);
  } catch (e) {
    bar.style.width = '100%';
    ghBadge('okb', 'Neustart');
    ghSetPhase('&#10003;', 'Neustart laeuft...', 'Verbindung unterbrochen — das Geraet startet neu. Seite wird in 8 Sekunden geladen.');
    setTimeout(function(){ location.reload(); }, 8000);
  }
}
function upBadge(cls, text) {
  var b = document.getElementById('upBadge');
  if (b) { b.className = 'badge ' + cls; b.innerText = text; }
}
function startUpload(ev) {
  ev.preventDefault();
  var file = document.getElementById('upFile').files[0];
  if (!file) return false;
  if (!file.name.toLowerCase().endsWith('.bin')) {
    alert('Bitte eine .bin-Datei waehlen.');
    return false;
  }
  if (!confirm('Firmware "' + file.name + '" jetzt installieren? Das Geraet startet danach automatisch neu. Falls die Datei nicht zur Hardware passt, kann ein serieller Flash noetig sein.')) return false;

  var wrap = document.getElementById('upProgressWrap');
  var bar = document.getElementById('upProgressBar');
  var text = document.getElementById('upProgressText');
  var msg = document.getElementById('upMsg');
  var sub = document.getElementById('upSubmit');

  wrap.style.display = '';
  bar.style.width = '0%';
  msg.innerText = '';
  sub.disabled = true;
  upBadge('warnb', 'Lade hoch...');

  var fd = new FormData();
  fd.append('firmware', file);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/uploadfirmware', true);
  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      var p = Math.round((e.loaded / e.total) * 100);
      bar.style.width = p + '%';
      text.innerText = p + '% (' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total) + ')';
    }
  };
  xhr.onload = function() {
    try {
      var j = JSON.parse(xhr.responseText);
      if (j.ok) {
        bar.style.width = '100%';
        upBadge('okb', 'Fertig');
        msg.innerText = 'Upload erfolgreich, das Geraet startet jetzt neu...';
      } else {
        upBadge('errb', 'Fehler');
        msg.innerText = j.error || 'Upload fehlgeschlagen.';
        sub.disabled = false;
      }
    } catch(e) {
      upBadge('errb', 'Fehler');
      msg.innerText = 'Antwort konnte nicht gelesen werden.';
      sub.disabled = false;
    }
  };
  xhr.onerror = function() {
    upBadge('warnb', 'Neustart');
    msg.innerText = 'Verbindung unterbrochen - das ist beim Neustart normal.';
  };
  xhr.send(fd);
  return false;
}
async function startGhUpdate() {
  if (!window.ghUpdateUrl) return;
  if (!confirm('Firmware jetzt aktualisieren? Das Geraet startet danach automatisch neu und ist kurz nicht erreichbar.')) return;
  var msg = document.getElementById('ghUpdateMsg');
  var wrap = document.getElementById('ghProgressWrap');
  var bar = document.getElementById('ghProgressBar');
  ghBadge('warnb', 'Aktualisiere...');
  msg.innerText = '';
  wrap.style.display = '';
  bar.style.width = '0%';
  _ghPollStart = Date.now();
  _ghLastBytes = 0;
  _ghLastBytesTime = 0;
  ghSetPhase('&#8987;', 'Verbinde mit GitHub...', 'GitHub leitet den Download um — das dauert 5-15 Sekunden.');
  try {
    // Der Server nutzt ohnehin nur noch seine eigene, bereits verifizierte
    // URL (siehe handleGithubUpdate()) - keine URL mehr mitschicken, die er
    // sowieso ignoriert.
    const r = await fetch('/githubupdate', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' }, body: '' });
    const j = await r.json();
    if (j.ok) {
      setTimeout(pollGhProgress, 300);
    } else {
      ghBadge('errb', 'Fehler');
      msg.innerText = j.error || 'Update fehlgeschlagen.';
      wrap.style.display = 'none';
    }
  } catch (e) {
    msg.innerText = 'Verbindung unterbrochen - das ist beim Neustart normal.';
  }
}

// Falls diese Seite waehrend eines laufenden Updates (neu) geladen wurde -
// z.B. weil der 60s-Seiten-Auto-Reload (siehe htmlHeader()) mitten in einem
// Download feuerte, bevor der Server den Update-Vorgang als laufend
// zurueckmelden konnte, oder weil der Nutzer manuell weg- und zurueck-navigiert ist -
// Fortschritts-UI direkt wieder anzeigen und Polling fortsetzen, statt einen
// scheinbar zurueckgesetzten Zustand zu zeigen, waehrend im Hintergrund
// tatsaechlich noch ein Update laeuft.
(function(){
  fetch('/otaprogress', { cache: 'no-store' }).then(function(r){ return r.json(); }).then(function(j){
    if (!j || !j.running) return;
    var wrap = document.getElementById('ghProgressWrap');
    var bar = document.getElementById('ghProgressBar');
    var msg = document.getElementById('ghUpdateMsg');
    if (!wrap || !bar) return;
    ghBadge('warnb', 'Aktualisiere...');
    if (msg) msg.innerText = '';
    wrap.style.display = '';
    bar.style.width = '0%';
    _ghPollStart = Date.now();
    _ghLastBytes = j.bytesWritten || 0;
    _ghLastBytesTime = Date.now();
    pollGhProgress();
  }).catch(function(){});
})();
)ACJS";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/javascript", js);
}
