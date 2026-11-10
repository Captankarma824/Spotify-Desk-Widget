#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SpotifyArduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <TJpg_Decoder.h>
#include <WiFiManager.h>

// --- YOUR CREDENTIALS ---
#include "secrets.h"

// --- HARDWARE ---
#define XPT2046_IRQ 36
#define XPT2046_MOSI 13
#define XPT2046_MISO 12
#define XPT2046_CLK 14
#define XPT2046_CS 33

// --- TOUCH CALIBRATION ---
#define TOUCH_MIN_X 280
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 340
#define TOUCH_MAX_Y 3800

#define DEBUG_TOUCH 1

SPIClass touchSpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();
WiFiClientSecure client;
SpotifyArduino spotify(client, SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);

// --- SYSTEM STATE ---
enum ScreenState { PLAYER_MODE,
                   PLAYLIST_MODE };
ScreenState currentScreen = PLAYER_MODE;

unsigned long lastRequest = 0;

// --- RATE LIMIT / BACKOFF HANDLING ---
const unsigned long BASE_DELAY = 5000;  // normal polling interval (ms)
const unsigned long MAX_DELAY = 60000;  // cap on backoff (ms)
unsigned long delayRequest = BASE_DELAY;

bool isPlaying = false;
bool shuffleState = false;
String currentImgUrl = "";
String lastTrackName = "";
String marqueeText = "";
int marqueeOffset = 0;
bool marqueeActive = false;
unsigned long lastMarqueeUpdate = 0;
long lastProgressMs = 0;
long lastDurationMs = 0;

bool showVolumeModal = false;
unsigned long lastVolInteraction = 0;
int currentVolume = 50;

struct SimplePlaylist {
  String name;
  String uri;
};
SimplePlaylist myPlaylists[6];
int playlistCount = 0;
bool playlistsLoaded = false;

// --- SPOTIFY-STYLE COLOR PALETTE ---
uint16_t COLOR_WHITE;
uint16_t COLOR_GREY;
uint16_t COLOR_DARKGREY;
uint16_t COLOR_GREEN;
uint16_t COLOR_PINK;
uint16_t COLOR_BG_DEFAULT;
uint16_t currentBgColor;

// --- LAYOUT (480x320 landscape) ---
#define SCR_W 480
#define SCR_H 320

#define TIME_TEXT_Y 242
#define BAR_Y 257
#define BAR_H 4
#define BAR_X 12
#define BAR_W (SCR_W - BAR_X * 2)

#define ART_SIZE 220
#define ART_X 10
#define ART_Y 24

#define TEXT_X0 (ART_X + ART_SIZE + 16)
#define TEXT_X1 (SCR_W - 10)
#define MENU_SIZE 24
#define MENU_X (TEXT_X1 - MENU_SIZE)
#define MENU_Y ART_Y
#define TITLE_Y (ART_Y + MENU_SIZE + 14)
#define ARTIST_Y (TITLE_Y + 36)
#define ALBUM_Y (ARTIST_Y + 26)

#define CTRL_CY 284
#define CTRL_HIT_Y0 268
#define CTRL_HIT_Y1 SCR_H

const int BTN_CX[5] = { 72, 156, 240, 324, 408 };
const int BTN_HALF_W[5] = { 32, 32, 36, 32, 36 };

// --- ALBUM ART DOMINANT-COLOR TRACKING ---
bool trackBgColor = false;
uint32_t sumR = 0, sumG = 0, sumB = 0, sumPixels = 0;

// Maximum image buffer size in bytes for the 300px album cover (keeps heap free)
#define MAX_IMAGE_SIZE 40000

// Forward declarations
void drawBaseInterface();
void drawPlaylistScreen();
void drawControls();
void drawProgressRow(long progressMs, long durationMs);
void handleTouchPlayer(int x, int y);
void handleTouchPlaylist(int x, int y);
void updateMarquee();
void drawCover(const char *url);
void drawPlayPauseIcon(bool playing);
void updateDisplay(CurrentlyPlaying current);
bool rateLimited();

