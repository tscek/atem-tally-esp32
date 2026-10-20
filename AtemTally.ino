// ATEM tally light for Waveshare ESP32-C6-LCD-1.47 (ST7789 172x320, WS2812 on GPIO8)
// Talks to an ATEM switcher (Mini Extreme etc.) over the ATEM UDP protocol, port 9910.
//
// Screen: red = program, green = preview, black = idle. Large camera number,
// camera name (ATEM input label) in small text below it.
// BOOT button on the main screen: click = next camera 1..8, hold 0.5 s = menu.
// Menu (rotation, brightness, WiFi, RGB LED, info, reset): click = next item,
// hold = OK, 2 s idle = back. The backlight pulses while WiFi/ATEM is down.
// ATEM IP and camera can be changed at http://<tally IP>/. If WiFi can't be
// reached for 30 s, AP "ATEM-Tally-XXXX" (password AP_PASS) opens with the same
// page at http://192.168.4.1. Settings are stored in NVS.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "mdns.h"
#include "peers_html.h"
#include "secrets.h"  // WIFI_SSID/PASS, WIFI2_SSID/PASS, AP_PASS (copy secrets.example.h)
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <Preferences.h>
#include <Wire.h>
#include <map>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

// ---- Board pins (Waveshare wiki / demo code) ----
// Default: ESP32-C6-LCD-1.47 (ST7789, RGB LED). Build with -DBOARD_TOUCH for the
// ESP32-C6-Touch-LCD-1.47 (JD9853 display, AXS5106L touch). That board has no RGB LED of its
// own; an external WS2812 can be wired to IO8 on its header.
#ifdef BOARD_TOUCH
#define PIN_MOSI 2
#define PIN_SCLK 1
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  22
#define PIN_BL   23
#define PIN_LED  8   // header pin IO8, external WS2812 (standard GRB order)
#define LED_SWAP_RG 0
#define PIN_BOOT 9
#define PIN_TP_SDA 18
#define PIN_TP_SCL 19
#define PIN_TP_RST 20
#define TP_ADDR    0x63
#define BOARD_NAME "ESP32-C6-Touch-LCD-1.47"
#else
#define PIN_MOSI 6
#define PIN_SCLK 7
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  21
#define PIN_BL   22
#define PIN_LED  8
#define LED_SWAP_RG 1  // the on-board LED has red and green swapped
#define PIN_BOOT 9
#define BOARD_NAME "ESP32-C6-LCD-1.47"
#endif

#define ATEM_PORT      9910
#define LED_BRIGHTNESS 80
#define MAX_BUTTON_CAM 8
#define MAX_INPUTS     64
#define FW_VERSION     "2.3"
#define OTA_REPO       "tscek/atem-tally-esp32"  // GitHub releases with manifest.json + .bin per board
#define DISC_PORT      9911  // UDP broadcast port the lights use to find each other
#define HOLD_MS        500   // hold = menu / OK
#define MENU_TIMEOUT_MS 2000  // menu: back to the main screen after 2 s without input
#define EDIT_TIMEOUT_MS 10000 // ATEM IP editor, reset question
#define VIEW_TIMEOUT_MS 30000 // info and web setup screens
#define PULSE_MS       2000  // backlight pulse period when a connection is down
#define STATUS_H       22
#define BOTTOM_H       14
#define TIMEZONE       "CET-1CEST,M3.5.0,M10.5.0/3"  // Europe/Ljubljana

#define COLOR_GREEN 0x07E0
#define COLOR_GREY  0x2104
#define COLOR_MENU  0x0846

Adafruit_ST7789 tft(PIN_CS, PIN_DC, PIN_RST);
Preferences prefs;
WebServer server(80);
DNSServer dns;
WiFiUDP udp;
WiFiUDP discUdp;

IPAddress cfgAtem(192, 168, 10, 100);
uint8_t cfgCam = 1;
uint8_t cfgRot = 1;  // 0..3, 1 = landscape
uint8_t cfgBright = 3;  // 0..3 = 25/50/75/100 %
uint8_t cfgWifi = 0;    // preferred network, index into knownNets
bool cfgLed = true;      // RGB LED shows tally
bool cfgTouch = false;   // touch screen acts as the button (touch board only)
uint8_t cfgNumSize = 1;  // camera number size 0..2 (small/medium/large)
uint8_t cfgInfoSize = 0; // clock, name and bottom line size 0..2
enum ClockMode { CLOCK_OFF, CLOCK_HM, CLOCK_HMS };
uint8_t cfgClock = CLOCK_HMS;

// Known WiFi networks, stored in NVS (n0s/n0p, ...). Seeded with the two built-in ones.
#define MAX_NETS 6
String netSsid[MAX_NETS], netPass[MAX_NETS];
int netRssi[MAX_NETS];    // signal from the last scan, -999 = not seen
uint32_t renameAt = 0;    // millis() of a rename / network change waiting for its WiFi rejoin
uint32_t restartAt = 0;   // millis() of a WiFi choice on the web page; restarts 1 s later
uint32_t scanWantedAt = 0;  // web page asked for fresh signal values (see webScanLoop)
int netCount = 0;

enum UiMode { UI_MAIN, UI_MENU, UI_INFO, UI_RESET, UI_IPEDIT, UI_SETUP, UI_OTA };
UiMode uiMode = UI_MAIN;
enum BtnEvent { B_NONE, B_CLICK, B_HOLD };
enum OtaJob { OJ_IDLE, OJ_CHECKING, OJ_FOUND, OJ_LATEST, OJ_FAILED, OJ_DOWNLOADING, OJ_DONE };
enum OtaScreen { OS_CHECKING, OS_OFFER, OS_PROGRESS, OS_MESSAGE };
volatile OtaJob otaJob = OJ_IDLE;   // written by the OTA task, consumed in otaLoop
bool apMode = false;
String cfgName;          // editable name of this light, shown in the shared web list

String hostName;         // DHCP and mDNS name derived from cfgName, e.g. tally-lucka-1
String hostId;           // last 4 hex digits of the MAC, e.g. db40
String apName;

// ---- Tally state from the switcher ----
enum Tally { T_NONE, T_PREVIEW, T_PROGRAM };
uint8_t tallyFlags[MAX_INPUTS];
uint16_t tallyCount = 0;
String inputNames[MAX_INPUTS + 1];

// ---- ATEM client ----
enum AtemState { A_DISCONNECTED, A_HELLO_SENT, A_CONNECTED };
AtemState atemState = A_DISCONNECTED;
uint16_t atemSession = 0;
uint32_t lastRx = 0, lastHello = 0;
uint16_t atemNextRx = 1;  // next packet id expected from the ATEM
uint32_t atemGaps = 0;    // out-of-order packets dropped (the ATEM resends them)
uint32_t atemConnectedAt = 0;  // for the "tally shown after ... ms" log line
uint8_t buf[2048];

// ---------------------------------------------------------------- config
void loadConfig() {
  prefs.begin("tally", true);
  cfgAtem.fromString(prefs.getString("atem", "192.168.10.100"));
  cfgCam = prefs.getUChar("cam", 1);
  cfgRot = prefs.getUChar("rot", 1) & 3;
  cfgBright = prefs.getUChar("bright", 3) & 3;
  netCount = prefs.getUChar("ncount", 255);
  if (netCount > MAX_NETS) {  // never saved: built-in defaults
    netSsid[0] = WIFI_SSID;  netPass[0] = WIFI_PASS;
    netSsid[1] = WIFI2_SSID; netPass[1] = WIFI2_PASS;
    netCount = 2;
  } else {
    for (int i = 0; i < netCount; i++) {
      netSsid[i] = prefs.getString(("n" + String(i) + "s").c_str(), "");
      netPass[i] = prefs.getString(("n" + String(i) + "p").c_str(), "");
    }
  }
  cfgWifi = netCount ? prefs.getUChar("wifi", 0) % netCount : 0;
  cfgLed = prefs.getUChar("led", 1);
  cfgTouch = prefs.getUChar("touch", 0);
  cfgNumSize = prefs.getUChar("numsize", 1) % 3;
  cfgInfoSize = prefs.getUChar("infosize", 0) % 3;
  cfgClock = prefs.getUChar("clock", CLOCK_HMS) % 3;
  cfgName = prefs.getString("name", "");
  prefs.end();
  if (cfgCam < 1) cfgCam = 1;
}

void saveByte(const char *key, uint8_t v) {
  prefs.begin("tally", false);
  prefs.putUChar(key, v);
  prefs.end();
}

// ---------------------------------------------------------------- display + LED
uint16_t programSource = 0;  // PrgI, mix effect 1
uint16_t previewSource = 0;  // PrvI, mix effect 1

Tally currentTally() {
  if (atemState != A_CONNECTED) return T_NONE;
  if (cfgCam <= tallyCount) {
    uint8_t f = tallyFlags[cfgCam - 1];
    return (f & 0x01) ? T_PROGRAM : (f & 0x02) ? T_PREVIEW : T_NONE;
  }
  // No TlIn yet (or a camera beyond it): fall back to the M/E 1 program / preview source.
  return programSource == cfgCam ? T_PROGRAM : previewSource == cfgCam ? T_PREVIEW : T_NONE;
}

void setLed(Tally t) {
  if (PIN_LED < 0) return;
  if (!cfgLed) t = T_NONE;
  uint8_t r = t == T_PROGRAM ? LED_BRIGHTNESS : 0, g = t == T_PREVIEW ? LED_BRIGHTNESS : 0;
  if (LED_SWAP_RG) std::swap(r, g);  // on-board LED: red and green swapped vs rgbLedWrite's GRB order
  rgbLedWrite(PIN_LED, r, g, 0);
}

String smallLine() {
  if (atemState == A_CONNECTED) return cfgCam <= MAX_INPUTS ? inputNames[cfgCam] : "";
  if (WiFi.status() == WL_CONNECTED) return "ATEM " + cfgAtem.toString() + " ...";
  if (apMode) return "WiFi " + apName + " / 192.168.4.1";
  return "WiFi ...";
}

// ---- Phone-style status bar: clock on the left, WiFi bars on the right ----
uint16_t screenBg = COLOR_GREY, screenFg = ST77XX_WHITE;
int shownSecond = -2, shownBars = -1;

// ---- Switcher-wide state for the bottom line ----
uint16_t recStatus = 0;      // RTMS: 0 idle, 1 recording, 128 stopping
uint16_t streamStatus = 1;   // StRS: 1 idle, 2 connecting, 4 streaming, 32 stopping
std::map<uint16_t, String> shortNames;  // InPr short names of every source (MP1, BLK, ...)
uint32_t recStartMs = 0, streamStartMs = 0;  // local start time, corrected by RTMR / SRST
int recDotX = -1, liveDotX = -1, dotY = 0, dotR = 3;
bool dotOn = false;

