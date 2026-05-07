#include "character.h"
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <ArduinoJson.h>

extern TFT_eSprite spr;

static const char* STATE_NAMES[] = {
  "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"
};
static const uint8_t N_STATES = 7;

struct TextState {
  char     frames[8][20];
  uint8_t  nFrames;
  uint16_t delayMs;
};
static TextState textStates[N_STATES];
static bool      textMode = false;
static uint8_t   textFrame = 0;
static uint32_t  textNext = 0;

static bool    loaded = false;
static Palette pal = { 0xC2A6, 0x0000, 0xFFFF, 0x8410, 0x0000 };
static char    basePath[48];
static const uint8_t MAX_GIFS = 32;
static char    gifPaths[MAX_GIFS][32];
static uint8_t stateStart[N_STATES];
static uint8_t stateCount[N_STATES];
static uint8_t stateRot[N_STATES];
static uint8_t gifTotal = 0;
static uint8_t curState = 0xFF;

static AnimatedGIF gif;
static fs::File        gifFile;
static int         gifX = 0, gifY = 0, gifW = 0, gifH = 0;
static const int   PEEK_TOP = 70;
static bool        peekMode = false;
static TFT_eSPI*   _tgt = &spr;

static void gifPlace() {
  int outW = peekMode ? gifW / 2 : gifW;
  int outH = peekMode ? gifH / 2 : gifH;
  gifX = (_tgt->width() - outW) / 2;
  gifY = peekMode ? (PEEK_TOP - outH) / 2 : (150 - outH) / 2;
}
static uint32_t    nextFrameAt = 0;
static uint32_t    animPauseUntil = 0;
static uint32_t    variantStartedMs = 0;
static const uint32_t VARIANT_DWELL_MS = 5000;
static const uint32_t ANIM_PAUSE_MS    = 800;
static bool        gifOpen = false;

