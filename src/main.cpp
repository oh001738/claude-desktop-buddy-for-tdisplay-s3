#include "ble_bridge.h"
#include "buddy.h"
#include "data.h"
#include "hal.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <WiFi.h>

TFT_eSprite spr = TFT_eSprite(hal_get_lcd());
TFT_eSprite petSpr = TFT_eSprite(hal_get_lcd());
TFT_eSprite txtSpr = TFT_eSprite(hal_get_lcd());
// Dedicated sprite for CJK transcript rendering (smooth font enabled).
// Created and font-loaded only if /ZhTW12.vlw exists in LittleFS.
TFT_eSprite zhSpr = TFT_eSprite(hal_get_lcd());
static bool zhFontReady = false;
bool g_rtcValid = false;

// Transcript-active hold: when a new transcript line arrives, we keep the
// transcript visible (covering the portrait clock area) for this many ms
// after the latest update. After expiry, drawClock reverts to time/date.
static const uint32_t TRANSCRIPT_HOLD_MS = 30000;
static uint32_t transcriptActiveUntilMs = 0;
static uint32_t lastSeenLineGen = 0;

// zhSpr is shared across three render paths (drawHUD-portrait, drawClock-
// portrait-transcript, drawClock-landscape-transcript). Tracking the last
// writer lets the heavy portrait-clock-transcript path skip re-rendering
// when nothing changed and the previous content is still valid.
enum ZhSprOwner : uint8_t {
  ZH_NONE = 0,
  ZH_HUD_PORTRAIT,
  ZH_CLOCK_PORTRAIT,
  ZH_CLOCK_LANDSCAPE,
};
static ZhSprOwner zhSprOwner = ZH_NONE;

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#define W SCREEN_W
#define H SCREEN_H
const int CX = W / 2;
const int CY_BASE = H / 2;
const int LED_PIN = 10;

// Colors used across multiple UI surfaces
const uint16_t HOT = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104; // overlay panel background
const uint16_t RED = 0xF800;
const uint16_t BLUE = 0x001F;
const uint16_t WHITE = 0xFFFF;
const uint16_t BLACK = 0x0000;
const uint16_t GREEN = 0x07E0;
const uint16_t YELLOW = 0xFFE0;

enum PersonaState {
  P_SLEEP,
  P_IDLE,
  P_BUSY,
  P_ATTENTION,
  P_CELEBRATE,
  P_DIZZY,
  P_HEART
};
const char *stateNames[] = {"sleep",     "idle",  "busy", "attention",
                            "celebrate", "dizzy", "heart"};

TamaState tama;
PersonaState baseState = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t oneShotUntil = 0;
uint32_t lastShakeCheck = 0;
float accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool menuOpen = false;
uint8_t menuSel = 0;
static bool menuDirty = true;
static inline void menuInvalidate() { menuDirty = true; }
uint8_t brightLevel = 4; // 0..4 → ScreenBreath 20..100
bool btnALong = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool dimmed = false;
bool screenOff = false;
bool swallowBtnA = false;
bool swallowBtnB = false;
bool buddyMode = false;
bool gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF; // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  if (buddyMode) {
    if (buddySpeciesIdx() + 1 >= buddySpeciesCount()) {
      if (gifAvailable) {
        buddyMode = false;
        speciesIdxSave(SPECIES_GIF);
        characterInvalidate();
        characterSetState(activeState);
        Serial.println("Switching to GIF mode");
      } else {
        buddySetSpeciesIdx(0);
        speciesIdxSave(0);
        Serial.println("No GIF, back to ASCII 0");
      }
    } else {
      buddyNextSpecies();
      Serial.printf("Next ASCII pet: %u\n", buddySpeciesIdx());
    }
  } else {
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
    Serial.println("Back to ASCII mode from GIF");
  }
  characterInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static void applyBrightness() {
  hal_set_brightness(brightLevel);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    hal_screen_on();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) {
    applyBrightness();
    dimmed = false;
  }
}
bool responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound)
    hal_beep(freq, dur);
}

static void sendCmd(const char *json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t *)json, n);
  bleWrite((const uint8_t *)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;
void drawPet();
void drawHUD();
static uint8_t wrapInto(const char *in, char out[][48], uint8_t maxRows,
                        uint8_t width);
static void clockUpdateOrient();
static void renderLandscapePet(int cx, int cy, bool force = false);

static bool wasInMenu = false;
static uint8_t wasDisplayMode = DISP_NORMAL;

static void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate(); // redraws character on next tick (text mode path)
}

const char *menuItems[]   = {"settings", "turn off", "about",
                             "demo",     "update", "close"};
const char *menuItemsZh[] = {"設定", "關機", "關於",
                             "示範", "更新",   "關閉"};
const uint8_t MENU_N = 6;

bool settingsOpen = false;
uint8_t settingsSel = 0;
const char *settingsItems[]   = {
    "brightness", "sound",     "bluetooth", "wifi",  "led",
    "transcript", "clock rot", "ascii pet", "reset", "language", "check update", "back"};
const char *settingsItemsZh[] = {
    "亮度", "音效", "藍牙",  "Wi-Fi", "LED",
    "字幕", "方向", "角色",  "重置",  "語言", "檢查更新", "返回"};
const uint8_t SETTINGS_N = 12;
static bool isSettingVisible(uint8_t i) {
  if (i == 1 || i == 4 || i == 10)
    return false; // Hide sound, led, and check update (update is now in main menu)
  if (i == 9 && !zhFontReady)
    return false; // language option requires CJK font
  return true;
}
static uint8_t visibleSettingsCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < SETTINGS_N; i++)
    if (isSettingVisible(i)) n++;
  return n;
}

bool resetOpen = false;
uint8_t resetSel = 0;
const char *resetItems[]   = {"delete char", "factory reset", "back"};
const char *resetItemsZh[] = {"刪除角色", "恢復出廠", "返回"};
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t resetConfirmIdx = 0xFF;
static uint8_t clockOrient = 0;

static void applySetting(uint8_t idx) {
  Settings &s = settings();
  switch (idx) {
  case 0:
    brightLevel = (brightLevel % 4) + 1;
    applyBrightness();
    return;
  case 1:
    s.sound = !s.sound;
    break;
  case 2:
    // BT toggle is a stored preference only — BLE stays live. Turning
    // BLE off cleanly would require tearing down the BLE stack which
    // the Arduino BLE library doesn't do reliably. If we need a
    // hard-off someday, stop advertising via BLEDevice::getAdvertising().
    s.bt = !s.bt;
    break;
  case 3:
    s.wifi = !s.wifi;
    settingsSave();
    if (s.wifi) {
      extern void startWifiPortal();
      // Only launch config portal if no credentials stored in NVS;
      // otherwise just enable the radio directly.
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      delay(300);
      if (WiFi.SSID().length() == 0) {
        // No saved network — launch portal to configure one
        startWifiPortal();
      }
      // If credentials exist, radio is now on and will connect in background
    } else {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    break;
  case 4:
    s.led = !s.led;
    break;
  case 5:
    s.hud = !s.hud;
    break;
  case 6:
    s.clockRot = (s.clockRot + 1) % 3;
    clockUpdateOrient();
    break;
  case 7:
    nextPet();
    return;
  case 8:
    resetOpen = true;
    resetSel = 0;
    resetConfirmIdx = 0xFF;
    settingsOpen = false;
    return;
  case 9:
    if (zhFontReady) {
      s.lang ^= 1;
      settingsSave();
    }
    return;
  case 10:
    extern void runOtaUpdate();
    runOtaUpdate();
    return;
  case 11:
    settingsOpen = false;
    menuOpen = true;
    menuSel = 0; // Highlight the 'settings' option in main menu
    menuInvalidate();
    return;
  }
  settingsSave();
}
static void deleteNonBufoCharacters() {
  fs::File d = LittleFS.open("/characters");
  if (!d || !d.isDirectory()) return;

  fs::File e;
  while ((e = d.openNextFile())) {
    const char* name = e.name();
    if (strcmp(name, "bufo") == 0) {
      e.close();
      continue;
    }
    
    char path[128];
    snprintf(path, sizeof(path), "/characters/%s", name);
    if (e.isDirectory()) {
      fs::File f;
      while ((f = e.openNextFile())) {
        char fp[192];
        snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
        f.close();
        LittleFS.remove(fp);
      }
      e.close();
      LittleFS.rmdir(path);
    } else {
      e.close();
      LittleFS.remove(path);
    }
  }
  d.close();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed =
      (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) {
    resetOpen = false;
    settingsOpen = true;
    return;
  }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe non-bufo characters, keep bufo
    deleteNonBufoCharacters();
  } else {
    // factory reset: NVS namespace wipe + delete non-bufo characters + BLE bonds.
    // Clears stats, owner, petname, species, settings, and any stored LTKs
    // so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    deleteNonBufoCharacters();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette &p, int mx, int mw, int hy,
                          const char *downLbl = "1",
                          const char *rightLbl = "2") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy);
  spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy);
  spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

// Returns true when CJK font is loaded AND user has selected Chinese.
static inline bool zhLang() { return zhFontReady && settings().lang == 1; }

// Returns zh string when Chinese mode is active, en string otherwise.
static inline const char *loc(const char *en, const char *zh) {
  return zhLang() ? zh : en;
}