bool isRecording() { return recStatus & 0x01; }
bool isStreaming() { return streamStatus & 0x04; }

int rssiBars(int rssi) {
  return rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  return rssiBars(WiFi.RSSI());
}

// "-52 dBm ▂▄▆█" for the web page; missing bars are drawn grey.
String rssiHtml(int rssi) {
  if (rssi <= -999) return "ni v dosegu";
  const char *b[] = {"&#9602;", "&#9604;", "&#9606;", "&#9608;"};
  String s = String(rssi) + " dBm <span style=letter-spacing:-1px>";
  int n = rssiBars(rssi);
  for (int i = 0; i < 4; i++) s += i < n ? String(b[i]) : "<span style=color:#ccc>" + String(b[i]) + "</span>";
  return s + "</span>";
}

int clockKey() {
  struct tm now;
  return getLocalTime(&now, 0) ? now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec : -1;
}

// Sizes chosen in the menu: cfgInfoSize 0..2 scales clock, camera name and bottom line.
int clockTextSize() { return cfgInfoSize + 1 + (cfgInfoSize == 0); }       // 2, 2, 3
int statusH()       { return 8 * clockTextSize() + 6; }                    // 22, 22, 30
int bottomTextSize() { return cfgInfoSize == 0 ? 1 : 2; }
int bottomH()       { return 8 * bottomTextSize() + 6; }                   // 14, 22, 22
const GFXfont *nameFont() {
  return cfgInfoSize == 0 ? &FreeSans9pt7b : cfgInfoSize == 1 ? &FreeSans12pt7b : &FreeSans18pt7b;
}

void drawClock() {
  shownSecond = clockKey();
  if (cfgClock == CLOCK_OFF) return;
  struct tm now;
  bool haveTime = getLocalTime(&now, 0);
  char clock[9];
  const char *fmt = cfgClock == CLOCK_HMS ? "%H:%M:%S" : "%H:%M";
  if (haveTime) strftime(clock, sizeof(clock), fmt, &now);
  else strcpy(clock, cfgClock == CLOCK_HMS ? "--:--:--" : "--:--");
  tft.setFont();
  tft.setTextSize(clockTextSize());
  tft.setTextColor(screenFg, screenBg);  // opaque text: no flicker on every second
  tft.setCursor(6, 3);
  tft.print(clock);
  tft.setTextSize(1);
}

// Bottom-left line: what is on program, recording and streaming state. While
// recording / streaming, a blinking red / blue dot replaces the word ON.
#define COLOR_REC  0xF800
#define COLOR_LIVE 0x2D7F

void drawDots() {
  uint16_t ring = ST77XX_BLACK;
  if (recDotX >= 0) {
    tft.fillCircle(recDotX, dotY, dotR + 1, ring);
    tft.fillCircle(recDotX, dotY, dotR, dotOn ? COLOR_REC : ring);
  }
  if (liveDotX >= 0) {
    tft.fillCircle(liveDotX, dotY, dotR + 1, ring);
    tft.fillCircle(liveDotX, dotY, dotR, dotOn ? COLOR_LIVE : ring);
  }
}

void drawBottomLine() {
  recDotX = liveDotX = -1;
  if (uiMode != UI_MAIN) return;
  int W = tft.width(), H = tft.height(), bh = bottomH();
  tft.fillRect(0, H - bh, W, bh, screenBg);
  if (atemState != A_CONNECTED) return;

  String pgm = programSource >= 1 && programSource <= MAX_INPUTS ? String(programSource)
             : shortNames.count(programSource) ? shortNames[programSource] : String(programSource);
  String rec = isRecording() ? "" : recStatus & 0x80 ? "STOP" : "OFF";
  String str = isStreaming() ? "" : (streamStatus & 2) ? "..." : (streamStatus & 32) ? "STOP" : "OFF";
  String all = "PGM " + pgm + "  REC " + (rec.length() ? rec : "  ") + "  LIVE " + (str.length() ? str : "  ");
  int size = bottomTextSize();
  if ((int)all.length() * 6 * size > W - 8) size = 1;  // doesn't fit (portrait): smaller text

  int y = H - 8 * size - 3, cw = 6 * size;
  dotR = 2 + size;
  dotY = y + 4 * size - 1;
  tft.setFont();
  tft.setTextSize(size);
  tft.setTextColor(screenFg);
  tft.setCursor(6, y);
  tft.print("PGM " + pgm + "  REC ");
  if (rec.length()) tft.print(rec);
  else { recDotX = tft.getCursorX() + cw / 2; tft.print("  "); }
  tft.print("  LIVE ");
  if (str.length()) tft.print(str);
  else liveDotX = tft.getCursorX() + cw / 2;
  tft.setTextSize(1);
  drawDots();
}

// Recording / streaming duration under the clock (red / blue on a black pill so
// it stays readable on the red and green tally backgrounds).
int timerSize() { return bottomTextSize(); }
int timerRowH() { return 8 * timerSize() + 6; }
int timersH()   { return (isRecording() + isStreaming()) * timerRowH(); }

String durationText(uint32_t startMs) {
  uint32_t s = (millis() - startMs) / 1000;
  char b[12];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return b;
}

void drawTimers() {
  if (uiMode != UI_MAIN || atemState != A_CONNECTED) return;
  int y = statusH(), size = timerSize();
  tft.setFont();
  tft.setTextSize(size);
  for (int i = 0; i < 2; i++) {
    bool on = i == 0 ? isRecording() : isStreaming();
    if (!on) continue;
    String t = String(i == 0 ? "REC " : "LIVE ") + durationText(i == 0 ? recStartMs : streamStartMs);
    tft.fillRoundRect(4, y, t.length() * 6 * size + 8, timerRowH() - 2, 4, ST77XX_BLACK);
    tft.setTextColor(i == 0 ? COLOR_REC : COLOR_LIVE, ST77XX_BLACK);
    tft.setCursor(8, y + 2);
    tft.print(t);
    y += timerRowH();
  }
  tft.setTextSize(1);
}

void blinkLoop() {
  static uint32_t last = 0;
  if (uiMode != UI_MAIN || millis() - last < 500) return;
  last = millis();
  dotOn = !dotOn;
  drawDots();
}
void drawStatusBar() {
  int W = tft.width(), sh = statusH();
  tft.fillRect(0, 0, W, sh, screenBg);
  drawClock();

  shownBars = wifiBars();
  int base = sh - 4;
  uint16_t dim = screenFg == ST77XX_WHITE ? 0x7BEF : 0x4208;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 4, x = W - 6 - (4 - i) * 6;
    if (i < shownBars) tft.fillRect(x, base - bh, 4, bh, screenFg);
    else tft.drawRect(x, base - bh, 4, bh, dim);
  }
  if (WiFi.status() != WL_CONNECTED) {  // no WiFi: cross over the bars
    tft.drawLine(W - 30, base - 16, W - 8, base, screenFg);
    tft.drawLine(W - 30, base, W - 8, base - 16, screenFg);
  }
}
void statusBarLoop() {
  static uint32_t lastCheck = 0;
  if (uiMode != UI_MAIN || millis() - lastCheck < 1000) return;
  lastCheck = millis();
  if (wifiBars() != shownBars) drawStatusBar();
  else if (clockKey() != shownSecond) drawClock();
  drawTimers();
}


void redraw() {
  switch (uiMode) {
    case UI_MAIN:  drawMain(); break;
    case UI_MENU:  drawMenu(); break;
    case UI_INFO:  openInfo(); break;
    case UI_RESET: drawResetConfirm(); break;
    case UI_IPEDIT: drawIpEdit(); break;
    case UI_SETUP: drawSetup(); break;
    case UI_OTA:   drawOta(); break;
  }
  setLed(currentTally());
}

void drawMain() {
  Tally t = currentTally();
  uint16_t bg = atemState != A_CONNECTED ? COLOR_GREY
              : t == T_PROGRAM ? ST77XX_RED
              : t == T_PREVIEW ? COLOR_GREEN : ST77XX_BLACK;
  uint16_t fg = t == T_PREVIEW ? ST77XX_BLACK : ST77XX_WHITE;
  int W = tft.width(), H = tft.height();
  int16_t x1, y1; uint16_t w, h;
  screenBg = bg;
  screenFg = fg;

  tft.fillScreen(bg);
  drawStatusBar();
  tft.setTextColor(fg);

  // Big camera number with the camera name under it, centred as a group between
  // the status bar and the bottom line. The chosen size shrinks if it can't fit.
  String num = String(cfgCam), s = smallLine();
  int areaTop = statusH() + timersH(), areaH = H - areaTop - bottomH();
  tft.setTextSize(1);
  tft.setFont(nameFont());
  tft.getTextBounds(s.length() ? s : String("Ag"), 0, 0, &x1, &y1, &w, &h);
  const GFXfont *nf = nameFont();
  if (w > W - 8) { nf = &FreeSans9pt7b; tft.setFont(nf); tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h); }
  if (w > W - 8) { nf = nullptr; tft.setFont(); tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h); }
  int nameH = h, gap = 12;

  int16_t nx1, ny1; uint16_t nw, nh;
  tft.setFont(&FreeSansBold24pt7b);
  int size = cfgNumSize + 2;  // 2, 3, 4
  for (;; size--) {
    tft.setTextSize(size);
    tft.getTextBounds(num, 0, 0, &nx1, &ny1, &nw, &nh);
    if (size == 1 || (nh + gap + nameH <= areaH - 8 && nw <= W - 8)) break;
  }
  int numTop = areaTop + (areaH - (nh + gap + nameH)) / 2;
  tft.setTextColor(fg);
  tft.setCursor((W - nw) / 2 - nx1, numTop - ny1);
  tft.print(num);

  tft.setTextSize(1);
  tft.setFont(nf);
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((W - w) / 2 - x1, numTop + nh + gap - y1);
  tft.print(s);
  drawTimers();
  drawBottomLine();
  setLed(t);
}

// ---------------------------------------------------------------- ATEM protocol
// Packet header (12 bytes): [flags:5|len:11] [session] [ackId] [0 0 0 0] [packetId]
// flags: 0x01 ack requested, 0x02 hello/SYN, 0x04 resend, 0x08 resend request, 0x10 ACK
void atemSendHello() {
  atemSession = 0x1000 + (esp_random() & 0x0FFF);
  uint8_t p[20] = {0x10, 0x14, (uint8_t)(atemSession >> 8), (uint8_t)atemSession,
                   0, 0, 0, 0, 0, 0x3A, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0};
  udp.beginPacket(cfgAtem, ATEM_PORT);
  udp.write(p, sizeof(p));
  udp.endPacket();
  atemState = A_HELLO_SENT;
  lastHello = millis();
}

