#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Adafruit_NeoPixel.h>
#include <WebSocketsClient.h>

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
#define FIRMWARE_VERSION "2.1.5"

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
String githubLatestVersion = "";
String githubLatestUrl = "";

// Leiser Hintergrund-Versionscheck fuer das Nav-Badge (alle 10 Minuten vom
// Browser aus per /versioncheck aufgerufen). Nutzt eigene Fehler-Variable,
// damit ein fehlgeschlagener Hintergrund-Check NICHT den globalen lastError
// (und damit den Status-OK/Fehler-Indikator auf der Startseite) verfaelscht.
String versionCheckError = "";
String versionCheckLatest = "";

// OTA-Fortschritt: laeuft in einem eigenen FreeRTOS-Task (siehe otaUpdateTask),
// damit der Webserver waehrend des Downloads weiter auf /otaprogress antworten
// kann (der Server selbst ist single-threaded und wuerde sonst blockieren).
volatile int otaBytesWritten = 0;
volatile int otaBytesTotal = 0;
volatile bool otaTaskRunning = false;
volatile bool otaTaskDone = false;
volatile bool otaTaskSuccess = false;
String otaTaskError = "";
String otaPendingUrl = "";

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
WebSocketsClient tibberWs;
bool tibberWsStarted = false;
float livePowerW = -1;
unsigned long livePowerUpdatedAtMs = 0;

// Strompreis-Quelle: "tibber" (Standard), "awattar_de" oder "awattar_at".
// aWATTar liefert nur den Boersenpreis ohne Netzentgelte/Steuern, deshalb
// zusaetzlich ein fixer Aufschlag (ct/kWh) und ein Mehrwertsteuersatz (%).
String priceProvider = "tibber";
float priceSurchargeCt = 0.0;
float priceVatPercent = 0.0;

String lastError = "Noch kein Update";
String webInterfaceName = "Dynamic Price Clock";

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

// -----------------------------------------------------------------------------
// Funktions-Prototypen
// -----------------------------------------------------------------------------

void ensureWifiConnected();

void updateTibber();
void updateAwattarPrices();
void updatePrices();
void updateTibberLiveMeasurement();
void handleTibberWsEvent(WStype_t type, uint8_t *payload, size_t length);
bool checkGithubUpdate();
bool checkGithubUpdateQuiet();
void handleVersionCheck();
bool performGithubUpdate(String url);
void otaUpdateTask(void *param);
void calculateMetrics();

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
void handleKioskLayoutPage();
void handleSaveKioskLayoutAjax();
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
void handleLivePower();

String buildSvgChart();
String buildChartPointsJson();
String buildPinoutSvg();
String buildPriceGaugeSvg();
void getKioskPriceStatus(String &statusText, String &statusColor);
String kioskWidgetCss(KioskWidgetLayout arr[]);
String kioskLayoutJson(KioskWidgetLayout arr[]);
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
  server.on("/kiosklayout", handleKioskLayoutPage);
  server.on("/savekiosklayoutajax", HTTP_POST, handleSaveKioskLayoutAjax);
  server.on("/resetkiosklayout", HTTP_POST, handleResetKioskLayout);
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
#endif

  if (!apMode && millis() - lastUpdate >= updateInterval) {
    updatePrices();
  }

  if (!apMode) {
    updateTibberLiveMeasurement();
  }

  #if ENABLE_TFT_DISPLAYS
  if (millis() - lastDisplayRefresh >= displayRefreshInterval) {
    lastDisplayRefresh = millis();
    showLayoutDisplays();
  }
#endif

  #if ENABLE_WS2812_RING
  if (millis() - lastLedRefresh >= ledRefreshInterval) {
    lastLedRefresh = millis();
    updateLedRing();
  }
#endif

  #if ENABLE_MAX7219_MATRIX
  if (millis() - lastMatrixRefresh >= matrixRefreshInterval) {
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
    "{\"query\":\"{ viewer { homes { id appNickname currentSubscription { priceInfo(resolution: QUARTER_HOURLY) { current { total startsAt } today { total startsAt } tomorrow { total startsAt } } } consumption(resolution: DAILY, last: 40) { nodes { from cost consumption currency } } } } }\"}";

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

// Startet die WebSocket-Verbindung einmalig, sobald Token+Home-ID bekannt
// sind. Wird jeden loop()-Durchlauf aufgerufen, die Guards machen das billig.
void updateTibberLiveMeasurement() {
  if (priceProvider != "tibber" || tibberToken.length() < 10 || selectedHomeId.length() == 0) {
    if (tibberWsStarted) {
      tibberWs.disconnect();
      tibberWsStarted = false;
      livePowerW = -1;
    }
    return;
  }

  if (!tibberWsStarted) {
    tibberWs.beginSSL(TIBBER_WS_HOST, 443, TIBBER_WS_URL, "", "graphql-transport-ws");
    tibberWs.onEvent(handleTibberWsEvent);
    tibberWs.setReconnectInterval(15000);
    tibberWsStarted = true;
  }

  tibberWs.loop();
}

void handleTibberWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      String initMsg = "{\"type\":\"connection_init\",\"payload\":{\"token\":\"" + tibberToken + "\"}}";
      tibberWs.sendTXT(initMsg);
      break;
    }

    case WStype_DISCONNECTED:
      livePowerW = -1;
      break;

    case WStype_TEXT: {
      DynamicJsonDocument doc(2048);
      if (deserializeJson(doc, payload, length)) return;

      String msgType = doc["type"] | "";

      if (msgType == "connection_ack") {
        String subMsg =
          "{\"id\":\"live1\",\"type\":\"subscribe\",\"payload\":{\"query\":\"subscription($homeId: ID!){ liveMeasurement(homeId: $homeId){ power } }\",\"variables\":{\"homeId\":\"" + selectedHomeId + "\"}}}";
        tibberWs.sendTXT(subMsg);
      } else if (msgType == "next") {
        float p = doc["payload"]["data"]["liveMeasurement"]["power"] | -1.0;
        if (p >= 0) {
          livePowerW = p;
          livePowerUpdatedAtMs = millis();
        }
      } else if (msgType == "ping") {
        tibberWs.sendTXT("{\"type\":\"pong\"}");
      }
      break;
    }

    default:
      break;
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

bool checkGithubUpdate() {
  githubLatestVersion = "";
  githubLatestUrl = "";

  if (githubRepo.length() == 0) {
    lastError = "Kein GitHub-Repository hinterlegt";
    return false;
  }

  if (apMode || WiFi.status() != WL_CONNECTED) {
    lastError = "Kein Internet / AP Modus";
    return false;
  }

  WiFiClientSecure client;

  // Keine manuelle Root-CA-Pinnung fuer GitHub: der Download laeuft ueber
  // zwei verschiedene Hosts (api.github.com fuer die Metadaten, per Redirect
  // objects.githubusercontent.com fuer die eigentliche .bin-Datei) mit
  // rotierenden Zertifikaten - ein einzelnes gepinntes Zertifikat waere kaum
  // wartbar und wuerde ohne zusaetzlichen Nutzen regelmaessig brechen.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);

  String url = "https://api.github.com/repos/" + githubRepo + "/releases/latest";

  if (!http.begin(client, url)) {
    lastError = "GitHub HTTP Fehler";
    return false;
  }

  http.addHeader("User-Agent", "dynamic-price-clock-esp32");
  http.addHeader("Accept", "application/vnd.github+json");
  if (githubToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + githubToken);
  }

  int code = http.GET();

  if (code != 200) {
    lastError = "GitHub API Fehler " + String(code);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError jsonErr = deserializeJson(doc, http.getStream());
  http.end();

  if (jsonErr) {
    lastError = "GitHub JSON Fehler";
    return false;
  }

  String tag = String(doc["tag_name"] | "");
  tag.trim();
  if (tag.startsWith("v") || tag.startsWith("V")) tag = tag.substring(1);

  String binUrl = "";
  JsonArray assets = doc["assets"];
  for (JsonObject asset : assets) {
    String name = String(asset["name"] | "");
    if (name.endsWith(".bin")) {
      binUrl = String(asset["browser_download_url"] | "");
      break;
    }
  }

  if (tag.length() == 0 || binUrl.length() == 0) {
    lastError = "Kein gueltiges Release gefunden (Tag oder .bin-Datei fehlt)";
    return false;
  }

  githubLatestVersion = tag;
  githubLatestUrl = binUrl;
  lastError = "";
  return true;
}

