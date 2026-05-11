#include <Arduino.h>
#include <esp_camera.h>
#include <driver/rtc_io.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ======================================================================
// Camera pins: XIAO ESP32S3 Sense
// ======================================================================

static const int XCLK_PIN  = 10;
static const int PCLK_PIN  = 13;
static const int VSYNC_PIN = 38;
static const int HREF_PIN  = 47;

static const int D0_PIN = 15;
static const int D1_PIN = 17;
static const int D2_PIN = 18;
static const int D3_PIN = 16;
static const int D4_PIN = 14;
static const int D5_PIN = 12;
static const int D6_PIN = 11;
static const int D7_PIN = 48;

static const int SDA_PIN   = 40;
static const int SCL_PIN   = 39;
static const int PWDN_PIN  = -1;
static const int RESET_PIN = -1;

// ======================================================================
// SD card pins: XIAO ESP32S3 Sense
// ======================================================================

static const int SD_CS   = 21;
static const int SD_SCK  = 7;
static const int SD_MISO = 8;
static const int SD_MOSI = 9;

// ======================================================================
// User pins
// ======================================================================

static const int PIR_PIN    = 4;
static const int MAINT_PIN  = 3;
static const int IR_LED_PIN = 2;
static const int ERROR_PIN  = 5;  // HIGH = fatal error / error AP active

// Battery measurement on GPIO1 through 1/2 voltage divider
static const int BAT_ADC_PIN = 1;
static const float BAT_DIVIDER_RATIO = 2.0;

// ======================================================================
// Config files / defaults
// ======================================================================

static const char *DEFAULT_AP_SSID = "Phototrap-AP";
static const char *DEFAULT_AP_PASS = "12345678";
static const char *AP_CONFIG_FILE = "/ap_config.txt";
static const char *CAMERA_CONFIG_FILE = "/camera_config.txt";

String apSsid = DEFAULT_AP_SSID;
String apPass = DEFAULT_AP_PASS;
String uiLang = "en";

Preferences prefs;

// ======================================================================
// Runtime camera settings, loaded from /camera_config.txt
// ======================================================================

static framesize_t cameraFrameSize = FRAMESIZE_FHD;
static String cameraFrameSizeId = "FHD";

static int jpegQuality = 4;     // 4 = best/largest, 63 = worst/smallest
static uint16_t aviFps = 5;
static int xclkHz = 16000000;

static bool cameraVFlip = true;
static bool cameraHMirror = false;

static const uint32_t RECORD_BLOCK_MS = 5000;
static const uint16_t MAX_FRAMES = 2000;

struct CameraResolutionOption {
  const char *id;
  const char *label;
  framesize_t frameSize;
  uint16_t width;
  uint16_t height;
  uint16_t maxFps;
};

static const CameraResolutionOption CAMERA_RES_OPTIONS[] = {
  {"96X96",   "96 x 96",      FRAMESIZE_96X96,   96,   96,   30},
  {"QQVGA",   "160 x 120",    FRAMESIZE_QQVGA,   160,  120,  30},
  {"QCIF",    "176 x 144",    FRAMESIZE_QCIF,    176,  144,  30},
  {"HQVGA",   "240 x 176",    FRAMESIZE_HQVGA,   240,  176,  30},
  {"240X240", "240 x 240",    FRAMESIZE_240X240, 240,  240,  25},
  {"QVGA",    "320 x 240",    FRAMESIZE_QVGA,    320,  240,  25},
  {"CIF",     "400 x 296",    FRAMESIZE_CIF,     400,  296,  20},
  {"HVGA",    "480 x 320",    FRAMESIZE_HVGA,    480,  320,  20},
  {"VGA",     "640 x 480",    FRAMESIZE_VGA,     640,  480,  15},
  {"SVGA",    "800 x 600",    FRAMESIZE_SVGA,    800,  600,  10},
  {"XGA",     "1024 x 768",   FRAMESIZE_XGA,     1024, 768,  8},
  {"HD",      "1280 x 720",   FRAMESIZE_HD,      1280, 720,  6},
  {"SXGA",    "1280 x 1024",  FRAMESIZE_SXGA,    1280, 1024, 5},
  {"UXGA",    "1600 x 1200",  FRAMESIZE_UXGA,    1600, 1200, 5},
  {"FHD",     "1920 x 1080",  FRAMESIZE_FHD,     1920, 1080, 5},
  {"P_HD",    "720 x 1280",   FRAMESIZE_P_HD,    720,  1280, 4},
  {"P_3MP",   "864 x 1536",   FRAMESIZE_P_3MP,   864,  1536, 3},
  {"QXGA",    "2048 x 1536",  FRAMESIZE_QXGA,    2048, 1536, 3}
};

static const size_t CAMERA_RES_OPTION_COUNT =
  sizeof(CAMERA_RES_OPTIONS) / sizeof(CAMERA_RES_OPTIONS[0]);

// ======================================================================
// Globals
// ======================================================================

WebServer server(80);

bool maintenanceActive = false;
bool sdAvailable = false;
bool cameraAvailable = false;

bool recordingActive = false;
bool irLedOverrideActive = false;
bool irLedOutputActive = false;

String criticalError = "";

uint32_t nextVideoId = 1;

// ======================================================================
// Language helpers
// ======================================================================

bool isCzech() {
  return uiLang == "cs";
}

String tr(const String &en, const String &cs) {
  return isCzech() ? cs : en;
}

String activeLangClass(const String &lang) {
  return uiLang == lang ? " activeLang" : "";
}

void saveUiLanguage(const String &lang) {
  if (lang != "en" && lang != "cs") return;

  uiLang = lang;

  prefs.begin("phototrap", false);
  prefs.putString("ui_lang", uiLang);
  prefs.end();
}