void atemSendAck(uint16_t ackId) {
  uint8_t p[12] = {0x80, 0x0C, (uint8_t)(atemSession >> 8), (uint8_t)atemSession,
                   (uint8_t)(ackId >> 8), (uint8_t)ackId, 0, 0, 0, 0, 0, 0};
  udp.beginPacket(cfgAtem, ATEM_PORT);
  udp.write(p, sizeof(p));
  udp.endPacket();
}

// Set while parsing; atemLoop redraws once after all waiting packets are handled, so a
// burst (the initial state dump) isn't slowed down by a screen redraw per packet.
bool atemNeedRedraw = false, atemNeedBottom = false;

void atemParseCommands(const uint8_t *d, int len) {
  bool changed = false, bottomChanged = false;
  int off = 0;
  while (off + 8 <= len) {
    uint16_t clen = (d[off] << 8) | d[off + 1];
    if (clen < 8 || off + clen > len) break;
    const uint8_t *name = d + off + 4;
    const uint8_t *data = d + off + 8;
    int dlen = clen - 8;

    if (memcmp(name, "TlIn", 4) == 0 && dlen >= 2) {  // tally by input index
      uint16_t count = min<int>((data[0] << 8) | data[1], min(MAX_INPUTS, dlen - 2));
      Tally before = currentTally();
      memcpy(tallyFlags, data + 2, count);
      tallyCount = count;
      if (currentTally() != before) changed = true;
    } else if (memcmp(name, "InPr", 4) == 0 && dlen >= 22) {  // input properties
      uint16_t id = (data[0] << 8) | data[1];
      char shortName[5];
      memcpy(shortName, data + 22, 4);
      shortName[4] = 0;
      shortNames[id] = shortName;
      if (id >= 1 && id <= MAX_INPUTS) {
        char longName[21];
        memcpy(longName, data + 2, 20);
        longName[20] = 0;
        if (inputNames[id] != longName) {
          inputNames[id] = longName;
          if (id == cfgCam) changed = true;
        }
      }
    } else if (memcmp(name, "PrgI", 4) == 0 && dlen >= 4 && data[0] == 0) {  // program, M/E 1
      uint16_t src = (data[2] << 8) | data[3];
      Tally before = currentTally();
      if (src != programSource) { programSource = src; bottomChanged = true; }
      if (currentTally() != before) changed = true;
    } else if (memcmp(name, "PrvI", 4) == 0 && dlen >= 4 && data[0] == 0) {  // preview, M/E 1
      Tally before = currentTally();
      previewSource = (data[2] << 8) | data[3];
      if (currentTally() != before) changed = true;
    } else if (memcmp(name, "RTMS", 4) == 0 && dlen >= 2) {
      // Recording status. Bit 0 = recording, bit 7 = stopping; the other bits are
      // error/media flags (idle with a disk is 0x0002, recording 0x0003).
      uint16_t st = (data[0] << 8) | data[1];
      Serial.printf("RTMS %04X\n", st);
      if (st != recStatus) {
        bool was = isRecording();
        recStatus = st;
        if (isRecording() != was) {
          changed = true;  // layout changes (timer row)
          if (isRecording()) { recStartMs = millis(); atemSendCommand("RMDR"); }
        } else bottomChanged = true;
      }
    } else if (memcmp(name, "StRS", 4) == 0 && dlen >= 2) {
      // Streaming status: 1 idle, 2 connecting, 4 streaming, 32 stopping (+ error bits).
      uint16_t st = (data[0] << 8) | data[1];
      Serial.printf("StRS %04X\n", st);
      if (st != streamStatus) {
        bool was = isStreaming();
        streamStatus = st;
        if (isStreaming() != was) {
          changed = true;
          if (isStreaming()) { streamStartMs = millis(); atemSendCommand("SRDR"); }
        } else bottomChanged = true;
      }
    } else if ((memcmp(name, "RTMR", 4) == 0 || memcmp(name, "SRST", 4) == 0) && dlen >= 3) {
      // Recording / streaming duration (h, m, s, frames): correct the local timer.
      uint32_t ms = (data[0] * 3600UL + data[1] * 60UL + data[2]) * 1000UL;
      if (name[0] == 'R') recStartMs = millis() - ms; else streamStartMs = millis() - ms;
    }
    off += clen;
  }
  if (changed) atemNeedRedraw = true;
  else if (bottomChanged) atemNeedBottom = true;
}

// Sends one command as a reliable packet (used to ask for recording / streaming
// durations). No resend: if it gets lost, the local timer is used.
uint16_t atemLocalId = 0;
void atemSendCommand(const char *cmd) {
  if (atemState != A_CONNECTED) return;
  atemLocalId++;
  uint8_t p[20] = {0x08, 20, (uint8_t)(atemSession >> 8), (uint8_t)atemSession, 0, 0, 0, 0, 0, 0,
                   (uint8_t)(atemLocalId >> 8), (uint8_t)atemLocalId, 0, 8, 0, 0};
  memcpy(p + 16, cmd, 4);
  udp.beginPacket(cfgAtem, ATEM_PORT);
  udp.write(p, sizeof(p));
  udp.endPacket();
}

void atemDisconnect() {
  atemState = A_DISCONNECTED;
  tallyCount = 0;
  programSource = previewSource = 0;
  redraw();
}

void atemLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (atemState != A_DISCONNECTED) atemDisconnect();
    return;
  }
  uint32_t now = millis();
  if (atemState != A_CONNECTED && now - lastHello > 1000) atemSendHello();
  if (atemState == A_CONNECTED && now - lastRx > 5000) {
    Serial.println("ATEM timeout");
    atemDisconnect();
  }

  while (udp.parsePacket() > 0) {
    int len = udp.read(buf, sizeof(buf));
    if (len < 12 || udp.remoteIP() != cfgAtem) continue;
    uint8_t flags = buf[0] >> 3;
    uint16_t plen = ((buf[0] & 0x07) << 8) | buf[1];
    if (plen > len) continue;
    atemSession = (buf[2] << 8) | buf[3];
    uint16_t remoteId = (buf[10] << 8) | buf[11];
    lastRx = now;

    if (flags & 0x02) {  // hello answer
      if (len >= 13 && buf[12] == 0x02) { atemSendAck(0); atemNextRx = 1; }
      else Serial.printf("ATEM refused connection (code %d)\n", len >= 13 ? buf[12] : -1);
      continue;
    }
    if (atemState != A_CONNECTED) {
      atemState = A_CONNECTED;
      atemLocalId = 0;
      Serial.println("ATEM connected");
      atemNeedRedraw = true;
      atemConnectedAt = now;
    }
    if (!(flags & 0x01)) continue;  // only reliable packets carry state
    // Acks are cumulative: acking a packet after a gap tells the ATEM the lost ones
    // arrived too, so it never resends them (that lost the tally of the initial dump).
    // Take packets strictly in order; after a gap, drop and let the ATEM resend.
    if (remoteId == atemNextRx) {
      atemNextRx = (atemNextRx + 1) & 0x7FFF;
      atemSendAck(remoteId);
      if (plen > 12) atemParseCommands(buf + 12, plen - 12);
    } else if (((atemNextRx - remoteId) & 0x7FFF) < 0x4000) {
      atemSendAck((atemNextRx - 1) & 0x7FFF);  // already have it (a resend): ack again
    } else {
      atemGaps++;
    }
  }
  if (atemNeedRedraw) { atemNeedRedraw = atemNeedBottom = false; redraw(); }
  else if (atemNeedBottom) { atemNeedBottom = false; drawBottomLine(); }
  if (atemConnectedAt && (tallyCount || programSource)) {
    Serial.printf("ATEM tally %d shown %lu ms after connecting (%lu dropped)\n", (int)currentTally(),
                  millis() - atemConnectedAt, atemGaps);
    atemConnectedAt = 0;
  }
}

// ---------------------------------------------------------------- web config
String htmlEscape(const String &s) {
  String o; for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '"') o += "&quot;"; else if (c == '&') o += "&amp;"; else o += c; }
  return o;
}

void saveNets() {
  prefs.begin("tally", false);
  prefs.putUChar("ncount", netCount);
  for (int i = 0; i < netCount; i++) {
    prefs.putString(("n" + String(i) + "s").c_str(), netSsid[i]);
    prefs.putString(("n" + String(i) + "p").c_str(), netPass[i]);
  }
  prefs.putUChar("wifi", cfgWifi);
  prefs.end();
}

void setAtemIp(IPAddress ip) {
  cfgAtem = ip;
  prefs.begin("tally", false);
  prefs.putString("atem", ip.toString());
  prefs.end();
  if (atemState != A_DISCONNECTED) atemDisconnect();  // reconnect to the new address
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

// Shared list of all lights (from /api/peers); "Uredi" loads that light's own
// /api/status and saves through its /api/set, so any light can edit any other.

// Signal of saved network k: live for the connected one, else from the last scan.
int netLiveRssi(int k) {
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == netSsid[k]) return WiFi.RSSI();
  return netRssi[k];
}