// Leiser Zwilling von checkGithubUpdate() fuer den periodischen Hintergrund-
// Check (Nav-Badge): identische GitHub-Abfrage, aber schreibt NIE in den
// globalen lastError, damit ein fehlgeschlagener Hintergrund-Check nicht den
// Status-OK/Fehler-Indikator auf der Startseite verfaelscht.
bool checkGithubUpdateQuiet() {
  versionCheckLatest = "";

  if (githubRepo.length() == 0) {
    versionCheckError = "Kein GitHub-Repository hinterlegt";
    return false;
  }

  if (apMode || WiFi.status() != WL_CONNECTED) {
    versionCheckError = "Kein Internet / AP Modus";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);

  String url = "https://api.github.com/repos/" + githubRepo + "/releases/latest";

  if (!http.begin(client, url)) {
    versionCheckError = "GitHub HTTP Fehler";
    return false;
  }

  http.addHeader("User-Agent", "dynamic-price-clock-esp32");
  http.addHeader("Accept", "application/vnd.github+json");
  if (githubToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + githubToken);
  }

  int code = http.GET();

  if (code != 200) {
    versionCheckError = "GitHub API Fehler " + String(code);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError jsonErr = deserializeJson(doc, http.getStream());
  http.end();

  if (jsonErr) {
    versionCheckError = "GitHub JSON Fehler";
    return false;
  }

  String tag = String(doc["tag_name"] | "");
  tag.trim();
  if (tag.startsWith("v") || tag.startsWith("V")) tag = tag.substring(1);

  if (tag.length() == 0) {
    versionCheckError = "Kein gueltiges Release gefunden";
    return false;
  }

  versionCheckLatest = tag;
  versionCheckError = "";
  return true;
}

bool performGithubUpdate(String url) {
  if (url.length() == 0 || url.indexOf("github") < 0) {
    lastError = "Ungueltige Update-URL";
    return false;
  }

  if (apMode || WiFi.status() != WL_CONNECTED) {
    lastError = "Kein Internet / AP Modus";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.rebootOnUpdate(false);
  // GitHub-Release-Downloads leiten per HTTP-Redirect auf einen anderen Host
  // (objects.githubusercontent.com) um. Ohne Redirect-Follow schlaegt der
  // Download mit "Wrong HTTP Code" fehl, da nur die 302-Antwort ankommt.
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  otaBytesWritten = 0;
  otaBytesTotal = 0;
  httpUpdate.onProgress([](int cur, int total) {
    otaBytesWritten = cur;
    otaBytesTotal = total;
  });
  t_httpUpdate_return ret = httpUpdate.update(client, url);

  if (ret != HTTP_UPDATE_OK) {
    lastError = "Update fehlgeschlagen: " + httpUpdate.getLastErrorString();
    return false;
  }

  return true;
}

// Laeuft in einem eigenen FreeRTOS-Task, damit server.handleClient() im
// Hauptloop waehrend des Downloads weiterlaeuft und /otaprogress bedienen
// kann. otaPendingUrl wird von handleGithubUpdate() vor dem Task-Start gesetzt.
void otaUpdateTask(void *param) {
  bool ok = performGithubUpdate(otaPendingUrl);

  otaTaskSuccess = ok;
  otaTaskError = lastError;
  otaTaskDone = true;
  otaTaskRunning = false;

  if (ok) {
    delay(800);
    ESP.restart();
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
      metricSecondLow60Day = metricLow60Day;
      metricSecondLow60DayTime = metricLow60DayTime;

      metricLow60Day = avg;
      metricLow60DayTime = quarterTimes[i];
    } else if (metricSecondLow60Day < 0 || avg < metricSecondLow60Day) {
      if (quarterTimes[i] != metricLow60DayTime) {
        metricSecondLow60Day = avg;
        metricSecondLow60DayTime = quarterTimes[i];
      }
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
}

void saveKioskWidget(bool landscape, int i, KioskWidgetLayout item) {
  String prefix = (landscape ? "kgL" : "kgP") + String(i);

  prefs.putUChar((prefix + "c").c_str(), item.colStart);
  prefs.putUChar((prefix + "s").c_str(), item.colSpan);
  prefs.putUChar((prefix + "r").c_str(), item.rowStart);
  prefs.putUChar((prefix + "p").c_str(), item.rowSpan);
  prefs.putBool((prefix + "v").c_str(), item.visible);
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

  String html;
  html.reserve(24500);

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

  html += "<section class='card'><div class='panelTitle'><h2>Aktuelle Werte</h2><div style='display:flex;gap:8px;flex-wrap:wrap'>";
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
      html += "<span class='badge errb'>Jetzt teuer</span>";
    } else if (nowCent >= ledYellowCent) {
      html += "<span class='badge warnb'>Jetzt mittel</span>";
    } else {
      html += "<span class='badge okb'>Jetzt guenstig</span>";
    }
  }
  html += "<span class='badge ";
  html += (lastError.length() == 0) ? "okb'><span class='status-dot pulse ok'></span>Status OK" : "errb'><span class='status-dot pulse'></span>Fehler";
  html += "</span></div></div>";
  html += "<div class='gaugeWrap'>";
  html += buildPriceGaugeSvg();
  html += "</div>";
  String liveHomeText = "";
  if (livePowerW >= 0 && millis() - livePowerUpdatedAtMs < 60000) {
    liveHomeText = "&#9889; Aktueller Verbrauch: <b>" + formatLivePowerValue() + "</b>";
  }
  html += "<div class='live-power' id='livePowerBadge'>" + liveHomeText + "</div>";
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

  html += "<div class='small' style='display:flex;flex-wrap:wrap;gap:6px 16px;margin-top:16px'>";
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
  html += "</section>";

  html += "<section class='card'><div class='panelTitle'><h2>Preisverlauf</h2><span class='badge okb'>";
  html += String(quarterCount);
  html += " Slots</span></div>";
  html += buildSvgChart();
  html += "<div style='display:flex;flex-wrap:wrap;gap:14px;margin-top:10px;font-size:12px;color:var(--muted)'>";
  html += "<span><span style='display:inline-block;width:10px;height:10px;border-radius:50%;background:#2563eb;margin-right:5px'></span>Preis</span>";
  html += "<span><span style='display:inline-block;width:10px;height:10px;border-radius:50%;background:#0d9488;margin-right:5px'></span>Jetzt</span>";
  html += "<span><span style='display:inline-block;width:10px;height:10px;border-radius:50%;background:#b45309;margin-right:5px'></span>Tiefstpreis (60 Min)</span>";
  html += "<span><span style='display:inline-block;width:10px;height:2px;background:#5b6478;margin-right:5px;vertical-align:middle'></span>Tagesschnitt</span>";
  html += "</div></section>";


  if (quarterCount > 0) {
    html += "<details class='card'><summary><h2 style='display:inline'>Geladene Preise heute/morgen (";
    html += String(quarterCount);
    html += " Slots)</h2></summary>";
    html += "<table><tr><th>Zeit</th><th>ct/kWh</th></tr>";

    for (int i = 0; i < quarterCount; i++) {
      html += "<tr><td>";
      html += htmlEscape(quarterTimes[i]);
      html += "</td><td>";
      html += String(euroToCentRounded(quarterPrices[i]));
      html += "</td></tr>";
    }

    html += "</table></details>";
  }

  html += "<p class='small'><a href='/json'>JSON-API (fuer Entwickler/Automatisierung)</a></p>";
  html += htmlFooter();

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

// -----------------------------------------------------------------------------
// Konto / Sicherheit
// -----------------------------------------------------------------------------

void handleAccountPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(17500);

  html += htmlHeader("Konto");
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(250,204,21,.20),rgba(251,146,60,.14))'><h1>Konto &amp; Sicherheit</h1><p>Admin-Login, Setup-WLAN-Passwort, allgemeine Geraeteeinstellungen und Firmware-Updates.</p></section>";
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
  html += "<div style='background:var(--surface-border);border-radius:999px;height:10px;overflow:hidden'><div id='ghProgressBar' style='background:var(--accent);height:100%;width:0%;transition:width .3s'></div></div>";
  html += "<p id='ghProgressText' class='small' style='margin-top:6px'></p>";
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

  html += R"JS(
