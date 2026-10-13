// ATEM tally light for Waveshare ESP32-C6-LCD-1.47 (ST7789 172x320, WS2812 on GPIO8)
// Talks to an ATEM switcher (Mini Extreme etc.) over the ATEM UDP protocol, port 9910.
//
// Screen: red = program, green = preview, black = idle. Large camera number,
// camera name (ATEM input label) in small text below it.
// BOOT button on the main screen: click = next camera 1..8, hold 0.5 s = menu.
// Menu (rotation, brightness, WiFi, RGB LED, info, reset): click = next item,
// hold = OK, double click = back. The backlight pulses while WiFi/ATEM is down.
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
#include <Preferences.h>
#include <map>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

// ---- Board pins (Waveshare wiki) ----
#define PIN_MOSI 6
#define PIN_SCLK 7
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  21
#define PIN_BL   22
#define PIN_LED  8
#define PIN_BOOT 9

#define ATEM_PORT      9910
#define LED_BRIGHTNESS 80
#define MAX_BUTTON_CAM 8
#define MAX_INPUTS     64
#define FW_VERSION     "1.9"
#define DISC_PORT      9911  // UDP broadcast port the lights use to find each other
#define HOLD_MS        500   // hold = menu / OK
#define MENU_TIMEOUT_MS 30000 // back to the main screen after 30 s without input
#define DOUBLE_MS      350   // second click within this time = double click
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
uint8_t cfgNumSize = 1;  // camera number size 0..2 (small/medium/large)
uint8_t cfgInfoSize = 0; // clock, name and bottom line size 0..2
enum ClockMode { CLOCK_OFF, CLOCK_HM, CLOCK_HMS };
uint8_t cfgClock = CLOCK_HMS;

// Known WiFi networks, stored in NVS (n0s/n0p, ...). Seeded with the two built-in ones.
#define MAX_NETS 6
String netSsid[MAX_NETS], netPass[MAX_NETS];
int netCount = 0;

enum UiMode { UI_MAIN, UI_MENU, UI_INFO, UI_RESET, UI_IPEDIT, UI_SETUP };
UiMode uiMode = UI_MAIN;
enum BtnEvent { B_NONE, B_CLICK, B_DOUBLE, B_HOLD };
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
Tally currentTally() {
  if (atemState != A_CONNECTED || cfgCam > tallyCount) return T_NONE;
  uint8_t f = tallyFlags[cfgCam - 1];
  return (f & 0x01) ? T_PROGRAM : (f & 0x02) ? T_PREVIEW : T_NONE;
}

void setLed(Tally t) {
  if (!cfgLed) t = T_NONE;
  switch (t) {
    // This board's LED has red and green swapped compared to rgbLedWrite's GRB order.
    case T_PROGRAM: rgbLedWrite(PIN_LED, 0, LED_BRIGHTNESS, 0); break;
    case T_PREVIEW: rgbLedWrite(PIN_LED, LED_BRIGHTNESS, 0, 0); break;
    default:        rgbLedWrite(PIN_LED, 0, 0, 0); break;
  }
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
uint16_t programSource = 0;  // PrgI, mix effect 1
uint16_t recStatus = 0;      // RTMS: 0 idle, 1 recording, 128 stopping
uint16_t streamStatus = 1;   // StRS: 1 idle, 2 connecting, 4 streaming, 32 stopping
std::map<uint16_t, String> shortNames;  // InPr short names of every source (MP1, BLK, ...)
uint32_t recStartMs = 0, streamStartMs = 0;  // local start time, corrected by RTMR / SRST
int recDotX = -1, liveDotX = -1, dotY = 0, dotR = 3;
bool dotOn = false;

bool isRecording() { return recStatus & 0x01; }
bool isStreaming() { return streamStatus & 0x04; }

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  return rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
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
      if (src != programSource) { programSource = src; bottomChanged = true; }
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
  if (changed) redraw();
  else if (bottomChanged) drawBottomLine();
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
      if (len >= 13 && buf[12] == 0x02) atemSendAck(0);
      else Serial.printf("ATEM refused connection (code %d)\n", len >= 13 ? buf[12] : -1);
      continue;
    }
    if (atemState != A_CONNECTED) {
      atemState = A_CONNECTED;
      atemLocalId = 0;
      Serial.println("ATEM connected");
      redraw();
    }
    if (flags & 0x01) atemSendAck(remoteId);
    if (plen > 12 && (flags & 0x01)) atemParseCommands(buf + 12, plen - 12);
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
  h += "<br>WiFi: " + (WiFi.status() == WL_CONNECTED ? htmlEscape(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ")" : String("ni povezave")) + "</p>";

  h += "<h3>ATEM</h3><form method=post action=/save>ATEM IP<input name=atem value=\"" + cfgAtem.toString() + "\">";
  h += "Kamera (vhod)<input name=cam type=number min=1 max=" + String(MAX_INPUTS) + " value=" + String(cfgCam) + ">";
  h += F("<button>Shrani</button></form>");

  h += "<h3>WiFi omrezja</h3><table>";
  for (int i = 0; i < netCount; i++) {
    h += "<tr><td>" + htmlEscape(netSsid[i]) + (i == cfgWifi ? " <b>(izbrano)</b>" : "") + "</td><td align=right>";
    h += "<form method=post action=/delwifi style=margin:0><input type=hidden name=i value=" + String(i) + ">";
    h += "<button>Izbrisi</button></form></td></tr>";
  }
  h += "</table>";
  if (netCount < MAX_NETS) {
    h += F("<form method=post action=/addwifi>Ime omrezja (SSID)<input name=ssid required>"
           "Geslo<input name=pass type=password><button>Dodaj in izberi</button></form>");
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
  cfgWifi = i;
  saveNets();
  redirectHome();
  if (WiFi.SSID() != ssid) wifiReconnect();  // switch to the newly chosen network
  redraw();
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

// ---------------------------------------------------------------- button
// Turns the raw BOOT button into click / double click / hold events. Double
// clicks are only detected when asked for (in the menu), so a click on the main
// screen reacts immediately.
BtnEvent readButton(bool wantDouble) {
  static uint32_t downAt = 0, clickAt = 0;
  static bool wasDown = false, held = false, pending = false;
  uint32_t now = millis();
  bool down = digitalRead(PIN_BOOT) == LOW;
  BtnEvent ev = B_NONE;

  if (down && !wasDown) { downAt = now; held = false; }
  if (down && !held && now - downAt >= HOLD_MS) { held = true; pending = false; ev = B_HOLD; }
  if (!down && wasDown && !held && now - downAt > 30) {
    if (!wantDouble) ev = B_CLICK;
    else if (pending) { pending = false; ev = B_DOUBLE; }
    else { pending = true; clickAt = now; }
  }
  if (pending && !down && now - clickAt > DOUBLE_MS) { pending = false; ev = B_CLICK; }
  wasDown = down;
  return ev;
}

// ---------------------------------------------------------------- menu
// Main screen: click = next camera 1..8, hold = open menu.
// Menu: click = next item, hold = OK (change value / open), double click = back.
enum MenuItem { M_ROT, M_BRIGHT, M_NUMSIZE, M_INFOSIZE, M_CLOCK, M_WIFI, M_ATEMIP, M_SETUP, M_LED, M_INFO, M_RESET, M_EXIT, M_COUNT };
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
    tft.setCursor(10, y + 16);
    tft.print(menuLabel(i));
  }
  drawHint("klik=naprej drzi=OK 2x=nazaj");
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
  drawHint("klik ali 2x = nazaj");
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
// digit (hold on the last one saves), double click = previous digit / cancel.
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
  drawHint(ipPos == 11 ? "klik=+1 drzi=SHRANI 2x=nazaj" : "klik=+1 drzi=naprej 2x=nazaj");
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
    uiMode = UI_MENU;
    redraw();
  } else if (ev == B_DOUBLE) {
    if (--ipPos < 0) { uiMode = UI_MENU; redraw(); } else drawIpEdit();
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
  drawHint("klik ali 2x = nazaj");
}