void handleRoot() {
  String h = F("<!doctype html><meta name=viewport content='width=device-width'><title>ATEM Tally</title>"
               "<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 12px}"
               "input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
               "button{padding:8px 16px}table{width:100%;border-collapse:collapse;margin-bottom:12px}"
               "td{padding:6px 0;border-bottom:1px solid #ddd}select{width:100%;padding:8px;margin:4px 0 12px}"
               ".card{border:1px solid #ccc;border-radius:8px;padding:10px;margin:8px 0}"
               ".dot{display:inline-block;width:12px;height:12px;border-radius:6px;margin-right:6px}</style>");
  h += "<h2>ATEM Tally</h2>";
  h += FPSTR(PEERS_HTML);
  h += "<h3>Ta lucka: " + htmlEscape(cfgName) + " (" + hostName + ".local)</h3>";
  h += "<p>ATEM: " + String(atemState == A_CONNECTED ? "povezan" : "ni povezave");
  h += "<br>WiFi: " + (WiFi.status() == WL_CONNECTED ? htmlEscape(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ") " + rssiHtml(WiFi.RSSI()) : String("ni povezave")) + "</p>";
  scanWantedAt = millis();  // fresh signal values for the network list next time

  h += "<h3>ATEM</h3><form method=post action=/save>ATEM IP<input name=atem value=\"" + cfgAtem.toString() + "\">";
  h += "Kamera (vhod)<input name=cam type=number min=1 max=" + String(MAX_INPUTS) + " value=" + String(cfgCam) + ">";
  h += F("<button>Shrani</button></form>");

  h += "<h3>WiFi omrezja</h3><table>";
  for (int i = 0; i < netCount; i++) {
    h += "<tr><td>" + htmlEscape(netSsid[i]) + (i == cfgWifi ? " <b>(izbrano)</b>" : "") +
         "<br><small>" + rssiHtml(netLiveRssi(i)) + "</small></td><td align=right>";
    h += "<form method=post action=/delwifi style=margin:0><input type=hidden name=i value=" + String(i) + ">";
    h += "<button>Izbrisi</button></form></td></tr>";
  }
  h += "</table>";
  if (netCount < MAX_NETS) {
    h += F("<form method=post action=/addwifi>Ime omrezja (SSID)<input name=ssid required>"
           "Geslo<input name=pass type=password><button>Dodaj</button></form>"
           "<p><small>Omrezje izberes zgoraj pri lucki (Uredi, WiFi).</small></p>");
  }
  server.send(200, "text/html; charset=utf-8", h);
}

void handleSave() {
  IPAddress ip;
  if (!ip.fromString(server.arg("atem"))) { server.send(400, "text/plain", "Napacen ATEM IP"); return; }
  cfgCam = constrain(server.arg("cam").toInt(), 1, MAX_INPUTS);
  saveByte("cam", cfgCam);
  if (ip != cfgAtem) setAtemIp(ip);
  redraw();
  redirectHome();
}

void handleAddWifi() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (!ssid.length()) { redirectHome(); return; }
  int i = 0;
  while (i < netCount && netSsid[i] != ssid) i++;  // same name: update the password
  if (i == netCount) {
    if (netCount >= MAX_NETS) { server.send(400, "text/plain", "Seznam je poln"); return; }
    netCount++;
  }
  netSsid[i] = ssid;
  netPass[i] = server.arg("pass");
  netRssi[i] = -999;
  saveNets();  // only added to the list; choosing it is a separate step
  redirectHome();
}

void handleDelWifi() {
  int i = server.arg("i").toInt();
  if (i >= 0 && i < netCount) {
    for (int k = i; k < netCount - 1; k++) { netSsid[k] = netSsid[k + 1]; netPass[k] = netPass[k + 1]; }
    netCount--;
    if (cfgWifi >= netCount) cfgWifi = 0;
    saveNets();
  }
  redirectHome();
  redraw();
}
void startAP() {
  if (apMode) return;
  apMode = true;
  uint8_t mac[6]; WiFi.macAddress(mac);
  char n[24]; snprintf(n, sizeof(n), "ATEM-Tally-%02X%02X", mac[4], mac[5]);
  apName = n;
  WiFi.mode(WIFI_AP_STA);  // keep trying the station connection in the background
  WiFi.softAP(apName.c_str(), AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  Serial.printf("AP %s started, open http://%s\n", n, WiFi.softAPIP().toString().c_str());
}

// ---------------------------------------------------------------- backlight
uint8_t brightnessDuty() { return 64 * (cfgBright + 1) - 1; }  // 63, 127, 191, 255

// Steady backlight while WiFi + ATEM are up; a gentle pulse (100 % -> 25 % of the
// chosen level) when either connection is down, so the operator notices.
void backlightLoop() {
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate < 20) return;
  lastUpdate = millis();
  uint8_t duty = brightnessDuty();
  if (atemState != A_CONNECTED) {
    float phase = (millis() % PULSE_MS) / (float)PULSE_MS;
    float k = 0.625f + 0.375f * cosf(phase * 2 * PI);
    duty = max(8, (int)(duty * k));
  }
  ledcWrite(PIN_BL, duty);
}

// ---------------------------------------------------------------- touch
// AXS5106L on I2C: register 0x01 holds the number of touch points in the low nibble
// of the second byte. Only "is it touched" is used, so the screen works as one big button.
#ifdef BOARD_TOUCH
void touchBegin() {
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);  delay(10);
  digitalWrite(PIN_TP_RST, HIGH); delay(10);
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
}

bool touchDown() {
  static uint32_t lastPoll = 0;
  static bool down = false;
  if (!cfgTouch && uiMode != UI_OTA) return false;  // the update offer always takes a tap
  if (millis() - lastPoll < 20) return down;
  lastPoll = millis();
  uint8_t d[2] = {0, 0};
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission() == 0 && Wire.requestFrom(TP_ADDR, 2) == 2) { d[0] = Wire.read(); d[1] = Wire.read(); }
  down = (d[1] & 0x0F) != 0;
  return down;
}
#else
void touchBegin() {}
bool touchDown() { return false; }
#endif

// The JD9853 understands the ST7789 commands Adafruit_ST7789 sends, but needs its own
// panel settings on top (from Waveshare's esp_lcd_jd9853 driver).
void panelInit() {
#ifdef BOARD_TOUCH
  struct Cmd { uint8_t cmd, len; uint8_t data[32]; };
  static const Cmd cmds[] = {
    {0xDF, 2, {0x98, 0x53}}, {0xDF, 2, {0x98, 0x53}}, {0xB2, 1, {0x23}},
    {0xB7, 4, {0x00, 0x47, 0x00, 0x6F}},
    {0xBB, 6, {0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0}},
    {0xC0, 2, {0x44, 0xA4}}, {0xC1, 1, {0x16}},
    {0xC3, 8, {0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77}},
    {0xC4, 12, {0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82}},
    {0xC8, 32, {0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
                0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00}},
    {0xD0, 5, {0x04, 0x06, 0x6B, 0x0F, 0x00}}, {0xD7, 2, {0x00, 0x30}}, {0xE6, 1, {0x14}},
    {0xDE, 1, {0x01}}, {0xB7, 5, {0x03, 0x13, 0xEF, 0x35, 0x35}}, {0xC1, 3, {0x14, 0x15, 0xC0}},
    {0xC2, 2, {0x06, 0x3A}}, {0xC4, 2, {0x72, 0x12}}, {0xBE, 1, {0x00}}, {0xDE, 1, {0x02}},
    {0xE5, 3, {0x00, 0x02, 0x00}}, {0xE5, 3, {0x01, 0x02, 0x00}}, {0xDE, 1, {0x00}},
    {0x35, 1, {0x00}}, {0x3A, 1, {0x05}},
    {0xDE, 1, {0x02}}, {0xE5, 3, {0x00, 0x02, 0x00}}, {0xDE, 1, {0x00}},
  };
  for (const Cmd &c : cmds) tft.sendCommand(c.cmd, c.data, c.len);
  tft.invertDisplay(true);
#endif
}

// ---------------------------------------------------------------- button
// Turns the raw BOOT button into click / hold events. A click fires on release, so
// two quick clicks are simply two clicks.
BtnEvent readButton() {
  static uint32_t downAt = 0;
  static bool wasDown = false, held = false;
  uint32_t now = millis();
  bool down = digitalRead(PIN_BOOT) == LOW || touchDown();  // a touch acts like the button
  BtnEvent ev = B_NONE;

  if (down && !wasDown) { downAt = now; held = false; }
  if (down && !held && now - downAt >= HOLD_MS) { held = true; ev = B_HOLD; }
  if (!down && wasDown && !held && now - downAt > 30) ev = B_CLICK;
  wasDown = down;
  return ev;
}

// ---------------------------------------------------------------- menu
// Main screen: click = next camera 1..8, hold = open menu.
// Menu: click = next item, hold = OK (change value / open). 2 s without a press = back to
// the main screen (sub-screens wait longer, see uiTimeout).
#ifdef BOARD_TOUCH  // the touch board also has a touch on/off item
enum MenuItem { M_ROT, M_BRIGHT, M_NUMSIZE, M_INFOSIZE, M_CLOCK, M_WIFI, M_ATEMIP, M_SETUP, M_LED, M_TOUCH, M_UPDATE, M_INFO, M_RESET, M_EXIT, M_COUNT };
#else
enum MenuItem { M_ROT, M_BRIGHT, M_NUMSIZE, M_INFOSIZE, M_CLOCK, M_WIFI, M_ATEMIP, M_SETUP, M_LED, M_UPDATE, M_INFO, M_RESET, M_EXIT, M_COUNT, M_TOUCH };
#endif
int menuSel = 0;
uint32_t uiActivity = 0;

const char *sizeNames[] = {"Mala", "Srednja", "Velika"};

String menuLabel(int i) {
  switch (i) {
    case M_ROT:    return "Zasuk: " + String(cfgRot * 90);
    case M_BRIGHT: return "Svetlost: " + String(25 * (cfgBright + 1)) + "%";
    case M_NUMSIZE:  return String("Stevilka: ") + sizeNames[cfgNumSize];
    case M_INFOSIZE: return String("Pisava: ") + sizeNames[cfgInfoSize];
    case M_CLOCK:    return String("Ura: ") + (cfgClock == CLOCK_OFF ? "IZKLOP" : cfgClock == CLOCK_HM ? "HH:MM" : "HH:MM:SS");
    case M_WIFI:   return "WiFi: " + (netCount ? netSsid[cfgWifi] : String("(ni omrezij)"));
    case M_ATEMIP: return "ATEM IP >";
    case M_SETUP:  return "Splet, WiFi >";
    case M_LED:    return String("RGB LED: ") + (cfgLed ? "VKLOP" : "IZKLOP");
    case M_TOUCH:  return String("Touch: ") + (cfgTouch ? "VKLOP" : "IZKLOP");
    case M_UPDATE: return "Preveri posodobitve >";
    case M_INFO:   return "Informacije >";
    case M_RESET:  return "Ponastavi vse >";
    default:       return "Izhod";
  }
}

// Title bar in the tally colour, so program/preview stays visible in the menu.
void drawTitle(const String &title) {
  Tally t = currentTally();
  uint16_t bg = atemState != A_CONNECTED ? COLOR_GREY
              : t == T_PROGRAM ? ST77XX_RED : t == T_PREVIEW ? COLOR_GREEN : ST77XX_BLACK;
  tft.fillRect(0, 0, tft.width(), STATUS_H, bg);
  tft.setFont();
  tft.setTextSize(2);
  tft.setTextColor(t == T_PREVIEW ? ST77XX_BLACK : ST77XX_WHITE);
  tft.setCursor(6, 3);
  tft.print(title);
  tft.setTextSize(1);
}

void drawHint(const char *hint) {
  int H = tft.height();
  tft.setFont();
  tft.setTextSize(1);
  tft.setTextColor(0xAD55);
  tft.setCursor(4, H - 11);
  tft.print(hint);
}