<script>
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
async function pollGhProgress() {
  var wrap = document.getElementById('ghProgressWrap');
  var bar = document.getElementById('ghProgressBar');
  var text = document.getElementById('ghProgressText');
  var msg = document.getElementById('ghUpdateMsg');

  try {
    const r = await fetch('/otaprogress', { cache: 'no-store' });
    const j = await r.json();

    if (j.percent >= 0) {
      bar.style.width = j.percent + '%';
      text.innerText = j.percent + '% (' + formatBytes(j.bytesWritten) + ' / ' + formatBytes(j.bytesTotal) + ')';
    } else {
      text.innerText = 'Download startet...';
    }

    if (j.done) {
      if (j.success) {
        bar.style.width = '100%';
        ghBadge('okb', 'Fertig');
        msg.innerText = 'Update erfolgreich, das Geraet startet jetzt neu...';
      } else {
        ghBadge('errb', 'Fehler');
        msg.innerText = j.error || 'Update fehlgeschlagen.';
        wrap.style.display = 'none';
      }
      return;
    }

    setTimeout(pollGhProgress, 700);
  } catch (e) {
    // Verbindung weg, waehrend ein Update lief -> Geraet startet gerade neu, das ist erwartet.
    bar.style.width = '100%';
    ghBadge('okb', 'Neustart');
    msg.innerText = 'Verbindung unterbrochen - das Geraet startet gerade neu (normal nach einem Update).';
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
  msg.innerText = 'Bitte warten, das kann 1-2 Minuten dauern. Die Spannungsversorgung nicht trennen.';
  wrap.style.display = '';
  bar.style.width = '0%';
  try {
    const body = new URLSearchParams();
    body.set('url', window.ghUpdateUrl);
    const r = await fetch('/githubupdate', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' }, body: body.toString() });
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
</script>
)JS";

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
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(250,204,21,.20),rgba(251,146,60,.14))'><h1>Anbieter</h1><p>Strompreis-Quelle waehlen und Zugangsdaten hinterlegen.</p></section>";
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
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(52,211,153,.20),rgba(94,234,212,.14))'><h1>Pinout / Verdrahtung</h1><p>GPIO-Belegung fuer ESP32-C5, zwei GC9A01 TFTs, MAX7219 Matrix und WS2812B/WS2818 Tagesring.</p></section>";
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

  String html;
  html.reserve(19500);

  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>";
  html += htmlEscape(webInterfaceName + " - Tablet-Modus");
  html += "</title>";
  html += "<script>(function(){try{if(localStorage.getItem('theme')==='dark'){document.documentElement.setAttribute('data-theme','dark');}}catch(e){}})();</script>";
  html += "<link rel='stylesheet' href='/style.css?v=" ASSET_VERSION "'>";
  html += "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>";
  html += "<style>";
  html += "html,body{height:100%;overflow:hidden}";
  html += "body{padding:0!important;display:flex;align-items:center;justify-content:center}";
  html += ".kiosk-wrap{display:flex;flex-direction:column;align-items:center;max-height:100vh;padding:clamp(8px,2.2vh,20px);box-sizing:border-box;text-align:center;overflow:hidden}";
  // Portrait: 6-Spalten x 12-Zeilen Grid. Height fluessig, keine aspect-ratio noetig.
  html += ".kiosk-canvas{display:grid;grid-template-columns:repeat(" + String(KIOSK_GRID_COLS_PORTRAIT) + ",1fr);grid-template-rows:repeat(" + String(KIOSK_GRID_ROWS_PORTRAIT) + ",1fr);gap:clamp(4px,1vh,10px);width:min(97vw,600px);height:min(94vh,1200px);box-sizing:border-box;margin:0 auto}";
  html += ".kw{overflow:hidden;box-sizing:border-box;display:flex;flex-direction:column;align-items:center;justify-content:center;min-width:0;min-height:0}";
  html += ".kiosk-time{font-size:clamp(18px,5vh,46px);font-weight:800;line-height:1.1;letter-spacing:1px}";
  html += ".kiosk-date{font-size:clamp(9px,1.7vh,15px);color:var(--muted);margin-top:2px;text-transform:capitalize}";
  html += ".kw-gauge svg{width:100%;height:100%;background:transparent;border:0;margin:0}";
  html += ".kiosk-live-power{font-size:clamp(20px,4vh,42px);font-weight:800;color:var(--text);letter-spacing:0.5px}";
  html += ".kiosk-live-power:empty{display:none}";
  // TV-kompatibel: vh-basiertes Sizing statt Container-Queries. Label und Skala
  // werden per JavaScript-ResizeObserver (Fallback-Klassen: klp-mini/small/full)
  // ein-/ausgeblendet, weil aeltere TV-Browser weder @container noch cqh koennen.
  html += ".kiosk-live-power.bar{display:flex;flex-direction:column;justify-content:center;align-items:stretch;gap:clamp(3px,0.8vh,8px);width:96%;max-width:520px;margin:0 auto;padding:clamp(3px,0.8vh,10px) clamp(6px,1.5vw,16px);box-sizing:border-box}";
  html += ".kiosk-live-power.bar .klpLbl{font-size:clamp(8px,1.2vh,12px);color:var(--muted);text-transform:uppercase;letter-spacing:.3px;font-weight:700;line-height:1;text-align:center}";
  html += ".kiosk-live-power.bar .klpVal{font-size:clamp(16px,3vh,36px);font-weight:800;line-height:1;font-variant-numeric:tabular-nums;text-align:center;color:var(--text);letter-spacing:0.5px}";
  html += ".kiosk-live-power.bar .klpTrack{position:relative;width:100%;height:clamp(6px,1.3vh,14px);border-radius:999px;overflow:hidden;background:rgba(255,255,255,.12);border:1px solid var(--surface-border);box-shadow:inset 0 1px 3px rgba(0,0,0,.25)}";
  html += ".kiosk-live-power.bar .klpFill{position:absolute;top:0;left:0;bottom:0;border-radius:999px;min-width:6px;transition:width .3s var(--ease),background .3s var(--ease)}";
  html += ".kiosk-live-power.bar .klpFill.zc{background:linear-gradient(90deg,#22c55e,#4ade80)}";
  html += ".kiosk-live-power.bar .klpFill.zm{background:linear-gradient(90deg,#facc15,#fb923c)}";
  html += ".kiosk-live-power.bar .klpFill.ze{background:linear-gradient(90deg,#fb923c,#fb7185)}";
  html += ".kiosk-live-power.bar .klpScale{display:flex;justify-content:space-between;font-size:clamp(7px,1vh,10px);color:var(--muted);font-weight:600;line-height:1;padding:0 4px}";
  // JS-Fallback fuer Groessen-abhaengiges Ein-/Ausblenden (statt Container-Query)
  html += ".kiosk-live-power.bar.klp-mini .klpLbl,.kiosk-live-power.bar.klp-mini .klpScale{display:none}";
  html += ".kiosk-live-power.bar.klp-small .klpScale{display:none}";
  html += ".kiosk-status{font-size:clamp(13px,2.8vh,25px);font-weight:800;padding:clamp(4px,0.9vh,8px) clamp(10px,3vw,22px);border-radius:999px;background:var(--overlay-faint)}";
  html += ".kw-chart{touch-action:none;cursor:crosshair}";
  html += ".kiosk-chart{position:relative;flex:1;min-height:0;width:100%}";
  html += ".kiosk-chart svg{width:100%;height:100%;display:block}";
  html += ".kiosk-crosshair-line{stroke:var(--text);stroke-width:1.5;stroke-dasharray:4,4;opacity:0;pointer-events:none}";
  html += ".kiosk-crosshair-dot{fill:var(--text);stroke:#0b1224;stroke-width:2;opacity:0;pointer-events:none}";
  html += ".kiosk-tooltip{position:absolute;transform:translate(-50%,-115%);background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:6px 10px;font-size:13px;font-weight:700;white-space:nowrap;pointer-events:none;opacity:0;box-shadow:0 8px 20px var(--shadow-soft)}";
  html += ".kiosk-chart-hint{color:var(--muted);font-size:clamp(9px,1.3vh,12px);margin:clamp(10px,2vh,20px) 0 0;flex:0 0 auto}";
  html += ".kiosk-meta{color:var(--muted);font-size:clamp(9px,1.6vh,14px);display:flex;flex-wrap:wrap;gap:clamp(4px,1vh,10px);justify-content:center;align-items:center;height:100%}";
  html += ".kiosk-meta span{padding:clamp(3px,0.7vh,7px) clamp(8px,2vw,16px);border-radius:999px;background:var(--overlay-faint);border:1px solid var(--line)}";
  html += ".kiosk-meta span:empty{display:none}";
  html += ".kiosk-topbar{position:fixed;top:14px;right:14px;display:flex;gap:8px;opacity:.3;transition:opacity .2s var(--ease);z-index:10}";
  html += ".kiosk-topbar:hover{opacity:1}";
  html += ".kiosk-hint{font-size:clamp(9px,1.3vh,12px);color:var(--muted);margin-top:clamp(4px,1vh,14px);max-width:520px}";
  html += ".actions{margin-top:clamp(6px,2vh,16px)!important;justify-content:center!important}";
  html += kioskWidgetCss(kioskPortrait);
  html += "@media (orientation:landscape){";
  html += ".kiosk-canvas{grid-template-columns:repeat(" + String(KIOSK_GRID_COLS_LANDSCAPE) + ",1fr);grid-template-rows:repeat(" + String(KIOSK_GRID_ROWS_LANDSCAPE) + ",1fr);width:min(97vw,1400px);height:min(94vh,900px)}";
  html += ".kiosk-time{font-size:clamp(14px,4vh,30px)}";
  html += ".kiosk-date{font-size:clamp(8px,1.4vh,12px)}";
  html += ".kiosk-status{font-size:clamp(12px,2.4vh,22px)}";
  html += ".kiosk-live-power{font-size:clamp(18px,3.5vh,36px)}";
  html += ".kiosk-meta{font-size:clamp(8px,1.4vh,12px)}";
  html += kioskWidgetCss(kioskLandscape);
  html += "}";
  html += "</style>";
  html += "</head><body>";

  html += "<div class='kiosk-topbar'>";
  html += "<button class='secondary' type='button' onclick='enterKioskFullscreen()'>Vollbild</button>";
  html += "<a href='/'><button class='secondary' type='button'>Dashboard</button></a>";
  html += "</div>";
  html += "<div class='kiosk-wrap'>";
  html += "<div class='kiosk-canvas' id='kioskCanvas'>";

  html += "<div class='kw kw-clock'><div class='kiosk-time' id='kioskTime'>--:--</div><div class='kiosk-date' id='kioskDate'></div></div>";

  html += "<div class='kw kw-gauge' id='kioskGaugeWrap'>";
  html += buildPriceGaugeSvg();
  html += "</div>";

  html += "<div class='kw kw-status kiosk-status' id='kioskStatus' style='color:" + statusColor + "'>" + statusText + "</div>";

  bool livePowerHave = (livePowerW >= 0 && millis() - livePowerUpdatedAtMs < 60000);
  if (kioskLivePowerStyle == "bar") {
    float pct = livePowerHave ? (livePowerW / 1000.0f / livePowerMaxKw * 100.0f) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    html += "<div class='kw kw-livepower kiosk-live-power bar' id='kioskLivePower'>";
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
    html += "</div>";
  } else {
    String livePowerText = livePowerHave ? ("&#9889; " + formatLivePowerValue()) : "";
    html += "<div class='kw kw-livepower kiosk-live-power' id='kioskLivePower'>" + livePowerText + "</div>";
  }

  html += "<div class='kw kw-chart'>";
  html += "<div class='kiosk-chart' id='kioskChartWrap'>";
  html += buildSvgChart();
  html += "<div class='kiosk-tooltip' id='kioskTooltip'></div>";
  html += "</div>";
  html += "<p class='small kiosk-chart-hint'>Mit Finger oder Maus über das Diagramm fahren, um Preise zu sehen.</p>";
  html += "</div>";

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

  html += "<div class='kw kw-meta'><div class='kiosk-meta'>";
  html += "<span id='kioskLowText'>Tief heute: " + lowText + "</span>";
  html += "<span id='kioskAvgText'>Tagesdurchschnitt: " + avgText + "</span>";
  html += "<span id='kioskMonthCost'>" + monthCostText + "</span>";
  html += "<span id='kioskMonthEstimate'>" + monthEstimateText + "</span>";
  html += "<span id='kioskStandText'>Stand: " + getCurrentIsoPrefix().substring(11) + " Uhr</span>";
  html += "</div></div>";

  html += "</div>"; // kiosk-canvas

  html += "<p class='kiosk-hint' id='kioskWakeHint'></p>";

  html += "</div>";

  html += "<script>var kioskChartPoints = " + buildChartPointsJson() + ";</script>";

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

var kioskSvg = null, kioskChartWrapEl = null, kioskTooltipEl = null, kioskVLine = null, kioskDot = null;

// Haengt Fadenkreuz-Linie/-Punkt an das (ggf. gerade per AJAX neu eingesetzte)
// Diagramm-SVG an. Wird beim ersten Laden und nach jedem refreshKioskData()
// erneut aufgerufen, weil chartSvg dabei komplett ersetzt wird.
function attachKioskChartCrosshair(){
  kioskSvg = document.getElementById('priceChartSvg');
  kioskTooltipEl = document.getElementById('kioskTooltip');
  if (!kioskSvg || !kioskTooltipEl) return;

  var svgNs = 'http://www.w3.org/2000/svg';
  kioskVLine = document.createElementNS(svgNs, 'line');
  kioskVLine.setAttribute('class', 'kiosk-crosshair-line');
  kioskVLine.setAttribute('y1', '25');
  kioskVLine.setAttribute('y2', '265');
  kioskSvg.appendChild(kioskVLine);

  kioskDot = document.createElementNS(svgNs, 'circle');
  kioskDot.setAttribute('class', 'kiosk-crosshair-dot');
  kioskDot.setAttribute('r', '6');
  kioskSvg.appendChild(kioskDot);
}

function kioskNearestPoint(svgX){
  var best = kioskChartPoints[0];
  var bestDist = Math.abs(best.x - svgX);
  for (var i = 1; i < kioskChartPoints.length; i++) {
    var d = Math.abs(kioskChartPoints[i].x - svgX);
    if (d < bestDist) { bestDist = d; best = kioskChartPoints[i]; }
  }
  return best;
}

// Das Diagramm-Widget kann jetzt frei in Groesse/Seitenverhaeltnis
// veraendert werden, deshalb passt das SVG (viewBox 760x320) per Default
// preserveAspectRatio="xMidYMid meet" oft mit Letterboxing rein - die
// tatsaechliche Inhaltsflaeche ist dann kleiner als die Bounding-Box.
function getSvgContentRect(svg){
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

function kioskShowCrosshairAt(clientX, clientY){
  if (!kioskSvg || !kioskChartPoints || !kioskChartPoints.length) return;
  var rect = getSvgContentRect(kioskSvg);
  var svgX = (clientX - rect.left) / rect.width * 760;
  var pt = kioskNearestPoint(svgX);

  kioskVLine.setAttribute('x1', pt.x);
  kioskVLine.setAttribute('x2', pt.x);
  kioskVLine.style.opacity = '1';
  kioskDot.setAttribute('cx', pt.x);
  kioskDot.setAttribute('cy', pt.y);
  kioskDot.style.opacity = '1';

  var dotClientX = rect.left + (pt.x / 760) * rect.width;
  var dotClientY = rect.top + (pt.y / 320) * rect.height;
  var wrapRect = kioskChartWrapEl.getBoundingClientRect();
  kioskTooltipEl.style.left = (dotClientX - wrapRect.left) + 'px';
  kioskTooltipEl.style.top = (dotClientY - wrapRect.top) + 'px';
  kioskTooltipEl.innerText = pt.p + ' ct um ' + pt.t;
  kioskTooltipEl.style.opacity = '1';
}

function kioskHideCrosshair(){
  if (kioskVLine) kioskVLine.style.opacity = '0';
  if (kioskDot) kioskDot.style.opacity = '0';
  if (kioskTooltipEl) kioskTooltipEl.style.opacity = '0';
}

kioskChartWrapEl = document.getElementById('kioskChartWrap');
var kioskGaugeWrapEl = document.getElementById('kioskGaugeWrap');
attachKioskChartCrosshair();
if (kioskChartWrapEl) {
  kioskChartWrapEl.addEventListener('pointermove', function(e){ kioskShowCrosshairAt(e.clientX, e.clientY); });
  kioskChartWrapEl.addEventListener('pointerdown', function(e){ kioskShowCrosshairAt(e.clientX, e.clientY); });
  kioskChartWrapEl.addEventListener('pointerleave', kioskHideCrosshair);
}

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
      attachKioskChartCrosshair();
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

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
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
  json += "\"chartSvg\":\"" + jsonEscapeValue(buildSvgChart()) + "\",";
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

  String json = "{\"text\":\"" + jsonEscapeValue(text) + "\",\"pct\":" + String(pct, 1) + ",\"zone\":\"" + zone + "\",\"max\":" + String(livePowerMaxKw, 1) + "}";

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

  if (index < 0 || index >= KIOSK_WIDGET_COUNT) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Ungueltiger Index\"}");
    return;
  }

  uint8_t cols = landscape ? KIOSK_GRID_COLS_LANDSCAPE : KIOSK_GRID_COLS_PORTRAIT;
  uint8_t rows = landscape ? KIOSK_GRID_ROWS_LANDSCAPE : KIOSK_GRID_ROWS_PORTRAIT;

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

  if (landscape) {
    kioskLandscape[index] = item;
  } else {
    kioskPortrait[index] = item;
  }
  saveKioskWidget(landscape, index, item);

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleResetKioskLayout() {
  if (!checkAuth()) return;

  String orientation = server.hasArg("orientation") ? server.arg("orientation") : "";
  bool doPortrait = (orientation == "portrait" || orientation.length() == 0);
  bool doLandscape = (orientation == "landscape" || orientation.length() == 0);

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

  String html;
  html.reserve(9500);

  html += htmlHeader("Kiosk-Layout");
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(96,165,250,.20),rgba(219,39,119,.10))'><h1>Kiosk-Layout</h1><p>Anordnung des Tablet-Modus frei per Ziehen/Skalieren gestalten - getrennt fuer Hoch- und Querformat.</p></section>";
  html += navTabs("/kiosklayout");

  html += R"CSS(<style>