static uint16_t parseHexColor(const char* s, uint16_t fallback) {
  if (!s) return fallback;
  if (*s == '#') s++;
  uint32_t v = strtoul(s, nullptr, 16);
  return (uint16_t)(((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F));
}

static void* gifOpenCb(const char* fname, int32_t* pSize) {
  gifFile = LittleFS.open(fname, "r");
  if (!gifFile) return nullptr;
  *pSize = gifFile.size();
  return (void*)&gifFile;
}
static void gifCloseCb(void* handle) {
  fs::File* f = (fs::File*)handle;
  if (f) f->close();
}
static int32_t gifReadCb(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  fs::File* f = (fs::File*)pFile->fHandle;
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos = f->position();
  return n;
}
static int32_t gifSeekCb(GIFFILE* pFile, int32_t iPosition) {
  fs::File* f = (fs::File*)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

static void gifDrawCb(GIFDRAW* d) {
  uint16_t* pal16 = d->pPalette;
  uint8_t*  src   = d->pPixels;
  uint8_t   t     = d->ucTransparent;
  bool      hasT  = d->ucHasTransparency;
  int       srcY  = d->iY + d->y;

  auto put = [&](int x, int y, uint8_t idx) {
    _tgt->drawPixel(x, y, (hasT && idx == t) ? pal.bg : pal16[idx]);
  };

  if (peekMode) {
    if (srcY & 1) return;
    int y = gifY + (srcY >> 1);
    if (y < 0 || y >= PEEK_TOP) return;
    int x0 = gifX + (d->iX >> 1);
    int w  = d->iWidth >> 1;
    for (int i = 0; i < w; i++) put(x0 + i, y, src[i << 1]);
    return;
  }

  int y = gifY + srcY;
  if (y < 0 || y >= _tgt->height()) return;
  int x0 = gifX + d->iX;
  int w  = d->iWidth;
  if (w > 256) w = 256;
  if (x0 < 0) { src -= x0; w += x0; x0 = 0; }
  if (x0 + w > _tgt->width()) w = _tgt->width() - x0;
  if (w <= 0) return;
  for (int i = 0; i < w; i++) put(x0 + i, y, src[i]);
}

bool characterInit(const char* name) {
  if (!LittleFS.begin(false)) {
    if (!LittleFS.open("/")) return false;
  }
  static char scanned[24];
  if (!name) {
    fs::File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      fs::File e = d.openNextFile();
      while (e) {
        if (e.isDirectory()) {
          const char* n = strrchr(e.name(), '/');
          strncpy(scanned, n ? n + 1 : e.name(), sizeof(scanned) - 1);
          scanned[sizeof(scanned) - 1] = 0;
          name = scanned;
          break;
        }
        e = d.openNextFile();
      }
      d.close();
    }
    if (!name) return false;
  }
  snprintf(basePath, sizeof(basePath), "/characters/%s", name);
  char mpath[64];
  snprintf(mpath, sizeof(mpath), "%s/manifest.json", basePath);
  fs::File mf = LittleFS.open(mpath, "r");
  if (!mf) return false;
  JsonDocument doc;
  deserializeJson(doc, mf);
  mf.close();
  JsonObject colors = doc["colors"];
  pal.body    = parseHexColor(colors["body"],    pal.body);
  pal.bg      = parseHexColor(colors["bg"],      pal.bg);
  pal.text    = parseHexColor(colors["text"],    pal.text);
  pal.textDim = parseHexColor(colors["textDim"], pal.textDim);
  pal.ink     = parseHexColor(colors["ink"],     pal.ink);
  const char* mode = doc["mode"];
  textMode = (mode && strcmp(mode, "text") == 0);
  JsonObject states = doc["states"];
  if (textMode) {
    for (uint8_t i = 0; i < N_STATES; i++) {
      TextState& ts = textStates[i];
      ts.nFrames = 0; ts.delayMs = 200;
      JsonObject st = states[STATE_NAMES[i]];
      if (st.isNull()) continue;
      ts.delayMs = st["delay"] | 200;
      JsonArray fr = st["frames"];
      for (JsonVariant v : fr) {
        if (ts.nFrames >= 8) break;
        strncpy(ts.frames[ts.nFrames++], v.as<const char*>() ? v.as<const char*>() : "", 19);
      }
    }
    loaded = true; return true;
  }
  gifTotal = 0;
  for (uint8_t i = 0; i < N_STATES; i++) {
    stateStart[i] = gifTotal; stateCount[i] = 0; stateRot[i] = 0;
    JsonVariant v = states[STATE_NAMES[i]];
    if (v.is<JsonArray>()) {
      for (JsonVariant e : v.as<JsonArray>()) {
        if (gifTotal >= MAX_GIFS) break;
        snprintf(gifPaths[gifTotal++], 32, "%s", e.as<const char*>() ? e.as<const char*>() : "");
        stateCount[i]++;
      }
    } else {
      const char* fn = v.as<const char*>();
      if (fn) { snprintf(gifPaths[gifTotal++], 32, "%s", fn); stateCount[i] = 1; }
    }
  }
  gif.begin(LITTLE_ENDIAN_PIXELS);
  loaded = true; return true;
}

bool characterLoaded() { return loaded; }
const Palette& characterPalette() { return pal; }

bool characterRenderTo(TFT_eSPI* tgt, int cx, int cy) {
  if (!loaded) return false;
  if (textMode) {
    TextState& ts = textStates[curState]; if (ts.nFrames == 0) return false;
    tgt->fillScreen(pal.bg);
    int tw = strlen(ts.frames[textFrame]) * 12;
    tgt->setTextColor(pal.body, pal.bg); tgt->setTextSize(2);
    tgt->setCursor(cx - tw / 2, cy - 8);
    tgt->print(ts.frames[textFrame]);
    return true;
  }
  
  uint32_t now = millis();
  if (!gifOpen) {
    // If not open, it might be in a pause.
    if (animPauseUntil && now < animPauseUntil) return false;
    // Try to restart if we were expecting to be open
    characterSetState(curState); 
    if (!gifOpen) return false;
  }
  if (now < nextFrameAt) return false;

  tgt->fillScreen(pal.bg);
  TFT_eSPI* prevT = _tgt; bool prevP = peekMode; int px = gifX, py = gifY;
  _tgt = tgt; peekMode = false;
  gifX = cx - gifW / 2;
  gifY = cy - gifH / 2;
  
  int delayMs = 0;
  if (!gif.playFrame(false, &delayMs)) { 
    gif.reset(); 
    gif.playFrame(false, &delayMs); 
  }
  nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
  
  _tgt = prevT; peekMode = prevP; gifX = px; gifY = py;
  return true;
}

void characterSetPeek(bool peek) { if (peekMode != peek) { peekMode = peek; characterInvalidate(); } }
static uint8_t* gifBuffer = nullptr;
static int      gifBufferSize = 0;

void characterClose() { 
  if (gifOpen) { gif.close(); gifOpen = false; } 
  if (gifBuffer) { free(gifBuffer); gifBuffer = nullptr; gifBufferSize = 0; }
  loaded = false; textMode = false; curState = 0xFF; 
}

void characterInvalidate() {
  if (!loaded) return;
  if (textMode) { spr.fillSprite(pal.bg); uint8_t s = curState; curState = 0xFF; characterSetState(s); return; }
  if (gifOpen) { gif.close(); gifOpen = false; }
  animPauseUntil = 0; uint8_t s = curState; curState = 0xFF; characterSetState(s);
}

void characterSetState(uint8_t s) {
  if (!loaded || s >= N_STATES || s == curState) return;
  if (textMode) { curState = s; textFrame = 0; textNext = 0; spr.fillSprite(pal.bg); return; }
  
  // Prepare for state change
  bool wasOpen = gifOpen;
  if (gifOpen) { gif.close(); gifOpen = false; }
  
  // Free old buffer
  if (gifBuffer) { free(gifBuffer); gifBuffer = nullptr; gifBufferSize = 0; }
  
  animPauseUntil = 0; curState = s;
  if (stateCount[s] == 0) return;
  uint8_t idx = stateStart[s] + stateRot[s];
  char full[80]; snprintf(full, sizeof(full), "%s/%s", basePath, gifPaths[idx]);
  
  // Read entire file into RAM
  fs::File f = LittleFS.open(full, "r");
  if (f) {
    gifBufferSize = f.size();
    gifBuffer = (uint8_t*)malloc(gifBufferSize);
    if (gifBuffer) {
      f.read(gifBuffer, gifBufferSize);
      f.close();
      if (gif.open(gifBuffer, gifBufferSize, gifDrawCb)) {
        gifOpen = true; gifW = gif.getCanvasWidth(); gifH = gif.getCanvasHeight(); gifPlace();
        
        // Immediately draw the first frame to prevent flickering during transition
        spr.fillSprite(pal.bg);
        int delayMs = 0;
        gif.playFrame(false, &delayMs);
        
        nextFrameAt = millis() + (delayMs > 0 ? delayMs : 100);
        variantStartedMs = millis();
      }
    } else {
      f.close();
    }
  }
}

void characterTick() {
  if (!loaded) return;
  if (textMode) {
    TextState& ts = textStates[curState]; if (ts.nFrames == 0) return;
    uint32_t now = millis(); if (now < textNext) return;
    textNext = now + ts.delayMs;
    int cy = peekMode ? 35 : 60;
    spr.fillRect(0, cy - 14, spr.width(), 28, pal.bg);
    int tw = strlen(ts.frames[textFrame]) * 12;
    spr.setTextColor(pal.body, pal.bg); spr.setTextSize(2);
    spr.setCursor((spr.width() - tw) / 2, cy - 8);
    spr.print(ts.frames[textFrame]);
    textFrame = (textFrame + 1) % ts.nFrames; return;
  }
  uint32_t now = millis();
  if (!gifOpen) {
    if (animPauseUntil && now >= animPauseUntil) {
      animPauseUntil = 0; uint8_t s = curState; curState = 0xFF; characterSetState(s);
    }
    return;
  }
  if (now < nextFrameAt) return;
  int delayMs = 0;
  if (!gif.playFrame(false, &delayMs)) {
    if (stateCount[curState] == 1) { 
      // Single GIF variant: pause then restart
      gif.close(); gifOpen = false;
      animPauseUntil = now + ANIM_PAUSE_MS; 
      return; 
    }
    if (now - variantStartedMs < VARIANT_DWELL_MS) { gif.reset(); nextFrameAt = now; return; }
    gif.close(); gifOpen = false;
    stateRot[curState] = (stateRot[curState] + 1) % stateCount[curState];
    animPauseUntil = now + ANIM_PAUSE_MS; return;
  }
  nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
}