void drawMenu() {
  int W = tft.width(), H = tft.height();
  const int rowH = 24, top = STATUS_H + 4;
  int visible = (H - top - BOTTOM_H) / rowH;
  int first = menuSel < visible ? 0 : menuSel - visible + 1;

  tft.fillScreen(COLOR_MENU);
  drawTitle("MENI  CAM " + String(cfgCam));
  tft.setFont(&FreeSans9pt7b);
  for (int r = 0; r < visible && first + r < M_COUNT; r++) {
    int i = first + r, y = top + r * rowH;
    bool sel = i == menuSel;
    if (sel) tft.fillRoundRect(3, y, W - 6, rowH - 2, 4, ST77XX_WHITE);
    tft.setTextColor(sel ? ST77XX_BLACK : ST77XX_WHITE);
    String label = menuLabel(i);
    int16_t x1, y1; uint16_t w, h;
    tft.setFont(&FreeSans9pt7b);
    tft.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    if (w > W - 14) { tft.setFont(); tft.setCursor(10, y + 8); }  // too long (portrait): small font
    else tft.setCursor(10, y + 16);
    tft.print(label);
  }
  drawHint("klik=naprej drzi=OK");
}

String uptimeText() {
  uint32_t s = millis() / 1000;
  char b[24];
  snprintf(b, sizeof(b), "%luh %02lum %02lus", s / 3600, (s / 60) % 60, s % 60);
  return b;
}

void drawInfo() {
  int W = tft.width(), H = tft.height();
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  String lines[] = {
    "Ime: " + asciiName(cfgName) + "  WiFi: " + (wifiUp ? WiFi.SSID() : String("-")),
    "IP: " + (wifiUp ? WiFi.localIP().toString() : String("-")),
    "Signal: " + (wifiUp ? String(WiFi.RSSI()) + " dBm" : String("-")),
    "ATEM: " + cfgAtem.toString(),
    String("Stanje: ") + (atemState == A_CONNECTED ? "povezan" : "ni povezave"),
    "Kamera: " + String(cfgCam) + " " + (cfgCam <= MAX_INPUTS ? inputNames[cfgCam] : ""),
    "Cas delovanja: " + uptimeText(),
    "FW: " FW_VERSION "  MAC " + WiFi.macAddress().substring(9),
  };
  tft.fillRect(0, STATUS_H, W, H - STATUS_H - BOTTOM_H, COLOR_MENU);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ST77XX_WHITE);
  int lineH = H > W ? 30 : 17;
  for (int i = 0; i < 8; i++) {
    int16_t x1, y1; uint16_t w, h;
    tft.setFont(&FreeSans9pt7b);
    tft.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &h);
    if (w > W - 8) {  // long line: small built-in font
      tft.setFont();
      tft.setCursor(6, STATUS_H + 6 + i * lineH);
    } else {
      tft.setCursor(6, STATUS_H + 16 + i * lineH);
    }
    tft.print(lines[i]);
  }
}

void openInfo() {
  uiMode = UI_INFO;
  tft.fillScreen(COLOR_MENU);
  drawTitle("INFO");
  drawInfo();
  drawHint("klik = nazaj");
}

void drawResetConfirm() {
  tft.fillScreen(COLOR_MENU);
  drawTitle("PONASTAVI");
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, STATUS_H + 30);
  tft.print("Izbrisem vse");
  tft.setCursor(8, STATUS_H + 52);
  tft.print("nastavitve?");
  drawHint("drzi=DA  klik=NE");
}

// ---- ATEM IP editor: 12 digits (3 per octet). Click = digit +1, hold = next
// digit (hold on the last one saves). 10 s without a press cancels.
uint8_t ipDigits[12];
int ipPos = 0;

void openIpEdit() {
  for (int o = 0; o < 4; o++) {
    ipDigits[o * 3] = cfgAtem[o] / 100;
    ipDigits[o * 3 + 1] = (cfgAtem[o] / 10) % 10;
    ipDigits[o * 3 + 2] = cfgAtem[o] % 10;
  }
  ipPos = 0;
  uiMode = UI_IPEDIT;
  drawIpEdit();
}

void drawIpEdit() {
  int W = tft.width();
  const int size = 3, cw = 6 * size, chH = 8 * size;
  bool oneLine = W >= 15 * cw + 10;  // landscape: one line, portrait: two
  tft.fillScreen(COLOR_MENU);
  drawTitle("ATEM IP");
  tft.setFont();
  tft.setTextSize(size);
  for (int line = 0; line < (oneLine ? 1 : 2); line++) {
    int firstOctet = oneLine ? 0 : line * 2, octets = oneLine ? 4 : 2;
    int chars = octets * 4 - (oneLine || line == 1 ? 1 : 0);
    int x = (W - chars * cw) / 2, y = STATUS_H + 30 + line * (chH + 16);
    for (int o = firstOctet; o < firstOctet + octets; o++) {
      for (int d = 0; d < 3; d++) {
        int i = o * 3 + d;
        bool sel = i == ipPos;
        if (sel) tft.fillRect(x - 1, y - 2, cw, chH + 4, ST77XX_WHITE);
        tft.setTextColor(sel ? ST77XX_BLACK : ST77XX_WHITE);
        tft.setCursor(x, y);
        tft.print((char)('0' + ipDigits[i]));
        x += cw;
      }
      if (o < 3) { tft.setTextColor(ST77XX_WHITE); tft.setCursor(x, y); tft.print('.'); x += cw; }
    }
  }
  tft.setTextSize(1);
  drawHint(ipPos == 11 ? "klik=+1 drzi=SHRANI" : "klik=+1 drzi=naprej");
}

void ipEditEvent(BtnEvent ev) {
  if (ev == B_CLICK) {
    ipDigits[ipPos] = (ipDigits[ipPos] + 1) % (ipPos % 3 == 0 ? 3 : 10);  // hundreds: 0..2
    drawIpEdit();
  } else if (ev == B_HOLD) {
    if (++ipPos < 12) { drawIpEdit(); return; }
    IPAddress ip;
    for (int o = 0; o < 4; o++) ip[o] = min(255, ipDigits[o * 3] * 100 + ipDigits[o * 3 + 1] * 10 + ipDigits[o * 3 + 2]);
    setAtemIp(ip);
    openMain();
  }
}

// ---- Web setup screen: turns on the setup AP and shows where to open the page.
void openSetup() {
  startAP();
  uiMode = UI_SETUP;
  drawSetup();
}

void drawSetup() {
  int W = tft.width(), H = tft.height();
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  String lines[] = {
    "Odpri v brskalniku:",
    wifiUp ? "http://" + WiFi.localIP().toString() : String("(ni WiFi povezave)"),
    "ali se povezi na WiFi:",
    apName,
    "geslo: " AP_PASS,
    "http://192.168.4.1",
  };
  tft.fillScreen(COLOR_MENU);
  drawTitle("SPLET");
  int lineH = H > W ? 34 : 20;
  for (int i = 0; i < 6; i++) {
    tft.setFont(&FreeSans9pt7b);
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &h);
    tft.setTextColor(i % 2 ? ST77XX_WHITE : 0xAD55);
    if (w > W - 8) { tft.setFont(); tft.setCursor(6, STATUS_H + 8 + i * lineH); }
    else tft.setCursor(6, STATUS_H + 18 + i * lineH);
    tft.print(lines[i]);
  }
  drawHint("klik = nazaj");
}

// A WiFi network chosen in the menu takes effect with a restart once the menu closes.
bool wifiRestartPending = false;

void restartNow(const char *why) {
  Serial.printf("Restart: %s\n", why);
  tft.fillScreen(COLOR_MENU);
  drawTitle("PONOVNI ZAGON");
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, STATUS_H + 30);
  tft.print(why);
  delay(800);
  ESP.restart();
}

void openMain() {
  if (wifiRestartPending) restartNow(("WiFi: " + netSsid[cfgWifi]).c_str());
  uiMode = UI_MAIN;
  redraw();
}

void menuOk() {
  switch (menuSel) {
    case M_ROT:
      cfgRot = (cfgRot + 1) & 3;
      saveByte("rot", cfgRot);
      tft.setRotation(cfgRot);
      break;
    case M_BRIGHT:
      cfgBright = (cfgBright + 1) & 3;
      saveByte("bright", cfgBright);
      break;
    case M_NUMSIZE:
      cfgNumSize = (cfgNumSize + 1) % 3;
      saveByte("numsize", cfgNumSize);
      break;
    case M_INFOSIZE:
      cfgInfoSize = (cfgInfoSize + 1) % 3;
      saveByte("infosize", cfgInfoSize);
      break;
    case M_CLOCK:
      cfgClock = (cfgClock + 1) % 3;
      saveByte("clock", cfgClock);
      break;
    case M_WIFI:
      if (!netCount) break;
      cfgWifi = (cfgWifi + 1) % netCount;
      saveByte("wifi", cfgWifi);
      wifiRestartPending = WiFi.SSID() != netSsid[cfgWifi];  // restart when the menu closes
      break;
    case M_LED:
      cfgLed = !cfgLed;
      saveByte("led", cfgLed);
      break;
    case M_TOUCH:
      cfgTouch = !cfgTouch;
      saveByte("touch", cfgTouch);
      break;
    case M_INFO:  openInfo(); return;
    case M_UPDATE: otaManualCheck(); return;
    case M_ATEMIP: openIpEdit(); return;
    case M_SETUP: openSetup(); return;
    case M_RESET: uiMode = UI_RESET; drawResetConfirm(); return;
    default:      openMain(); return;
  }
  redraw();
}

// How long a screen waits for a press before going back to the main screen.
uint32_t uiTimeout() {
  switch (uiMode) {
    case UI_IPEDIT: case UI_RESET: return EDIT_TIMEOUT_MS;
    case UI_INFO: case UI_SETUP:   return VIEW_TIMEOUT_MS;
    default:                       return MENU_TIMEOUT_MS;
  }
}

void uiLoop() {
  BtnEvent ev = readButton();
  uint32_t now = millis();
  if (uiMode == UI_OTA) { otaUiEvent(ev); return; }  // has its own timeouts
  if (ev != B_NONE) uiActivity = now;
  else if (uiMode != UI_MAIN && now - uiActivity > uiTimeout()) { openMain(); return; }

  switch (uiMode) {
    case UI_MAIN:
      if (ev == B_CLICK) {
        cfgCam = cfgCam >= MAX_BUTTON_CAM ? 1 : cfgCam + 1;
        saveByte("cam", cfgCam);
        redraw();
      } else if (ev == B_HOLD) {
        uiMode = UI_MENU;
        menuSel = 0;
        redraw();
      }
      break;
    case UI_MENU:
      if (ev == B_CLICK) { menuSel = (menuSel + 1) % M_COUNT; redraw(); }
      else if (ev == B_HOLD) menuOk();
      break;
    case UI_INFO:
      if (ev == B_CLICK) openMain();
      else {
        static uint32_t lastInfo = 0;
        if (now - lastInfo > 1000) { lastInfo = now; drawInfo(); }
      }
      break;
    case UI_IPEDIT:
      ipEditEvent(ev);
      break;
    case UI_SETUP:
      if (ev == B_CLICK) openMain();
      break;
    case UI_RESET:
      if (ev == B_HOLD) {
        prefs.begin("tally", false);
        prefs.clear();
        prefs.end();
        tft.fillScreen(ST77XX_BLUE);
        delay(300);
        ESP.restart();
      } else if (ev == B_CLICK) openMain();
      break;
  }
}