.kl-shell{display:flex;gap:16px;flex-wrap:wrap;margin-top:14px}
.kl-canvas-wrap{flex:1;min-width:280px;display:flex;justify-content:center}
.kl-canvas{position:relative;width:min(360px,90vw);aspect-ratio:9/16;display:grid;grid-template-columns:repeat(6,1fr);grid-template-rows:repeat(12,1fr);gap:2px;padding:6px;background:var(--overlay-faint);border:1px solid var(--line);border-radius:12px;overflow:hidden;touch-action:none;box-sizing:border-box}
.kl-canvas.landscape{width:min(560px,90vw);aspect-ratio:16/9;grid-template-columns:repeat(12,1fr);grid-template-rows:repeat(8,1fr)}
.kl-cell{background:rgba(96,165,250,.06);border-radius:2px;pointer-events:none}
.kl-item{position:relative;border:2px dashed var(--accent2);background:rgba(96,165,250,.14);border-radius:6px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;padding:4px;cursor:move;box-sizing:border-box;user-select:none;overflow:visible;text-align:center;min-width:0;min-height:0;z-index:2}
.kl-item.selected{border-style:solid;background:rgba(96,165,250,.24)}
.kl-item.kl-hidden{opacity:.32;border-style:dotted}
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

  html += "<section class='card'>";
  html += "<div class='panelTitle'><h2>Anordnung</h2><div style='display:flex;gap:8px'>";
  html += "<button type='button' id='klTabPortrait' onclick=\"klSwitchOrientation('portrait')\">Hochformat</button>";
  html += "<button type='button' class='secondary' id='klTabLandscape' onclick=\"klSwitchOrientation('landscape')\">Querformat</button>";
  html += "</div></div>";
  html += "<p class='small'>Elemente ziehen zum Verschieben, Punkt unten rechts zum Skalieren. Auge-Symbol = ein-/ausblenden. Aenderungen werden automatisch gespeichert.</p>";

  html += "<div class='kl-shell'>";
  html += "<div class='kl-canvas-wrap'><div class='kl-canvas' id='klCanvas'></div></div>";
  html += "<div class='kl-layers' id='klLayers'></div>";
  html += "</div>";

  html += "<div class='actions'><button type='button' class='secondary' onclick='klReset()'>Diese Ausrichtung zuruecksetzen</button><a href='/kiosk' target='_blank'><button type='button' class='secondary'>Tablet-Modus oeffnen</button></a><span id='klSaveState' class='badge warnb'>Bereit</span></div>";
  html += "</section>";

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

  html += "<script>var klData = {portrait:" + kioskLayoutJson(kioskPortrait) + ",landscape:" + kioskLayoutJson(kioskLandscape) + "};</script>";

  html += R"JS(<script>