// ==========================================
// COLOR HELPERS
// ==========================================
void rgbToHsv(uint8_t r, uint8_t g, uint8_t b, float &h, float &s, float &v) {
  float rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
  float mx = max(rf, max(gf, bf)), mn = min(rf, min(gf, bf));
  v = mx;
  float d = mx - mn;
  s = (mx <= 0.0001) ? 0 : d / mx;
  if (d < 0.0001) {
    h = 205;
  } else if (mx == rf) {
    h = 60.0 * fmod(((gf - bf) / d), 6.0);
  } else if (mx == gf) {
    h = 60.0 * (((bf - rf) / d) + 2.0);
  } else {
    h = 60.0 * (((rf - gf) / d) + 4.0);
  }
  if (h < 0) h += 360;
}

void hsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
  float c = v * s;
  float x = c * (1 - fabs(fmod(h / 60.0, 2.0) - 1));
  float m = v - c;
  float rf, gf, bf;
  if (h < 60) {
    rf = c;
    gf = x;
    bf = 0;
  } else if (h < 120) {
    rf = x;
    gf = c;
    bf = 0;
  } else if (h < 180) {
    rf = 0;
    gf = c;
    bf = x;
  } else if (h < 240) {
    rf = 0;
    gf = x;
    bf = c;
  } else if (h < 300) {
    rf = x;
    gf = 0;
    bf = c;
  } else {
    rf = c;
    gf = 0;
    bf = x;
  }
  r = (uint8_t)constrain((rf + m) * 255.0, 0, 255);
  g = (uint8_t)constrain((gf + m) * 255.0, 0, 255);
  b = (uint8_t)constrain((bf + m) * 255.0, 0, 255);
}

uint16_t makeBgColorFromAverage(uint8_t r8, uint8_t g8, uint8_t b8) {
  float h, s, v;
  rgbToHsv(r8, g8, b8, h, s, v);
  if (s < 0.45) s = 0.45;
  v = 0.24;
  uint8_t r, g, b;
  hsvToRgb(h, s, v, r, g, b);
  return tft.color565(r, g, b);
}

// ==========================================
// HELPERS
// ==========================================
String fitText(const String &text, int maxWidth) {
  if (tft.textWidth(text) <= maxWidth) return text;
  String t = text;
  while (t.length() > 0 && tft.textWidth(t + "...") > maxWidth) {
    t.remove(t.length() - 1);
  }
  return t + "...";
}

String formatTime(long ms) {
  if (ms < 0) ms = 0;
  long totalSec = ms / 1000;
  int m = totalSec / 60;
  int s = totalSec % 60;
  return String(m) + ":" + (s < 10 ? "0" : "") + String(s);
}

// Returns true when we're currently backed off due to a prior 429.
// Useful for gating button-triggered API calls so they don't add to the pile-up.
bool rateLimited() {
  return delayRequest > BASE_DELAY;
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return 0;
  if (trackBgColor) {
    uint32_t n = (uint32_t)w * h;
    for (uint32_t i = 0; i < n; i++) {
      uint16_t px = bitmap[i];
      sumR += (px >> 11) & 0x1F;
      sumG += (px >> 5) & 0x3F;
      sumB += px & 0x1F;
      sumPixels++;
    }
  }
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

void configModeCallback(WiFiManager *myWiFiManager) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("WiFi Setup");
  tft.println(myWiFiManager->getConfigPortalSSID());
}

// ==========================================
// SETUP / LOOP
// ==========================================
void setup() {
  Serial.begin(115200);

  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);

  COLOR_WHITE = TFT_WHITE;
  COLOR_GREY = tft.color565(179, 179, 179);
  COLOR_DARKGREY = tft.color565(64, 64, 64);
  COLOR_GREEN = tft.color565(30, 215, 96);
  COLOR_PINK = tft.color565(0xF7, 0x58, 0xCB);
  COLOR_BG_DEFAULT = makeBgColorFromAverage(220, 70, 160);
  currentBgColor = COLOR_BG_DEFAULT;

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(3);

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  if (!wm.autoConnect("Spotify-CYD")) ESP.restart();

  tft.fillScreen(currentBgColor);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, currentBgColor);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting to Spotify...", SCR_W / 2, SCR_H / 2);
  tft.setTextDatum(TL_DATUM);

  client.setInsecure();

  if (spotify.refreshAccessToken()) {
    delay(500);
    drawBaseInterface();
  }
}