// Render a menu label into dst at (x,y) using dst's current font.
// In zh mode callers pass zhSpr (smooth font); in en mode pass spr/txtSpr.
static void menuText(TFT_eSprite &dst, int x, int y, const char *label,
                     uint16_t col, uint16_t bg) {
  dst.setTextColor(col, bg);
  dst.setCursor(x, y);
  dst.print(label);
}

// Fast block copy: zhSpr (170×170) → spr at (mx, my).
// memcpy per row is far faster than pushToSprite's pixel-by-pixel drawPixel.
static void zhSprToSpr(int mx, int my) {
  uint16_t *src = (uint16_t *)zhSpr.getPointer();
  uint16_t *dst = (uint16_t *)spr.getPointer();
  int copyW = min(170, W - mx);
  int copyH = min(170, H - my);
  for (int row = 0; row < copyH; row++)
    memcpy(dst + (my + row) * W + mx, src + row * 170, copyW * 2);
}

// Helper: render settings value column into dst at (vx, vy).
static void drawSettingsValue(TFT_eSprite &dst, int i, int vx, int vy,
                              uint16_t dimCol, uint16_t bg) {
  const Settings &s = settings();
  static const char *const RN[] = {"0", "90", "270"};
  dst.setTextColor(dimCol, bg);
  dst.setCursor(vx, vy);
  if (i == 0)
    dst.printf("%u/4", brightLevel);
  else if (i == 2 || i == 3 || i == 5) {
    bool v = (i == 2) ? s.bt : (i == 3 ? s.wifi : s.hud);
    dst.setTextColor(v ? GREEN : dimCol, bg);
    dst.print(v ? "on" : "off");
  } else if (i == 10) {
    dst.print(""); // check update option has no toggle value
  } else if (i == 6)
    dst.print(RN[s.clockRot]);
  else if (i == 7) {
    uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
    dst.printf("%u/%u", buddyMode ? buddySpeciesIdx() + 1 : total, total);
  } else if (i == 9)
    dst.print(s.lang ? "ZH" : "EN");
}

static void drawSettings() {
  const Palette &p = characterPalette();
  if (clockOrient != 0) {
    hal_get_lcd()->setRotation(clockOrient);
    renderLandscapePet(85, 85,
                       wasInMenu != (menuOpen || settingsOpen || resetOpen));

    // Landscape: zh mode uses zhSpr, en mode uses txtSpr — both pushed at (170,0)
    TFT_eSprite &panel = zhLang() ? zhSpr : txtSpr;
    if (!zhLang() || menuDirty) {
      panel.fillSprite(p.bg);
      panel.drawRect(0, 0, 150, 170, p.textDim);
      panel.setTextColor(p.text, p.bg);
      panel.setTextSize(1);
      menuText(panel, 10, 8, loc("SETTINGS", "設定"), p.text, p.bg);
      panel.drawFastHLine(5, 20, 140, p.textDim);

      int vLine = 0;
      for (int i = 0; i < SETTINGS_N; i++) {
        if (!isSettingVisible(i)) continue;
        bool sel = (i == settingsSel);
        int y = 30 + vLine * 14;
        uint16_t itemCol = sel ? p.text : p.textDim;
        if (sel) panel.fillRect(5, y - 2, 140, 13, 0x2104);
        panel.setTextColor(itemCol, p.bg);
        panel.setCursor(10, y);
        panel.print(sel ? "> " : "  ");
        menuText(panel, 10 + 12, y,
                 loc(settingsItems[i], settingsItemsZh[i]), itemCol, p.bg);
        drawSettingsValue(panel, i, 110, y, p.textDim, p.bg);
        vLine++;
      }
      zhSprOwner = ZH_NONE;
      if (zhLang()) menuDirty = false;
    }
    panel.pushSprite(170, 0);
    return;
  }

  // --- Portrait Settings ---
  int mw = 118, mh = 16 + visibleSettingsCount() * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = H - mh - 10;

  if (zhLang()) {
    if (menuDirty) {
      // zh: render full panel into zhSpr (relative coords), push to spr once.
      zhSpr.fillSprite(p.bg);           // corners blend with spr background
      zhSpr.fillRoundRect(0, 0, mw, mh, 4, PANEL);
      zhSpr.drawRoundRect(0, 0, mw, mh, 4, p.textDim);
      int vLine = 0;
      for (int i = 0; i < SETTINGS_N; i++) {
        if (!isSettingVisible(i)) continue;
        bool sel = (i == settingsSel);
        int ly = 8 + vLine * 14;
        uint16_t itemCol = sel ? p.text : p.textDim;
        zhSpr.setTextColor(itemCol, PANEL);
        zhSpr.setCursor(6, ly);
        zhSpr.print(sel ? "> " : "  ");
        menuText(zhSpr, 6 + 12, ly,
                 settingsItemsZh[i], itemCol, PANEL);
        drawSettingsValue(zhSpr, i, mw - 36, ly, p.textDim, PANEL);
        vLine++;
      }
      zhSprOwner = ZH_NONE;
      menuDirty = false;
    }
    zhSprToSpr(mx, my);
    spr.setTextSize(1);
    drawMenuHints(p, mx, mw, my + mh - 12, "1:Next", "2:Change");
    return;
  }

  // en: render directly into spr
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  int vLine = 0;
  for (int i = 0; i < SETTINGS_N; i++) {
    if (!isSettingVisible(i)) continue;
    bool sel = (i == settingsSel);
    int ly = my + 8 + vLine * 14;
    uint16_t itemCol = sel ? p.text : p.textDim;
    spr.setTextColor(itemCol, PANEL);
    spr.setCursor(mx + 6, ly);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    drawSettingsValue(spr, i, mx + mw - 36, ly, p.textDim, PANEL);
    vLine++;
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "1:Next", "2:Change");
}

static void drawReset() {
  const Palette &p = characterPalette();
  if (clockOrient != 0) {
    hal_get_lcd()->setRotation(clockOrient);
    renderLandscapePet(85, 85,
                       wasInMenu != (menuOpen || settingsOpen || resetOpen));

    TFT_eSprite &panel = zhLang() ? zhSpr : txtSpr;
    bool anyArmed = (resetConfirmIdx != 0xFF) && (int32_t)(millis() - resetConfirmUntil) < 0;
    if (!zhLang() || menuDirty || anyArmed) {
      panel.fillSprite(p.bg);
      int mx = 5, my = 60;
      for (int i = 0; i < RESET_N; i++) {
        bool sel = (i == resetSel);
        bool armed = (i == resetConfirmIdx) && anyArmed;
        uint16_t itemCol = armed ? HOT : (sel ? p.text : p.textDim);
        panel.setTextColor(itemCol, p.bg);
        panel.setCursor(mx, my + i * 14);
        panel.print(sel ? "> " : "  ");
        const char *label = armed ? loc("really?", "確定？")
                                  : loc(resetItems[i], resetItemsZh[i]);
        menuText(panel, mx + 12, my + i * 14, label, itemCol, p.bg);
      }
      zhSprOwner = ZH_NONE;
      if (zhLang() && !anyArmed) menuDirty = false;
    }
    panel.pushSprite(170, 0);
    return;
  }

  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = H - mh - 10;

  if (zhLang()) {
    bool anyArmed = (resetConfirmIdx != 0xFF) && (int32_t)(millis() - resetConfirmUntil) < 0;
    if (menuDirty || anyArmed) {
      zhSpr.fillSprite(p.bg);
      zhSpr.fillRoundRect(0, 0, mw, mh, 4, PANEL);
      zhSpr.drawRoundRect(0, 0, mw, mh, 4, HOT);
      for (int i = 0; i < RESET_N; i++) {
        bool sel = (i == resetSel);
        bool armed = (i == resetConfirmIdx) && anyArmed;
        uint16_t itemCol = armed ? HOT : (sel ? p.text : p.textDim);
        zhSpr.setTextColor(itemCol, PANEL);
        zhSpr.setCursor(6, 8 + i * 14);
        zhSpr.print(sel ? "> " : "  ");
        const char *label = armed ? loc("really?", "確定？")
                                  : resetItemsZh[i];
        menuText(zhSpr, 6 + 12, 8 + i * 14, label, itemCol, PANEL);
      }
      zhSprOwner = ZH_NONE;
      if (!anyArmed) menuDirty = false;
    }
    zhSprToSpr(mx, my);
    spr.setTextSize(1);
    drawMenuHints(p, mx, mw, my + mh - 12);
    return;
  }

  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    bool armed =
        (i == resetConfirmIdx) && (int32_t)(millis() - resetConfirmUntil) < 0;
    uint16_t itemCol = armed ? HOT : (sel ? p.text : p.textDim);
    spr.setTextColor(itemCol, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
  case 0:
    settingsOpen = true;
    menuOpen = false;
    settingsSel = 0;
    break;
  case 1:
    hal_power_off();
    break;
  case 2:
    menuOpen = false;
    displayMode = DISP_INFO;
    infoPage = 0;
    applyDisplayMode();
    characterInvalidate();
    break;
  case 3:
    dataSetDemo(!dataDemo());
    break;
  case 4: {
    // Check for Updates — no manual Wi-Fi toggle needed, OTA handles it
    extern void runOtaUpdate();
    menuOpen = false;
    runOtaUpdate();
    break;
  }
  case 5:
    menuOpen = false;
    characterInvalidate();
    break;
  }
}