void handleSetLanguage() {
  String lang = server.arg("lang");

  if (lang == "cs" || lang == "en") {
    saveUiLanguage(lang);
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

// ======================================================================
// Formatting / log helpers
// ======================================================================

String formatFileSize(uint64_t bytes) {
  if (bytes >= 1024ULL * 1024ULL) {
    return String((float)bytes / (1024.0f * 1024.0f), 2) + " MB";
  }

  if (bytes >= 1024ULL) {
    return String((float)bytes / 1024.0f, 1) + " KB";
  }

  return String((uint32_t)bytes) + " B";
}

static const size_t LOG_LINES = 120;
String logBuffer[LOG_LINES];
size_t logIndex = 0;

void logLine(const String &msg) {
  String line = "[" + String(millis()) + "] " + msg;

  Serial.println(line);

  logBuffer[logIndex % LOG_LINES] = line;
  logIndex++;

  if (sdAvailable) {
    File f = SD.open("/phototrap.log", FILE_APPEND);
    if (f) {
      f.println(line);
      f.close();
    }
  }
}

String logText() {
  String text;

  size_t count = min((size_t)LOG_LINES, logIndex);
  size_t start = (logIndex > LOG_LINES) ? (logIndex - LOG_LINES) : 0;

  for (size_t i = start; i < start + count; i++) {
    text += logBuffer[i % LOG_LINES];
    text += "\n";
  }

  return text;
}

void handleLog() {
  String html;

  html += "<html><head><meta charset='UTF-8'><title>Phototrap Log</title>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "</head><body>";
  html += "<h1>Phototrap Log</h1>";
  html += "<p><a href='/'>";
  html += tr("Back", "Zpět");
  html += "</a></p>";
  html += "<pre>";
  html += logText();
  html += "</pre>";
  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleLogData() {
  server.send(200, "text/plain; charset=utf-8", logText());
}

// ======================================================================
// IR LED control
// ======================================================================

void updateIrLedOutput() {
  bool shouldBeOn = recordingActive || irLedOverrideActive;

  digitalWrite(IR_LED_PIN, shouldBeOn ? HIGH : LOW);
  irLedOutputActive = shouldBeOn;
}

void setRecordingLedState(bool active) {
  recordingActive = active;
  updateIrLedOutput();
}

void setIrLedOverride(bool active) {
  if (irLedOverrideActive == active) {
    updateIrLedOutput();
    return;
  }

  irLedOverrideActive = active;
  updateIrLedOutput();

  logLine(active ? "IR LED override enabled" : "IR LED override disabled");
}

void handleLedOverride() {
  String state = server.arg("state");

  if (state == "1" || state == "on" || state == "true") {
    setIrLedOverride(true);
    server.send(200, "text/plain; charset=utf-8", tr("IR LED override ON", "Vynucené zapnutí IR LED zapnuto"));
    return;
  }

  setIrLedOverride(false);
  server.send(200, "text/plain; charset=utf-8", tr("IR LED override OFF", "Vynucené zapnutí IR LED vypnuto"));
}

// ======================================================================
// Pin / battery helpers
// ======================================================================

bool pirActive() {
  return digitalRead(PIR_PIN) == HIGH;
}

bool maintenancePinActive() {
  return digitalRead(MAINT_PIN) == HIGH;
}

uint32_t readBatteryMilliVolts() {
  const int samples = 16;
  uint32_t sum = 0;

  for (int i = 0; i < samples; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }

  uint32_t adcMv = sum / samples;
  return (uint32_t)(adcMv * BAT_DIVIDER_RATIO);
}

int batteryPercentFromMilliVolts(uint32_t mv) {
  struct Point {
    uint16_t mv;
    uint8_t pct;
  };

  static const Point table[] = {
    {4060, 100},
    {4020, 95},
    {3980, 90},
    {3940, 85},
    {3900, 80},
    {3860, 75},
    {3820, 70},
    {3780, 65},
    {3740, 60},
    {3700, 55},
    {3660, 50},
    {3620, 40},
    {3580, 30},
    {3540, 20},
    {3500, 15},
    {3450, 10},
    {3400, 5},
    {3300, 0}
  };

  if (mv >= table[0].mv) return 100;
  if (mv <= table[9].mv) return 0;

  for (int i = 0; i < 9; i++) {
    if (mv <= table[i].mv && mv >= table[i + 1].mv) {
      uint16_t mvHigh = table[i].mv;
      uint16_t mvLow  = table[i + 1].mv;
      uint8_t pctHigh = table[i].pct;
      uint8_t pctLow  = table[i + 1].pct;

      float t = (float)(mv - mvLow) / (float)(mvHigh - mvLow);
      return pctLow + (int)((pctHigh - pctLow) * t);
    }
  }

  return 0;
}

// ======================================================================
// Camera settings helpers
// ======================================================================

const CameraResolutionOption *findCameraResolutionOption(const String &id) {
  for (size_t i = 0; i < CAMERA_RES_OPTION_COUNT; i++) {
    if (id == CAMERA_RES_OPTIONS[i].id) {
      return &CAMERA_RES_OPTIONS[i];
    }
  }

  return nullptr;
}

const CameraResolutionOption *currentCameraResolutionOption() {
  const CameraResolutionOption *opt = findCameraResolutionOption(cameraFrameSizeId);
  if (opt) return opt;

  return findCameraResolutionOption("FHD");
}

void applyCameraResolutionOption(const CameraResolutionOption *opt) {
  if (!opt) {
    opt = findCameraResolutionOption("FHD");
  }

  cameraFrameSizeId = opt->id;
  cameraFrameSize = opt->frameSize;

  if (aviFps < 1) {
    aviFps = 1;
  }

  if (aviFps > opt->maxFps) {
    aviFps = opt->maxFps;
  }
}

void clampCameraSettings() {
  const CameraResolutionOption *opt = currentCameraResolutionOption();
  applyCameraResolutionOption(opt);

  if (jpegQuality < 4) jpegQuality = 4;
  if (jpegQuality > 63) jpegQuality = 63;

  if (xclkHz != 10000000 && xclkHz != 12000000 &&
      xclkHz != 16000000 && xclkHz != 20000000) {
    xclkHz = 16000000;
  }
}

bool writeCameraSettingsToSD() {
  if (!sdAvailable) {
    logLine("WARNING: Cannot write camera config, SD unavailable");
    return false;
  }

  if (SD.exists(CAMERA_CONFIG_FILE)) {
    SD.remove(CAMERA_CONFIG_FILE);
  }

  File f = SD.open(CAMERA_CONFIG_FILE, FILE_WRITE);

  if (!f) {
    logLine("ERROR: Failed to write /camera_config.txt");
    return false;
  }

  clampCameraSettings();

  f.println("# Phototrap camera settings");
  f.println("# resolution: 96X96, QQVGA, QCIF, HQVGA, 240X240, QVGA, CIF, HVGA, VGA, SVGA, XGA, HD, SXGA, UXGA, FHD, P_HD, P_3MP, QXGA");
  f.println("# jpeg_quality: 4 = best/largest, 63 = worst/smallest");
  f.println("# fps is clamped based on resolution");
  f.println("# xclk_hz: 10000000, 12000000, 16000000, 20000000");

  f.print("resolution=");
  f.println(cameraFrameSizeId);

  f.print("fps=");
  f.println(aviFps);

  f.print("jpeg_quality=");
  f.println(jpegQuality);

  f.print("xclk_hz=");
  f.println(xclkHz);

  f.print("vflip=");
  f.println(cameraVFlip ? 1 : 0);

  f.print("hmirror=");
  f.println(cameraHMirror ? 1 : 0);

  f.close();

  logLine("Camera settings written to SD: /camera_config.txt");
  return true;
}

bool loadCameraSettingsFromSD() {
  if (!sdAvailable) {
    return false;
  }

  if (!SD.exists(CAMERA_CONFIG_FILE)) {
    logLine("Camera config not found, writing defaults");
    writeCameraSettingsToSD();
    return false;
  }

  File f = SD.open(CAMERA_CONFIG_FILE, FILE_READ);

  if (!f) {
    logLine("WARNING: Failed to read /camera_config.txt");
    return false;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);

    key.trim();
    val.trim();

    if (key == "resolution") {
      const CameraResolutionOption *opt = findCameraResolutionOption(val);
      if (opt) {
        cameraFrameSizeId = val;
        cameraFrameSize = opt->frameSize;
      } else {
        logLine("WARNING: Invalid camera resolution in config: " + val);
      }
    } else if (key == "fps") {
      aviFps = (uint16_t)val.toInt();
    } else if (key == "jpeg_quality") {
      jpegQuality = val.toInt();
    } else if (key == "xclk_hz") {
      xclkHz = val.toInt();
    } else if (key == "vflip") {
      cameraVFlip = (val.toInt() != 0);
    } else if (key == "hmirror") {
      cameraHMirror = (val.toInt() != 0);
    }
  }

  f.close();

  clampCameraSettings();

  logLine("Camera settings loaded from SD");
  logLine("Resolution: " + cameraFrameSizeId);
  logLine("FPS: " + String(aviFps));
  logLine("JPEG quality: " + String(jpegQuality));
  logLine("XCLK Hz: " + String(xclkHz));
  logLine("VFlip: " + String(cameraVFlip));
  logLine("HMirror: " + String(cameraHMirror));

  return true;
}

// ======================================================================
// AP settings: NVS + SD backup
// ======================================================================

bool apSettingsAreValid(const String &ssid, const String &pass, String &error) {
  if (ssid.length() == 0) {
    error = "SSID cannot be empty";
    return false;
  }

  if (ssid.length() > 31) {
    error = "SSID must be 31 characters or less";
    return false;
  }

  if (pass.length() < 8) {
    error = "Password must be at least 8 characters";
    return false;
  }

  if (pass.length() > 63) {
    error = "Password must be 63 characters or less";
    return false;
  }

  return true;
}

void saveApSettingsToNVSOnly(const String &ssid, const String &pass) {
  prefs.begin("phototrap", false);
  prefs.putString("ap_ssid", ssid);
  prefs.putString("ap_pass", pass);
  prefs.end();
}

bool writeApSettingsToSD() {
  if (!sdAvailable) {
    return false;
  }

  if (SD.exists(AP_CONFIG_FILE)) {
    SD.remove(AP_CONFIG_FILE);
  }

  File f = SD.open(AP_CONFIG_FILE, FILE_WRITE);

  if (!f) {
    logLine("WARNING: Failed to write /ap_config.txt");
    return false;
  }

  f.println("# Phototrap AP settings");
  f.println("# Edit this file on the SD card to recover/change AP access.");
  f.println("# Password must be at least 8 characters.");
  f.print("ssid=");
  f.println(apSsid);
  f.print("pass=");
  f.println(apPass);
  f.close();

  logLine("AP settings written to SD: /ap_config.txt");
  return true;
}

bool loadApSettingsFromSD() {
  if (!sdAvailable) return false;
  if (!SD.exists(AP_CONFIG_FILE)) return false;

  File f = SD.open(AP_CONFIG_FILE, FILE_READ);

  if (!f) {
    logLine("WARNING: Failed to read /ap_config.txt");
    return false;
  }

  String ssid = "";
  String pass = "";

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);

    key.trim();
    val.trim();

    if (key == "ssid") ssid = val;
    else if (key == "pass") pass = val;
  }

  f.close();

  String error;
  if (!apSettingsAreValid(ssid, pass, error)) {
    logLine("WARNING: Ignoring invalid /ap_config.txt: " + error);
    return false;
  }

  apSsid = ssid;
  apPass = pass;

  saveApSettingsToNVSOnly(apSsid, apPass);

  logLine("AP settings loaded from SD and synced to NVS");
  logLine("AP SSID from SD: " + apSsid);

  return true;
}

void loadApSettingsFromNVS() {
  prefs.begin("phototrap", true);

  apSsid = prefs.getString("ap_ssid", DEFAULT_AP_SSID);
  apPass = prefs.getString("ap_pass", DEFAULT_AP_PASS);
  uiLang = prefs.getString("ui_lang", "en");

  prefs.end();

  String error;
  if (!apSettingsAreValid(apSsid, apPass, error)) {
    apSsid = DEFAULT_AP_SSID;
    apPass = DEFAULT_AP_PASS;
  }

  if (uiLang != "en" && uiLang != "cs") {
    uiLang = "en";
  }

  logLine("AP settings loaded from NVS");
  logLine("AP SSID: " + apSsid);
  logLine("UI language: " + uiLang);
}