void loop() {
  if (showVolumeModal) {
    if (millis() - lastVolInteraction > 2000) {
      showVolumeModal = false;
      drawControls();
      if (lastDurationMs > 0) drawProgressRow(lastProgressMs, lastDurationMs);
    }
  }

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int screenX = constrain(map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, SCR_W, 0), 0, SCR_W);
    int screenY = constrain(map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, SCR_H, 0), 0, SCR_H);

#if DEBUG_TOUCH
    Serial.printf("raw x=%d y=%d -> screen x=%d y=%d\n", p.x, p.y, screenX, screenY);
#endif

    if (currentScreen == PLAYER_MODE) {
      handleTouchPlayer(screenX, screenY);
    } else {
      handleTouchPlaylist(screenX, screenY);
    }
    delay(200);
  }

  if (currentScreen == PLAYER_MODE && !showVolumeModal) {
    updateMarquee();

    if (millis() > lastRequest + delayRequest) {
      int statusCode = spotify.getCurrentlyPlaying(updateDisplay, "US");
      lastRequest = millis();

      if (statusCode == 429) {
        int retrySeconds = spotify.getRetryAfterSeconds();
        if (retrySeconds > 0) {
          delayRequest = (unsigned long)retrySeconds * 1000UL;
          Serial.printf("Rate limited (429). Retry-After: %d s -> waiting %lu ms\n", retrySeconds, delayRequest);
        } else {
          // Header missing/unparseable - fall back to exponential backoff
          delayRequest = min(delayRequest * 2, MAX_DELAY);
          Serial.printf("Rate limited (429), no Retry-After header. Backing off to %lu ms\n", delayRequest);
        }
      } else if (statusCode == 200) {
        delayRequest = BASE_DELAY;
      }
      // Other status codes (network hiccups, etc.) leave delayRequest unchanged.
    }
  }
}

// ==========================================
// TOUCH HIT-TESTING
// ==========================================
int getTappedButton(int x, int y) {
  if (y < CTRL_HIT_Y0 || y > CTRL_HIT_Y1) return -1;
  for (int i = 0; i < 5; i++) {
    if (x >= BTN_CX[i] - BTN_HALF_W[i] && x <= BTN_CX[i] + BTN_HALF_W[i]) return i;
  }
  return -1;
}

// ==========================================
// VOLUME MODAL
// ==========================================
void drawVolumeModal() {
  tft.fillRect(0, 180, SCR_W, 60, currentBgColor);
  tft.drawRoundRect(4, 184, SCR_W - 8, 52, 8, COLOR_DARKGREY);

  tft.fillRoundRect(20, 194, 55, 36, 8, COLOR_DARKGREY);
  tft.setTextColor(COLOR_WHITE, COLOR_DARKGREY);
  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("-", 47, 212);

  tft.fillRoundRect(245, 194, 55, 36, 8, COLOR_DARKGREY);
  tft.drawString("+", 272, 212);

  int barX = 90, barY = 206, barW = 140, barH = 12;
  tft.fillRoundRect(barX, barY, barW, barH, 6, COLOR_DARKGREY);
  int fillW = map(currentVolume, 0, 100, 0, barW);
  if (fillW > 0) tft.fillRoundRect(barX, barY, fillW, barH, 6, COLOR_PINK);

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY, currentBgColor);
  tft.drawString(String(currentVolume) + "%", 160, 192);
  tft.setTextDatum(TL_DATUM);
}

void changeVolume(int change) {
  currentVolume = constrain(currentVolume + change, 0, 100);
  spotify.setVolume(currentVolume);
  drawVolumeModal();
  lastVolInteraction = millis();
}