// ---------------------------------------------------------------- WiFi
// Non-blocking: async scan, connect to the network chosen in the menu if it is
// in range (otherwise the other known one), give it 15 s, then scan again.
// Keeps the screen pulse running while disconnected.
enum WifiState { W_IDLE, W_SCANNING, W_CONNECTING };
WifiState wifiSt = W_IDLE;
uint8_t wifiBlindTry = 0;

void wifiReconnect() {
  WiFi.disconnect();
  wifiSt = W_IDLE;
  wifiBlindTry = 0;
}

// Remember the signal of each saved network from a finished scan (n results).
void storeScan(int n) {
  for (int k = 0; k < netCount; k++) {
    netRssi[k] = -999;
    for (int i = 0; i < n; i++)
      if (WiFi.SSID(i) == netSsid[k] && WiFi.RSSI(i) > netRssi[k]) netRssi[k] = WiFi.RSSI(i);
  }
}

// While connected, scan in the background when the web page asks for the network
// list (at most every 20 s), so it can show the signal of every saved network.
bool webScanning = false;
void webScanLoop() {
  static uint32_t lastScan = 0;
  if (WiFi.status() != WL_CONNECTED) { webScanning = false; return; }
  if (otaJob == OJ_DOWNLOADING) return;  // keep the radio on the download
  if (webScanning) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING && millis() - lastScan < 10000) return;
    if (n >= 0) storeScan(n);
    WiFi.scanDelete();
    webScanning = false;
  } else if (scanWantedAt && (!lastScan || millis() - lastScan > 20000)) {
    scanWantedAt = 0;
    lastScan = millis();
    webScanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
  }
}

void wifiLoop() {
  static uint32_t since = 0;
  if (WiFi.status() == WL_CONNECTED || netCount == 0) { wifiSt = W_IDLE; return; }
  uint32_t now = millis();

  if (wifiSt == W_IDLE) {
    WiFi.scanNetworks(true);
    wifiSt = W_SCANNING;
    since = now;
  } else if (wifiSt == W_SCANNING) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) { if (now - since > 10000) { WiFi.scanDelete(); wifiSt = W_IDLE; } return; }
    storeScan(n);
    int best = -1, bestScore = -100000;
    for (int i = 0; i < n; i++)
      for (int k = 0; k < netCount; k++)
        if (WiFi.SSID(i) == netSsid[k]) {
          int score = WiFi.RSSI(i) + (k == cfgWifi ? 1000 : 0);  // chosen network wins when in range
          if (score > bestScore) { best = k; bestScore = score; }
        }
    WiFi.scanDelete();
    if (best >= 0) {
      Serial.printf("WiFi: connecting to %s\n", netSsid[best].c_str());
    } else {
      // Not in the scan (hidden SSID or missed beacon): try chosen first, then the others.
      best = (cfgWifi + wifiBlindTry++) % netCount;
      Serial.printf("WiFi: %s not seen in scan, trying anyway\n", netSsid[best].c_str());
    }
    WiFi.begin(netSsid[best].c_str(), netPass[best].c_str());
    wifiSt = W_CONNECTING;
    since = now;
  } else if (now - since > 15000) {
    WiFi.disconnect();
    wifiSt = W_IDLE;
  }
}

// ---------------------------------------------------------------- discovery
// Every light broadcasts a short status line every 2 s on UDP DISC_PORT and keeps
// a table of the others, so the web page of any light can list and edit all of
// them (no master). Format: TLY1<TAB>id<TAB>name<TAB>cam<TAB>tally<TAB>atem<TAB>rssi<TAB>fw<TAB>camName
struct Peer { String id, name, ip, fw, camName; int cam, tally, atem, rssi; uint32_t seen; };
#define MAX_PEERS 16
Peer peers[MAX_PEERS];
int peerCount = 0;

String clean(String s) { s.replace("\t", " "); s.replace("\n", " "); return s; }

void discoveryLoop() {
  static uint32_t lastSend = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastSend > 2000) {
    lastSend = millis();
    String msg = "TLY1\t" + hostId + "\t" + clean(cfgName) + "\t" + String(cfgCam) + "\t" + String((int)currentTally()) +
                 "\t" + String(atemState == A_CONNECTED) + "\t" + String(WiFi.RSSI()) + "\t" FW_VERSION "\t" +
                 clean(cfgCam <= MAX_INPUTS ? inputNames[cfgCam] : "");
    discUdp.beginPacket(WiFi.broadcastIP(), DISC_PORT);
    discUdp.print(msg);
    discUdp.endPacket();
  }
  while (discUdp.parsePacket() > 0) {
    char b[200];
    int n = discUdp.read(b, sizeof(b) - 1);
    if (n <= 0) continue;
    b[n] = 0;
    String f[9];
    int k = 0, start = 0;
    for (int i = 0; i <= n && k < 9; i++)
      if (b[i] == '\t' || b[i] == 0) { b[i] = 0; f[k++] = String(b + start); start = i + 1; }
    if (k < 8 || f[0] != "TLY1" || f[1] == hostId) continue;
    int slot = 0;
    while (slot < peerCount && peers[slot].id != f[1]) slot++;
    if (slot == peerCount) {
      if (peerCount == MAX_PEERS) continue;
      peerCount++;
    }
    Peer &p = peers[slot];
    p.id = f[1]; p.name = f[2]; p.cam = f[3].toInt(); p.tally = f[4].toInt(); p.atem = f[5].toInt();
    p.rssi = f[6].toInt(); p.fw = f[7]; p.camName = f[8]; p.ip = discUdp.remoteIP().toString(); p.seen = millis();
  }
  for (int i = 0; i < peerCount; i++)  // forget lights not heard from for 10 s
    if (millis() - peers[i].seen > 10000) { peers[i] = peers[--peerCount]; i--; }
}

// ---------------------------------------------------------------- HTTP API
String jsonEsc(const String &s) {
  String o;
  for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; if ((uint8_t)ch >= 0x20) o += ch; }
  return o;
}

String peerJson(const String &id, const String &name, const String &ip, int cam, int tally, int atem,
                int rssi, const String &fw, const String &camName, bool self) {
  return "{\"id\":\"" + jsonEsc(id) + "\",\"name\":\"" + jsonEsc(name) + "\",\"ip\":\"" + ip + "\",\"cam\":" + cam +
         ",\"tally\":" + tally + ",\"atem\":" + atem + ",\"rssi\":" + rssi + ",\"fw\":\"" + jsonEsc(fw) +
         "\",\"camName\":\"" + jsonEsc(camName) + "\",\"self\":" + (self ? "true" : "false") + "}";
}

String selfJson() {
  return peerJson(hostId, cfgName, WiFi.localIP().toString(), cfgCam, currentTally(), atemState == A_CONNECTED,
                  WiFi.RSSI(), FW_VERSION, cfgCam <= MAX_INPUTS ? inputNames[cfgCam] : "", true);
}

void sendJson(const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", body);
}

void handleApiStatus() {
  String s = selfJson();
  s.remove(s.length() - 1);  // add the settings that only the light itself knows
  s += ",\"atemIp\":\"" + cfgAtem.toString() + "\",\"bright\":" + cfgBright + ",\"rot\":" + cfgRot +
       ",\"led\":" + (PIN_LED < 0 ? -1 : cfgLed ? 1 : 0) +  // -1 = the board doesn't have it
#ifdef BOARD_TOUCH
       ",\"touch\":" + (cfgTouch ? 1 : 0) +
#else
       ",\"touch\":-1" +
#endif
       ",\"board\":\"" BOARD_NAME "\"";
  // Saved networks with their signal (-999 = not seen in the last scan), "wifi" = chosen one.
  s += ",\"wifi\":" + String(cfgWifi) + ",\"scanning\":" + (webScanning ? "true" : "false") + ",\"nets\":[";
  for (int k = 0; k < netCount; k++)
    s += String(k ? "," : "") + "{\"ssid\":\"" + jsonEsc(netSsid[k]) + "\",\"rssi\":" + netLiveRssi(k) + "}";
  s += "]}";
  scanWantedAt = millis();
  sendJson(s);
}

void handleApiPeers() {
  String s = "[" + selfJson();
  for (int i = 0; i < peerCount; i++) {
    Peer &p = peers[i];
    s += "," + peerJson(p.id, p.name, p.ip, p.cam, p.tally, p.atem, p.rssi, p.fw, p.camName, false);
  }
  sendJson(s + "]");
}

void handleApiOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// POST /api/set with any of: name, cam, atem, bright (0..3), rot (0..3), led (0/1), touch (0/1), wifi (index)
void handleApiSet() {
  if (server.hasArg("name")) {
    String n = server.arg("name");
    n.trim();
    if (n.length() && n != cfgName) setName(n);
  }
  if (server.hasArg("cam")) { cfgCam = constrain(server.arg("cam").toInt(), 1, MAX_INPUTS); saveByte("cam", cfgCam); }
  if (server.hasArg("bright")) { cfgBright = server.arg("bright").toInt() & 3; saveByte("bright", cfgBright); }
  if (server.hasArg("led")) { cfgLed = server.arg("led").toInt() != 0; saveByte("led", cfgLed); }
  if (server.hasArg("touch")) { cfgTouch = server.arg("touch").toInt() != 0; saveByte("touch", cfgTouch); }
  if (server.hasArg("wifi")) {  // index into this light's saved networks
    int k = server.arg("wifi").toInt();
    if (k >= 0 && k < netCount && k != cfgWifi) {
      cfgWifi = k;
      saveByte("wifi", cfgWifi);
      if (WiFi.SSID() != netSsid[k]) restartAt = millis();  // restart after the reply is sent
    }
  }
  if (server.hasArg("rot")) {
    cfgRot = server.arg("rot").toInt() & 3;
    saveByte("rot", cfgRot);
    tft.setRotation(cfgRot);
  }
  if (server.hasArg("atem")) {
    IPAddress ip;
    if (ip.fromString(server.arg("atem")) && ip != cfgAtem) setAtemIp(ip);
  }
  redraw();
  handleApiStatus();
}