void syncApSettingsWithSD() {
  if (!sdAvailable) return;

  if (loadApSettingsFromSD()) return;

  writeApSettingsToSD();
}

bool saveApSettings(const String &ssid, const String &pass, String &error) {
  String cleanSsid = ssid;
  String cleanPass = pass;

  cleanSsid.trim();
  cleanPass.trim();

  if (!apSettingsAreValid(cleanSsid, cleanPass, error)) {
    return false;
  }

  apSsid = cleanSsid;
  apPass = cleanPass;

  saveApSettingsToNVSOnly(apSsid, apPass);
  writeApSettingsToSD();

  logLine("AP settings saved");
  logLine("New AP SSID: " + apSsid);

  return true;
}

// ======================================================================
// ID / filename helpers
// ======================================================================

uint32_t parseVideoId(String name) {
  if (name.startsWith("/")) {
    name.remove(0, 1);
  }

  if (!name.startsWith("video_") || !name.endsWith(".avi")) {
    return 0;
  }

  int underscore = name.indexOf('_');
  int dot = name.lastIndexOf('.');

  if (underscore < 0 || dot <= underscore) {
    return 0;
  }

  return (uint32_t)name.substring(underscore + 1, dot).toInt();
}

uint32_t scanHighestExistingVideoId() {
  uint32_t maxIdx = 0;

  File root = SD.open("/");

  if (!root) {
    return 0;
  }

  while (true) {
    File file = root.openNextFile();
    if (!file) break;

    if (!file.isDirectory()) {
      uint32_t id = parseVideoId(file.name());
      if (id > maxIdx) {
        maxIdx = id;
      }
    }

    file.close();
  }

  root.close();

  return maxIdx;
}

uint32_t readLastUsedIdFile() {
  if (!SD.exists("/last_id.txt")) {
    return 0;
  }

  File f = SD.open("/last_id.txt", FILE_READ);
  if (!f) {
    return 0;
  }

  String s = f.readStringUntil('\n');
  f.close();
  s.trim();

  return (uint32_t)s.toInt();
}

bool writeLastUsedIdFile(uint32_t id) {
  if (SD.exists("/last_id.txt")) {
    SD.remove("/last_id.txt");
  }

  File f = SD.open("/last_id.txt", FILE_WRITE);
  if (!f) {
    logLine("WARNING: Failed to write /last_id.txt");
    return false;
  }

  f.println(String(id));
  f.close();

  return true;
}

void initVideoIdCounter() {
  if (!sdAvailable) {
    nextVideoId = 1;
    return;
  }

  uint32_t highestExisting = scanHighestExistingVideoId();
  uint32_t lastUsed = readLastUsedIdFile();

  uint32_t effectiveLast = max(highestExisting, lastUsed);

  nextVideoId = effectiveLast + 1;

  writeLastUsedIdFile(effectiveLast);

  logLine("Highest existing video ID: " + String(highestExisting));
  logLine("Last used video ID: " + String(lastUsed));
  logLine("Next video ID: " + String(nextVideoId));
}

String makeFilename() {
  if (!sdAvailable) {
    return "/video_error.avi";
  }

  for (uint32_t attempt = 0; attempt < 1000; attempt++) {
    uint32_t id = nextVideoId++;

    writeLastUsedIdFile(id);

    char buf[32];
    snprintf(buf, sizeof(buf), "/video_%05lu.avi", (unsigned long)id);

    if (!SD.exists(buf)) {
      logLine("Allocated filename: " + String(buf));
      return String(buf);
    }

    logLine("WARNING: Allocated filename already exists, skipping: " + String(buf));
  }

  return "/video_overflow.avi";
}

bool syncLastIdToHighestExisting() {
  if (!sdAvailable) {
    logLine("ERROR: Cannot sync last_id.txt, SD unavailable");
    return false;
  }

  uint32_t highestExisting = scanHighestExistingVideoId();

  if (!writeLastUsedIdFile(highestExisting)) {
    logLine("ERROR: Failed to sync last_id.txt");
    return false;
  }

  nextVideoId = highestExisting + 1;

  logLine("Synced last_id.txt to highest existing ID: " + String(highestExisting));
  logLine("Next video ID after sync: " + String(nextVideoId));

  return true;
}

void handleSyncLastId() {
  if (!sdAvailable) {
    server.send(503, "text/plain; charset=utf-8", tr("SD card unavailable", "SD karta není dostupná"));
    return;
  }

  bool ok = syncLastIdToHighestExisting();

  if (ok) {
    server.send(200, "text/plain; charset=utf-8",
                tr("last_id.txt synced. Next video ID: ",
                   "last_id.txt synchronizován. Další ID videa: ") + String(nextVideoId));
  } else {
    server.send(500, "text/plain; charset=utf-8",
                tr("Failed to sync last_id.txt", "Nepodařilo se synchronizovat last_id.txt"));
  }
}

// ======================================================================
// Camera
// ======================================================================

bool initCamera() {
  clampCameraSettings();

  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = D0_PIN;
  config.pin_d1 = D1_PIN;
  config.pin_d2 = D2_PIN;
  config.pin_d3 = D3_PIN;
  config.pin_d4 = D4_PIN;
  config.pin_d5 = D5_PIN;
  config.pin_d6 = D6_PIN;
  config.pin_d7 = D7_PIN;

  config.pin_xclk  = XCLK_PIN;
  config.pin_pclk  = PCLK_PIN;
  config.pin_vsync = VSYNC_PIN;
  config.pin_href  = HREF_PIN;

  config.pin_sccb_sda = SDA_PIN;
  config.pin_sccb_scl = SCL_PIN;

  config.pin_pwdn  = PWDN_PIN;
  config.pin_reset = RESET_PIN;

  config.xclk_freq_hz = xclkHz;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = cameraFrameSize;
  config.jpeg_quality = jpegQuality;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  if (!psramFound()) {
    logLine("WARNING: PSRAM not found");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    char buf[80];
    snprintf(buf, sizeof(buf), "Camera init failed: 0x%x", err);
    criticalError = buf;
    logLine("ERROR: " + criticalError);
    cameraAvailable = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    logLine("Camera sensor PID: 0x" + String(s->id.PID, HEX));
    s->set_vflip(s, cameraVFlip ? 1 : 0);
    s->set_hmirror(s, cameraHMirror ? 1 : 0);
  }

  cameraAvailable = true;
  logLine("Camera initialized");
  logLine("Camera config: " + cameraFrameSizeId + ", fps=" + String(aviFps) +
          ", jpeg=" + String(jpegQuality) + ", xclk=" + String(xclkHz));

  return true;
}

bool ov3660SoftStandby(bool enable) {
  if (!cameraAvailable) {
    logLine("Camera standby skipped: camera not available");
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (!s) {
    logLine("ERROR: OV3660 standby failed: sensor pointer is null");
    return false;
  }

  // OV3660 software standby / power-down.
  // Register 0x3008, bit 6 / mask 0x40.
  int r = s->set_reg(s, 0x3008, 0x40, enable ? 0x40 : 0x00);
  delay(100);

  if (r != 0) {
    logLine("ERROR: OV3660 soft standby write failed, r=" + String(r));
    return false;
  }

  logLine(enable ? "OV3660 soft standby enabled" : "OV3660 soft standby disabled");
  return true;
}

void shutdownCameraBeforeDeepSleep() {
  if (!cameraAvailable) {
    return;
  }

  ov3660SoftStandby(true);

  esp_camera_deinit();
  cameraAvailable = false;

  pinMode(XCLK_PIN, INPUT);
  pinMode(PCLK_PIN, INPUT);
  pinMode(VSYNC_PIN, INPUT);
  pinMode(HREF_PIN, INPUT);

  pinMode(D0_PIN, INPUT);
  pinMode(D1_PIN, INPUT);
  pinMode(D2_PIN, INPUT);
  pinMode(D3_PIN, INPUT);
  pinMode(D4_PIN, INPUT);
  pinMode(D5_PIN, INPUT);
  pinMode(D6_PIN, INPUT);
  pinMode(D7_PIN, INPUT);

  pinMode(SDA_PIN, INPUT);
  pinMode(SCL_PIN, INPUT);

  logLine("Camera driver deinitialized before deep sleep");
}

// ======================================================================
// SD card
// ======================================================================

bool initSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, SPI)) {
    criticalError = "Failed to mount SD card";
    sdAvailable = false;
    logLine("ERROR: " + criticalError);
    return false;
  }

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    criticalError = "No SD card detected";
    sdAvailable = false;
    logLine("ERROR: " + criticalError);
    return false;
  }

  sdAvailable = true;

  String typeName;
  if (cardType == CARD_MMC) typeName = "MMC";
  else if (cardType == CARD_SD) typeName = "SDSC";
  else if (cardType == CARD_SDHC) typeName = "SDHC";
  else typeName = "UNKNOWN";

  uint64_t cardSize = SD.cardSize() / (1024ULL * 1024ULL);

  logLine("SD card mounted");
  logLine("SD card type: " + typeName);
  logLine("SD card size MB: " + String((uint32_t)cardSize));

  syncApSettingsWithSD();
  loadCameraSettingsFromSD();
  initVideoIdCounter();

  return true;
}