// ==========================================
// PLAYLIST SCREEN
// ==========================================
void downloadPlaylists_Manual() {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  HTTPClient https;

  String userId = "";

  // --- Get user ID ---
  if (https.begin(secureClient, "https://api.spotify.com/v1/me")) {
    https.addHeader("Authorization", String(spotify.getBearerToken()));
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      String payload = https.getString();
      DynamicJsonDocument docUser(1024);
      if (!deserializeJson(docUser, payload)) {
        userId = docUser["id"].as<String>();
      }
    }
    https.end();
  }

  // --- Get playlists ---
  if (https.begin(secureClient, "https://api.spotify.com/v1/me/playlists?limit=5&fields=items(name,uri)")) {
    https.addHeader("Authorization", String(spotify.getBearerToken()));
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      String payload = https.getString();
      DynamicJsonDocument doc(2048);
      DeserializationError err = deserializeJson(doc, payload);

      if (!err) {
        JsonArray items = doc["items"];
        playlistCount = 0;

        if (userId != "") {
          myPlaylists[0].name = "Liked Songs";
          myPlaylists[0].uri = "spotify:user:" + userId + ":collection";
          playlistCount++;
        }

        for (JsonObject item : items) {
          if (playlistCount < 6) {
            myPlaylists[playlistCount].name = item["name"].as<String>();
            myPlaylists[playlistCount].uri = item["uri"].as<String>();
            playlistCount++;
          }
        }
        playlistsLoaded = true;
        https.end();
        drawPlaylistScreen();
        return;
      } else {
        Serial.print("Playlist JSON parse failed: ");
        Serial.println(err.c_str());
      }
    } else if (code == 429) {
      Serial.println("Playlist HTTP error: 429 rate limited");
    } else {
      Serial.printf("Playlist HTTP error: %d\n", code);
    }
    https.end();
  }

  // --- Error fallback ---
  tft.fillScreen(currentBgColor);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, currentBgColor);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connection error", SCR_W / 2, SCR_H / 2);
  tft.setTextDatum(TL_DATUM);
  delay(5000);
  currentScreen = PLAYER_MODE;
  drawBaseInterface();
}

void loadPlaylists() {
  tft.fillScreen(currentBgColor);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, currentBgColor);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Loading playlists...", SCR_W / 2, SCR_H / 2);
  tft.setTextDatum(TL_DATUM);
  downloadPlaylists_Manual();
}

void drawPlaylistScreen() {
  currentScreen = PLAYLIST_MODE;
  tft.fillScreen(currentBgColor);

  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_PINK, currentBgColor);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Your Playlists", 20, 18);

  tft.fillCircle(SCR_W - 30, 30, 20, COLOR_DARKGREY);
  tft.setTextFont(2);
  tft.setTextColor(COLOR_WHITE, COLOR_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("X", SCR_W - 30, 31);
  tft.setTextDatum(TL_DATUM);

  tft.setTextFont(2);
  for (int i = 0; i < playlistCount; i++) {
    int yPos = 80 + (i * 42);
    tft.setTextColor(COLOR_WHITE, currentBgColor);
    tft.setTextDatum(TL_DATUM);
    String name = fitText(myPlaylists[i].name, SCR_W - 60);
    tft.drawString(name, 22, yPos);
    tft.drawFastHLine(20, yPos + 32, SCR_W - 40, COLOR_DARKGREY);
  }
}

void handleTouchPlaylist(int x, int y) {
  if (x > SCR_W - 60 && y < 60) {
    currentScreen = PLAYER_MODE;
    drawBaseInterface();
    return;
  }

  if (y > 70) {
    int index = (y - 70) / 42;
    if (index >= 0 && index < playlistCount) {
      tft.fillScreen(currentBgColor);
      tft.setTextFont(2);
      tft.setTextSize(1);
      tft.setTextColor(COLOR_WHITE, currentBgColor);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Starting " + fitText(myPlaylists[index].name, SCR_W - 60), SCR_W / 2, SCR_H / 2);
      tft.setTextDatum(TL_DATUM);

      String body = "{\"context_uri\":\"" + myPlaylists[index].uri + "\"}";
      spotify.playAdvanced((char *)body.c_str());

      delay(1000);
      currentScreen = PLAYER_MODE;
      drawBaseInterface();
    }
  }
}