// Serial console (115200): "wifi <ssid>" selects a known network (and restarts), "atem <ip>"
// sets the switcher address, "status" prints state,
// "ota" checks for an update (like the menu item), "reboot" restarts.
void serialLoop() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') { if (line.length() < 80) line += ch; continue; }
    line.trim();
    if (line.startsWith("wifi ")) {
      String ssid = line.substring(5);
      int i = 0;
      while (i < netCount && netSsid[i] != ssid) i++;
      if (i < netCount) {
        cfgWifi = i;
        saveByte("wifi", cfgWifi);
        restartNow(("WiFi: " + ssid).c_str());
      } else Serial.println("WiFi: unknown network");
    } else if (line.startsWith("atem ")) {
      IPAddress ip;
      if (ip.fromString(line.substring(5))) { setAtemIp(ip); redraw(); Serial.printf("ATEM IP %s\n", ip.toString().c_str()); }
      else Serial.println("ATEM: invalid IP");
    } else if (line == "reboot") {
      ESP.restart();
    } else if (line == "ota") {
      otaManualCheck();
    } else if (line == "status") {
      Serial.printf("name %s, WiFi %s %s, ATEM %s %s, peers %d\n", cfgName.c_str(), WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), cfgAtem.toString().c_str(),
                    atemState == A_CONNECTED ? "connected" : "not connected", peerCount);
      Serial.printf("cam %d tally %d (TlIn %d inputs), PGM %d PVW %d, dropped %lu, FW " FW_VERSION "\n", cfgCam,
                    (int)currentTally(), tallyCount, programSource, previewSource, atemGaps);
    }
    line = "";
  }
}

// Name without Slovenian diacritics (the LCD fonts are ASCII only): "Tally lučka 1" -> "Tally lucka 1".
String asciiName(const String &s) {
  String o;
  for (int i = 0; i < (int)s.length(); i++) {
    uint8_t c = s[i];
    if (c == 0xC4 || c == 0xC5) {  // UTF-8 lead byte of č ć đ š ž (and capitals)
      uint16_t u = (c << 8) | (i + 1 < (int)s.length() ? (uint8_t)s[++i] : 0);
      switch (u) {
        case 0xC48D: case 0xC487: o += 'c'; break;  // č ć
        case 0xC48C: case 0xC486: o += 'C'; break;  // Č Ć
        case 0xC491: o += 'd'; break;  case 0xC490: o += 'D'; break;  // đ Đ
        case 0xC5A1: o += 's'; break;  case 0xC5A0: o += 'S'; break;  // š Š
        case 0xC5BE: o += 'z'; break;  case 0xC5BD: o += 'Z'; break;  // ž Ž
      }
    } else if (c < 0x80) o += (char)c;
  }
  return o;
}

// "Tally lučka 1" -> "tally-lucka-1": lowercase letters/digits, anything else becomes one dash.
String makeHostName(const String &name) {
  String a = asciiName(name), o;
  a.toLowerCase();
  for (char c : a) {
    if (isalnum((uint8_t)c)) o += c;
    else if (o.length() && !o.endsWith("-")) o += '-';
  }
  while (o.endsWith("-")) o.remove(o.length() - 1);
  return o.substring(0, 32);
}

bool aliasOwned = false;  // this light answers the shared name tally.local

void startMdns() {
  MDNS.end();
  aliasOwned = false;
  if (MDNS.begin(hostName.c_str())) MDNS.addService("http", "tcp", 80);
}

// Rename the light: store it, then rejoin WiFi so the router sees the new DHCP hostname
// (mDNS restarts with it once the connection is back).
void setName(String n) {
  n.trim();
  if (n.length() > 24) n = n.substring(0, 24);
  while (n.length() && ((uint8_t)n[n.length() - 1] & 0xC0) == 0xC0) n.remove(n.length() - 1);  // cut UTF-8 lead byte
  if (!n.length()) return;
  cfgName = n;
  prefs.begin("tally", false); prefs.putString("name", cfgName); prefs.end();
  String h = makeHostName(cfgName);
  if (!h.length()) h = "tally-" + hostId;
  if (h != hostName) {
    hostName = h;
    WiFi.setHostname(hostName.c_str());     // default for new interfaces
    WiFi.STA.setHostname(hostName.c_str()); // the running one, used at the next DHCP request
    renameAt = millis();                    // reconnect later, after the HTTP reply is sent
  }
}

void renameLoop() {
  if (restartAt && millis() - restartAt > 1000) { restartAt = 0; restartNow(("WiFi: " + netSsid[cfgWifi]).c_str()); }
  if (renameAt && millis() - renameAt > 1000) { renameAt = 0; wifiReconnect(); }
}

// Shared name tally.local: the online light with the lowest id claims it, so when it
// goes offline the next one takes over once the peer entry times out (10 s).
void aliasLoop() {
  static uint32_t connectedAt = 0;
  static uint32_t lastCheck = 0;
  if (WiFi.status() != WL_CONNECTED) { connectedAt = 0; aliasOwned = false; return; }
  if (!connectedAt) connectedAt = millis();
  if (millis() - connectedAt < 6000 || millis() - lastCheck < 1000) return;  // let discovery fill the peer list
  lastCheck = millis();
  bool lowest = true;
  for (int i = 0; i < peerCount; i++)
    if (peers[i].id < hostId) lowest = false;
  if (lowest && !aliasOwned) {
    mdns_ip_addr_t addr = {};
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = (uint32_t)WiFi.localIP();
    aliasOwned = mdns_delegate_hostname_add("tally", &addr) == ESP_OK;
    if (aliasOwned) Serial.println("mDNS: answering tally.local");
  } else if (!lowest && aliasOwned) {
    mdns_delegate_hostname_remove("tally");
    aliasOwned = false;
    Serial.println("mDNS: tally.local handed over");
  }
}

// ---------------------------------------------------------------- OTA from GitHub
// After boot and every 12 h (or from the menu) the light reads manifest.json of the
// latest GitHub release over HTTPS (CA bundle built into the core). A newer version is
// offered on screen for 5 s, never while on PROGRAM: click / tap installs it, no answer
// cancels. The image is checked against the manifest MD5 before it becomes the boot
// image, and the new firmware must reach WiFi within 3 min or the bootloader goes back
// to the old one (app rollback).
#define OTA_MANIFEST_URL "https://github.com/" OTA_REPO "/releases/latest/download/manifest.json"
#define OTA_FIRST_MS  20000UL               // first check this long after WiFi is up
#define OTA_EVERY_MS  (12UL * 3600 * 1000)
#define OTA_OFFER_S   5

volatile int otaPercent = 0;
String otaVersion, otaUrl, otaMd5, otaError;
bool otaManual = false;   // check started from the menu: show the result
bool otaOffer = false;    // a newer version waits to be offered
OtaScreen otaScreen = OS_MESSAGE;
String otaMsg[3];
uint32_t otaScreenAt = 0;
int otaShown = -1;        // countdown second / percent currently drawn