var klOrientation = 'portrait';
var klSelected = 0;
var KL_GRID = {
  portrait:  { cols: 6,  rows: 12 },
  landscape: { cols: 12, rows: 8  }
};
function klGrid(){ return KL_GRID[klOrientation]; }
function klClamp(v, lo, hi){ return Math.max(lo, Math.min(hi, v)); }

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
  var items = klData[klOrientation];
  items.forEach(function(item, i){
    var el = document.createElement('div');
    el.className = 'kl-item' + (i === klSelected ? ' selected' : '') + (!item.visible ? ' kl-hidden' : '');
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
    handle.addEventListener('pointerdown', function(e){ klStartResize(e, i); });
    el.appendChild(handle);
    el.addEventListener('pointerdown', function(e){ klStartDrag(e, i); });
    canvas.appendChild(el);
  });
  klRenderLayers();
}

function klRenderLayers(){
  var wrap = document.getElementById('klLayers');
  wrap.innerHTML = '';
  klData[klOrientation].forEach(function(item, i){
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
    vis.addEventListener('click', function(e){ e.stopPropagation(); item.visible = !item.visible; klSave(i); klRenderCanvas(); });
    row.appendChild(label);
    row.appendChild(vis);
    wrap.appendChild(row);
  });
}

// Drag: verschiebt das Widget um ganze Zellen. Snap ist automatisch, weil
// Ziel-Koordinaten immer ganzzahlig sind.
function klStartDrag(e, i){
  e.preventDefault();
  klSelected = i;
  var canvas = document.getElementById('klCanvas');
  var rect = canvas.getBoundingClientRect();
  var g = klGrid();
  var cellW = rect.width / g.cols;
  var cellH = rect.height / g.rows;
  var item = klData[klOrientation][i];
  var startColStart = item.colStart, startRowStart = item.rowStart;
  var startX = e.clientX, startY = e.clientY;
  function onMove(ev){
    var dc = Math.round((ev.clientX - startX) / cellW);
    var dr = Math.round((ev.clientY - startY) / cellH);
    item.colStart = klClamp(startColStart + dc, 1, g.cols - item.colSpan + 1);
    item.rowStart = klClamp(startRowStart + dr, 1, g.rows - item.rowSpan + 1);
    klRenderCanvas();
  }
  function onUp(){
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', onUp);
    klSave(i);
  }
  document.addEventListener('pointermove', onMove);
  document.addEventListener('pointerup', onUp);
}