void shutdownSDBeforeDeepSleep() {
  if (!sdAvailable) {
    return;
  }

  SD.end();
  sdAvailable = false;

  pinMode(SD_CS, INPUT);
  pinMode(SD_SCK, INPUT);
  pinMode(SD_MISO, INPUT);
  pinMode(SD_MOSI, INPUT);
}

// ======================================================================
// AVI writer
// ======================================================================

static File aviFile;
static File idxFile;

static uint32_t aviFrameCount = 0;
static uint32_t aviRiffSizePos = 0;
static uint32_t aviHdrlSizePos = 0;
static uint32_t aviAvihFramesPos = 0;
static uint32_t aviStrhFramesPos = 0;
static uint32_t aviMoviSizePos = 0;
static uint32_t aviMoviListPos = 0;
static uint32_t aviMoviDataStart = 0;

void writeU16(File &f, uint16_t v) {
  f.write((uint8_t *)&v, 2);
}

void writeU32(File &f, uint32_t v) {
  f.write((uint8_t *)&v, 4);
}

void writeFourCC(File &f, const char *s) {
  f.write((const uint8_t *)s, 4);
}

uint32_t aviTell() {
  return aviFile.position();
}

void aviPatchU32(uint32_t pos, uint32_t value) {
  uint32_t cur = aviFile.position();
  aviFile.seek(pos);
  writeU32(aviFile, value);
  aviFile.seek(cur);
}

bool aviBegin(const String &filename, uint16_t width, uint16_t height) {
  if (SD.exists(filename.c_str())) {
    logLine("ERROR: Refusing to overwrite existing AVI file: " + filename);
    return false;
  }

  aviFile = SD.open(filename.c_str(), FILE_WRITE);

  if (!aviFile) {
    logLine("ERROR: Failed to create AVI file: " + filename);
    return false;
  }

  String idxName = filename;
  idxName.replace(".avi", ".idx");

  if (SD.exists(idxName.c_str())) {
    SD.remove(idxName.c_str());
  }

  idxFile = SD.open(idxName.c_str(), FILE_WRITE);

  if (!idxFile) {
    logLine("ERROR: Failed to create AVI index file");
    aviFile.close();
    return false;
  }

  aviFrameCount = 0;

  writeFourCC(aviFile, "RIFF");
  aviRiffSizePos = aviTell();
  writeU32(aviFile, 0);
  writeFourCC(aviFile, "AVI ");

  writeFourCC(aviFile, "LIST");
  aviHdrlSizePos = aviTell();
  writeU32(aviFile, 0);
  uint32_t hdrlStart = aviTell();
  writeFourCC(aviFile, "hdrl");

  writeFourCC(aviFile, "avih");
  writeU32(aviFile, 56);
  writeU32(aviFile, 1000000UL / aviFps);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0x10);

  aviAvihFramesPos = aviTell();
  writeU32(aviFile, 0);

  writeU32(aviFile, 0);
  writeU32(aviFile, 1);
  writeU32(aviFile, 0);
  writeU32(aviFile, width);
  writeU32(aviFile, height);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);

  writeFourCC(aviFile, "LIST");
  uint32_t strlSizePos = aviTell();
  writeU32(aviFile, 0);
  uint32_t strlStart = aviTell();
  writeFourCC(aviFile, "strl");

  writeFourCC(aviFile, "strh");
  writeU32(aviFile, 56);
  writeFourCC(aviFile, "vids");
  writeFourCC(aviFile, "MJPG");
  writeU32(aviFile, 0);
  writeU16(aviFile, 0);
  writeU16(aviFile, 0);
  writeU32(aviFile, 0);
  writeU32(aviFile, 1);
  writeU32(aviFile, aviFps);
  writeU32(aviFile, 0);

  aviStrhFramesPos = aviTell();
  writeU32(aviFile, 0);

  writeU32(aviFile, 0);
  writeU32(aviFile, 0xFFFFFFFF);
  writeU32(aviFile, 0);
  writeU16(aviFile, 0);
  writeU16(aviFile, 0);
  writeU16(aviFile, width);
  writeU16(aviFile, height);

  writeFourCC(aviFile, "strf");
  writeU32(aviFile, 40);
  writeU32(aviFile, 40);
  writeU32(aviFile, width);
  writeU32(aviFile, height);
  writeU16(aviFile, 1);
  writeU16(aviFile, 24);
  writeFourCC(aviFile, "MJPG");
  writeU32(aviFile, width * height * 3);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);
  writeU32(aviFile, 0);

  uint32_t strlEnd = aviTell();
  aviPatchU32(strlSizePos, strlEnd - strlStart);

  uint32_t hdrlEnd = aviTell();
  aviPatchU32(aviHdrlSizePos, hdrlEnd - hdrlStart);

  writeFourCC(aviFile, "LIST");
  aviMoviSizePos = aviTell();
  writeU32(aviFile, 0);
  aviMoviListPos = aviTell();
  writeFourCC(aviFile, "movi");
  aviMoviDataStart = aviTell();

  logLine("AVI started: " + filename);

  return true;
}

bool aviAddFrame(camera_fb_t *fb) {
  if (!aviFile || !idxFile || !fb) {
    return false;
  }

  uint32_t chunkStart = aviTell();

  writeFourCC(aviFile, "00dc");
  writeU32(aviFile, fb->len);
  aviFile.write(fb->buf, fb->len);

  if (fb->len & 1) {
    uint8_t pad = 0;
    aviFile.write(&pad, 1);
  }

  uint32_t offset = chunkStart - aviMoviDataStart;

  writeFourCC(idxFile, "00dc");
  writeU32(idxFile, 0x10);
  writeU32(idxFile, offset);
  writeU32(idxFile, fb->len);

  aviFrameCount++;

  return true;
}

void aviEnd(const String &filename) {
  if (!aviFile) {
    return;
  }

  uint32_t idx1Start = aviTell();

  idxFile.flush();
  idxFile.close();

  String idxName = filename;
  idxName.replace(".avi", ".idx");

  File idxRead = SD.open(idxName.c_str(), FILE_READ);

  writeFourCC(aviFile, "idx1");
  writeU32(aviFile, aviFrameCount * 16);

  if (idxRead) {
    uint8_t buf[512];

    while (idxRead.available()) {
      size_t n = idxRead.read(buf, sizeof(buf));
      aviFile.write(buf, n);
    }

    idxRead.close();
    SD.remove(idxName.c_str());
  } else {
    logLine("WARNING: AVI index file missing");
  }

  uint32_t fileEnd = aviTell();

  aviPatchU32(aviRiffSizePos, fileEnd - 8);
  aviPatchU32(aviMoviSizePos, idx1Start - aviMoviListPos);
  aviPatchU32(aviAvihFramesPos, aviFrameCount);
  aviPatchU32(aviStrhFramesPos, aviFrameCount);

  aviFile.flush();
  aviFile.close();

  logLine("AVI finished: " + filename + ", frames=" + String(aviFrameCount));
}

// ======================================================================
// Recording
// ======================================================================

bool recordSegment(const String &filename) {
  if (!cameraAvailable || !sdAvailable) {
    logLine("ERROR: Cannot record, camera or SD unavailable");
    return false;
  }

  logLine("Recording: " + filename);

  camera_fb_t *first = esp_camera_fb_get();

  if (!first) {
    logLine("ERROR: First camera capture failed");
    return false;
  }

  uint16_t width = first->width;
  uint16_t height = first->height;

  if (!aviBegin(filename, width, height)) {
    esp_camera_fb_return(first);
    return false;
  }

  setRecordingLedState(true);

  aviAddFrame(first);
  esp_camera_fb_return(first);

  uint32_t nextStop = millis() + RECORD_BLOCK_MS;
  uint32_t frameCount = 1;

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
      logLine("ERROR: Camera capture failed during recording");
      break;
    }

    aviAddFrame(fb);
    esp_camera_fb_return(fb);
    frameCount++;

    uint32_t now = millis();
    bool motion = pirActive();

    if (now >= nextStop) {
      if (motion) {
        nextStop = now + RECORD_BLOCK_MS;
        logLine("Motion still active, extending recording");
      } else {
        logLine("Motion stopped, ending recording");
        break;
      }
    }

    if (frameCount >= MAX_FRAMES) {
      logLine("WARNING: Max frame count reached");
      break;
    }

    if (maintenanceActive) {
      server.handleClient();
    }

    delay(1000 / aviFps);
  }

  setRecordingLedState(false);

  aviEnd(filename);

  logLine("Finished recording, frames=" + String(frameCount));

  return frameCount > 0;
}

// ======================================================================
// Web: stream
// ======================================================================