void openMain() {
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
      wifiReconnect();
      break;
    case M_LED:
      cfgLed = !cfgLed;
      saveByte("led", cfgLed);
      break;
    case M_INFO:  openInfo(); return;
    case M_ATEMIP: openIpEdit(); return;
    case M_SETUP: openSetup(); return;
    case M_RESET: uiMode = UI_RESET; drawResetConfirm(); return;
    default:      openMain(); return;
  }
  redraw();
}

void uiLoop() {
  BtnEvent ev = readButton(uiMode != UI_MAIN);
  uint32_t now = millis();
  if (ev != B_NONE) uiActivity = now;
  else if (uiMode != UI_MAIN && now - uiActivity > MENU_TIMEOUT_MS) { openMain(); return; }

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
      else if (ev == B_DOUBLE) openMain();
      break;
    case UI_INFO:
      if (ev == B_CLICK || ev == B_DOUBLE) { uiMode = UI_MENU; redraw(); }
      else {
        static uint32_t lastInfo = 0;
        if (now - lastInfo > 1000) { lastInfo = now; drawInfo(); }
      }
      break;
    case UI_IPEDIT:
      ipEditEvent(ev);
      break;
    case UI_SETUP:
      if (ev == B_CLICK || ev == B_DOUBLE) { uiMode = UI_MENU; redraw(); }
      break;
    case UI_RESET:
      if (ev == B_HOLD) {
        prefs.begin("tally", false);
        prefs.clear();
        prefs.end();
        tft.fillScreen(ST77XX_BLUE);
        delay(300);
        ESP.restart();
      } else if (ev == B_CLICK || ev == B_DOUBLE) { uiMode = UI_MENU; redraw(); }
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
       ",\"led\":" + (cfgLed ? 1 : 0) + "}";
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

// POST /api/set with any of: name, cam, atem, bright (0..3), rot (0..3), led (0/1)
void handleApiSet() {
  if (server.hasArg("name")) {
    String n = server.arg("name");
    n.trim();
    if (n.length() && n != cfgName) setName(n);
  }
  if (server.hasArg("cam")) { cfgCam = constrain(server.arg("cam").toInt(), 1, MAX_INPUTS); saveByte("cam", cfgCam); }
  if (server.hasArg("bright")) { cfgBright = server.arg("bright").toInt() & 3; saveByte("bright", cfgBright); }
  if (server.hasArg("led")) { cfgLed = server.arg("led").toInt() != 0; saveByte("led", cfgLed); }
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

// Serial console (115200): "wifi <ssid>" selects a known network, "status" prints state.
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
        wifiReconnect();
        Serial.printf("WiFi: switching to %s\n", ssid.c_str());
      } else Serial.println("WiFi: unknown network");
    } else if (line == "status") {
      Serial.printf("name %s, WiFi %s %s, ATEM %s %s, peers %d\n", cfgName.c_str(), WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), cfgAtem.toString().c_str(),
                    atemState == A_CONNECTED ? "connected" : "not connected", peerCount);
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
uint32_t renameAt = 0;    // millis() of a rename waiting for its WiFi rejoin

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

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  loadConfig();
  ledcAttach(PIN_BL, 5000, 8);
  ledcWrite(PIN_BL, brightnessDuty());
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  tft.init(172, 320);
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
  aliasLoop();
  renameLoop();
  serialLoop();
  delay(2);
}