// ==========================================
// PLAYER SCREEN
// ==========================================
void drawMenuIcon() {
  tft.fillRect(MENU_X - 2, MENU_Y - 2, MENU_SIZE + 4, MENU_SIZE + 4, currentBgColor);
  for (int i = 0; i < 3; i++) {
    tft.fillRoundRect(MENU_X, MENU_Y + i * 7, MENU_SIZE, 3, 2, COLOR_WHITE);
  }
}

void drawArtPlaceholder() {
  tft.fillRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 6, COLOR_DARKGREY);
  int cx = ART_X + ART_SIZE / 2, cy = ART_Y + ART_SIZE / 2;
  tft.fillCircle(cx - 14, cy + 20, 9, COLOR_GREY);
  tft.fillRect(cx - 6, cy - 30, 4, 50, COLOR_GREY);
  tft.fillRect(cx - 6, cy - 30, 22, 8, COLOR_GREY);
}

void fillBackgroundExceptArt() {
  tft.fillRect(0, 0, SCR_W, ART_Y, currentBgColor);
  tft.fillRect(0, ART_Y, ART_X, SCR_H - ART_Y, currentBgColor);
  tft.fillRect(ART_X + ART_SIZE, ART_Y, SCR_W - (ART_X + ART_SIZE), SCR_H - ART_Y, currentBgColor);
  tft.fillRect(ART_X, ART_Y + ART_SIZE, ART_SIZE, SCR_H - (ART_Y + ART_SIZE), currentBgColor);
}

void drawBaseInterface() {
  currentBgColor = COLOR_BG_DEFAULT;
  tft.fillScreen(currentBgColor);
  tft.setTextWrap(false);

  drawArtPlaceholder();
  drawMenuIcon();

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY, currentBgColor);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Waiting for", TEXT_X0, TITLE_Y);
  tft.drawString("playback...", TEXT_X0, TITLE_Y + 24);

  drawControls();

  lastTrackName = "";
  currentImgUrl = "";
  marqueeText = "";
  marqueeOffset = 0;
  marqueeActive = false;
}

void setMarqueeText(const String &text) {
  int colWidth = TEXT_X1 - TEXT_X0;
  marqueeText = text;
  marqueeOffset = 0;

  tft.setTextFont(4);
  tft.setTextSize(1);
  marqueeActive = (tft.textWidth(marqueeText) > colWidth);
  lastMarqueeUpdate = millis();
}

void drawMarqueeTitle() {
  int colWidth = TEXT_X1 - TEXT_X0;

  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE, currentBgColor);
  tft.setTextDatum(TL_DATUM);

  tft.fillRect(TEXT_X0, TITLE_Y, colWidth, 28, currentBgColor);
  tft.setViewport(TEXT_X0, TITLE_Y, colWidth, 28);

  if (!marqueeActive) {
    tft.drawString(fitText(marqueeText, colWidth), 0, 0);
    tft.resetViewport();
    return;
  }

  int textW = tft.textWidth(marqueeText);
  const int gap = 28;
  int x = -marqueeOffset;

  tft.drawString(marqueeText, x, 0);
  tft.drawString(marqueeText, x + textW + gap, 0);

  tft.resetViewport();
}

void updateMarquee() {
  if (!marqueeActive) return;

  if (millis() - lastMarqueeUpdate < 70) return;
  lastMarqueeUpdate = millis();

  tft.setTextFont(4);
  tft.setTextSize(1);

  int textW = tft.textWidth(marqueeText);
  const int gap = 28;
  marqueeOffset += 2;

  if (marqueeOffset >= textW + gap) {
    marqueeOffset = 0;
  }

  drawMarqueeTitle();
}