void handleStream() {
  if (!cameraAvailable) {
    server.send(503, "text/plain; charset=utf-8", tr("Camera unavailable", "Kamera není dostupná"));
    return;
  }

  bool streamLedOverride = false;

  if (server.hasArg("led")) {
    String ledArg = server.arg("led");
    streamLedOverride = (ledArg == "1" || ledArg == "on" || ledArg == "true");
  }

  setIrLedOverride(streamLedOverride);

  WiFiClient client = server.client();

  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Cache-Control: no-cache\r\n");
  client.print("Connection: close\r\n\r\n");

  logLine(streamLedOverride ? "Live stream client connected, IR LED override ON"
                            : "Live stream client connected, IR LED override OFF");

  while (client.connected() && maintenanceActive) {
    if (pirActive()) {
      logLine("Stream stopped because PIR became active");
      break;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
      delay(100);
      continue;
    }

    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: ");
    client.print(fb->len);
    client.print("\r\n\r\n");

    client.write(fb->buf, fb->len);
    client.print("\r\n");

    esp_camera_fb_return(fb);

    delay(100);
  }

  setIrLedOverride(false);
  logLine("Live stream client disconnected");
}

// ======================================================================
// Web: files
// ======================================================================

String filesListHTML() {
  String html;

  if (!sdAvailable) {
    html += "<p class='bad'>";
    html += tr("SD card unavailable", "SD karta není dostupná");
    html += "</p>";
    return html;
  }

  File root = SD.open("/");

  if (!root) {
    html += "<p class='bad'>";
    html += tr("Failed to open SD root", "Nepodařilo se otevřít kořen SD karty");
    html += "</p>";
    return html;
  }

  html += "<p><button onclick='syncLastId()'>";
  html += tr("Sync last_id.txt to highest existing video ID", "Synchronizovat last_id.txt podle nejvyššího existujícího ID videa");
  html += "</button></p>";

  html += "<p class='note'>";
  html += tr("Current next video ID: ", "Aktuální další ID videa: ");
  html += String(nextVideoId);
  html += "</p>";

  html += "<ul>";

  while (true) {
    File file = root.openNextFile();
    if (!file) break;

    String name = file.name();

    if (!file.isDirectory() && name.endsWith(".avi")) {
      String cleanName = name;

      if (cleanName.startsWith("/")) {
        cleanName.remove(0, 1);
      }

      html += "<li><a href='/download?name=" + cleanName + "'>";
      html += name;
      html += "</a> ";
      html += formatFileSize(file.size());
      html += " <button onclick=\"deleteFile('";
      html += cleanName;
      html += "')\">";
      html += tr("Delete", "Smazat");
      html += "</button></li>";
    }

    file.close();
  }

  root.close();

  html += "</ul>";

  return html;
}