static void renderLandscapePet(int cx, int cy, bool force) {
  bool updated = false;

  if (buddyMode) {
    static uint32_t lastBuddyTick = 0;
    if (millis() - lastBuddyTick >= 100 || force) {
      lastBuddyTick = millis();
      petSpr.fillSprite(characterPalette().bg);
      buddyRenderTo(&petSpr, activeState);
      updated = true;
    }
  } else {
    if (force)
      characterInvalidate();
    updated = characterRenderTo(&petSpr, cx, cy);
  }

  if (updated) {
    petSpr.pushSprite(0, 0);
  }
}

void drawMenu() {
  const Palette &p = characterPalette();
  if (clockOrient != 0) {
    // --- Landscape Main Menu ---
    hal_get_lcd()->setRotation(clockOrient);
    renderLandscapePet(85, 85,
                       wasInMenu != (menuOpen || settingsOpen || resetOpen));

    TFT_eSprite &panel = zhLang() ? zhSpr : txtSpr;
    if (!zhLang() || menuDirty) {
      panel.fillSprite(p.bg);
      int mx = 5, my = 40;
      for (int i = 0; i < MENU_N; i++) {
        bool sel = (i == menuSel);
        uint16_t itemCol = sel ? p.text : p.textDim;
        panel.setTextColor(itemCol, p.bg);
        panel.setCursor(mx, my + i * 14);
        panel.print(sel ? "> " : "  ");
        menuText(panel, mx + 12, my + i * 14,
                 loc(menuItems[i], menuItemsZh[i]), itemCol, p.bg);
        if (i == 4) {
          panel.setTextColor(p.textDim, p.bg);
          panel.print(dataDemo() ? " on" : " off");
        }
      }
      zhSprOwner = ZH_NONE;
      if (zhLang()) menuDirty = false;
    }
    panel.pushSprite(170, 0);
    return;
  }

  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = H - mh - 10;

  if (zhLang()) {
    if (menuDirty) {
      zhSpr.fillSprite(p.bg);
      zhSpr.fillRoundRect(0, 0, mw, mh, 4, PANEL);
      zhSpr.drawRoundRect(0, 0, mw, mh, 4, p.textDim);
      for (int i = 0; i < MENU_N; i++) {
        bool sel = (i == menuSel);
        uint16_t itemCol = sel ? p.text : p.textDim;
        zhSpr.setTextColor(itemCol, PANEL);
        zhSpr.setCursor(6, 8 + i * 14);
        zhSpr.print(sel ? "> " : "  ");
        menuText(zhSpr, 6 + 12, 8 + i * 14,
                 menuItemsZh[i], itemCol, PANEL);
        if (i == 4) {
          zhSpr.setTextColor(p.textDim, PANEL);
          zhSpr.print(dataDemo() ? " on" : " off");
        }
      }
      zhSprOwner = ZH_NONE;
      menuDirty = false;
    }
    zhSprToSpr(mx, my);
    spr.setTextSize(1);
    drawMenuHints(p, mx, mw, my + mh - 12);
    return;
  }

  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    uint16_t itemCol = sel ? p.text : p.textDim;
    spr.setTextColor(itemCol, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) {
      spr.setTextColor(p.textDim, PANEL);
      spr.print(dataDemo() ? "  on" : "  off");
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static int8_t orientFrames = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static HAL_Time _clkTm;
static HAL_Date _clkDt;
uint32_t _clkLastRead = 0;
static bool _onUsb = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000)
    return;
  _clkLastRead = millis();
  _onUsb = hal_is_on_usb();
  int h, m, s, y, mon, d, dow;
  hal_get_time(h, m, s);
  hal_get_date(y, mon, d, dow);
  _clkTm.Hours = h;
  _clkTm.Minutes = m;
  _clkTm.Seconds = s;
  _clkDt.Date = d;
  _clkDt.Month = mon;
  _clkDt.Year = y;
  _clkDt.WeekDay = dow;
}

static void clockUpdateOrient() {
  uint8_t lock = settings().clockRot;
  if (lock == 0)
    clockOrient = 0; // Portrait
  else if (lock == 1)
    clockOrient = 1; // Landscape 1
  else
    clockOrient = 3; // Landscape 2
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const DOW[] = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette &p = characterPalette();
  char hm[6];
  snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4];
  snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8];
  snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    // T-Display S3 Portrait Layout: Pet at top, clock centered in lower half.
    // The fillRect below also wipes any leftover transcript pixels when this
    // mode reverts to clock — guarantees no overlap on transition.
    spr.fillRect(0, 150, W, H - 150, p.bg);

    // Transcript-active mode: when transcript was updated within HOLD_MS,
    // show transcript covering the clock area (pet stays at top).
    bool transcriptActive = zhFontReady && tama.nLines > 0 &&
        (int32_t)(transcriptActiveUntilMs - millis()) > 0;
    if (transcriptActive) {
      const int LH = 15;
      const int WIDTH = 26;
      const int SHOW = 10;

      // Smooth-font rendering of ~220 CJK glyphs costs ~50ms/frame; doing it
      // every loop iteration starves the button-poll loop. Cache the rendered
      // zhSpr and only re-draw when content changed OR another render path
      // last wrote to zhSpr (which would have left the wrong content there).
      // Also throttle re-renders to at most every 300ms, so streaming responses
      // (which can bump lineGen several times per second) don't peg the loop.
      static uint32_t cachedLineGen = 0xFFFFFFFF;
      static uint32_t lastRenderMs = 0;
      bool ownerChanged = (zhSprOwner != ZH_CLOCK_PORTRAIT);
      bool contentChanged = (cachedLineGen != tama.lineGen);
      uint32_t nowMs = millis();
      bool needRender = ownerChanged ||
          (contentChanged && (nowMs - lastRenderMs >= 300));

      if (needRender) {
        static char tdisp[32][48];
        static uint8_t tsrcOf[32];
        uint8_t nDisp = 0;
        for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
          uint8_t got = wrapInto(tama.lines[i], &tdisp[nDisp], 32 - nDisp, WIDTH);
          for (uint8_t j = 0; j < got; j++)
            tsrcOf[nDisp + j] = i;
          nDisp += got;
        }
        int end = (int)nDisp;
        int start = end - SHOW;
        if (start < 0)
          start = 0;
        uint8_t newest = tama.nLines - 1;

        zhSpr.fillSprite(p.bg);
        for (int i = 0; start + i < end; i++) {
          uint8_t row = start + i;
          bool fresh = (tsrcOf[row] == newest);
          zhSpr.setTextColor(fresh ? p.text : p.textDim, p.bg);
          zhSpr.setCursor(4, 2 + i * LH);
          zhSpr.print(tdisp[row]);
        }
        cachedLineGen = tama.lineGen;
        lastRenderMs = nowMs;
        zhSprOwner = ZH_CLOCK_PORTRAIT;
      }
      // Push at y=160; sprite is 170 tall, so it covers y=160..320 (10px gap
      // above is intentional for visual separation from pet bottom edge).
      zhSpr.pushToSprite(&spr, 0, 160);
      return;
    }

    // Default clock-and-date view
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(5);
    spr.setTextColor(p.text, p.bg);
    spr.drawString(hm, CX, 180);
    spr.setTextSize(2);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(ss, CX, 225);
    spr.setTextSize(2);
    spr.drawString(dl, CX, 265);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape: 320x170 direct-to-LCD (T-Display S3)
  hal_get_lcd()->setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) {
    hal_get_lcd()->fillScreen(p.bg);
    paintedOrient = clockOrient;
    lastSec = 0xFF;
  }

  // Right side clock area (approx 140..320)
  int clockX = 230;

  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12];
    snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi],
             _clkDt.Date);
    char ssl[3];
    snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    hal_get_lcd()->setTextDatum(MC_DATUM);
    hal_get_lcd()->setTextSize(4);
    hal_get_lcd()->setTextColor(p.text, p.bg);
    hal_get_lcd()->drawString(hm, clockX, 55);
    hal_get_lcd()->setTextSize(2);
    hal_get_lcd()->setTextColor(p.textDim, p.bg);
    hal_get_lcd()->drawString(ssl, clockX, 90);
    hal_get_lcd()->drawString(wdl, clockX, 120);
    hal_get_lcd()->setTextDatum(TL_DATUM);
    hal_get_lcd()->setTextSize(1);
  }

  // Draw status message or transcript.
  // Transcript shown only while activity is recent (TRANSCRIPT_HOLD_MS); after
  // expiry, fall through to else-branch which clears the area and shows tama.msg.
  bool transcriptActive = tama.nLines > 0 &&
      (int32_t)(transcriptActiveUntilMs - millis()) > 0;
  static bool transcriptWasActive = false;
  if (transcriptActive) {
    // CJK path: draw through zhSpr (smooth font); ASCII path: stay on txtSpr.
    if (zhFontReady) {
      uint32_t now = millis();
      // Per-line viewport clipping prevents line 0's marquee glyphs from
      // bleeding into line 1's row when the offset takes them far off-canvas
      // (smooth-font bbox extends ~descender pixels into next line; without
      // the per-line viewport, that's visible as line 0 tails at line 1 y).
      zhSpr.fillSprite(p.bg);
      for (uint8_t i = 0; i < tama.nLines && i < 2; i++) {
        zhSpr.setViewport(0, i * 15, 170, 15);
        zhSpr.setTextColor(i == 0 ? p.text : p.textDim, p.bg);
        int tw = zhSpr.textWidth(tama.lines[i]);
        int x = 0;
        if (tw > 145) {
          int range = tw - 145;
          int offset = (now / 40) % (range * 2);
          if (offset > range)
            offset = range * 2 - offset;
          x -= offset;
        }
        // Within viewport, y is row-relative (top of viewport).
        zhSpr.drawString(tama.lines[i], x, 0);
      }
      zhSpr.resetViewport();
      zhSprOwner = ZH_CLOCK_LANDSCAPE;
      // Push only the 145×30 sub-region that's actually visible on the LCD
      // (x=175..320, y=140..170). Strictly excludes the buffer used by other
      // zhSpr render paths AND avoids any ST7789 RAM wraparound from address
      // windows extending past the visible 320×170 panel edge.
      zhSpr.pushSprite(175, 140, 0, 0, 145, 30);
      transcriptWasActive = true;
    } else {
      // Re-use txtSpr for the transcript block (145x35 approx)
      txtSpr.fillSprite(p.bg);
      txtSpr.setTextSize(1);
      txtSpr.setTextDatum(TL_DATUM);
      uint32_t now = millis();
      for (uint8_t i = 0; i < tama.nLines && i < 3; i++) {
        txtSpr.setTextColor(i == 0 ? p.text : p.textDim, p.bg);
        int tw = txtSpr.textWidth(tama.lines[i]);
        int x = 0;
        if (tw > 145) {
          int range = tw - 145;
          int offset = (now / 40) % (range * 2);
          if (offset > range)
            offset = range * 2 - offset;
          x -= offset;
        }
        txtSpr.drawString(tama.lines[i], x, i * 11);
      }
      txtSpr.pushSprite(175, 138); // Push only the transcript block
      transcriptWasActive = true;
    }
  } else {
    // Only clear once on the transition from active→inactive; doing the
    // fillRect every frame causes flicker (LCD has no double-buffer for
    // direct writes). drawString with text+bg colors overwrites previous
    // glyphs cleanly on subsequent frames.
    if (transcriptWasActive) {
      hal_get_lcd()->fillRect(175, 135, 145, 35, p.bg);
      transcriptWasActive = false;
    }
    hal_get_lcd()->setTextDatum(BC_DATUM);
    hal_get_lcd()->setTextSize(1);
    hal_get_lcd()->setTextColor(p.textDim, p.bg);
    hal_get_lcd()->drawString(tama.msg, clockX, 160);
  }
  hal_get_lcd()->setTextDatum(TL_DATUM);

  // Pet on left side
  renderLandscapePet(85, 85, repaint);
  hal_get_lcd()->setRotation(0);
}