// "2.10" > "2.9": compares the dot-separated numbers.
int versionCmp(const String &a, const String &b) {
  int i = 0, j = 0, na = a.length(), nb = b.length();
  while (i < na || j < nb) {
    long x = 0, y = 0;
    while (i < na && isdigit((uint8_t)a[i])) x = x * 10 + (a[i++] - '0');
    while (j < nb && isdigit((uint8_t)b[j])) y = y * 10 + (b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    while (i < na && !isdigit((uint8_t)a[i])) i++;
    while (j < nb && !isdigit((uint8_t)b[j])) j++;
  }
  return 0;
}

// String or number value of "key" in the manifest, searching from position from.
String jsonValue(const String &js, const String &key, int from = 0) {
  int k = js.indexOf("\"" + key + "\"", from);
  if (k < 0) return "";
  int s = js.indexOf(':', k + key.length() + 2);
  if (s < 0) return "";
  s++;
  while (s < (int)js.length() && isspace((uint8_t)js[s])) s++;
  if (s < (int)js.length() && js[s] == '"') {
    int e = js.indexOf('"', s + 1);
    return e < 0 ? "" : js.substring(s + 1, e);
  }
  int e = s;
  while (e < (int)js.length() && (isalnum((uint8_t)js[e]) || js[e] == '.' || js[e] == '-')) e++;
  return js.substring(s, e);
}

// GET over HTTPS, following GitHub's redirects to its download host.
bool otaGet(HTTPClient &http, NetworkClientSecure &client, const String &url) {
  client.useBuiltinCACertBundle();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("atem-tally/" FW_VERSION);
  if (!http.begin(client, url)) { otaError = "napacen URL"; return false; }
  int code = http.GET();
  if (code == 200) return true;
  otaError = code < 0 ? HTTPClient::errorToString(code) : "HTTP " + String(code);
  http.end();
  return false;
}

OtaJob otaCheck() {
  NetworkClientSecure client;
  HTTPClient http;
  if (!otaGet(http, client, OTA_MANIFEST_URL)) return OJ_FAILED;
  String js = http.getString();
  http.end();
  String v = jsonValue(js, "version");
  int b = js.indexOf("\"" BOARD_NAME "\"");
  if (!v.length() || b < 0) { otaError = "napacen manifest"; return OJ_FAILED; }
  Serial.printf("OTA: latest %s, running " FW_VERSION "\n", v.c_str());
  if (versionCmp(v, FW_VERSION) <= 0) return OJ_LATEST;
  otaVersion = v;
  otaUrl = jsonValue(js, "url", b);
  otaMd5 = jsonValue(js, "md5", b);
  if (!otaUrl.startsWith("https://")) { otaError = "ni datoteke za to plosco"; return OJ_FAILED; }
  return OJ_FOUND;
}

OtaJob otaDownload() {
  NetworkClientSecure client;
  HTTPClient http;
  if (!otaGet(http, client, otaUrl)) return OJ_FAILED;
  int total = http.getSize();
  if (total <= 0 || !Update.begin(total)) {
    otaError = total <= 0 ? "neznana velikost" : "premalo prostora";
    http.end();
    return OJ_FAILED;
  }
  if (otaMd5.length() == 32) Update.setMD5(otaMd5.c_str());
  NetworkClient *s = http.getStreamPtr();
  static uint8_t chunk[2048];
  int done = 0;
  uint32_t lastData = millis();
  while (done < total && millis() - lastData < 15000) {
    int n = s->available();
    if (n <= 0) { delay(5); continue; }
    n = s->read(chunk, min(n, (int)sizeof(chunk)));
    if (n <= 0) continue;
    if (Update.write(chunk, n) != (size_t)n) break;
    done += n;
    lastData = millis();
    otaPercent = (int)(100LL * done / total);
  }
  http.end();
  if (done < total) {
    otaError = Update.hasError() ? Update.errorString() : "prenos prekinjen";
    Update.abort();
    return OJ_FAILED;
  }
  if (!Update.end()) { otaError = Update.errorString(); return OJ_FAILED; }  // MD5 / image check
  return OJ_DONE;
}

// Runs the check or download in its own task so the screen and ATEM keep working.
void otaTask(void *download) {
  otaJob = download ? otaDownload() : otaCheck();
  vTaskDelete(NULL);
}

void otaStart(bool download) {
  otaError = "";
  otaPercent = 0;
  otaJob = download ? OJ_DOWNLOADING : OJ_CHECKING;
  if (xTaskCreate(otaTask, "ota", 12288, download ? (void *)1 : nullptr, 1, nullptr) != pdPASS) {
    otaError = "ni pomnilnika";
    otaJob = OJ_FAILED;
  }
}

void openOta(OtaScreen sc) {
  uiMode = UI_OTA;
  otaScreen = sc;
  otaScreenAt = millis();
  drawOta();
}

void otaMessage(const String &a, const String &b, const String &c) {
  otaMsg[0] = a; otaMsg[1] = b; otaMsg[2] = c;
  openOta(OS_MESSAGE);
}

void drawOta() {
  int W = tft.width(), H = tft.height();
  String l[3];
  const char *hint = "";
  switch (otaScreen) {
    case OS_CHECKING: l[0] = "Preverjam GitHub ..."; l[1] = "zdaj FW " FW_VERSION; hint = "klik = nazaj"; break;
    case OS_OFFER:    l[0] = "Nova FW " + otaVersion; l[1] = "zdaj FW " FW_VERSION; l[2] = "Klik = posodobi"; hint = "brez odziva = preklic"; break;
    case OS_PROGRESS: l[0] = "Nalagam FW " + otaVersion; l[1] = "Ne izklapljaj!"; break;
    case OS_MESSAGE:  for (int i = 0; i < 3; i++) l[i] = otaMsg[i]; hint = "klik = nazaj"; break;
  }
  tft.fillScreen(COLOR_MENU);
  drawTitle("POSODOBITEV");
  int lineH = H > W ? 30 : 22;
  for (int i = 0; i < 3; i++) {
    int16_t x1, y1; uint16_t w, h;
    tft.setFont(&FreeSans9pt7b);
    tft.getTextBounds(l[i], 0, 0, &x1, &y1, &w, &h);
    tft.setTextColor(i == 2 && otaScreen == OS_OFFER ? ST77XX_YELLOW : ST77XX_WHITE);
    if (w > W - 8) { tft.setFont(); tft.setCursor(6, STATUS_H + 8 + i * lineH); }
    else tft.setCursor(6, STATUS_H + 18 + i * lineH);
    tft.print(l[i]);
  }
  drawHint(hint);
  otaShown = -1;
  otaDrawDynamic();
}

// Countdown number (offer) or progress bar (download) under the text.
void otaDrawDynamic() {
  int W = tft.width(), H = tft.height(), y = H - BOTTOM_H - 40;
  if (otaScreen == OS_OFFER) {
    int left = max(0, OTA_OFFER_S - (int)((millis() - otaScreenAt) / 1000));
    if (left == otaShown) return;
    otaShown = left;
    tft.fillRect(0, y - 4, W, 42, COLOR_MENU);
    tft.setFont(&FreeSansBold24pt7b);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(String(left), 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((W - w) / 2 - x1, y + 36);
    tft.print(left);
  } else if (otaScreen == OS_PROGRESS) {
    int p = otaPercent;
    if (p == otaShown) return;
    otaShown = p;
    int bw = W - 16;
    tft.drawRect(8, y, bw, 18, ST77XX_WHITE);
    tft.fillRect(10, y + 2, (bw - 4) * p / 100, 14, COLOR_GREEN);
    tft.fillRect(0, y + 22, W, 16, COLOR_MENU);
    tft.setFont();
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE);
    String t = String(p) + " %";
    tft.setCursor((W - t.length() * 12) / 2, y + 22);
    tft.print(t);
    tft.setTextSize(1);
  }
}

void otaUiEvent(BtnEvent ev) {
  uint32_t age = millis() - otaScreenAt;
  switch (otaScreen) {
    case OS_CHECKING:  // the check goes on in the background; otaLoop shows its result
      if (ev != B_NONE) { otaManual = false; openMain(); }
      break;
    case OS_OFFER:
      if (ev != B_NONE) {
        otaOffer = false;
        Serial.printf("OTA: installing %s\n", otaVersion.c_str());
        otaStart(true);
        openOta(OS_PROGRESS);
      } else if (age >= OTA_OFFER_S * 1000UL) {
        otaOffer = false;
        Serial.println("OTA: offer not answered, cancelled");
        openMain();
      } else otaDrawDynamic();
      break;
    case OS_PROGRESS:
      otaDrawDynamic();
      break;
    case OS_MESSAGE:
      if (ev != B_NONE || age > 5000) openMain();
      break;
  }
}

void otaManualCheck() {
  otaManual = true;
  if (otaOffer) {
    if (currentTally() == T_PROGRAM) otaMessage("Na voljo FW " + otaVersion, "ponudim jo, ko lucka", "ni na PROGRAM");
    else openOta(OS_OFFER);
  } else if (WiFi.status() != WL_CONNECTED) {
    otaMessage("Ni WiFi povezave", "", "");
  } else {
    if (otaJob == OJ_IDLE) otaStart(false);
    openOta(OS_CHECKING);
  }
}

void otaLoop() {
  static uint32_t nextCheck = 0;
  if (WiFi.status() == WL_CONNECTED && !nextCheck) nextCheck = millis() + OTA_FIRST_MS;
  if (nextCheck && (int32_t)(millis() - nextCheck) >= 0 && otaJob == OJ_IDLE && WiFi.status() == WL_CONNECTED) {
    nextCheck = millis() + OTA_EVERY_MS;
    otaManual = uiMode == UI_OTA && otaScreen == OS_CHECKING;
    otaStart(false);
  }
  bool shown = uiMode == UI_OTA;
  switch (otaJob) {
    case OJ_FOUND:
      otaJob = OJ_IDLE;
      otaOffer = true;
      Serial.printf("OTA: FW %s available\n", otaVersion.c_str());
      if (otaManual && currentTally() == T_PROGRAM) otaMessage("Na voljo FW " + otaVersion, "ponudim jo, ko lucka", "ni na PROGRAM");
      break;
    case OJ_LATEST:
      otaJob = OJ_IDLE;
      if (otaManual && shown) otaMessage("FW " FW_VERSION " je", "najnovejsa.", "");
      break;
    case OJ_FAILED:
      otaJob = OJ_IDLE;
      Serial.printf("OTA: failed: %s\n", otaError.c_str());
      if ((otaManual || otaScreen == OS_PROGRESS) && shown) otaMessage("Napaka: " + otaError, "ostaja FW " FW_VERSION, "");
      break;
    case OJ_DONE:
      Serial.printf("OTA: FW %s written, restarting\n", otaVersion.c_str());
      otaMessage("Posodobljeno na FW " + otaVersion, "ponovni zagon ...", "");
      delay(1500);
      ESP.restart();
      break;
    default: break;
  }
  bool program = currentTally() == T_PROGRAM;
  if (otaOffer && !program && (uiMode == UI_MAIN || (shown && otaScreen == OS_CHECKING))) openOta(OS_OFFER);
  if (shown && otaScreen == OS_OFFER && program) openMain();  // went on air: offer again afterwards
}

// The bootloader keeps a freshly installed image on probation: keep it once it has
// WiFi, go back to the previous one if it can't get there in 3 minutes.
extern "C" bool verifyRollbackLater() { return true; }

void otaHealthLoop() {
  static int8_t pending = -1;
  if (pending == 0) return;
  if (pending < 0) {
    esp_ota_img_states_t st;
    pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
              st == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending) Serial.println("OTA: new firmware on probation until WiFi connects");
    return;
  }
  if (WiFi.status() == WL_CONNECTED && millis() > 15000) {
    esp_ota_mark_app_valid_cancel_rollback();
    pending = 0;
    Serial.println("OTA: FW " FW_VERSION " confirmed");
  } else if (millis() > 180000) {
    Serial.println("OTA: no WiFi with the new firmware, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  loadConfig();
  for (int k = 0; k < MAX_NETS; k++) netRssi[k] = -999;
  ledcAttach(PIN_BL, 5000, 8);
  ledcWrite(PIN_BL, brightnessDuty());
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  tft.init(172, 320);
  panelInit();
  touchBegin();
  setLed(T_NONE);

  tft.setRotation(cfgRot);
  redraw();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  uint8_t mac[6]; WiFi.macAddress(mac);
  char id[8]; snprintf(id, sizeof(id), "%02x%02x", mac[4], mac[5]);
  hostId = id;
  if (!cfgName.length()) cfgName = "Tally-" + hostId;
  hostName = makeHostName(cfgName);
  if (!hostName.length()) hostName = "tally-" + hostId;
  WiFi.setHostname(hostName.c_str());     // default for new interfaces
  WiFi.STA.setHostname(hostName.c_str()); // the running one, used at the next DHCP request
  discUdp.begin(DISC_PORT);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("WiFi: disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  udp.begin(ATEM_PORT + 1);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/addwifi", HTTP_POST, handleAddWifi);
  server.on("/api/status", handleApiStatus);
  server.on("/api/peers", handleApiPeers);
  server.on("/api/set", HTTP_POST, handleApiSet);
  server.on("/api/set", HTTP_OPTIONS, handleApiOptions);
  server.on("/delwifi", HTTP_POST, handleDelWifi);
  server.onNotFound(handleRoot);
  server.begin();
}

void loop() {
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  static uint32_t bootAt = millis();
  wifiLoop();
  wl_status_t ws = WiFi.status();
  if (ws != lastWifi) {
    lastWifi = ws;
    if (ws == WL_CONNECTED) startMdns();
    if (ws == WL_CONNECTED) Serial.printf("WiFi connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    redraw();
  }
  // Can't reach any WiFi for 30 s: also open the setup AP.
  if (!apMode && ws != WL_CONNECTED && millis() - bootAt > 30000) { startAP(); redraw(); }

  if (apMode) dns.processNextRequest();
  server.handleClient();
  uiLoop();
  atemLoop();
  statusBarLoop();
  blinkLoop();
  backlightLoop();
  discoveryLoop();
  webScanLoop();
  aliasLoop();
  renameLoop();
  otaLoop();
  otaHealthLoop();
  serialLoop();
  delay(2);
}