function klStartResize(e, i){
  e.preventDefault();
  e.stopPropagation();
  klSelected = i;
  var canvas = document.getElementById('klCanvas');
  var rect = canvas.getBoundingClientRect();
  var g = klGrid();
  var cellW = rect.width / g.cols;
  var cellH = rect.height / g.rows;
  var item = klData[klOrientation][i];
  var startColSpan = item.colSpan, startRowSpan = item.rowSpan;
  var startX = e.clientX, startY = e.clientY;
  function onMove(ev){
    var dc = Math.round((ev.clientX - startX) / cellW);
    var dr = Math.round((ev.clientY - startY) / cellH);
    item.colSpan = klClamp(startColSpan + dc, 1, g.cols - item.colStart + 1);
    item.rowSpan = klClamp(startRowSpan + dr, 1, g.rows - item.rowStart + 1);
    klRenderCanvas();
  }
  function onUp(){
    document.removeEventListener('pointermove', onMove);
    document.removeEventListener('pointerup', onUp);
    klSave(i);
  }
  document.addEventListener('pointermove', onMove);
  document.addEventListener('pointerup', onUp);
}

function klSave(i){
  var item = klData[klOrientation][i];
  var state = document.getElementById('klSaveState');
  if (state) { state.className = 'badge warnb'; state.textContent = 'Speichere...'; }
  var body = new URLSearchParams();
  body.set('orientation', klOrientation);
  body.set('index', i);
  body.set('colStart', item.colStart);
  body.set('colSpan', item.colSpan);
  body.set('rowStart', item.rowStart);
  body.set('rowSpan', item.rowSpan);
  body.set('visible', item.visible ? '1' : '0');
  fetch('/savekiosklayoutajax', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(r){ return r.json(); })
    .then(function(d){
      if (state) { state.className = d.ok ? 'badge okb' : 'badge errb'; state.textContent = d.ok ? 'Gespeichert' : 'Fehler'; }
    })
    .catch(function(){ if (state) { state.className = 'badge errb'; state.textContent = 'Fehler'; } });
}

function klSwitchOrientation(o){
  klOrientation = o;
  klSelected = 0;
  document.getElementById('klTabPortrait').className = o === 'portrait' ? '' : 'secondary';
  document.getElementById('klTabLandscape').className = o === 'landscape' ? '' : 'secondary';
  klRenderCanvas();
}

function klReset(){
  var name = klOrientation === 'landscape' ? 'Querformat' : 'Hochformat';
  if (!confirm(name + ' auf Standard zuruecksetzen?')) return;
  var body = new URLSearchParams();
  body.set('orientation', klOrientation);
  fetch('/resetkiosklayout', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'}, body: body.toString() })
    .then(function(){ location.reload(); });
}

klRenderCanvas();
</script>)JS";

  html += htmlFooter();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", html);
}

void handleWifiPage() {
  if (!checkAuth()) return;

  String html;
  html.reserve(26000);
  html += htmlHeader("WLAN");
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(56,189,248,.22),rgba(96,165,250,.12))'><h1>WLAN</h1><p>WLAN-Verwaltung mit Scan, Auswahl und Live-Status.</p></section>";
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
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(167,139,250,.22),rgba(244,114,182,.12))'><h1>Displays</h1><p>Anzeigemodi, feste Overlays und der freie Layout-Editor fuer Display 1 und Display 2 - an einem Ort.</p></section>";
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

  html += R"JS(
<script>
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
</script>
)JS";

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
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(251,146,60,.22),rgba(244,114,182,.14))'><h1>WS2812B/WS2818 Tagesring</h1><p>60 LEDs zeigen den heutigen Strompreisverlauf als Tageskreis.</p></section>";
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
  html += "<section class='hero' style='background:linear-gradient(120deg,rgba(163,230,53,.20),rgba(52,211,153,.12))'><h1>8x8 Matrix</h1><p>Bis zu 4 MAX7219 LED-Matrixmodule in Daisy-Chain, jedes einzeln konfigurierbar.</p></section>";
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
// Preis-Gauge
// -----------------------------------------------------------------------------