void handleFiles() {
  String html;

  html += "<html><head><meta charset='UTF-8'><title>Phototrap Files</title></head><body>";
  html += "<h1>";
  html += tr("Recorded files", "Nahrané soubory");
  html += "</h1>";
  html += "<p><a href='/'>";
  html += tr("Back", "Zpět");
  html += "</a></p>";
  html += filesListHTML();
  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleFilesData() {
  server.send(200, "text/html; charset=utf-8", filesListHTML());
}

void handleDownload() {
  if (!sdAvailable) {
    server.send(503, "text/plain; charset=utf-8", tr("SD card unavailable", "SD karta není dostupná"));
    return;
  }

  String name = server.arg("name");

  if (name.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", tr("Missing filename", "Chybí název souboru"));
    return;
  }

  name.replace("/", "");

  String path = "/" + name;

  File file = SD.open(path.c_str(), FILE_READ);

  if (!file) {
    server.send(404, "text/plain; charset=utf-8", tr("File not found", "Soubor nenalezen"));
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.sendHeader("Cache-Control", "no-cache");

  server.streamFile(file, "video/avi");
  file.close();

  logLine("Downloaded file: " + path);
}

void handleDeleteFile() {
  if (!sdAvailable) {
    server.send(503, "text/plain; charset=utf-8", tr("SD card unavailable", "SD karta není dostupná"));
    return;
  }

  String name = server.arg("name");

  if (name.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", tr("Missing filename", "Chybí název souboru"));
    return;
  }

  name.replace("/", "");

  if (!name.startsWith("video_") || !name.endsWith(".avi")) {
    server.send(400, "text/plain; charset=utf-8", tr("Invalid filename", "Neplatný název souboru"));
    return;
  }

  String path = "/" + name;

  if (!SD.exists(path.c_str())) {
    server.send(404, "text/plain; charset=utf-8", tr("File not found", "Soubor nenalezen"));
    return;
  }

  bool ok = SD.remove(path.c_str());

  if (ok) {
    logLine("Deleted file: " + path);
    server.send(200, "text/plain; charset=utf-8", tr("Deleted ", "Smazáno ") + name);
  } else {
    logLine("ERROR: Failed to delete file: " + path);
    server.send(500, "text/plain; charset=utf-8", tr("Failed to delete ", "Nepodařilo se smazat ") + name);
  }
}

// ======================================================================
// Web: settings
// ======================================================================

String apSettingsHTML() {
  String html;

  html += "<div class='card'>";
  html += "<h2>";
  html += tr("AP settings", "Nastavení AP");
  html += "</h2>";

  html += "<p class='note'>";
  html += tr("Changes are saved to ESP32 flash and also to SD card as /ap_config.txt.",
             "Změny se uloží do paměti ESP32 a také na SD kartu jako /ap_config.txt.");
  html += "</p>";

  html += "<form id='settingsForm' onsubmit='saveSettings(event)'>";

  html += "<label>";
  html += tr("AP SSID", "Název AP");
  html += "</label><br>";

  html += "<input name='ssid' value='";
  html += apSsid;
  html += "' maxlength='31'><br>";

  html += "<label>";
  html += tr("AP password", "Heslo AP");
  html += "</label><br>";

  html += "<input name='pass' value='";
  html += apPass;
  html += "' minlength='8' maxlength='63'><br>";

  html += "<p class='note'>";
  html += tr("Password must be at least 8 characters.",
             "Heslo musí mít alespoň 8 znaků.");
  html += "</p>";

  html += "<button type='submit'>";
  html += tr("Save AP settings", "Uložit nastavení AP");
  html += "</button>";

  html += "</form>";
  html += "<p id='settingsResult'></p>";
  html += "</div>";

  return html;
}

String cameraSettingsHTML() {
  clampCameraSettings();

  const CameraResolutionOption *cur = currentCameraResolutionOption();

  String html;

  html += "<div class='card'>";
  html += "<h2>";
  html += tr("Camera settings", "Nastavení kamery");
  html += "</h2>";

  html += "<p class='note'>";
  html += tr("Settings are saved to SD card as /camera_config.txt.",
             "Nastavení se ukládá na SD kartu jako /camera_config.txt.");
  html += "</p>";

  html += "<form id='cameraForm' onsubmit='saveCameraSettings(event)'>";

  html += "<label>";
  html += tr("Resolution", "Rozlišení");
  html += "</label><br>";

  html += "<select name='resolution' id='cameraResolution' onchange='updateCameraFpsLimit()'>";

  for (size_t i = 0; i < CAMERA_RES_OPTION_COUNT; i++) {
    html += "<option value='";
    html += CAMERA_RES_OPTIONS[i].id;
    html += "' data-maxfps='";
    html += String(CAMERA_RES_OPTIONS[i].maxFps);
    html += "'";

    if (cameraFrameSizeId == CAMERA_RES_OPTIONS[i].id) {
      html += " selected";
    }

    html += ">";
    html += CAMERA_RES_OPTIONS[i].id;
    html += " - ";
    html += CAMERA_RES_OPTIONS[i].label;
    html += " / max ";
    html += String(CAMERA_RES_OPTIONS[i].maxFps);
    html += " fps";
    html += "</option>";
  }

  html += "</select><br><br>";

  html += "<label>FPS</label><br>";
  html += "<input name='fps' id='cameraFps' type='number' min='1' max='";
  html += String(cur ? cur->maxFps : 5);
  html += "' value='";
  html += String(aviFps);
  html += "'><br>";

  html += "<p class='note' id='fpsLimitNote'>";
  html += tr("Maximum FPS for this resolution: ", "Maximální FPS pro toto rozlišení: ");
  html += String(cur ? cur->maxFps : 5);
  html += "</p>";

  html += "<label>";
  html += tr("JPEG quality", "Kvalita JPEG");
  html += "</label><br>";

  html += "<input name='jpeg_quality' type='number' min='4' max='63' value='";
  html += String(jpegQuality);
  html += "'>";

  html += "<p class='note'>";
  html += tr("4 = best quality/largest file, 63 = worst quality/smallest file.",
             "4 = nejlepší kvalita/největší soubor, 63 = nejhorší kvalita/nejmenší soubor.");
  html += "</p>";

  html += "<label>";
  html += tr("XCLK frequency", "Frekvence XCLK");
  html += "</label><br>";

  html += "<select name='xclk_hz'>";

  const int xclkOptions[] = {10000000, 12000000, 16000000, 20000000};

  for (int i = 0; i < 4; i++) {
    html += "<option value='";
    html += String(xclkOptions[i]);
    html += "'";

    if (xclkHz == xclkOptions[i]) {
      html += " selected";
    }

    html += ">";
    html += String(xclkOptions[i] / 1000000);
    html += " MHz";
    html += "</option>";
  }

  html += "</select><br><br>";

  html += "<label><input type='checkbox' name='vflip' value='1'";
  if (cameraVFlip) html += " checked";
  html += "> ";
  html += tr("Vertical flip", "Otočit svisle");
  html += "</label><br>";

  html += "<label><input type='checkbox' name='hmirror' value='1'";
  if (cameraHMirror) html += " checked";
  html += "> ";
  html += tr("Horizontal mirror", "Zrcadlit vodorovně");
  html += "</label><br>";

  html += "<button type='submit'>";
  html += tr("Save camera settings", "Uložit nastavení kamery");
  html += "</button>";

  html += "</form>";
  html += "<p id='cameraSettingsResult'></p>";
  html += "</div>";

  return html;
}

String settingsHTML() {
  String html;
  html += apSettingsHTML();
  html += cameraSettingsHTML();
  return html;
}

void handleSettingsData() {
  server.send(200, "text/html; charset=utf-8", settingsHTML());
}

void handleSaveSettings() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  String error;
  bool ok = saveApSettings(ssid, pass, error);

  if (!ok) {
    server.send(400, "text/plain; charset=utf-8", error);
    return;
  }

  server.send(200, "text/plain; charset=utf-8",
              tr("Settings saved to ESP32 and SD. Restart AP or reboot to apply.",
                 "Nastavení uloženo do ESP32 a na SD kartu. Pro použití restartujte AP nebo zařízení."));
}

void handleSaveCameraSettings() {
  if (!sdAvailable) {
    server.send(503, "text/plain; charset=utf-8",
                tr("SD card unavailable, cannot save camera settings",
                   "SD karta není dostupná, nastavení kamery nelze uložit"));
    return;
  }

  String resolution = server.arg("resolution");
  const CameraResolutionOption *opt = findCameraResolutionOption(resolution);

  if (!opt) {
    server.send(400, "text/plain; charset=utf-8",
                tr("Invalid resolution", "Neplatné rozlišení"));
    return;
  }

  cameraFrameSizeId = resolution;
  cameraFrameSize = opt->frameSize;

  aviFps = (uint16_t)server.arg("fps").toInt();
  jpegQuality = server.arg("jpeg_quality").toInt();
  xclkHz = server.arg("xclk_hz").toInt();

  cameraVFlip = server.hasArg("vflip");
  cameraHMirror = server.hasArg("hmirror");

  clampCameraSettings();

  if (!writeCameraSettingsToSD()) {
    server.send(500, "text/plain; charset=utf-8",
                tr("Failed to write camera settings to SD",
                   "Nepodařilo se uložit nastavení kamery na SD kartu"));
    return;
  }

  bool restartOk = true;

  if (cameraAvailable && !recordingActive) {
    logLine("Restarting camera after settings change");

    esp_camera_deinit();
    cameraAvailable = false;
    delay(300);

    restartOk = initCamera();
  }

  if (!restartOk) {
    server.send(500, "text/plain; charset=utf-8",
                tr("Camera settings saved, but camera restart failed",
                   "Nastavení kamery bylo uloženo, ale restart kamery selhal"));
    return;
  }

  server.send(200, "text/plain; charset=utf-8",
              tr("Camera settings saved to SD and applied",
                 "Nastavení kamery bylo uloženo na SD kartu a použito"));
}

// ======================================================================
// Web: pages
// ======================================================================

void handleErrorPage() {
  String html;

  html += "<html><head><meta charset='UTF-8'><title>Phototrap Error</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;background:#f2f2f2;color:#222;}";
  html += ".card{background:white;border-radius:8px;padding:16px;box-shadow:0 1px 4px #aaa;}";
  html += ".bad{color:red;font-weight:bold;font-size:24px;}";
  html += "button{font-size:16px;padding:10px 14px;border:0;border-radius:6px;margin:2px;background:#555;color:white;}";
  html += "button.activeLang{background:#0b79d0;}";
  html += "pre{white-space:pre-wrap;background:#111;color:#0f0;padding:12px;border-radius:8px;overflow:auto;}";
  html += "</style>";
  html += "</head><body>";

  html += "<div class='card'>";
  html += "<p>";
  html += "<button class='" + activeLangClass("en") + "' onclick=\"location.href='/set-lang?lang=en'\">EN</button>";
  html += "<button class='" + activeLangClass("cs") + "' onclick=\"location.href='/set-lang?lang=cs'\">CZ</button>";
  html += "</p>";

  html += "<h1>";
  html += tr("Phototrap critical error", "Kritická chyba fotopasti");
  html += "</h1>";

  html += "<p class='bad'>";
  html += criticalError.length() ? criticalError : tr("Unknown critical error", "Neznámá kritická chyba");
  html += "</p>";

  html += "<p>";
  html += tr("The device started the emergency WiFi access point because normal startup failed.",
             "Zařízení spustilo nouzový WiFi přístupový bod, protože běžné spuštění selhalo.");
  html += "</p>";

  html += "<p><a href='/log'>Log</a></p>";
  html += "</div>";

  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleHomePage() {
  uint32_t batMv = readBatteryMilliVolts();
  int batPct = batteryPercentFromMilliVolts(batMv);

  String html;

  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Phototrap</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;background:#f2f2f2;color:#222;}";
  html += "header{background:#222;color:white;padding:12px 16px;font-size:22px;}";
  html += "nav{display:flex;gap:6px;background:#333;padding:8px;position:sticky;top:0;z-index:10;flex-wrap:wrap;}";
  html += "button{font-size:16px;padding:10px 14px;border:0;border-radius:6px;margin:2px;}";
  html += "nav button{background:#555;color:white;}";
  html += "nav button.active{background:#0b79d0;}";
  html += "button.activeLang{background:#0b79d0;color:white;}";
  html += ".page{display:none;padding:16px;}";
  html += ".page.active{display:block;}";
  html += ".card{background:white;border-radius:8px;padding:14px;margin-bottom:14px;box-shadow:0 1px 4px #aaa;}";
  html += ".ok{color:green;font-weight:bold;}";
  html += ".bad{color:red;font-weight:bold;}";
  html += "img{max-width:100%;border:1px solid #999;background:#111;}";
  html += "pre{white-space:pre-wrap;background:#111;color:#0f0;padding:12px;border-radius:8px;overflow:auto;max-height:70vh;}";
  html += "a{font-size:17px;}";
  html += ".note{font-size:13px;color:#555;}";
  html += "li{margin:8px 0;}";
  html += "input,select{font-size:18px;width:100%;max-width:420px;padding:8px;margin:4px 0 12px 0;box-sizing:border-box;}";
  html += ".switchRow{display:flex;align-items:center;gap:12px;margin:12px 0;}";
  html += ".switch{position:relative;display:inline-block;width:58px;height:32px;}";
  html += ".switch input{opacity:0;width:0;height:0;}";
  html += ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#ccc;transition:.2s;border-radius:34px;}";
  html += ".slider:before{position:absolute;content:'';height:24px;width:24px;left:4px;bottom:4px;background:white;transition:.2s;border-radius:50%;}";
  html += ".switch input:checked + .slider{background:#0b79d0;}";
  html += ".switch input:checked + .slider:before{transform:translateX(26px);}";
  html += "</style>";

  html += "</head><body>";

  html += "<header>Phototrap maintenance</header>";

  html += "<nav>";
  html += "<button id='btnStatus' onclick=\"showPage('status')\">";
  html += tr("Status", "Stav");
  html += "</button>";
  html += "<button id='btnStream' onclick=\"showPage('stream')\">";
  html += tr("Stream", "Přenos");
  html += "</button>";
  html += "<button id='btnFiles' onclick=\"showPage('files')\">";
  html += tr("Files", "Soubory");
  html += "</button>";
  html += "<button id='btnLog' onclick=\"showPage('log')\">Log</button>";
  html += "<button id='btnSettings' onclick=\"showPage('settings')\">";
  html += tr("Settings", "Nastavení");
  html += "</button>";
  html += "<button class='" + activeLangClass("en") + "' onclick=\"setLang('en')\">EN</button>";
  html += "<button class='" + activeLangClass("cs") + "' onclick=\"setLang('cs')\">CZ</button>";
  html += "</nav>";

  html += "<section id='pageStatus' class='page'>";
  html += "<div class='card'><h2>";
  html += tr("Status", "Stav");
  html += "</h2>";

  html += "<p>PIR: ";
  html += pirActive() ? "<span class='bad'>HIGH</span>" : "<span class='ok'>LOW</span>";
  html += "</p>";

  html += "<p>";
  html += tr("Maintenance pin", "Servisní pin");
  html += ": ";
  html += maintenancePinActive() ? "<span class='ok'>HIGH</span>" : "<span class='bad'>LOW</span>";
  html += "</p>";

  html += "<p>SD: ";
  html += sdAvailable ? "<span class='ok'>OK</span>" : "<span class='bad'>ERROR</span>";
  html += "</p>";

  html += "<p>";
  html += tr("Camera", "Kamera");
  html += ": ";
  html += cameraAvailable ? "<span class='ok'>OK</span>" : "<span class='bad'>ERROR</span>";
  html += "</p>";

  html += "<p>";
  html += tr("Camera config", "Nastavení kamery");
  html += ": ";
  html += cameraFrameSizeId + ", " + String(aviFps) + " fps, JPEG " + String(jpegQuality);
  html += "</p>";

  html += "<p>";
  html += tr("IR LED", "IR LED");
  html += ": ";
  html += irLedOutputActive ? "<span class='ok'>ON</span>" : "<span>OFF</span>";
  html += " / ";
  html += tr("override", "vynucení");
  html += ": ";
  html += irLedOverrideActive ? "<span class='bad'>ON</span>" : "<span>OFF</span>";
  html += "</p>";

  html += "<p>";
  html += tr("Battery", "Baterie");
  html += ": ";
  html += String(batMv);
  html += " mV / ";
  html += String(batPct);
  html += "%</p>";

  html += "<p class='note'>";
  html += tr("Battery percentage is approximate.", "Procenta baterie jsou pouze orientační.");
  html += "</p>";

  html += "<p>";
  html += tr("Next video ID", "Další ID videa");
  html += ": ";
  html += String(nextVideoId);
  html += "</p>";

  html += "<p>AP SSID: ";
  html += apSsid;
  html += "</p>";

  if (criticalError.length()) {
    html += "<p>";
    html += tr("Critical error", "Kritická chyba");
    html += ": <span class='bad'>";
    html += criticalError;
    html += "</span></p>";
  }

  html += "</div></section>";

  html += "<section id='pageStream' class='page'>";
  html += "<div class='card'><h2>";
  html += tr("Live stream", "Živý přenos");
  html += "</h2>";

  html += "<p><button onclick='toggleStream()' id='streamToggle'>";
  html += tr("Start stream", "Spustit přenos");
  html += "</button></p>";

  html += "<div class='switchRow'><label class='switch'>";
  html += "<input type='checkbox' id='ledOverrideSwitch' onchange='toggleLedOverride(this.checked)'>";
  html += "<span class='slider'></span></label><span>";
  html += tr("Force IR LED ON", "Vynutit zapnutí IR LED");
  html += "</span></div>";

  html += "<p class='note'>";
  html += tr("This can only force the LED on. Recording can still turn it on regardless of this switch. Leaving this tab turns override off.",
             "Toto umí LED pouze vynuceně zapnout. Nahrávání ji může zapnout vždy bez ohledu na přepínač. Při opuštění této záložky se vynucení vypne.");
  html += "</p>";

  html += "<img id='streamImg' src='' style='display:none;' width='640'>";
  html += "</div></section>";

  html += "<section id='pageFiles' class='page'>";
  html += "<div class='card'><h2>";
  html += tr("Recorded files", "Nahrané soubory");
  html += "</h2><button onclick='refreshFiles()'>";
  html += tr("Refresh files", "Obnovit soubory");
  html += "</button><div id='filesBox'>Loading...</div></div></section>";

  html += "<section id='pageLog' class='page'>";
  html += "<div class='card'><h2>Log</h2><button onclick='refreshLog()'>";
  html += tr("Refresh log", "Obnovit log");
  html += "</button><pre id='logBox'>Loading...</pre></div></section>";

  html += "<section id='pageSettings' class='page'>";
  html += "<div id='settingsBox'>Loading...</div>";
  html += "</section>";

  html += "<script>";

  html += "let currentPage='status';";
  html += "let streamOn=false;";
  html += "let logTimer=null;";
  html += "let ledOverrideWanted=false;";

  html += "const TXT_START_STREAM='";
  html += tr("Start stream", "Spustit přenos");
  html += "';";
  html += "const TXT_STOP_STREAM='";
  html += tr("Stop stream", "Zastavit přenos");
  html += "';";
  html += "const TXT_DELETE_Q='";
  html += tr("Delete ", "Smazat ");
  html += "';";
  html += "const TXT_DELETE_FAIL='";
  html += tr("Delete failed", "Smazání selhalo");
  html += "';";
  html += "const TXT_SYNC_Q='";
  html += tr("Rewrite last_id.txt to highest existing video ID?", "Přepsat last_id.txt podle nejvyššího existujícího ID videa?");
  html += "';";
  html += "const TXT_SYNC_FAIL='";
  html += tr("Sync failed", "Synchronizace selhala");
  html += "';";
  html += "const TXT_PASS_SHORT='";
  html += tr("Password must be at least 8 characters", "Heslo musí mít alespoň 8 znaků");
  html += "';";
  html += "const TXT_FILES_FAIL='";
  html += tr("Failed to load files", "Nepodařilo se načíst soubory");
  html += "';";
  html += "const TXT_LOG_FAIL='";
  html += tr("Failed to load log", "Nepodařilo se načíst log");
  html += "';";
  html += "const TXT_SETTINGS_FAIL='";
  html += tr("Failed to load settings", "Nepodařilo se načíst nastavení");
  html += "';";
  html += "const TXT_SAVE_FAIL='";
  html += tr("Save failed", "Uložení selhalo");
  html += "';";
  html += "const TXT_CAM_SAVE_FAIL='";
  html += tr("Camera settings save failed", "Uložení nastavení kamery selhalo");
  html += "';";
  html += "const TXT_MAX_FPS='";
  html += tr("Maximum FPS for this resolution: ", "Maximální FPS pro toto rozlišení: ");
  html += "';";

  html += "function cap(s){return s.charAt(0).toUpperCase()+s.slice(1);}";

  html += "function streamUrl(){return '/stream?led='+(ledOverrideWanted?'1':'0')+'&t='+Date.now();}";

  html += "function setLang(lang){stopStream();stopLedOverride();fetch('/set-lang?lang='+encodeURIComponent(lang)).then(()=>{location.reload();});}";

  html += "function showPage(name){";
  html += "currentPage=name;";
  html += "document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));";
  html += "document.querySelectorAll('nav button').forEach(b=>b.classList.remove('active'));";
  html += "document.getElementById('page'+cap(name)).classList.add('active');";
  html += "document.getElementById('btn'+cap(name)).classList.add('active');";
  html += "if(name!=='stream'){stopStream();stopLedOverride();}";
  html += "if(name==='files'){refreshFiles();}";
  html += "if(name==='settings'){refreshSettings();}";
  html += "if(name==='log'){refreshLog();startLogAuto();}else{stopLogAuto();}";
  html += "}";

  html += "function startStream(){";
  html += "streamOn=true;";
  html += "const img=document.getElementById('streamImg');";
  html += "img.style.display='block';";
  html += "img.src=streamUrl();";
  html += "document.getElementById('streamToggle').innerText=TXT_STOP_STREAM;";
  html += "}";

  html += "function restartStream(){";
  html += "if(!streamOn)return;";
  html += "const img=document.getElementById('streamImg');";
  html += "img.src='';";
  html += "setTimeout(()=>{if(streamOn){img.src=streamUrl();}},300);";
  html += "}";

  html += "function stopStream(){";
  html += "streamOn=false;";
  html += "ledOverrideWanted=false;";
  html += "const sw=document.getElementById('ledOverrideSwitch');";
  html += "if(sw){sw.checked=false;}";
  html += "const img=document.getElementById('streamImg');";
  html += "if(img){img.src='';img.style.display='none';}";
  html += "const btn=document.getElementById('streamToggle');";
  html += "if(btn){btn.innerText=TXT_START_STREAM;}";
  html += "}";

  html += "function toggleStream(){if(streamOn){stopStream();}else{startStream();}}";
  html += "function toggleLedOverride(on){ledOverrideWanted=on;restartStream();}";
  html += "function stopLedOverride(){ledOverrideWanted=false;const sw=document.getElementById('ledOverrideSwitch');if(sw){sw.checked=false;}}";

  html += "function refreshFiles(){";
  html += "stopStream();stopLedOverride();";
  html += "fetch('/files-data?t='+Date.now()).then(r=>r.text()).then(t=>{document.getElementById('filesBox').innerHTML=t;}).catch(e=>{document.getElementById('filesBox').innerHTML=TXT_FILES_FAIL;});";
  html += "}";

  html += "function deleteFile(name){";
  html += "stopStream();stopLedOverride();";
  html += "if(!confirm(TXT_DELETE_Q+name+'?'))return;";
  html += "fetch('/delete?name='+encodeURIComponent(name),{method:'POST'}).then(r=>r.text()).then(t=>{alert(t);refreshFiles();}).catch(e=>{alert(TXT_DELETE_FAIL);});";
  html += "}";

  html += "function syncLastId(){";
  html += "stopStream();stopLedOverride();";
  html += "if(!confirm(TXT_SYNC_Q))return;";
  html += "fetch('/sync-last-id',{method:'POST'}).then(r=>r.text()).then(t=>{alert(t);refreshFiles();}).catch(e=>{alert(TXT_SYNC_FAIL);});";
  html += "}";

  html += "function refreshLog(){";
  html += "stopStream();stopLedOverride();";
  html += "fetch('/log-data?t='+Date.now()).then(r=>r.text()).then(t=>{document.getElementById('logBox').innerText=t;}).catch(e=>{document.getElementById('logBox').innerText=TXT_LOG_FAIL;});";
  html += "}";

  html += "function startLogAuto(){if(logTimer)return;logTimer=setInterval(refreshLog,3000);}";
  html += "function stopLogAuto(){if(logTimer){clearInterval(logTimer);logTimer=null;}}";

  html += "function refreshSettings(){";
  html += "stopStream();stopLedOverride();";
  html += "fetch('/settings-data?t='+Date.now()).then(r=>r.text()).then(t=>{";
  html += "document.getElementById('settingsBox').innerHTML=t;";
  html += "updateCameraFpsLimit();";
  html += "}).catch(e=>{document.getElementById('settingsBox').innerHTML=TXT_SETTINGS_FAIL;});";
  html += "}";

  html += "function saveSettings(e){";
  html += "e.preventDefault();";
  html += "const form=document.getElementById('settingsForm');";
  html += "const fd=new FormData(form);";
  html += "const pass=fd.get('pass') || '';";
  html += "if(pass.length<8){document.getElementById('settingsResult').innerText=TXT_PASS_SHORT;return;}";
  html += "fetch('/settings-save',{method:'POST',body:new URLSearchParams(fd)}).then(async r=>{";
  html += "const t=await r.text();document.getElementById('settingsResult').innerText=t;if(r.ok){refreshSettings();}";
  html += "}).catch(e=>{document.getElementById('settingsResult').innerText=TXT_SAVE_FAIL;});";
  html += "}";

  html += "function updateCameraFpsLimit(){";
  html += "const sel=document.getElementById('cameraResolution');";
  html += "const fps=document.getElementById('cameraFps');";
  html += "const note=document.getElementById('fpsLimitNote');";
  html += "if(!sel||!fps)return;";
  html += "const max=parseInt(sel.options[sel.selectedIndex].dataset.maxfps||'5');";
  html += "fps.max=max;";
  html += "if(parseInt(fps.value)>max){fps.value=max;}";
  html += "if(note){note.innerText=TXT_MAX_FPS+max;}";
  html += "}";

  html += "function saveCameraSettings(e){";
  html += "e.preventDefault();";
  html += "stopStream();stopLedOverride();updateCameraFpsLimit();";
  html += "const form=document.getElementById('cameraForm');";
  html += "const fd=new FormData(form);";
  html += "fetch('/camera-settings-save',{method:'POST',body:new URLSearchParams(fd)}).then(async r=>{";
  html += "const t=await r.text();";
  html += "document.getElementById('cameraSettingsResult').innerText=t;";
  html += "if(r.ok){refreshSettings();}";
  html += "}).catch(e=>{document.getElementById('cameraSettingsResult').innerText=TXT_CAM_SAVE_FAIL;});";
  html += "}";

  html += "window.addEventListener('beforeunload',()=>{stopStream();stopLedOverride();});";
  html += "showPage('status');";

  html += "</script></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

// ======================================================================
// Web routes / AP
// ======================================================================

void registerCommonRoutes(bool errorOnly) {
  server.on("/set-lang", HTTP_GET, handleSetLanguage);

  server.on("/log", HTTP_GET, handleLog);
  server.on("/log-data", HTTP_GET, handleLogData);

  if (errorOnly) {
    server.on("/", HTTP_GET, handleErrorPage);
    return;
  }

  server.on("/", HTTP_GET, handleHomePage);

  server.on("/stream", HTTP_GET, handleStream);
  server.on("/led-override", HTTP_POST, handleLedOverride);

  server.on("/files", HTTP_GET, handleFiles);
  server.on("/files-data", HTTP_GET, handleFilesData);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/delete", HTTP_POST, handleDeleteFile);
  server.on("/sync-last-id", HTTP_POST, handleSyncLastId);

  server.on("/settings-data", HTTP_GET, handleSettingsData);
  server.on("/settings-save", HTTP_POST, handleSaveSettings);
  server.on("/camera-settings-save", HTTP_POST, handleSaveCameraSettings);
}

void startMaintenanceServer() {
  if (maintenanceActive) return;

  logLine("Starting maintenance WiFi AP");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  IPAddress ip = WiFi.softAPIP();

  logLine("AP SSID: " + apSsid);
  logLine("AP PASS: " + apPass);
  logLine("AP IP: " + ip.toString());

  server.stop();
  registerCommonRoutes(false);
  server.begin();

  maintenanceActive = true;
}

void stopMaintenanceServer() {
  if (!maintenanceActive) return;

  irLedOverrideActive = false;
  updateIrLedOutput();

  logLine("Stopping maintenance WiFi AP");

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  maintenanceActive = false;
}

void startErrorAP(const String &err) {
  digitalWrite(ERROR_PIN, HIGH);

  criticalError = err;

  Serial.println("CRITICAL ERROR: " + criticalError);
  logLine("CRITICAL ERROR: " + criticalError);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  IPAddress ip = WiFi.softAPIP();

  Serial.println("Error AP started");
  Serial.println("SSID: " + apSsid);
  Serial.println("PASS: " + apPass);
  Serial.println("IP: " + ip.toString());

  server.stop();
  registerCommonRoutes(true);
  server.begin();

  while (true) {
    server.handleClient();
    delay(20);
  }
}

// ======================================================================
// Deep sleep
// Wake by PIR GPIO4 OR maintenance GPIO6, both active HIGH.
// Uses EXT1 wakeup with ANY_HIGH.
// ======================================================================

void goDeepSleepIfIdle() {
  bool maint = maintenancePinActive();
  bool pir = pirActive();

  logLine("Sleep check PIR=" + String(pir) + " MAINT=" + String(maint));

  if (maint) {
    logLine("Deep sleep blocked by maintenance pin");
    return;
  }

  if (pir) {
    logLine("Deep sleep blocked by PIR pin");
    return;
  }

  irLedOverrideActive = false;
  recordingActive = false;
  updateIrLedOutput();
  digitalWrite(IR_LED_PIN, LOW);

  logLine("Preparing for deep sleep");

  shutdownCameraBeforeDeepSleep();

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  logLine("Entering deep sleep");
  logLine("Wake source armed: PIR GPIO" + String(PIR_PIN) + " OR maintenance GPIO" + String(MAINT_PIN));

  delay(100);

  shutdownSDBeforeDeepSleep();

  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(MAINT_PIN, INPUT_PULLDOWN);

  uint64_t wakeMask = (1ULL << PIR_PIN) | (1ULL << MAINT_PIN);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext1_wakeup_io(wakeMask, ESP_EXT1_WAKEUP_ANY_HIGH);

  Serial.flush();
  delay(200);

  esp_deep_sleep_start();
}

// ======================================================================
// Wake reason
// ======================================================================

void printWakeReason() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      logLine("Wakeup from deep sleep by EXT1 GPIO");

      uint64_t wakeMask = esp_sleep_get_ext1_wakeup_status();

      if (wakeMask & (1ULL << PIR_PIN)) {
        logLine("Wake source: PIR");
      }

      if (wakeMask & (1ULL << MAINT_PIN)) {
        logLine("Wake source: maintenance pin");
      }

      break;
    }

    case ESP_SLEEP_WAKEUP_EXT0:
      logLine("Wakeup from deep sleep by EXT0");
      break;

    case ESP_SLEEP_WAKEUP_TIMER:
      logLine("Wakeup from deep sleep by timer");
      break;

    default:
      logLine("Power-on or unknown wakeup");
      break;
  }
}