void drawTrackText(CurrentlyPlaying &current) {
  int colWidth = TEXT_X1 - TEXT_X0;

  setMarqueeText(String(current.trackName));
  drawMarqueeTitle();

  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY, currentBgColor);
  tft.drawString(fitText(String(current.artists[0].artistName), colWidth), TEXT_X0, ARTIST_Y);

  tft.setTextFont(1);
  tft.setTextColor(COLOR_GREY, currentBgColor);
  tft.drawString(fitText(String(current.albumName), colWidth), TEXT_X0, ALBUM_Y);
}

void drawProgressRow(long progressMs, long durationMs) {
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY, currentBgColor);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(formatTime(progressMs), BAR_X, TIME_TEXT_Y);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(formatTime(durationMs), BAR_X + BAR_W, TIME_TEXT_Y);
  tft.setTextDatum(TL_DATUM);

  int fillW = durationMs > 0 ? map(progressMs, 0, durationMs, 0, BAR_W) : 0;
  fillW = constrain(fillW, 0, BAR_W);

  tft.fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 1, COLOR_DARKGREY);
  if (fillW > 0) tft.fillRoundRect(BAR_X, BAR_Y, fillW, BAR_H, 1, COLOR_PINK);
}

// ==========================================
// FIXED ALBUM ART LOGIC & DISPLAY UPDATE
// ==========================================
void updateDisplay(CurrentlyPlaying current) {
  if (showVolumeModal) return;
  tft.setTextWrap(false);

  bool trackChanged = (String(current.trackName) != lastTrackName);
  bool newIsPlaying = current.isPlaying;

  if (trackChanged) {
    const char *newUrl = "";
    if (current.numImages > 0) {
      // Pick the ~300px image (index 1 / closest to 300px).
      // 300px JPEGs are ~10-15KB, avoiding ESP32 TLS heap allocation failures.
      int imgIndex = 0;
      int minDiff = 9999;
      for (int i = 0; i < current.numImages; i++) {
        int diff = abs(current.albumImages[i].width - 300);
        if (diff < minDiff) {
          minDiff = diff;
          imgIndex = i;
        }
      }
      newUrl = current.albumImages[imgIndex].url;
    }

    bool artChanged = (String(newUrl) != currentImgUrl);

    if (artChanged && strlen(newUrl) > 0) {
      currentImgUrl = String(newUrl);
      sumR = sumG = sumB = sumPixels = 0;
      trackBgColor = true;
      drawCover(newUrl);
      trackBgColor = false;

      if (sumPixels > 0) {
        uint8_t r8 = (sumR / sumPixels) << 3;
        uint8_t g8 = (sumG / sumPixels) << 2;
        uint8_t b8 = (sumB / sumPixels) << 3;
        currentBgColor = makeBgColorFromAverage(r8, g8, b8);
      } else {
        currentBgColor = COLOR_BG_DEFAULT;
      }
    }

    fillBackgroundExceptArt();
    tft.drawRoundRect(ART_X - 3, ART_Y - 3, ART_SIZE + 6, ART_SIZE + 6, 8, COLOR_PINK);
    drawMenuIcon();
    drawTrackText(current);
    isPlaying = newIsPlaying;
    drawControls();

    lastTrackName = String(current.trackName);
  } else if (isPlaying != newIsPlaying) {
    isPlaying = newIsPlaying;
    drawPlayPauseIcon(isPlaying);
  }

  lastProgressMs = current.progressMs;
  lastDurationMs = current.durationMs;
  drawProgressRow(lastProgressMs, lastDurationMs);
}