String buildPriceGaugeSvg() {
  if (quarterCount == 0 || metricCurrent15 < 0) {
    return "<p class='small'>Noch keine Preisdaten fuer die Anzeige geladen.</p>";
  }

  float minP = quarterPrices[0];
  float maxP = quarterPrices[0];

  for (int i = 1; i < quarterCount; i++) {
    if (quarterPrices[i] < minP) minP = quarterPrices[i];
    if (quarterPrices[i] > maxP) maxP = quarterPrices[i];
  }

  if (maxP <= minP) maxP = minP + 0.01;

  float f = (metricCurrent15 - minP) / (maxP - minP);
  if (f < 0) f = 0;
  if (f > 1) f = 1;

  float angleDeg = 180.0 - (f * 180.0);
  float angleRad = angleDeg * PI / 180.0;
  int cx = 120;
  int cy = 120;
  int r = 100;
  int needleLen = r - 18;
  int tipX = cx + (int)(needleLen * cos(angleRad));
  int tipY = cy - (int)(needleLen * sin(angleRad));

  int nowCent = euroToCentRounded(metricCurrent15);
  int minCent = euroToCentRounded(minP);
  int maxCent = euroToCentRounded(maxP);

  String needleColor = "#4ade80";
  if (nowCent >= ledRedCent) {
    needleColor = "#fb7185";
  } else if (nowCent >= ledYellowCent) {
    needleColor = "#facc15";
  }

  String svg;
  svg.reserve(2200);

  svg += "<svg viewBox='0 0 240 150' xmlns='http://www.w3.org/2000/svg' style='font-family:Inter,system-ui,sans-serif'>";
  svg += "<defs><linearGradient id='gaugeGrad' x1='20' y1='0' x2='220' y2='0' gradientUnits='userSpaceOnUse'>";
  svg += "<stop offset='0%' stop-color='#4ade80'/><stop offset='50%' stop-color='#facc15'/><stop offset='100%' stop-color='#fb7185'/>";
  svg += "</linearGradient></defs>";
  svg += "<path d='M 20 120 A 100 100 0 0 1 220 120' fill='none' stroke='#1c2740' stroke-width='16' stroke-linecap='round'/>";
  svg += "<path d='M 20 120 A 100 100 0 0 1 220 120' fill='none' stroke='url(#gaugeGrad)' stroke-width='16' stroke-linecap='round' opacity='.9'/>";
  svg += "<line x1='" + String(cx) + "' y1='" + String(cy) + "' x2='" + String(tipX) + "' y2='" + String(tipY) + "' stroke='" + needleColor + "' stroke-width='4' stroke-linecap='round'/>";
  svg += "<circle cx='" + String(cx) + "' cy='" + String(cy) + "' r='7' fill='" + needleColor + "' stroke='#0b1224' stroke-width='2'/>";
  svg += "<text x='" + String(cx) + "' y='90' fill='var(--text)' font-size='30' font-weight='900' text-anchor='middle'>" + String(nowCent) + "</text>";
  svg += "<text x='" + String(cx) + "' y='108' fill='var(--muted)' font-size='12' text-anchor='middle'>ct/kWh jetzt</text>";
  svg += "<text x='20' y='140' fill='var(--muted)' font-size='11' text-anchor='start'>" + String(minCent) + " ct</text>";
  svg += "<text x='220' y='140' fill='var(--muted)' font-size='11' text-anchor='end'>" + String(maxCent) + " ct</text>";
  svg += "</svg>";

  return svg;
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
String kioskWidgetCss(KioskWidgetLayout arr[]) {
  String css;
  css.reserve(700);

  for (int i = 0; i < KIOSK_WIDGET_COUNT; i++) {
    css += ".kw-" + String(KIOSK_WIDGET_KEYS[i]) + "{";
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
// Neuladen umschalten kann.
String kioskLayoutJson(KioskWidgetLayout arr[]) {
  String json = "[";
  for (int i = 0; i < KIOSK_WIDGET_COUNT; i++) {
    if (i > 0) json += ",";
    String preview = "";
    String key = String(KIOSK_WIDGET_KEYS[i]);
    if (key == "clock") {
      preview = getDisplayTimeText();
    } else if (key == "gauge") {
      preview = (metricCurrent15 >= 0) ? (priceToCentText(metricCurrent15) + " ct/kWh") : "-- ct/kWh";
    } else if (key == "status") {
      String st, sc; getKioskPriceStatus(st, sc);
      preview = st;
    } else if (key == "livepower") {
      String p = formatLivePowerValue();
      preview = (p.length() > 0) ? ("&#9889; " + p) : "&#9889; -- W";
    } else if (key == "chart") {
      preview = "&#128200; Preisverlauf (" + String(quarterCount) + " Slots)";
    } else if (key == "meta") {
      String low = (metricLow15Day >= 0) ? priceToCentText(metricLow15Day) : "--";
      String avg = (metricDayAvg >= 0) ? priceToCentText(metricDayAvg) : "--";
      preview = "Tief " + low + " &middot; Schnitt " + avg;
    }
    json += "{";
    json += "\"key\":\"" + key + "\",";
    json += "\"label\":\"" + String(KIOSK_WIDGET_LABELS[i]) + "\",";
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

String buildSvgChart() {
  if (quarterCount == 0) {
    return "<p>Keine Diagrammdaten</p>";
  }

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

  svg += "<svg id='priceChartSvg' viewBox='0 0 760 320' xmlns='http://www.w3.org/2000/svg' style='font-family:Inter,system-ui,sans-serif'>";
  svg += "<defs><linearGradient id='chartFill' x1='0' y1='0' x2='0' y2='1'>";
  svg += "<stop offset='0%' stop-color='#60a5fa' stop-opacity='.4'/><stop offset='100%' stop-color='#60a5fa' stop-opacity='0'/>";
  svg += "</linearGradient></defs>";
  svg += "<rect x='0' y='0' width='760' height='320' fill='#0b1224' rx='16'/>";

  for (int t = 0; t <= 4; t++) {
    float value = minP + ((maxP - minP) * t / 4.0);
    int y = top + chartH - ((chartH * t) / 4);

    svg += "<line x1='" + String(left - 4) + "' y1='" + String(y) + "' x2='" + String(left + chartW) + "' y2='" + String(y) + "' stroke='#1c2740'/>";
    svg += "<text x='5' y='" + String(y + 4) + "' fill='#9aa8c7' font-size='12'>" + String(euroToCentRounded(value)) + " ct</text>";
  }

  for (int i = 0; i < quarterCount; i += 12) {
    svg += "<line x1='" + String(xs[i]) + "' y1='" + String(top + chartH) + "' x2='" + String(xs[i]) + "' y2='" + String(top + chartH + 5) + "' stroke='#26324f'/>";
    svg += "<text x='" + String(xs[i] - 14) + "' y='" + String(top + chartH + 22) + "' fill='#9aa8c7' font-size='12'>" + formatTimeOnly(quarterTimes[i]) + "</text>";
  }

  svg += "<line x1='" + String(left) + "' y1='" + String(top + chartH) + "' x2='" + String(left + chartW) + "' y2='" + String(top + chartH) + "' stroke='#26324f'/>";
  svg += "<line x1='" + String(left) + "' y1='" + String(top) + "' x2='" + String(left) + "' y2='" + String(top + chartH) + "' stroke='#26324f'/>";

  svg += "<text x='" + String(left + chartW / 2 - 40) + "' y='310' fill='#9aa8c7' font-size='13'>Uhrzeit</text>";
  svg += "<text x='8' y='18' fill='#9aa8c7' font-size='13'>ct/kWh</text>";

  String linePoints;

  for (int i = 0; i < quarterCount; i++) {
    linePoints += String(xs[i]) + "," + String(ys[i]) + " ";
  }

  String areaPoints = String(xs[0]) + "," + String(top + chartH) + " " + linePoints + String(xs[quarterCount - 1]) + "," + String(top + chartH);

  svg += "<polygon points='" + areaPoints + "' fill='url(#chartFill)' stroke='none'/>";

  if (metricDayAvg >= 0) {
    float normAvg = (metricDayAvg - minP) / (maxP - minP);
    if (normAvg < 0) normAvg = 0;
    if (normAvg > 1) normAvg = 1;
    int yAvg = top + chartH - int(normAvg * chartH);

    svg += "<line x1='" + String(left) + "' y1='" + String(yAvg) + "' x2='" + String(left + chartW) + "' y2='" + String(yAvg) + "' stroke='#9aa8c7' stroke-width='1.2' stroke-dasharray='5,5'/>";
    svg += "<text x='" + String(left + chartW - 100) + "' y='" + String(yAvg - 6) + "' fill='#9aa8c7' font-size='11'>Schnitt " + String(euroToCentRounded(metricDayAvg)) + " ct</text>";
  }

  svg += "<polyline fill='none' stroke='#60a5fa' stroke-width='3' stroke-linecap='round' stroke-linejoin='round' points='" + linePoints + "'/>";

  if (tomorrowIndex > 0) {
    svg += "<line x1='" + String(xs[tomorrowIndex]) + "' y1='" + String(top) + "' x2='" + String(xs[tomorrowIndex]) + "' y2='" + String(top + chartH) + "' stroke='#3a4a72' stroke-width='1.4' stroke-dasharray='3,4'/>";
    svg += "<text x='" + String(xs[tomorrowIndex] + 6) + "' y='" + String(top + 14) + "' fill='#7184ad' font-size='11'>Morgen</text>";
  }

  if (metricLow60Day >= 0) {
    int lowIndex = 0;

    for (int i = 0; i < quarterCount; i++) {
      if (quarterTimes[i] == metricLow60DayTime) {
        lowIndex = i;
        break;
      }
    }

    svg += "<line x1='" + String(xs[lowIndex]) + "' y1='" + String(top) + "' x2='" + String(xs[lowIndex]) + "' y2='" + String(top + chartH) + "' stroke='#facc15' stroke-width='1.6' stroke-dasharray='2,3'/>";
    svg += "<circle cx='" + String(xs[lowIndex]) + "' cy='" + String(ys[lowIndex]) + "' r='5' fill='#facc15'/>";
    svg += "<text x='" + String(xs[lowIndex] + 8) + "' y='" + String(max(top + 14, ys[lowIndex] - 8)) + "' fill='#facc15' font-size='13' font-weight='700'>" + String(euroToCentRounded(metricLow60Day)) + " ct " + formatTimeOnly(metricLow60DayTime) + "</text>";
  }

  if (nowIndex >= 0) {
    int nowCent = euroToCentRounded(quarterPrices[nowIndex]);
    String nowColor = "#4ade80";
    if (nowCent >= ledRedCent) nowColor = "#fb7185";
    else if (nowCent >= ledYellowCent) nowColor = "#facc15";

    int labelY = ys[nowIndex] - 14;
    if (labelY < top + 12) labelY = ys[nowIndex] + 22;
    int labelX = xs[nowIndex] + 8;
    if (labelX > left + chartW - 70) labelX = left + chartW - 70;

    svg += "<line x1='" + String(xs[nowIndex]) + "' y1='" + String(top) + "' x2='" + String(xs[nowIndex]) + "' y2='" + String(top + chartH) + "' stroke='#5eead4' stroke-width='1.4' stroke-dasharray='1,3'/>";
    svg += "<circle cx='" + String(xs[nowIndex]) + "' cy='" + String(ys[nowIndex]) + "' r='6' fill='" + nowColor + "' stroke='#0b1224' stroke-width='2'/>";
    svg += "<text x='" + String(labelX) + "' y='" + String(labelY) + "' fill='#5eead4' font-size='13' font-weight='700'>Jetzt " + String(nowCent) + " ct</text>";
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
  String redirectTarget = (requestedRedirect == "/anbieter" || requestedRedirect == "/account") ? requestedRedirect : "/";
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

  bool ok = checkGithubUpdate();
  bool updateAvailable = ok && githubLatestVersion.length() > 0 && githubLatestVersion != String(FIRMWARE_VERSION);

  String json = "{";
  json += "\"ok\":" + String(ok ? "true" : "false") + ",";
  json += "\"currentVersion\":\"" + jsEscape(String(FIRMWARE_VERSION)) + "\",";
  json += "\"latestVersion\":\"" + jsEscape(githubLatestVersion) + "\",";
  json += "\"updateAvailable\":" + String(updateAvailable ? "true" : "false") + ",";
  json += "\"downloadUrl\":\"" + jsEscape(githubLatestUrl) + "\",";
  json += "\"error\":\"" + jsEscape(lastError) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleVersionCheck() {
  if (!checkAuth()) return;

  bool ok = checkGithubUpdateQuiet();
  bool updateAvailable = ok && versionCheckLatest.length() > 0 && versionCheckLatest != String(FIRMWARE_VERSION);

  String json = "{";
  json += "\"ok\":" + String(ok ? "true" : "false") + ",";
  json += "\"currentVersion\":\"" + jsEscape(String(FIRMWARE_VERSION)) + "\",";
  json += "\"latestVersion\":\"" + jsEscape(versionCheckLatest) + "\",";
  json += "\"updateAvailable\":" + String(updateAvailable ? "true" : "false") + ",";
  json += "\"error\":\"" + jsEscape(versionCheckError) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleGithubUpdate() {
  if (!checkAuth()) return;

  if (otaTaskRunning) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Update laeuft bereits\"}");
    return;
  }

  String url = server.hasArg("url") ? server.arg("url") : githubLatestUrl;

  if (url.length() == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Keine Update-URL\"}");
    return;
  }

  otaPendingUrl = url;
  otaBytesWritten = 0;
  otaBytesTotal = 0;
  otaTaskDone = false;
  otaTaskSuccess = false;
  otaTaskError = "";
  otaTaskRunning = true;

  // Eigener Task, damit der Webserver waehrend des Downloads erreichbar
  // bleibt und /otaprogress den Fortschritt live zurueckgeben kann.
  xTaskCreate(otaUpdateTask, "otaUpdateTask", 8192, NULL, 1, NULL);

  server.send(200, "application/json", "{\"ok\":true,\"started\":true}");
}

void handleOtaProgress() {
  if (!checkAuth()) return;

  int percent = -1;
  if (otaBytesTotal > 0) {
    percent = (int)(((long)otaBytesWritten * 100L) / otaBytesTotal);
  }

  String json = "{";
  json += "\"running\":" + String(otaTaskRunning ? "true" : "false") + ",";
  json += "\"done\":" + String(otaTaskDone ? "true" : "false") + ",";
  json += "\"success\":" + String(otaTaskSuccess ? "true" : "false") + ",";
  json += "\"bytesWritten\":" + String(otaBytesWritten) + ",";
  json += "\"bytesTotal\":" + String(otaBytesTotal) + ",";
  json += "\"percent\":" + String(percent) + ",";
  json += "\"error\":\"" + jsEscape(otaTaskError) + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", json);
}

void handleUploadFirmwareData() {
  if (!checkAuth()) return;

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaBytesWritten = 0;
    otaBytesTotal = 0;
    otaTaskError = "";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaTaskError = "Update.begin fehlgeschlagen: " + String(Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaTaskError = "Update.write fehlgeschlagen: " + String(Update.errorString());
    }
    otaBytesWritten += upload.currentSize;
    otaBytesTotal = otaBytesWritten;
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      otaTaskError = "Update.end fehlgeschlagen: " + String(Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaTaskError = "Upload abgebrochen";
  }
}

void handleUploadFirmware() {
  if (!checkAuth()) return;

  bool ok = (otaTaskError.length() == 0) && !Update.hasError();
  String json = "{\"ok\":" + String(ok ? "true" : "false");
  if (!ok) {
    json += ",\"error\":\"" + jsEscape(otaTaskError.length() > 0 ? otaTaskError : String(Update.errorString())) + "\"";
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
  json += htmlEscape(webInterfaceName);
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
  json += "\"selectedHomeId\":\"" + selectedHomeId + "\",";
  json += "\"homeCount\":" + String(homeCount) + ",";
  json += "\"wifiSsid\":\"" + wifiSsid + "\",";
  json += "\"apMode\":" + String(apMode ? "true" : "false") + ",";
  json += "\"tlsVerified\":" + String(tibberRootCaPem.length() > 0 ? "true" : "false") + ",";

  json += "\"ip\":\"";
  if (apMode) {
    json += WiFi.softAPIP().toString();
  } else {
    json += WiFi.localIP().toString();
  }
  json += "\",";

  json += "\"error\":\"" + lastError + "\"";

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
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'SF Pro Display','Inter','Segoe UI',Roboto,sans-serif;background:var(--bg1);color:var(--text);padding:0;-webkit-tap-highlight-color:transparent;transition:background .2s var(--ease),color .2s var(--ease);font-weight:400;letter-spacing:-0.01em}
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
svg{background:#0b1224;border:1px solid var(--line);border-radius:18px;margin-top:8px;width:100%;height:320px}
.gaugeWrap{display:flex;justify-content:center;margin:6px 0 4px}
.gaugeWrap svg{width:240px;max-width:100%;height:auto;background:transparent;border:0;margin:0}
.live-power{display:flex;justify-content:center;align-items:center;gap:6px;font-size:15px;font-weight:700;color:var(--text);background:var(--overlay-faint);border-radius:999px;padding:6px 16px;margin:0 auto 10px;width:fit-content}
.live-power:empty{display:none}
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
  function poll(){
    fetch('/livepower').then(function(r){ return r.json(); }).then(function(data){
      el.innerHTML = data.text || '';
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

String htmlHeader(String title) {
  String html;
  if (title.length() == 0) title = webInterfaceName;

  html += "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='60'>";
  html += "<title>";
  html += htmlEscape(webInterfaceName + " - " + title);
  html += "</title>";
  html += "<script>(function(){try{if(localStorage.getItem('theme')==='dark'){document.documentElement.setAttribute('data-theme','dark');}}catch(e){}})();</script>";
  html += "<link rel='stylesheet' href='/style.css?v=" ASSET_VERSION "'>";
  html += "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>";
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

  if (today.length() == 0) return true;
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