PersonaState derive(const TamaState &s) {
  if (!s.connected)
    return P_IDLE;
  if (s.sessionsWaiting > 0)
    return P_ATTENTION;
  if (s.recentlyCompleted)
    return P_CELEBRATE;
  if (s.sessionsRunning >= 3)
    return P_BUSY;
  return P_IDLE; // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette &p, int &y, const char *section,
                        uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y);
  spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y);
  spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y);
  spr.print(section);
  y += 12;
}

void drawPasskey() {
  static uint32_t lastPasskey = 0;
  static uint32_t lastDraw = 0;
  uint32_t currentPasskey = blePasskey();

  // Only redraw if the passkey changed or 1 second has passed (to keep it
  // alive)
  if (currentPasskey == lastPasskey && millis() - lastDraw < 1000)
    return;
  lastPasskey = currentPasskey;
  lastDraw = millis();

  const Palette &p = characterPalette();
  if (clockOrient == 0) {
    // Portrait: use sprite to avoid flicker
    spr.fillSprite(p.bg);
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, 56);
    spr.print("BLUETOOTH PAIRING");
    spr.setCursor(8, 184);
    spr.print("enter on desktop:");
    spr.setTextSize(3);
    spr.setTextColor(p.text, p.bg);
    char b[8];
    snprintf(b, sizeof(b), "%06lu", (unsigned long)currentPasskey);
    spr.setCursor((W - 18 * 6) / 2, 110);
    spr.print(b);
    hal_get_lcd()->setRotation(0);
    spr.pushSprite(0, 0);
  } else {
    // Landscape: draw direct to LCD (no sprite big enough)
    auto lcd = hal_get_lcd();
    lcd->setRotation(clockOrient);
    lcd->fillScreen(p.bg);
    lcd->setTextDatum(MC_DATUM);
    lcd->setTextSize(1);
    lcd->setTextColor(p.textDim, p.bg);
    lcd->drawString("BLUETOOTH PAIRING", lcd->width() / 2, 40);
    lcd->drawString("enter on desktop:", lcd->width() / 2, lcd->height() - 40);
    lcd->setTextSize(4);
    lcd->setTextColor(p.text, p.bg);
    char b[8];
    snprintf(b, sizeof(b), "%06lu", (unsigned long)currentPasskey);
    lcd->drawString(b, lcd->width() / 2, lcd->height() / 2);
  }
}

void drawInfo() {
  const Palette &p = characterPalette();

  if (clockOrient != 0) {
    static uint32_t lastDraw = 0;
    bool force = wasInMenu != (menuOpen || settingsOpen || resetOpen);
    if (!force && millis() - lastDraw < 33)
      return; // Limit to ~30fps
    lastDraw = millis();

    hal_get_lcd()->setRotation(clockOrient);
    renderLandscapePet(85, 85, force);

    // Landscape Info Grid on the right (txtSpr: 150x170)
    txtSpr.fillSprite(p.bg);
    txtSpr.drawRect(0, 0, 150, 170, p.textDim);
    txtSpr.setTextSize(1);
    txtSpr.setTextColor(p.text, p.bg);
    txtSpr.setCursor(10, 8);
    txtSpr.printf("INFO %u/%u", infoPage + 1, INFO_PAGES);
    txtSpr.drawFastHLine(5, 20, 140, p.textDim);

    auto gridLn = [&](int col, int row, const char *label, const char *val,
                      uint16_t valCol = 0) {
      int x = (col == 0) ? 10 : 80;
      int y = 30 + row * 22;
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(x, y);
      txtSpr.print(label);
      txtSpr.setCursor(x, y + 10);
      txtSpr.setTextColor(valCol ? valCol : p.text, p.bg);
      txtSpr.print(val);
    };

    if (infoPage == 0) {
      // 1. Dashboard
      gridLn(0, 0, "OWNER", ownerName()[0] ? ownerName() : "none");
      gridLn(1, 0, "PET", petName());
      int vBat_mV = (int)(hal_get_battery_voltage() * 1000);
      char batBuf[16];
      snprintf(batBuf, sizeof(batBuf), "%d.%02dV", vBat_mV / 1000,
               (vBat_mV % 1000) / 10);
      gridLn(0, 1, "BATTERY", batBuf);
      bool usb = hal_is_on_usb();
      gridLn(1, 1, "POWER", usb ? (vBat_mV > 4100 ? "FULL" : "USB") : "BAT",
             usb ? GREEN : p.text);
      gridLn(0, 2, "BLE", bleConnected() ? "CONNECTED" : "DISC",
             bleConnected() ? GREEN : HOT);
      uint32_t up = millis() / 1000;
      char upBuf[16];
      snprintf(upBuf, sizeof(upBuf), "%luh%02um", up / 3600, (up / 60) % 60);
      gridLn(1, 2, "UPTIME", upBuf);
      gridLn(0, 3, "MOOD", stateNames[activeState]);
      gridLn(1, 3, "BT NAME", btName);
      extern const char* CURRENT_VERSION;
      gridLn(0, 4, "FIRMWARE", CURRENT_VERSION);

    } else if (infoPage == 1) {
      // 2. Buttons Guide
      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.setCursor(10, 30);
      txtSpr.print("BTN A: FRONT");
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(15, 40);
      txtSpr.print("- TAP:  SCREEN/OK");
      txtSpr.setCursor(15, 50);
      txtSpr.print("- HOLD: MENU");
      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.setCursor(10, 65);
      txtSpr.print("BTN B: SIDE");
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(15, 75);
      txtSpr.print("- TAP:  NEXT/NO");
      txtSpr.setCursor(15, 85);
      txtSpr.print("- HOLD: ROTATE");

    } else if (infoPage == 2) {
      // 3. Bluetooth Detail
      gridLn(0, 0, "NAME", btName);
      uint8_t mac[6] = {0};
      esp_read_mac(mac, ESP_MAC_BT);
      char macB[20];
      snprintf(macB, sizeof(macB), "%02X:%02X:%02X:%02X", mac[2], mac[3],
               mac[4], mac[5]);
      gridLn(0, 1, "MAC (SHORT)", macB);
      gridLn(0, 2, "SECURITY",
             bleConnected() ? (bleSecure() ? "SECURE" : "OPEN") : "N/A");
      gridLn(0, 3, "BONDED", bleConnected() ? "YES" : "NO");

    } else if (infoPage == 3) {
      // 4. Claude Stats
      gridLn(0, 0, "SESSIONS", String(tama.sessionsTotal).c_str());
      gridLn(1, 0, "RUNNING", String(tama.sessionsRunning).c_str());
      gridLn(0, 1, "WAITING", String(tama.sessionsWaiting).c_str(),
             tama.sessionsWaiting > 0 ? HOT : p.text);
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      char ageB[16];
      snprintf(ageB, sizeof(ageB), "%lus ago", (unsigned long)age);
      gridLn(0, 2, "LAST MSG", ageB);

    } else if (infoPage == 4) {
      // 5. Credits
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(10, 30);
      txtSpr.print("original by:");
      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.setCursor(10, 40);
      txtSpr.print("Felix Rieseberg");

      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(10, 55);
      txtSpr.print("modified by:");
      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.setCursor(10, 65);
      txtSpr.print("Kai Tsai");

      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(10, 85);
      txtSpr.print("source:");
      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.setCursor(10, 95);
      txtSpr.print("github.com/oh001738/");
      txtSpr.setCursor(10, 105);
      txtSpr.print("claude-desktop-buddy-");
      txtSpr.setCursor(10, 115);
      txtSpr.print("for-tdisplay-s3");
    } else {
      // 6. About
      txtSpr.setCursor(10, 30);
      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.print("CLAUDE BUDDY S3");
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(10, 45);
      txtSpr.print("A physical companion");
      txtSpr.setCursor(10, 55);
      txtSpr.print("for your AI coding");
      txtSpr.setCursor(10, 65);
      txtSpr.print("sessions.");
    }

    txtSpr.pushSprite(170, 0);
    return;
  }

  // Fallback to existing Portrait Info layout
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char *fmt, ...) {
    char b[48];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    spr.setCursor(4, y);
    spr.print(b);
    y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);
    ln("A   front");
    spr.setTextColor(p.textDim, p.bg);
    ln("    next screen");
    ln("    approve prompt");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg);
    ln("    next page");
    ln("    deny prompt");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("hold A");
    spr.setTextColor(p.textDim, p.bg);
    ln("    menu");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg);
    ln("    tap = screen off");
    ln("    hold 6s = off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-"
                         : bleSecure()   ? "encrypted"
                                         : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(hal_get_battery_voltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;
    if (pct < 0)
      pct = 0;
    if (pct > 100)
      pct = 100;
    bool usb = hal_is_on_usb();

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(usb ? GREEN : p.textDim, p.bg);
    spr.setCursor(60, y + 4);
    spr.print(usb ? (vBat_mV > 4100 ? "full" : "charging") : "battery");
    y += 20;
    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV / 1000, (vBat_mV % 1000) / 10);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0])
      ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);

  } else if (infoPage == 4) {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("original by");
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("modified by");
    spr.setTextColor(p.text, p.bg);
    ln("Kai Tsai");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    spr.setTextColor(p.text, p.bg);
    ln("github.com/oh001738/");
    ln("claude-desktop-buddy-");
    ln("for-tdisplay-s3");
  } else {
    _infoHeader(p, y, "LEGAL", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("MIT License");
    ln("Copyright (c) 2024");
    ln("Felix Rieseberg");
  }
}