void drawCover(const char *url) {
  if (url == nullptr || strlen(url) == 0) {
    Serial.println("Album art: empty Spotify URL");
    return;
  }

  Serial.println("Album art: downloading...");
  Serial.println(url);

  WiFiClientSecure artClient;
  artClient.setInsecure();
  artClient.setTimeout(8000);

  HTTPClient https;
  https.setConnectTimeout(8000);
  https.setTimeout(10000);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!https.begin(artClient, String(url))) {
    Serial.println("Album art: HTTPS begin failed");
    return;
  }

  int httpCode = https.GET();
  Serial.printf("Album art: HTTP %d\n", httpCode);

  if (httpCode != HTTP_CODE_OK) {
    https.end();
    return;
  }

  int contentLength = https.getSize();
  size_t capacity = (contentLength > 0 && contentLength < MAX_IMAGE_SIZE) ? (size_t)contentLength : MAX_IMAGE_SIZE;

  uint8_t *jpgBuffer = (uint8_t *)malloc(capacity);
  if (jpgBuffer == nullptr) {
    Serial.printf("Album art: malloc failed, free heap=%u\n", (unsigned)ESP.getFreeHeap());
    https.end();
    return;
  }

  WiFiClient *stream = https.getStreamPtr();
  size_t received = 0;
  unsigned long start = millis();

  while (millis() - start < 10000 && received < capacity) {
    int availableBytes = stream->available();
    if (availableBytes > 0) {
      size_t want = min((size_t)availableBytes, capacity - received);
      int got = stream->readBytes((char *)jpgBuffer + received, want);
      if (got > 0) {
        received += (size_t)got;
        start = millis();
      }
    } else {
      if (!https.connected() && contentLength < 0) break;
      delay(2);
    }
  }

  https.end();

  if (received < 2 || jpgBuffer[0] != 0xFF || jpgBuffer[1] != 0xD8) {
    Serial.println("Album art: invalid or empty JPEG buffer");
    free(jpgBuffer);
    return;
  }

  uint16_t jpgW = 0, jpgH = 0;
  JRESULT sizeResult = TJpgDec.getJpgSize(&jpgW, &jpgH, jpgBuffer, received);

  if (sizeResult == JDR_OK) {
    uint8_t scale = 1;
    if (jpgW >= 500) {
      scale = 4;  // 640px -> 160px
    } else if (jpgW >= 250) {
      scale = 2;  // 300px -> 150px
    }
    TJpgDec.setJpgScale(scale);

    int scaledW = jpgW / scale;
    int scaledH = jpgH / scale;
    int drawX = ART_X + (ART_SIZE - scaledW) / 2;
    int drawY = ART_Y + (ART_SIZE - scaledH) / 2;

    // Clear placeholder interior before drawing art
    tft.fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, TFT_BLACK);
    TJpgDec.drawJpg(drawX, drawY, jpgBuffer, received);
  } else {
    Serial.println("Album art: JPEG decoder rejected image");
  }

  free(jpgBuffer);
}

// ==========================================
// ICONS & CONTROLS
// ==========================================
void drawShuffleIcon(int cx, int cy, bool active) {
  uint16_t c = active ? COLOR_PINK : COLOR_WHITE;
  tft.drawWideLine(cx - 12, cy + 6, cx + 8, cy - 6, 2, c, currentBgColor);
  tft.fillTriangle(cx + 4, cy - 10, cx + 4, cy - 2, cx + 12, cy - 6, c);
  tft.drawWideLine(cx - 12, cy - 6, cx + 8, cy + 6, 2, c, currentBgColor);
  tft.fillTriangle(cx + 4, cy + 2, cx + 4, cy + 10, cx + 12, cy + 6, c);
}

void drawSkipIcon(int cx, int cy, bool isNext) {
  uint16_t c = COLOR_WHITE;
  int triW = 14, triH = 18, barW = 3;
  if (isNext) {
    tft.fillTriangle(cx - triW / 2, cy - triH / 2, cx - triW / 2, cy + triH / 2, cx + triW / 2 - 2, cy, c);
    tft.fillRect(cx + triW / 2, cy - triH / 2, barW, triH, c);
  } else {
    tft.fillTriangle(cx + triW / 2, cy - triH / 2, cx + triW / 2, cy + triH / 2, cx - triW / 2 + 2, cy, c);
    tft.fillRect(cx - triW / 2 - barW, cy - triH / 2, barW, triH, c);
  }
}