// ======================================================================
// Setup
// ======================================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(MAINT_PIN, INPUT_PULLDOWN);
  pinMode(IR_LED_PIN, OUTPUT);
  pinMode(ERROR_PIN, OUTPUT);
  pinMode(BAT_ADC_PIN, INPUT);

  digitalWrite(ERROR_PIN, LOW);

  recordingActive = false;
  irLedOverrideActive = false;
  updateIrLedOutput();

  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  loadApSettingsFromNVS();

  printWakeReason();

  bool initialPir = pirActive();
  bool initialMaintenance = maintenancePinActive();

  logLine("Initial PIR: " + String(initialPir));
  logLine("Initial maintenance: " + String(initialMaintenance));

  // SD first, because camera configuration is stored on SD.
  if (!initSD()) {
    startErrorAP(criticalError.length() ? criticalError : "SD card initialization failed");
    return;
  }

  if (!initCamera()) {
    startErrorAP(criticalError.length() ? criticalError : "Camera initialization failed");
    return;
  }

  if (!initialPir && !initialMaintenance) {
    logLine("No PIR and no maintenance request after init");
    goDeepSleepIfIdle();
    return;
  }

  digitalWrite(ERROR_PIN, LOW);

  if (initialMaintenance) {
    startMaintenanceServer();
  }

  while (true) {
    bool maintNow = maintenancePinActive();

    if (maintNow && !maintenanceActive) {
      startMaintenanceServer();
    }

    if (!maintNow && maintenanceActive) {
      stopMaintenanceServer();
    }

    if (maintenanceActive) {
      server.handleClient();
    }

    if (pirActive()) {
      String filename = makeFilename();
      recordSegment(filename);
    }

    if (!maintenancePinActive() && !pirActive()) {
      goDeepSleepIfIdle();
    }

    delay(50);
  }
}

void loop() {
}