// UTF-8 codepoint metrics: returns byte length and display column width.
// ASCII / 2-byte = 1 col. CJK (3-byte) / 4-byte = 2 cols (full-width).
// On invalid leading byte or truncated continuation, treats as 1 col / 1 byte
// so input never stalls the wrapper.
static inline void utf8Cp(const char *p, uint8_t *cpLen, uint8_t *cols) {
  unsigned char c = (unsigned char)*p;
  if (c < 0x80) {
    *cpLen = 1;
    *cols = 1;
    return;
  }
  uint8_t len;
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    *cols = 1;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    *cols = 2;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    *cols = 2;
  } else {
    *cpLen = 1;
    *cols = 1;
    return;
  }
  for (uint8_t i = 1; i < len; i++) {
    if (!p[i] || (((unsigned char)p[i]) & 0xC0) != 0x80) {
      *cpLen = 1;
      *cols = 1;
      return;
    }
  }
  *cpLen = len;
}

// Greedy word-wrap into fixed-width rows. UTF-8 aware: CJK chars take 2
// columns; multi-byte sequences are never split. Continuation rows after
// a wrap get a leading space.
static uint8_t wrapInto(const char *in, char out[][48], uint8_t maxRows,
                        uint8_t width) {
  uint8_t row = 0, col = 0, byteCol = 0;
  const char *p = in;
  while (*p && row < maxRows) {
    while (*p == ' ')
      p++; // skip leading spaces
    if (!*p)
      break;
    // measure next non-space "word" (mix of ASCII and CJK is fine)
    const char *w = p;
    uint8_t wBytes = 0, wCols = 0;
    while (*p && *p != ' ') {
      uint8_t bl, cw;
      utf8Cp(p, &bl, &cw);
      wBytes += bl;
      wCols += cw;
      p += bl;
    }
    if (wBytes == 0)
      break;
    uint8_t need = (col > 0 ? 1 : 0) + wCols;
    if (col + need > width && col > 0) {
      // Only wrap if current row has content; otherwise fall through to
      // hard-break so we don't waste an empty row on a long unbreakable
      // word (common with CJK lines that have no whitespace).
      out[row][byteCol] = 0;
      if (++row >= maxRows)
        return row;
      out[row][0] = ' ';
      byteCol = 1;
      col = 1; // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) {
      if (byteCol < 47) {
        out[row][byteCol++] = ' ';
        col++;
      }
    }
    // hard-break words that still don't fit, codepoint by codepoint
    while (wCols > width - col) {
      uint8_t taken = 0, takenBytes = 0;
      const char *q = w;
      while (taken < width - col) {
        uint8_t bl, cw;
        utf8Cp(q, &bl, &cw);
        if (taken + cw > width - col)
          break;
        if (byteCol + bl >= 47)
          break;
        for (uint8_t i = 0; i < bl; i++)
          out[row][byteCol++] = q[i];
        col += cw;
        taken += cw;
        takenBytes += bl;
        q += bl;
      }
      // safety: ensure forward progress (e.g. a CJK char on a tight row)
      if (takenBytes == 0) {
        uint8_t bl, cw;
        utf8Cp(w, &bl, &cw);
        if (bl > 0 && byteCol + bl < 47) {
          for (uint8_t i = 0; i < bl; i++)
            out[row][byteCol++] = w[i];
          col += cw;
          taken += cw;
          takenBytes += bl;
        } else {
          break;
        }
      }
      out[row][byteCol] = 0;
      if (++row >= maxRows)
        return row;
      out[row][0] = ' ';
      byteCol = 1;
      col = 1;
      w += takenBytes;
      wBytes -= takenBytes;
      wCols -= taken;
    }
    if (byteCol + wBytes < 47) {
      for (uint8_t i = 0; i < wBytes; i++)
        out[row][byteCol++] = w[i];
      col += wCols;
    }
  }
  if (col > 0 && row < maxRows) {
    out[row][byteCol] = 0;
    row++;
  }
  return row;
}

static void drawApproval() {
  const Palette &p = characterPalette();
  if (clockOrient != 0) {
    // --- Landscape Approval: Stable Menu-style logic ---
    hal_get_lcd()->setRotation(clockOrient);

    // Ensure pet doesn't flicker, using our unified logic
    renderLandscapePet(85, 85, true);

    txtSpr.fillSprite(p.bg);
    txtSpr.setTextSize(1);
    txtSpr.setTextColor(HOT, p.bg);
    txtSpr.setCursor(5, 10);
    uint32_t waited = (millis() - promptArrivedMs) / 1000;
    txtSpr.printf("APPROVE? %lus", (unsigned long)waited);

    txtSpr.setTextColor(p.text, p.bg);
    txtSpr.setTextSize(2);
    txtSpr.setCursor(5, 25);
    txtSpr.print(tama.promptTool);

    txtSpr.setTextSize(1);
    txtSpr.setTextColor(p.textDim, p.bg);
    txtSpr.setCursor(5, 50);
    // Hint with manual wrap for the 150px txtSpr
    txtSpr.printf("%.20s", tama.promptHint);
    if (strlen(tama.promptHint) > 20) {
      txtSpr.setCursor(5, 60);
      txtSpr.printf("%.20s", tama.promptHint + 20);
    }

    // Bottom Action Area
    if (responseSent) {
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.drawString("sent...", 75, 150, 2);
    } else {
      txtSpr.setTextColor(GREEN, p.bg);
      txtSpr.drawString("1:Approve", 5, 150, 2);
      txtSpr.setTextColor(HOT, p.bg);
      txtSpr.drawString("2:Deny", 85, 150, 2);
    }

    txtSpr.pushSprite(170, 0);
    return;
  }

  // --- Portrait Approval ---
  const int AREA = 110;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10)
    spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("1: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("2: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette &p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2);
  spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++)
    tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2);
  spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed)
      spr.fillCircle(px, y + 1, 2, p.body);
    else
      spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2);
  spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en)
      spr.fillRect(px, y - 2, 9, 6, enCol);
    else
      spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1);
  spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap / 3600, (nap / 60) % 60);
  auto tokFmt = [&](const char *label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)
      spr.printf("%s%lu.%luM", label, v / 1000000, (v / 100000) % 10);
    else if (v >= 1000)
      spr.printf("%s%lu.%luK", label, v / 1000, (v / 100) % 10);
    else
      spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette &p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char *s) {
    spr.setTextColor(c, p.bg);
    spr.setCursor(6, y);
    spr.print(s);
    y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12; // room for the PET header drawn by drawPet()

  ln(p.body, "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down");
  gap();

  ln(p.body, "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti");
  gap();

  ln(p.body, "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full");
  gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake");
  gap();

  ln(p.textDim, "1: screens  2: page");
  ln(p.textDim, "hold 1: menu");
}