void drawPlayPauseIcon(bool playing) {
  int cx = BTN_CX[2], cy = CTRL_CY;
  tft.fillRect(cx - 15, cy - 15, 30, 30, currentBgColor);
  if (playing) {
    tft.fillRoundRect(cx - 9, cy - 10, 6, 20, 2, COLOR_WHITE);
    tft.fillRoundRect(cx + 3, cy - 10, 6, 20, 2, COLOR_WHITE);
  } else {
    tft.fillTriangle(cx - 8, cy - 11, cx - 8, cy + 11, cx + 11, cy, COLOR_WHITE);
  }
}

#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295
#endif

void drawArcOutline(int cx, int cy, float r, float startDeg, float endDeg, uint16_t color) {
  const int steps = 6;
  float startRad = startDeg * DEG_TO_RAD;
  float endRad = endDeg * DEG_TO_RAD;
  int prevX = cx + (int)round(r * cos(startRad));
  int prevY = cy + (int)round(r * sin(startRad));
  for (int i = 1; i <= steps; i++) {
    float t = startRad + (endRad - startRad) * i / steps;
    int x = cx + (int)round(r * cos(t));
    int y = cy + (int)round(r * sin(t));
    tft.drawWideLine(prevX, prevY, x, y, 2, color, currentBgColor);
    prevX = x;
    prevY = y;
  }
}

void drawVolumeIcon(int cx, int cy) {
  uint16_t c = COLOR_WHITE;
  int speakerCx = cx - 6;
  tft.fillRect(speakerCx - 10, cy - 5, 6, 10, c);
  tft.fillTriangle(speakerCx - 4, cy - 5, speakerCx - 4, cy + 5, speakerCx + 6, cy - 12, c);
  tft.fillTriangle(speakerCx - 4, cy - 5, speakerCx - 4, cy + 5, speakerCx + 6, cy + 12, c);
  drawArcOutline(speakerCx + 6, cy, 7, -50, 50, c);
  drawArcOutline(speakerCx + 6, cy, 13, -50, 50, c);
}

void drawControls() {
  tft.fillRect(0, CTRL_HIT_Y0, SCR_W, SCR_H - CTRL_HIT_Y0, currentBgColor);

  drawShuffleIcon(BTN_CX[0], CTRL_CY, shuffleState);
  drawSkipIcon(BTN_CX[1], CTRL_CY, false);
  drawPlayPauseIcon(isPlaying);
  drawSkipIcon(BTN_CX[3], CTRL_CY, true);
  drawVolumeIcon(BTN_CX[4], CTRL_CY);
}

// ==========================================
// TOUCH HANDLING
// ==========================================
void handleTouchPlayer(int x, int y) {
  if (y < MENU_Y + MENU_SIZE + 10 && x > MENU_X - 10) {
    if (!playlistsLoaded) loadPlaylists();
    else drawPlaylistScreen();
    return;
  }

  if (showVolumeModal) {
    if (y > 180) {
      if (x < 110) {
        changeVolume(-10);
      } else if (x > 210) {
        changeVolume(+10);
      }
      lastVolInteraction = millis();
    }
    return;
  }

  // While backed off from a 429, skip further button-triggered API calls
  // so we don't add to the pile-up that got us rate-limited in the first place.
  if (rateLimited()) {
    Serial.println("Rate limited - ignoring control tap for now");
    return;
  }

  int b = getTappedButton(x, y);
  switch (b) {
    case 0:
      spotify.toggleShuffle(!shuffleState);
      shuffleState = !shuffleState;
      drawShuffleIcon(BTN_CX[0], CTRL_CY, shuffleState);
      break;
    case 1:
      spotify.previousTrack();
      break;
    case 2:
      if (isPlaying) {
        spotify.pause();
        isPlaying = false;
      } else {
        spotify.play();
        isPlaying = true;
      }
      drawPlayPauseIcon(isPlaying);
      break;
    case 3:
      spotify.nextTrack();
      break;
    case 4:
      showVolumeModal = true;
      lastVolInteraction = millis();
      drawVolumeModal();
      break;
    default:
      break;
  }
}