void drawPet() {
  const Palette &p = characterPalette();

  if (clockOrient != 0) {
    static uint32_t lastDraw = 0;
    bool force = wasInMenu != (menuOpen || settingsOpen || resetOpen);
    if (!force && millis() - lastDraw < 33)
      return; // Limit to ~30fps
    lastDraw = millis();

    hal_get_lcd()->setRotation(clockOrient);
    renderLandscapePet(85, 85, force);

    // Landscape Pet Stats on the right (txtSpr: 150x170)
    txtSpr.fillSprite(p.bg);
    txtSpr.drawRect(0, 0, 150, 170, p.textDim);
    txtSpr.setTextSize(1);
    txtSpr.setTextColor(p.text, p.bg);
    txtSpr.setCursor(10, 8);
    txtSpr.printf("STATUS %u/%u", petPage + 1, PET_PAGES);
    txtSpr.drawFastHLine(5, 20, 140, p.textDim);

    if (petPage == 0) {
      // Draw Stats in Grid
      auto statBar = [&](int y, const char *label, int val, int maxVal) {
        txtSpr.setTextColor(p.textDim, p.bg);
        txtSpr.setCursor(10, y);
        txtSpr.print(label);
        int barW = 100;
        int fillW = (val * barW) / maxVal;
        txtSpr.drawRect(10, y + 10, barW, 8, p.textDim);
        txtSpr.fillRect(11, y + 11, fillW - 2, 6, GREEN);
      };

      statBar(30, "MOOD", statsMoodTier(), 4);
      statBar(60, "FED", statsFedProgress(), 10);
      statBar(90, "ENERGY", statsEnergyTier(), 5);

      txtSpr.setTextColor(p.text, p.bg);
      txtSpr.setCursor(10, 130);
      txtSpr.printf("MOOD: %s", stateNames[activeState]);
    } else {
      txtSpr.setCursor(10, 30);
      txtSpr.print("PET GUIDE");
      txtSpr.setTextColor(p.textDim, p.bg);
      txtSpr.setCursor(10, 50);
      txtSpr.print("Interact on desktop");
      txtSpr.setCursor(10, 60);
      txtSpr.print("to boost mood!");
    }
    txtSpr.pushSprite(170, 0);
    return;
  }

  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  if (petPage == 0)
    drawPetStats(p);
  else
    drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) {
    drawApproval();
    return;
  }
  const Palette &p = characterPalette();

  if (clockOrient != 0) {
    // --- Landscape HUD: Unified with Clock ---
    drawClock();

    // drawClock already handled pet, clock, and (if active) the transcript.
    // Only overlay tama.msg when transcript is NOT active — otherwise the
    // status text would clobber transcript line 2 (both occupy y=140..170).
    bool transcriptActive = tama.nLines > 0 &&
        (int32_t)(transcriptActiveUntilMs - millis()) > 0;
    if (!transcriptActive) {
      hal_get_lcd()->setTextDatum(BC_DATUM);
      hal_get_lcd()->setTextSize(1);
      hal_get_lcd()->setTextColor(p.textDim, p.bg);
      hal_get_lcd()->drawString(tama.msg, 230, 160);
      hal_get_lcd()->setTextDatum(TL_DATUM);
    }
    return;
  }

  // --- Traditional Portrait HUD ---
  // Layout adapts to whether the CJK font is available. ASCII layout: 6
  // lines of 10px = 60px area. CJK layout: 4 lines of 15px = 60px area —
  // matches the font's yAdvance to prevent descender overlap.
  const int SHOW = zhFontReady ? 4 : 6;
  const int LH = zhFontReady ? 15 : 10;
  const int WIDTH = zhFontReady ? 26 : 26;
  const int AREA = SHOW * LH + 8;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) {
    msgScroll = 0;
    lastLineGen = tama.lineGen;
    wake();
  }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  static char disp[32][48];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++)
      srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack)
    msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW;
  if (start < 0)
    start = 0;
  uint8_t newest = tama.nLines - 1;

  if (zhFontReady) {
    // Render to zhSpr (smooth font), then COMPOSITE into the main `spr` via
    // pushToSprite. We must NOT pushSprite to LCD here, because spr.pushSprite
    // runs at end-of-frame and would overwrite our transcript with bg.
    zhSpr.fillSprite(p.bg);
    for (int i = 0; start + i < end; i++) {
      uint8_t row = start + i;
      bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
      zhSpr.setTextColor(fresh ? p.text : p.textDim, p.bg);
      zhSpr.setCursor(4, 2 + i * LH);
      zhSpr.print(disp[row]);
    }
    zhSprOwner = ZH_HUD_PORTRAIT;
    zhSpr.pushToSprite(&spr, 0, H - AREA);
  } else {
    for (int i = 0; start + i < end; i++) {
      uint8_t row = start + i;
      bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
      spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
      spr.setCursor(4, H - AREA + 2 + i * LH);
      spr.print(disp[row]);
    }
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[SYSTEM] Kai's Buddy is waking up...");
  
  hal_init();
  if (!LittleFS.begin()) {
    Serial.println("[ERROR] LittleFS Mount Failed!");
  } else {
    Serial.printf("[SYSTEM] FS Space: %lu/%lu KB used\n",
                  (unsigned long)LittleFS.usedBytes()/1024,
                  (unsigned long)LittleFS.totalBytes()/1024);

    Serial.println("[SYSTEM] Scanning for Pets...");
    fs::File root = LittleFS.open("/characters");
    if (root && root.isDirectory()) {
      fs::File f = root.openNextFile();
      while (f) {
        if (f.isDirectory()) {
          Serial.printf("  - Found Pet: %s\n", f.name());
        }
        f = root.openNextFile();
      }
    }
  }
  
  startBt();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // off
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  clockUpdateOrient(); // Ensure orientation is applied immediately after load
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.createSprite(W, H);
  petSpr.createSprite(170, 170);
  txtSpr.createSprite(150, 170);

  // Optional CJK transcript font. Falls back to ASCII bitmap if file missing
  // or sprite alloc fails — never blocks boot.
  // Sprite sized 170x170 so the same buffer handles both: 4-line HUD bottom
  // strip AND 10-line idle-clock-replacement transcript view.
  if (LittleFS.exists("/ZhTW12.vlw")) {
    zhSpr.setColorDepth(16);
    if (zhSpr.createSprite(170, 170)) {
      zhSpr.loadFont("ZhTW12", LittleFS);
      zhSpr.setTextDatum(TL_DATUM);
      zhFontReady = true;
      Serial.println("[SYSTEM] Chinese transcript font loaded");
    } else {
      Serial.println("[WARN] zhSpr alloc failed; transcript stays ASCII");
    }
  }

  characterInit(nullptr);
  gifAvailable = characterLoaded();
  Serial.printf("GIF Pet Available: %s\n", gifAvailable ? "YES" : "NO");

  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF
  uint8_t savedSpecies = speciesIdxLoad();
  buddyMode = !(gifAvailable && savedSpecies == SPECIES_GIF);
  if (buddyMode && savedSpecies < buddySpeciesCount()) {
    buddySetSpeciesIdx(savedSpecies);
  }

  applyDisplayMode();
  clockUpdateOrient(); // Load saved orientation

  {
    auto lcd = hal_get_lcd();
    lcd->setRotation(clockOrient);
    
    // Cyberpunk Neon Boot Animation
    int sw = lcd->width();
    int sh = lcd->height();
    
    TFT_eSprite bootSpr(lcd);
    if (bootSpr.createSprite(sw, sh)) {
      bootSpr.setSwapBytes(true);
      
      // Keep backlight off until the first frame is ready to avoid screen flash
      bool backlightOn = false;
      
      uint32_t startMs = millis();
      const uint32_t dur = 4000; // 4 seconds animation
      
      while (millis() - startMs < dur) {
        uint32_t elapsed = millis() - startMs;
        
        // 1. Draw solid dark purple background
        bootSpr.fillSprite(lcd->color565(5, 2, 12));
        
        // 2. Background Grid lines (20px spacing)
        if (elapsed > 200) {
          uint16_t gridColor = lcd->color565(45, 10, 26);
          for (int x = 0; x <= sw; x += 20) {
            bootSpr.drawFastVLine(x, 0, sh, gridColor);
          }
          for (int y = 0; y <= sh; y += 20) {
            bootSpr.drawFastHLine(0, y, sw, gridColor);
          }
        }
        
        // 3. Corner brackets (appear at 500ms, solid magenta, no alpha)
        if (elapsed > 500) {
          uint16_t bracketColor = lcd->color565(255, 0, 127);
          int m = 8, len = 15;
          // Top-Left
          bootSpr.drawFastHLine(m, m, len, bracketColor);
          bootSpr.drawFastVLine(m, m, len, bracketColor);
          // Top-Right
          bootSpr.drawFastHLine(sw - m - len, m, len, bracketColor);
          bootSpr.drawFastVLine(sw - m, m, len, bracketColor);
          // Bottom-Left
          bootSpr.drawFastHLine(m, sh - m, len, bracketColor);
          bootSpr.drawFastVLine(m, sh - m - len, len, bracketColor);
          // Bottom-Right
          bootSpr.drawFastHLine(sw - m - len, sh - m, len, bracketColor);
          bootSpr.drawFastVLine(sw - m, sh - m - len, len, bracketColor);
        }
        
        // 4. Moving Scan beam (between 300ms and 2000ms)
        if (elapsed > 300 && elapsed < 2000) {
          float beamProgress = (float)(elapsed - 300) / 1700.0f;
          int beamY = (int)(beamProgress * sh);
          bootSpr.drawFastHLine(0, beamY, sw, lcd->color565(255, 0, 127));
          if (beamY < sh - 1) {
            bootSpr.drawFastHLine(0, beamY + 1, sw, lcd->color565(255, 0, 127));
          }
        }
        
        // 5. Title Text & Glow shadow (appears at 800ms)
        if (elapsed > 800) {
          bootSpr.setTextDatum(MC_DATUM);
          int x = sw / 2;
          
          // Glitch flicker (simulate random coordinates offsets and channel lines)
          if ((elapsed > 1000 && elapsed < 1100) || (elapsed > 2200 && elapsed < 2250)) {
            x += (random(100) > 50 ? 4 : -4);
            bootSpr.fillRect(x - 50, sh / 2 - 10, 100, 2, TFT_CYAN);
          }
          
          uint16_t glowColor = lcd->color565(187, 0, 85);
          uint16_t fgColor = TFT_WHITE;
          uint16_t subColor = TFT_CYAN;
          
          if (sw < 200) {
            // PORTRAIT LAYOUT
            int y1 = sh / 2 - 35;
            int y2 = sh / 2 - 10;
            int y3 = sh / 2 + 15;
            
            // "CLAUDE" shadow glow + foreground
            bootSpr.setTextColor(glowColor);
            bootSpr.drawString("CLAUDE", x - 1, y1, 4);
            bootSpr.drawString("CLAUDE", x + 1, y1, 4);
            bootSpr.drawString("CLAUDE", x, y1 - 1, 4);
            bootSpr.drawString("CLAUDE", x, y1 + 1, 4);
            bootSpr.setTextColor(fgColor);
            bootSpr.drawString("CLAUDE", x, y1, 4);
            
            // "BUDDY" shadow glow + foreground
            bootSpr.setTextColor(glowColor);
            bootSpr.drawString("BUDDY", x - 1, y2, 4);
            bootSpr.drawString("BUDDY", x + 1, y2, 4);
            bootSpr.drawString("BUDDY", x, y2 - 1, 4);
            bootSpr.drawString("BUDDY", x, y2 + 1, 4);
            bootSpr.setTextColor(fgColor);
            bootSpr.drawString("BUDDY", x, y2, 4);
            
            // Subtitle
            bootSpr.setTextColor(subColor);
            bootSpr.drawString("S3 EDITION", x, y3, 2);
          } else {
            // LANDSCAPE LAYOUT
            int y1 = sh / 2 - 18;
            int y2 = sh / 2 + 10;
            
            // "CLAUDE BUDDY" shadow glow + foreground
            bootSpr.setTextColor(glowColor);
            bootSpr.drawString("CLAUDE BUDDY", x - 1, y1, 4);
            bootSpr.drawString("CLAUDE BUDDY", x + 1, y1, 4);
            bootSpr.drawString("CLAUDE BUDDY", x, y1 - 1, 4);
            bootSpr.drawString("CLAUDE BUDDY", x, y1 + 1, 4);
            bootSpr.setTextColor(fgColor);
            bootSpr.drawString("CLAUDE BUDDY", x, y1, 4);
            
            // Subtitle
            bootSpr.setTextColor(subColor);
            bootSpr.drawString("S3 EDITION", x, y2, 2);
          }
        }
        
        // 6. Version and Divider (appears at 1500ms)
        if (elapsed > 1500) {
          int divY = (sw < 200) ? (sh / 2 + 38) : (sh / 2 + 22);
          int verY = (sw < 200) ? (sh / 2 + 50) : (sh / 2 + 36);
          
          bootSpr.drawFastHLine(sw / 2 - 50, divY, 100, lcd->color565(68, 0, 34));
          bootSpr.setTextColor(lcd->color565(255, 0, 127));
          bootSpr.drawString("v1.1.3", sw / 2, verY, 2);
        }
        
        // 7. Scan line filter (CRT simulation) - drawn on top of everything
        for (int y = 1; y < sh; y += 2) {
          bootSpr.drawFastHLine(0, y, sw, TFT_BLACK);
        }
        
        // Push double-buffered frame to LCD
        bootSpr.pushSprite(0, 0);
        
        // Turn on screen backlight PWM on the very first frame to prevent startup flash
        if (!backlightOn) {
          applyBrightness();
          backlightOn = true;
        }
        
        // Check for skip click (any button instantly skips)
        hal_loop();
        if (hal_btn_a_clicked() || hal_btn_b_clicked()) {
          Serial.println("[SYSTEM] Boot animation skipped by user.");
          break;
        }
        
        delay(16);
      }
      bootSpr.deleteSprite();
    }
    
    // Restore default settings
    lcd->setTextDatum(TL_DATUM);
    lcd->setTextSize(1);
    lcd->setRotation(0); // Reset for main sprite path
  }

  Serial.printf("buddy: %s\n",
                buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  hal_loop();
  static bool wasPasskey = false;
  uint32_t now = millis();
  bool btnA = hal_btn_a_clicked();
  bool btnB = hal_btn_b_clicked();
  btnALong = hal_btn_a_long_pressed();
  t++;

  dataPoll(&tama);
  // Transcript activity tracker: each new line resets the hold timer so the
  // portrait clock view shows transcript instead of time for HOLD_MS ms after
  // the most recent line arrived.
  if (tama.lineGen != lastSeenLineGen) {
    lastSeenLineGen = tama.lineGen;
    transcriptActiveUntilMs = millis() + TRANSCRIPT_HOLD_MS;
    wake();
  }
  if (statsPollLevelUp())
    triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0)
    baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0)
    activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && hal_check_shake() &&
        (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80); // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode)
        buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;
  clockRefreshRtc();
  bool clocking = displayMode == DISP_NORMAL && !menuOpen && !settingsOpen &&
                  !resetOpen && !inPrompt && tama.sessionsRunning == 0 &&
                  tama.sessionsWaiting == 0 && dataRtcValid() && _onUsb;
  bool inMenu = menuOpen || settingsOpen || resetOpen;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (hal_btn_a_pressed() || hal_btn_b_pressed()) {
    if (screenOff) {
      if (hal_btn_a_pressed())
        swallowBtnA = true;
      if (hal_btn_b_pressed())
        swallowBtnB = true;
    }
    wake();
  }

  // If a passkey is active, we swallow most button actions to avoid
  // accidental state changes, but we still allow wake/interact.
  bool pairingActive = blePasskey() != 0;

  if (btnALong && !swallowBtnA) {
    btnALong = true;
    if (pairingActive) { /* swallow */
    } else {
      beep(800, 60);
      if (resetOpen) {
        resetOpen = false;
        menuInvalidate();
      } else if (settingsOpen) {
        settingsOpen = false;
        menuInvalidate();
        characterInvalidate();
      } else {
        menuOpen = !menuOpen;
        menuSel = 0;
        menuInvalidate();
        if (!menuOpen)
          characterInvalidate();
      }
    }
  }
  if (hal_btn_a_clicked()) {
    lastInteractMs = millis();
    if (!btnALong && !swallowBtnA) {
      if (pairingActive) {
        beep(1800, 30); /* feedback only */
      } else if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd),
                 "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
                 tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5)
          triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
        menuInvalidate();
      } else if (settingsOpen) {
        beep(1800, 30);
        do {
          settingsSel = (settingsSel + 1) % SETTINGS_N;
        } while (!isSettingVisible(settingsSel));
        menuInvalidate();
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
        menuInvalidate();
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (btnB) {
    lastInteractMs = millis();
    if (swallowBtnB) {
      swallowBtnB = false;
    } else if (pairingActive) {
      beep(2400, 30); /* feedback only */
    } else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
      menuInvalidate();
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
      menuInvalidate();
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
      menuInvalidate();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else if (displayMode == DISP_NORMAL && !inMenu && !inPrompt) {
      // Button 2 cycles the rotation setting in normal mode
      Settings &s = settings();
      s.clockRot = (s.clockRot + 1) % 3;
      clockUpdateOrient();
      beep(2400, 30);
      settingsSave();
      characterInvalidate(); // Force redraw for pet position
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc(); // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  static bool wasPrompt = false;
  // inMenu and inPrompt moved up

  if (clocking != wasClocking || landscapeClock != wasLandscape ||
      (wasInMenu && !inMenu) || (wasDisplayMode != displayMode) ||
      (wasPrompt != inPrompt)) {
    if (clocking && !landscapeClock)
      characterSetPeek(buddyMode);
    else
      applyDisplayMode();
    characterInvalidate();
    if (buddyMode)
      buddyInvalidate();

    // Trigger repaint if it was a mode/prompt/menu transition
    if ((wasInMenu != inMenu) || (wasDisplayMode != displayMode) ||
        (wasPrompt != inPrompt)) {
      paintedOrient = 0xFF;
    }

    wasClocking = clocking;
    wasLandscape = landscapeClock;
    wasPrompt = inPrompt;
  }
  wasInMenu = inMenu;
  wasDisplayMode = displayMode;
  wasPrompt = inPrompt; // Ensure global sync too
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)
      activeState = P_SLEEP;
    else if (weekend)
      activeState = (now / 8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)
      activeState = (now / 6000 % 4 == 0) ? P_IDLE : P_SLEEP;
    else if (h == 12)
      activeState = (now / 5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)
      activeState = (now / 4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)
      activeState = (now / 7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else
      activeState = (now / 10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) {
    wake();
    beep(1800, 60);
  }
  lastPasskey = pk;

  if (napping || screenOff || landscapeClock) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else // In landscape mode, the dedicated drawInfo/drawPet/drawClock handle
    // their own pet rendering to ensure proper scaling and positioning.
    // We only run the background ticks in portrait (normal) mode.
  if (characterLoaded()) {
    characterSetState(activeState);
  }

  if (clockOrient == 0) {
    if (buddyMode) {
      buddyTick(activeState);
    } else if (characterLoaded()) {
      characterTick();
    }
  } else {
      const Palette &p = characterPalette();
      spr.fillSprite(p.bg);
      spr.setTextColor(p.textDim, p.bg);
      spr.setTextSize(1);
      if (xferActive()) {
        uint32_t done = xferProgress(), total = xferTotal();
        spr.setCursor(8, 90);
        spr.print("installing");
        spr.setCursor(8, 102);
        spr.printf("%luK / %luK", done / 1024, total / 1024);
        int barW = W - 16;
        spr.drawRect(8, 116, barW, 8, p.textDim);
        if (total > 0) {
          int fill = (int)((uint64_t)barW * done / total);
          if (fill > 1)
            spr.fillRect(9, 117, fill - 1, 6, p.body);
        }
      } else {
        spr.setCursor(8, 100);
        spr.print("no character loaded");
      }
    }
  // Passkey is a global priority: wake and render even if screen was off
  if (blePasskey()) {
    if (screenOff || napping) {
      wake();
      napping = false;
      hal_set_brightness(brightLevel);
    }
    drawPasskey();
    wasPasskey = true;
  } else if (!napping && !screenOff) {
    if (wasPasskey) {
      hal_get_lcd()->fillScreen(characterPalette().bg);
      wasPasskey = false;
    }
    if (menuOpen || settingsOpen || resetOpen) {
      if (clockOrient == 0) {
        hal_get_lcd()->setRotation(0);
        if (activeState == P_IDLE)
          drawClock();
        else
          drawHUD();
        if (menuOpen)
          drawMenu();
        else if (settingsOpen)
          drawSettings();
        else if (resetOpen)
          drawReset();
        spr.pushSprite(0, 0);
      } else {
        if (menuOpen)
          drawMenu();
        else if (settingsOpen)
          drawSettings();
        else if (resetOpen)
          drawReset();
      }
    } else if (clocking) {
      drawClock();
      if (clockOrient == 0)
        spr.pushSprite(0, 0);
    } else if (displayMode == DISP_INFO) {
      drawInfo();
      if (clockOrient == 0)
        spr.pushSprite(0, 0);
    } else if (displayMode == DISP_PET) {
      drawPet();
      if (clockOrient == 0)
        spr.pushSprite(0, 0);
    } else if (displayMode == DISP_NORMAL) {
      if (activeState == P_IDLE)
        drawClock();
      else
        drawHUD();
      if (clockOrient == 0)
        spr.pushSprite(0, 0);
    }
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = hal_is_face_down();
    if (down) {
      if (faceDownFrames < 20)
        faceDownFrames++;
    } else {
      if (faceDownFrames > -10)
        faceDownFrames--;
    }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    hal_screen_off();
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  // No auto-off on USB power or during pairing/prompts
  if (!screenOff && !inPrompt && !blePasskey() && !_onUsb &&
      millis() - lastInteractMs > SCREEN_OFF_MS) {
    hal_screen_off();
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}

#include "ota.h"

void drawWifiPortalScreen(const char* apName) {
  const Palette &p = characterPalette();
  auto lcd = hal_get_lcd();
  
  if (clockOrient == 0) {
    // Portrait: draw using spr
    spr.fillSprite(p.bg);
    spr.drawRoundRect(10, 10, W - 20, H - 20, 8, p.textDim);
    
    spr.setTextSize(1);
    spr.setTextColor(p.text, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.drawString("WI-FI SETUP", W / 2, 40);
    spr.drawFastHLine(20, 55, W - 40, p.textDim);
    
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("Connect to AP:", W / 2, 100);
    
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.drawString(apName, W / 2, 130);
    
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("on your phone/PC", W / 2, 160);
    spr.drawString("to configure Wi-Fi", W / 2, 180);
    
    spr.setTextDatum(TL_DATUM);
    lcd->setRotation(0);
    spr.pushSprite(0, 0);
  } else {
    // Landscape: draw direct to LCD
    lcd->setRotation(clockOrient);
    lcd->fillScreen(p.bg);
    
    int sw = lcd->width();
    int sh = lcd->height();
    
    lcd->drawRoundRect(10, 10, sw - 20, sh - 20, 8, p.textDim);
    
    lcd->setTextSize(2);
    lcd->setTextColor(p.text, p.bg);
    lcd->setTextDatum(MC_DATUM);
    lcd->drawString("WI-FI SETUP", sw / 2, 40);
    lcd->drawFastHLine(30, 60, sw - 60, p.textDim);
    
    lcd->setTextSize(1);
    lcd->setTextColor(p.textDim, p.bg);
    lcd->drawString("Connect to AP:", sw / 2, 85);
    
    lcd->setTextColor(p.text, p.bg);
    lcd->setTextSize(2);
    lcd->drawString(apName, sw / 2, 110);
    
    lcd->setTextSize(1);
    lcd->setTextColor(p.textDim, p.bg);
    lcd->drawString("to configure Wi-Fi settings", sw / 2, 140);
    
    lcd->setTextDatum(TL_DATUM);
  }
}

void drawOtaProgress(const char* status, int progress) {
  const Palette &p = characterPalette();
  auto lcd = hal_get_lcd();
  
  if (clockOrient == 0) {
    // Portrait: draw using spr
    spr.fillSprite(p.bg);
    spr.drawRoundRect(10, 10, W - 20, H - 20, 8, p.textDim);
    
    spr.setTextSize(1);
    spr.setTextColor(p.text, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.drawString("SYSTEM UPDATE", W / 2, 40);
    spr.drawFastHLine(20, 55, W - 40, p.textDim);
    
    // Status text
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString(status, W / 2, 120);
    
    if (progress >= 0 && progress <= 100) {
      // Progress bar
      int barW = W - 40;
      spr.drawRect(20, 160, barW, 12, p.textDim);
      int fill = (barW - 2) * progress / 100;
      if (fill > 0) {
        spr.fillRect(21, 161, fill, 10, p.body);
      }
      
      // Percent text
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", progress);
      spr.setTextColor(p.text, p.bg);
      spr.drawString(pct, W / 2, 190);
    }
    
    spr.setTextDatum(TL_DATUM);
    lcd->setRotation(0);
    spr.pushSprite(0, 0);
  } else {
    // Landscape: draw direct to LCD with partial clearing to avoid flickering
    int sw = lcd->width();
    int sh = lcd->height();
    
    static int lastOrient = -1;
    static bool firstFrame = true;
    if (progress <= 0) { // Reset on start (0) or error (-1)
      firstFrame = true;
    }
    
    bool redrawStatic = (clockOrient != lastOrient) || firstFrame;
    lastOrient = clockOrient;
    firstFrame = false;
    
    if (redrawStatic) {
      lcd->setRotation(clockOrient);
      lcd->fillScreen(p.bg);
      lcd->drawRoundRect(10, 10, sw - 20, sh - 20, 8, p.textDim);
      
      lcd->setTextSize(2);
      lcd->setTextColor(p.text, p.bg);
      lcd->setTextDatum(MC_DATUM);
      lcd->drawString("SYSTEM UPDATE", sw / 2, 40);
      lcd->drawFastHLine(30, 60, sw - 60, p.textDim);
    } else {
      // Clear the dynamic text & progress bar area only
      lcd->fillRect(12, 70, sw - 24, 90, p.bg);
    }
    
    // Status text
    lcd->setTextSize(1);
    lcd->setTextColor(p.textDim, p.bg);
    lcd->setTextDatum(MC_DATUM);
    lcd->drawString(status, sw / 2, 90);
    
    if (progress >= 0 && progress <= 100) {
      // Progress bar
      int barW = sw - 80;
      lcd->drawRect(40, 115, barW, 14, p.textDim);
      int fill = (barW - 2) * progress / 100;
      if (fill > 0) {
        lcd->fillRect(41, 116, fill, 12, p.body);
      }
      
      // Percent text
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", progress);
      lcd->setTextColor(p.text, p.bg);
      lcd->drawString(pct, sw / 2, 145);
    }
    
    lcd->setTextDatum(TL_DATUM);
  }
}

static void otaProgressCallback(const char* status, int progress) {
  drawOtaProgress(status, progress);
  yield(); // feed WDT
}

void runOtaUpdate() {
  menuOpen = settingsOpen = resetOpen = false; // Close menu during update
  char errBuf[128] = "";
  
  bool ok = otaUpdateFlow(otaProgressCallback, errBuf, sizeof(errBuf));
  
  if (!ok) {
    drawOtaProgress(errBuf, -1);
    beep(600, 500); 
    delay(4000);
    
    // Return to main screen
    menuOpen = false;
    settingsOpen = false;
    menuDirty = true;
    characterInvalidate();
  }
}
