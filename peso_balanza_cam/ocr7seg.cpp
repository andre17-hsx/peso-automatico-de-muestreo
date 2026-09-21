// ===========================================================================
//  ocr7seg.cpp  -  Lee la pantalla PESO con la camara OV2640
//
//  El bucle de lectura corre en su PROPIA tarea (nucleo 0), separada del
//  servidor web (nucleo 1).  Aunque el navegador vaya lento pintando el
//  video, la lectura sigue a ~5 Hz.
//
//  Metodo: frame en ESCALA DE GRISES -> se recorta la zona de los digitos
//  (config en /config) -> por cada digito se mide el brillo de 7 rectangulos
//  (segmentos a..g); si supera el umbral dinamico esta encendido -> se
//  decodifica el digito.
//
//  CAPTURA: la lectura en vivo se pasa a weighFeed() (weighlog.cpp), que lleva
//  la maquina de estados del pesaje y el historial (comun con el sniffer).
//
//  Todo este fichero solo se compila si  ENABLE_OCR == 1  (ver config.h).
// ===========================================================================
#include "config.h"

#if ENABLE_OCR
// ===========================================================================

#include <Arduino.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "seg7.h"
#include "ocr7seg.h"
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#if ENABLE_SNIFFER && OCR_PAUSE_SNIFFER
  #include "sniffer_tm1640.h"
#endif

// ----------------------- pines camara (de config.h) ------------------------
OcrParams ocr;

static uint8_t*  g_gray    = nullptr;   // ultimo frame (PSRAM)
static uint8_t*  g_scratch = nullptr;   // copia para dibujar el debug
static int       g_w = 0, g_h = 0;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static OcrResult    g_last = {};

// El historial de pesajes y la maquina de estados viven ahora en weighlog.cpp
// (comun a OCR y sniffer).  Aqui solo se ALIMENTA con weighFeed() -> captureStep.

// rectangulos de cada segmento en coords relativas [0..1] dentro de la celda
//                 x0     y0     x1     y1
static const float SEG_BOX[7][4] = {
  { 0.15f, 0.00f, 0.85f, 0.15f },   // a  (arriba)
  { 0.80f, 0.04f, 1.00f, 0.49f },   // b  (arriba-derecha)
  { 0.80f, 0.51f, 1.00f, 0.96f },   // c  (abajo-derecha)
  { 0.15f, 0.85f, 0.85f, 1.00f },   // d  (abajo)
  { 0.00f, 0.51f, 0.20f, 0.96f },   // e  (abajo-izquierda)
  { 0.00f, 0.04f, 0.20f, 0.49f },   // f  (arriba-izquierda)
  { 0.15f, 0.43f, 0.85f, 0.57f },   // g  (centro)
};

// ---------------------------------------------------------------------------
static inline uint8_t px(int x, int y) {
  if (x < 0) x = 0; else if (x >= g_w) x = g_w - 1;
  if (y < 0) y = 0; else if (y >= g_h) y = g_h - 1;
  return g_gray[y * g_w + x];
}

static void applyCam() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  s->set_whitebal(s, 0);
  s->set_awb_gain(s, 0);
  s->set_exposure_ctrl(s, 0);        // AEC off  -> exposicion fija
  s->set_aec2(s, 0);
  s->set_aec_value(s, ocr.aecValue); // 0..1200
  s->set_gain_ctrl(s, 0);            // AGC off
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, GAINCEILING_2X);
  s->set_brightness(s, 0);
  s->set_contrast(s, ocr.contrast);  // -2..2
  s->set_lenc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_hmirror(s, 0);
  s->set_vflip(s, 0);
}

void ocrApplyCam() { applyCam(); }

// ---------------------------------------------------------------------------
void ocrLoadParams() {
  ocr.autoMode    = OCR_AUTO;
  ocr.digitAspect = OCR_DIGIT_ASPECT;
  ocr.digitPitch  = OCR_DIGIT_PITCH;
  ocr.autoBright  = OCR_AUTO_BRIGHT;
  ocr.boxX = OCR_BOX_X; ocr.boxY = OCR_BOX_Y;
  ocr.boxW = OCR_BOX_W; ocr.boxH = OCR_BOX_H;
  ocr.digitGap = OCR_DIGIT_GAP;
  ocr.slant = OCR_SLANT;
  ocr.onRatio = OCR_ON_RATIO;
  ocr.minContrast = OCR_MIN_CONTRAST;
  ocr.numDigits = OCR_NUM_DIGITS;
  ocr.decimals = OCR_DECIMALS;
  ocr.aecValue = OCR_AEC_VALUE;
  ocr.contrast = OCR_CONTRAST;
  ocr.flashLed = false;

  Preferences p;
  p.begin("ocr", true);
  ocr.autoMode    = p.getInt  ("auto", ocr.autoMode ? 1 : 0) != 0;
  ocr.digitAspect = p.getFloat("asp",  ocr.digitAspect);
  ocr.digitPitch  = p.getFloat("pit",  ocr.digitPitch);
  ocr.autoBright  = p.getInt  ("abr",  ocr.autoBright);
  ocr.boxX        = p.getInt  ("boxX", ocr.boxX);
  ocr.boxY        = p.getInt  ("boxY", ocr.boxY);
  ocr.boxW        = p.getInt  ("boxW", ocr.boxW);
  ocr.boxH        = p.getInt  ("boxH", ocr.boxH);
  ocr.digitGap    = p.getInt  ("gap",  ocr.digitGap);
  ocr.slant       = p.getFloat("slant", ocr.slant);
  ocr.onRatio     = p.getFloat("onR",  ocr.onRatio);
  ocr.minContrast = p.getInt  ("minC", ocr.minContrast);
  ocr.numDigits   = p.getInt  ("nd",   ocr.numDigits);
  ocr.decimals    = p.getInt  ("dec",  ocr.decimals);
  ocr.aecValue    = p.getInt  ("aec",  ocr.aecValue);
  ocr.contrast    = p.getInt  ("cont", ocr.contrast);
  ocr.flashLed    = p.getInt  ("flash", ocr.flashLed ? 1 : 0) != 0;
  p.end();

  if (ocr.numDigits < 1) ocr.numDigits = 1;
  if (ocr.numDigits > 8) ocr.numDigits = 8;
  if (ocr.decimals < 0)  ocr.decimals = 0;
  if (ocr.decimals > 4)  ocr.decimals = 4;
  if (ocr.digitAspect < 0.2f) ocr.digitAspect = 0.2f;
  if (ocr.digitAspect > 1.2f) ocr.digitAspect = 1.2f;
  if (ocr.digitPitch  < 1.0f) ocr.digitPitch  = 1.0f;
  if (ocr.digitPitch  > 3.0f) ocr.digitPitch  = 3.0f;
}

void ocrSaveParams() {
  Preferences p;
  p.begin("ocr", false);
  p.putInt  ("auto", ocr.autoMode ? 1 : 0);
  p.putFloat("asp",  ocr.digitAspect);
  p.putFloat("pit",  ocr.digitPitch);
  p.putInt  ("abr",  ocr.autoBright);
  p.putInt  ("boxX", ocr.boxX);   p.putInt  ("boxY", ocr.boxY);
  p.putInt  ("boxW", ocr.boxW);   p.putInt  ("boxH", ocr.boxH);
  p.putInt  ("gap",  ocr.digitGap);
  p.putFloat("slant", ocr.slant);
  p.putFloat("onR",  ocr.onRatio);
  p.putInt  ("minC", ocr.minContrast);
  p.putInt  ("nd",   ocr.numDigits);
  p.putInt  ("dec",  ocr.decimals);
  p.putInt  ("aec",  ocr.aecValue);
  p.putInt  ("cont", ocr.contrast);
  p.putInt  ("flash", ocr.flashLed ? 1 : 0);
  p.end();
  applyCam();
}

// ---------------------------------------------------------------------------
bool ocrBegin() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = CAM_PIN_D0;  c.pin_d1 = CAM_PIN_D1;
  c.pin_d2 = CAM_PIN_D2;  c.pin_d3 = CAM_PIN_D3;
  c.pin_d4 = CAM_PIN_D4;  c.pin_d5 = CAM_PIN_D5;
  c.pin_d6 = CAM_PIN_D6;  c.pin_d7 = CAM_PIN_D7;
  c.pin_xclk = CAM_PIN_XCLK;   c.pin_pclk = CAM_PIN_PCLK;
  c.pin_vsync = CAM_PIN_VSYNC; c.pin_href = CAM_PIN_HREF;
  // Core ESP32 3.x -> pin_sccb_sda / pin_sccb_scl.
  // Si compilas con el core 2.x y da error, cambia "sccb" por "sscb" aqui:
  c.pin_sccb_sda = CAM_PIN_SIOD; c.pin_sccb_scl = CAM_PIN_SIOC;
  c.pin_pwdn = CAM_PIN_PWDN;   c.pin_reset = CAM_PIN_RESET;
  c.xclk_freq_hz  = 20000000;
  c.frame_size    = OCR_FRAMESIZE;
  c.pixel_format  = PIXFORMAT_GRAYSCALE;
  c.jpeg_quality  = 12;
  c.fb_location   = CAMERA_FB_IN_PSRAM;
  c.fb_count      = 2;
  c.grab_mode     = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[ocr] esp_camera_init fallo 0x%x  -> revisa CAM_PIN_* en config.h\n", err);
    return false;
  }
  applyCam();

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[ocr] no llega frame"); return false; }
  g_w = fb->width; g_h = fb->height;
  esp_camera_fb_return(fb);

  g_gray    = (uint8_t*) ps_malloc((size_t)g_w * g_h);
  g_scratch = (uint8_t*) ps_malloc((size_t)g_w * g_h);
  if (!g_gray || !g_scratch) {
    Serial.println("[ocr] sin PSRAM para los buffers (Tools -> PSRAM: OPI PSRAM)");
    return false;
  }
#if OCR_FLASH_PIN >= 0
  pinMode(OCR_FLASH_PIN, OUTPUT); digitalWrite(OCR_FLASH_PIN, LOW);
#endif
  Serial.printf("[ocr] camara %dx%d gris OK\n", g_w, g_h);
  return true;
}

// mide el brillo medio (0..255) de un segmento dentro de la celda dada
static uint8_t segMean(float cx, float cy, float cw, float ch, int seg) {
  float rx0 = SEG_BOX[seg][0], ry0 = SEG_BOX[seg][1];
  float rx1 = SEG_BOX[seg][2], ry1 = SEG_BOX[seg][3];
  float ix = (rx1 - rx0) * 0.15f, iy = (ry1 - ry0) * 0.15f;
  rx0 += ix; rx1 -= ix; ry0 += iy; ry1 -= iy;

  long sum = 0; int n = 0;
  const int STEPS = 5;
  for (int j = 0; j <= STEPS; j++) {
    float ry = ry0 + (ry1 - ry0) * j / STEPS;
    float ay = cy + ry * ch;
    for (int i = 0; i <= STEPS; i++) {
      float rx = rx0 + (rx1 - rx0) * i / STEPS;
      float ax = cx + rx * cw + ocr.slant * (0.5f - ry) * cw;
      sum += px((int)ax, (int)ay);
      n++;
    }
  }
  return n ? (uint8_t)(sum / n) : 0;
}

// segmento "encendido" por COBERTURA: % de puntos del trazo por encima de 'thr'.
// Muestrea DENSO a lo largo del eje del trazo (los verticales por su altura,
// los horizontales por su anchura) para no dejarse enganar por el sangrado.
static bool segOn(float cx, float cy, float cw, float ch, int seg, int thr) {
  float rx0 = SEG_BOX[seg][0], ry0 = SEG_BOX[seg][1];
  float rx1 = SEG_BOX[seg][2], ry1 = SEG_BOX[seg][3];
  float ix = (rx1 - rx0) * 0.20f, iy = (ry1 - ry0) * 0.20f;   // recorta las puntas
  rx0 += ix; rx1 -= ix; ry0 += iy; ry1 -= iy;

  bool vertical = (ry1 - ry0) > (rx1 - rx0);   // b,c,e,f  vs  a,d,g
  int  along = vertical ? 9 : 7;               // muestras a lo largo del trazo
  int  cross = 2;                              // a lo ancho
  int hit = 0, tot = 0;
  for (int a = 0; a <= along; a++) {
    float t = (float)a / along;
    for (int b = 0; b <= cross; b++) {
      float u = (cross ? (float)b / cross : 0.5f);
      float rx = vertical ? (rx0 + (rx1 - rx0) * u) : (rx0 + (rx1 - rx0) * t);
      float ry = vertical ? (ry0 + (ry1 - ry0) * t) : (ry0 + (ry1 - ry0) * u);
      float ay = cy + ry * ch;
      float ax = cx + rx * cw + ocr.slant * (0.5f - ry) * cw;
      if (px((int)ax, (int)ay) > thr) hit++;
      tot++;
    }
  }
  return tot && (hit * 100 >= tot * OCR_SEG_COVERAGE);
}

// --- rejilla efectiva de celdas (la escribe la tarea OCR) ----------------
static int  g_cellX[8];
static int  g_cellY = 0, g_cellW = 0, g_cellH = 0;
static volatile int g_cellN = 0;
static volatile bool g_cellAuto = false;
static int  g_rawX0 = 0, g_rawY0 = 0, g_rawX1 = 0, g_rawY1 = 0;   // deteccion cruda
static int  g_autoThr = 0;

// ===========================================================================
//  RECONOCIMIENTO POR PLANTILLA
//  Compara la forma completa de cada digito (deslantada y re-escalada a
//  OCR_TPL_W x OCR_TPL_H) contra una plantilla aprendida de cada cifra 0-9,
//  por correlacion normalizada (NCC).  Robusto a desalineacion y desenfoque.
// ===========================================================================
#define TPL_N (OCR_TPL_W * OCR_TPL_H)
static uint8_t  g_tpl[10][TPL_N];        // plantillas crudas (para guardar en NVS)
static int16_t  g_tplZ[10][TPL_N];       // plantillas con media cero (para la NCC)
static float    g_tplNorm[10];           // norma de cada plantilla
static uint16_t g_tplMask = 0;           // bit d = plantilla d aprendida

static uint8_t  g_crop[TPL_N];
static int16_t  g_cropZ[TPL_N];

// media cero + norma
static float prepZ(const uint8_t* buf, int16_t* z) {
  long s = 0;
  for (int i = 0; i < TPL_N; i++) s += buf[i];
  int m = (int)(s / TPL_N);
  double ss = 0;
  for (int i = 0; i < TPL_N; i++) { int d = (int)buf[i] - m; z[i] = (int16_t)d; ss += (double)d * d; }
  return sqrtf((float)ss);
}

// recorta la celda 'slot' (desplazada dx,dy), deslantada, re-escalada a TPL
static void sampleCell(int slot, int dx, int dy, uint8_t* out) {
  float cx = (float)(g_cellX[slot] + dx), cy = (float)(g_cellY + dy);
  float cw = (float)g_cellW, ch = (float)g_cellH;
  for (int oy = 0; oy < OCR_TPL_H; oy++) {
    float ry = (oy + 0.5f) / OCR_TPL_H;
    float ay = cy + ry * ch;
    for (int ox = 0; ox < OCR_TPL_W; ox++) {
      float rx = (ox + 0.5f) / OCR_TPL_W;
      float ax = cx + rx * cw + ocr.slant * (0.5f - ry) * cw;
      out[oy * OCR_TPL_W + ox] = px((int)ax, (int)ay);
    }
  }
}

static float nccRaw(const int16_t* z, float znorm, int d) {
  if (znorm < 1.0f || g_tplNorm[d] < 1.0f) return 0;
  const int16_t* t = g_tplZ[d];
  long dot = 0;
  for (int i = 0; i < TPL_N; i++) dot += (long)z[i] * t[i];
  return (float)dot / (znorm * g_tplNorm[d]);
}

// decodifica todas las celdas por plantilla.  out[slot] = cifra 0-9 o -1 (dudoso)
static void matchCells(int ncell, int* out) {
  static const int SH[3] = { -2, 0, 2 };
  int nlearned = __builtin_popcount((unsigned)g_tplMask);
  // mientras el juego de plantillas no este casi completo, solo se acepta un
  // match MUY claro (si no, una cifra sin plantilla se pegaria a la mas parecida)
  float minNcc = (nlearned >= 8) ? OCR_TPL_MIN_NCC : 0.80f;

  // fase 1: mejor desplazamiento global (el display se mueve entero)
  int bsx = 1, bsy = 1; float bestTot = -1e9f;
  for (int syi = 0; syi < 3; syi++)
    for (int sxi = 0; sxi < 3; sxi++) {
      float tot = 0;
      for (int c = 0; c < ncell && c < 8; c++) {
        sampleCell(c, SH[sxi], SH[syi], g_crop);
        float nrm = prepZ(g_crop, g_cropZ);
        float best = -2;
        for (int d = 0; d < 10; d++)
          if (g_tplMask & (1 << d)) { float v = nccRaw(g_cropZ, nrm, d); if (v > best) best = v; }
        tot += best;
      }
      if (tot > bestTot) { bestTot = tot; bsx = sxi; bsy = syi; }
    }

  // fase 2: decodificar con ese desplazamiento
  for (int c = 0; c < ncell && c < 8; c++) {
    sampleCell(c, SH[bsx], SH[bsy], g_crop);
    float nrm = prepZ(g_crop, g_cropZ);
    float b1 = -2, b2 = -2; int bd = -1;
    for (int d = 0; d < 10; d++) {
      if (!(g_tplMask & (1 << d))) continue;
      float v = nccRaw(g_cropZ, nrm, d);
      if (v > b1) { b2 = b1; b1 = v; bd = d; }
      else if (v > b2) b2 = v;
    }
    out[c] = (bd >= 0 && b1 >= minNcc && (b1 - b2) >= OCR_TPL_MARGIN) ? bd : -1;
  }
}

int ocrTemplatesMask() { return g_tplMask; }

static void templatesSave() {
  Preferences p; p.begin("ocrtpl", false);
  p.putBytes("data", g_tpl, sizeof(g_tpl));
  p.putUShort("mask", g_tplMask);
  p.end();
}

static void templatesLoad() {
  Preferences p; p.begin("ocrtpl", true);
  g_tplMask = p.getUShort("mask", 0);
  if (g_tplMask) p.getBytes("data", g_tpl, sizeof(g_tpl));
  p.end();
  for (int d = 0; d < 10; d++)
    if (g_tplMask & (1 << d)) g_tplNorm[d] = prepZ(g_tpl[d], g_tplZ[d]);
}

void ocrTemplatesClear() {
  g_tplMask = 0;
  Preferences p; p.begin("ocrtpl", false); p.clear(); p.end();
}

// --- peticion de "aprender" (viene del servidor web, se ejecuta en la tarea) --
static volatile char g_learnReq[12] = "";
static volatile int  g_learnState  = 0;   // 0 idle, 1 pendiente, 2 hecho
static volatile int  g_learnResult = 0;

int ocrLearn(const char* shown) {
  char d[12]; int n = 0;
  for (const char* p = shown; *p && n < 10; p++) if (*p >= '0' && *p <= '9') d[n++] = *p;
  d[n] = 0;
  if (n == 0) return -3;
  strncpy((char*)g_learnReq, d, sizeof(g_learnReq) - 1);
  ((char*)g_learnReq)[sizeof(g_learnReq) - 1] = 0;
  g_learnResult = 0;
  g_learnState = 1;
  uint32_t t0 = millis();
  while (g_learnState == 1 && millis() - t0 < 2500) delay(20);
  return (g_learnState == 2) ? g_learnResult : -4;   // -4 = timeout
}

// se llama desde la tarea OCR justo despues de un ocrRead con exito
static void learnIfPending() {
  if (g_learnState != 1) return;
  const char* d = (const char*)g_learnReq;
  int nd = (int)strlen(d);
  int nc = g_cellN;
  if (nc <= 0 || g_cellW < 4)      { g_learnResult = -2; g_learnState = 2; return; }
  if (nd > nc)                     { g_learnResult = -1; g_learnState = 2; return; }
  for (int i = 0; i < nd; i++) {
    int slot  = nc - nd + i;                 // alinea por la derecha
    int digit = d[i] - '0';
    sampleCell(slot, 0, 0, g_tpl[digit]);
    g_tplNorm[digit] = prepZ(g_tpl[digit], g_tplZ[digit]);
    g_tplMask |= (1 << digit);
  }
  templatesSave();
  Serial.printf("[plantilla] aprendidas %d cifras, mask=0x%03X\n", nd, g_tplMask);
  g_learnResult = nd;
  g_learnState = 2;
}

// pixel "de LED": brillante Y con negro en LADOS OPUESTOS.
//  Un trazo de 7-seg es fino -> perpendicular al trazo hay negro a ambos lados.
//  El bisel blanco junto a un texto o a un borde solo tiene negro en UN lado,
//  asi que se descarta aunque este igual de brillante (util con luz encendida).
static inline bool segIsLed(int x, int y, int Tb, int Td) {
  if (g_gray[y * g_w + x] < Tb) return false;
  const int D = OCR_DARK_DIST;
  int xl = x - D < 0 ? 0 : x - D;
  int xr = x + D >= g_w ? g_w - 1 : x + D;
  int yu = y - D < 0 ? 0 : y - D;
  int yd = y + D >= g_h ? g_h - 1 : y + D;
  bool dl = g_gray[y * g_w + xl] < Td, dr = g_gray[y * g_w + xr] < Td;
  bool du = g_gray[yu * g_w + x]  < Td, dd = g_gray[yd * g_w + x]  < Td;
  return (dl && dr) || (du && dd);
}

// Localiza la BANDA vertical de los digitos y el BORDE DERECHO del ultimo LED.
// Se re-ejecuta en CADA frame -> si la camara se mueve unos cm, la rejilla
// vuelve a engancharse sola en ~1 s.
static bool autoLocate(int& ox0, int& oy0, int& ox1, int& oy1) {
  const int S = 2;
  if (g_h > 800) return false;

  int gmax = 0, gmin = 255;
  for (int y = 0; y < g_h; y += 6)
    for (int x = 0; x < g_w; x += 6) {
      uint8_t v = g_gray[y * g_w + x];
      if (v > gmax) gmax = v;
      if (v < gmin) gmin = v;
    }
  if (gmax - gmin < ocr.minContrast) return false;
  // Tb = un pixel "encendido".  0 (auto) = casi saturado; con luz encendida el
  // bisel blanco tambien satura, asi que si molesta se fija a mano (abr ~245).
  int Tb = ocr.autoBright > 0 ? ocr.autoBright : (gmax - 25);
  if (Tb < 130) Tb = 130;
  // Td = fondo oscuro de la pantalla (mas alto con luz encendida)
  int Td = gmin + (gmax - gmin) / 4;
  if (Td < 60)  Td = 60;
  if (Td > 130) Td = 130;
  g_autoThr = Tb;

  // banda vertical: histograma por fila de pixeles de LED
  static uint16_t rc[800];
  int rMax = 0, yPk = 0;
  for (int y = 0; y < g_h; y += S) {
    int c = 0;
    for (int x = 0; x < g_w; x += S) if (segIsLed(x, y, Tb, Td)) c++;
    rc[y] = c;
    if (c > rMax) { rMax = c; yPk = y; }
  }
  if (rMax < 4) return false;
  int rThr = rMax / 6; if (rThr < 2) rThr = 2;
  int y0 = yPk, y1 = yPk, miss = 0;
  for (int y = yPk; y >= 0;  y -= S) { if (rc[y] >= rThr) { y0 = y; miss = 0; } else if (++miss > 6) break; }
  miss = 0;
  for (int y = yPk; y < g_h; y += S) { if (rc[y] >= rThr) { y1 = y; miss = 0; } else if (++miss > 6) break; }
  if (y1 - y0 < 14) return false;

  // extension horizontal: la tira mas ancha de columnas con LED (tolera huecos)
  const int MAXGAP = (y1 - y0) / 2;
  int bx0 = -1, bx1 = -1, cx0 = -1, cx1 = -1, g = 0;
  for (int x = 0; x < g_w; x += S) {
    int c = 0;
    for (int y = y0; y <= y1; y += S) if (segIsLed(x, y, Tb, Td)) { c++; if (c >= 2) break; }
    if (c >= 2) { if (cx0 < 0) cx0 = x; cx1 = x; g = 0; }
    else if (cx0 >= 0) { g += S; if (g > MAXGAP) { if (bx0 < 0 || cx1 - cx0 > bx1 - bx0) { bx0 = cx0; bx1 = cx1; } cx0 = -1; } }
  }
  if (cx0 >= 0 && (bx0 < 0 || cx1 - cx0 > bx1 - bx0)) { bx0 = cx0; bx1 = cx1; }
  if (bx0 < 0 || bx1 - bx0 < 8) return false;

  ox0 = bx0; oy0 = y0; ox1 = bx1; oy1 = y1;
  return true;
}

bool ocrRead(OcrResult& out) {
  if (!g_gray) return false;

#if OCR_FLASH_PIN >= 0
  if (ocr.flashLed) { digitalWrite(OCR_FLASH_PIN, HIGH); delay(80); }
#endif
  camera_fb_t* fb = esp_camera_fb_get();
#if OCR_FLASH_PIN >= 0
  if (ocr.flashLed) digitalWrite(OCR_FLASH_PIN, LOW);
#endif
  if (!fb) return false;
  if ((int)fb->width != g_w || (int)fb->height != g_h ||
      fb->format != PIXFORMAT_GRAYSCALE) {
    esp_camera_fb_return(fb);
    return false;
  }
  memcpy(g_gray, fb->buf, (size_t)g_w * g_h);
  esp_camera_fb_return(fb);

  OcrResult r = {};
  r.frameMs = millis();

  // ---- 1. rejilla de celdas ----
  int nd = ocr.numDigits; if (nd > 8) nd = 8;
  int ncell = 0;

  if (ocr.autoMode) {
    static float    sR = 0, sY0 = 0, sY1 = 0;    // rejilla suavizada
    static uint32_t lastLocMs = 0;
    int ax0, ay0, ax1, ay1;
    bool loc = autoLocate(ax0, ay0, ax1, ay1);
    if (loc) {
      lastLocMs = millis();
      g_rawX0 = ax0; g_rawY0 = ay0; g_rawX1 = ax1; g_rawY1 = ay1;
      if (sR == 0) { sR = ax1; sY0 = ay0; sY1 = ay1; }   // primer enganche
      else {
        // sigue el movimiento de la camara; filtra la vibracion.  Si el salto
        // es grande (relativo al tamano del display) reengancha de golpe.
        float snap = (sY1 - sY0) * 0.20f; if (snap < 8) snap = 8;
        float kR = fabsf(ax1 - sR) > snap ? 1.0f : 0.35f;
        float kY = (fabsf((float)ay0 - sY0) > snap ||
                    fabsf((float)ay1 - sY1) > snap) ? 1.0f : 0.30f;
        sR  = sR  * (1 - kR) + ax1 * kR;
        sY0 = sY0 * (1 - kY) + ay0 * kY;
        sY1 = sY1 * (1 - kY) + ay1 * kY;
      }
    } else if (sR != 0 && millis() - lastLocMs > 3000) {
      sR = 0; g_rawX0 = g_rawX1 = 0;            // enganche perdido hace rato
    }

    if (sR != 0) {                // rejilla vigente (recien vista o de <3 s)
      float cyf  = sY0;
      float chf  = sY1 - sY0;
      float digW = chf * ocr.digitAspect;
      float pit  = digW * ocr.digitPitch;
      float rEdge = sR + digW * 0.08f;
      for (int slot = 0; slot < nd; slot++) {
        int d = nd - 1 - slot;                 // d=0 = digito de unidades (derecha)
        g_cellX[slot] = (int)(rEdge - digW - d * pit);
      }
      g_cellY = (int)cyf; g_cellW = (int)digW; g_cellH = (int)chf;
      ncell = nd;
    } else {
      g_rawX0 = g_rawX1 = 0;
    }
    g_cellAuto = true;
  } else {
    float dw = (ocr.boxW - (nd - 1) * ocr.digitGap) / (float)nd;
    for (int i = 0; i < nd; i++)
      g_cellX[i] = ocr.boxX + (int)(i * (dw + ocr.digitGap));
    g_cellY = ocr.boxY; g_cellW = (int)dw; g_cellH = ocr.boxH;
    g_cellAuto = false;
    ncell = nd;
  }
  g_cellN = ncell;
  r.ncell = (uint8_t)ncell;
  r.autoThr = g_autoThr;

  bool displayOff = (ncell <= 0) || g_cellW < 4 || g_cellH < 6;

  // ---- 2. umbral de segmento a partir del rango de brillo en las celdas ----
  int bmin = 255, bmax = 0;
  if (!displayOff) {
    int x0 = g_cellX[0], x1 = g_cellX[ncell - 1] + g_cellW;
    if (x0 < 0) x0 = 0;
    if (x1 > g_w) x1 = g_w;
    int yA = g_cellY < 0 ? 0 : g_cellY;
    int yB = g_cellY + g_cellH > g_h ? g_h : g_cellY + g_cellH;
    for (int y = yA; y < yB; y += 2)
      for (int x = x0; x < x1; x += 2) {
        uint8_t v = px(x, y);
        if (v < bmin) bmin = v;
        if (v > bmax) bmax = v;
      }
    if (bmax - bmin < ocr.minContrast) displayOff = true;
  }
  r.bMin = bmin; r.bMax = bmax;
  int thr = bmin + (int)((bmax - bmin) * ocr.onRatio);
  r.thr = thr;

  // ---- 3. leer cada celda (izquierda -> derecha) ----
  //   Se prefiere la PLANTILLA (forma completa) cuando gana con confianza;
  //   si no hay plantilla o es dudosa, se usa el metodo de 7 segmentos.
  int tplD[8] = { 0 };
  bool haveTpl = OCR_USE_TEMPLATES && (g_tplMask != 0) && !displayOff;
  if (haveTpl) matchCells(ncell, tplD);

  char txt[16]; int ti = 0;
  bool anyDigit = false, anyUnknown = false;

  for (int slot = 0; slot < ncell && slot < 8; slot++) {
    int cx = g_cellX[slot];
    uint8_t mask = 0;
    for (int s = 0; s < 7; s++) {
      r.segBright[slot][s] = displayOff ? 0 : segMean(cx, g_cellY, g_cellW, g_cellH, s);
      if (!displayOff && segOn(cx, g_cellY, g_cellW, g_cellH, s, thr)) mask |= (1 << s);
    }
    r.segmask[slot] = mask;

    int dg;
    if (displayOff)                         dg = -2;
    else if (haveTpl && tplD[slot] >= 0)    dg = tplD[slot];          // plantilla
    else                                    dg = seg7decodeFuzzy(mask, 1);  // 7-seg
    r.digit[slot] = dg;

    if      (dg >= 0)  { txt[ti++] = '0' + dg; anyDigit = true; }
    else if (dg == -2) { txt[ti++] = ' '; }
    else if (dg == -3) { txt[ti++] = '-'; }
    else               { txt[ti++] = '?'; anyUnknown = true; }
  }
  txt[ti] = 0;
  strncpy(r.text, txt, sizeof(r.text) - 1);      // lectura CRUDA por digito
  r.text[sizeof(r.text) - 1] = 0;

  r.valid = anyDigit && !anyUnknown && !displayOff;
  if (r.valid) {
    long iv = 0; bool neg = false;
    for (int i = 0; txt[i]; i++) {
      if (txt[i] == '-') neg = true;
      else if (txt[i] >= '0' && txt[i] <= '9') iv = iv * 10 + (txt[i] - '0');
    }
    float v = iv;
    for (int i = 0; i < ocr.decimals; i++) v /= 10.0f;
    r.value = neg ? -v : v;
  } else {
    r.value = NAN;
  }

  portENTER_CRITICAL(&g_mux);
  g_last = r;
  portEXIT_CRITICAL(&g_mux);
  out = r;
  return true;
}

void ocrGetLast(OcrResult& out) {
  portENTER_CRITICAL(&g_mux);
  out = g_last;
  portEXIT_CRITICAL(&g_mux);
}

int ocrAutoThr() { return g_autoThr; }

// ===========================================================================
//  CAPTURA DE PESAJES
// ===========================================================================
//  La maquina de estados esta en weighlog.cpp (comun con el sniffer).  Aqui
//  solo se le pasa la lectura en vivo.  Si el sniffer esta activo, es EL quien
//  alimenta el pesaje (el bus es mas fiable que el OCR).
static void captureStep(const OcrResult& r) {
#if ENABLE_SNIFFER
  (void)r;
#else
  weighFeed(r.value, NAN, NAN, false, r.valid);   // camara: solo PESO, estable interno
#endif
}

// ------------------------------ tarea ------------------------------------
static void ocrTask(void*) {
  for (;;) {
    OcrResult r;
#if ENABLE_SNIFFER && OCR_PAUSE_SNIFFER
    snifSetEnabled(false);
#endif
    bool ok = ocrRead(r);
#if ENABLE_SNIFFER && OCR_PAUSE_SNIFFER
    snifSetEnabled(true);
#endif
    if (ok) { learnIfPending(); captureStep(r); }
    vTaskDelay(pdMS_TO_TICKS(OCR_PERIOD_MS));
  }
}

void ocrStartTask() {
  templatesLoad();
  Serial.printf("[ocr] plantillas cargadas: mask=0x%03X\n", g_tplMask);
  xTaskCreatePinnedToCore(ocrTask, "ocr", 6144, nullptr, 2, nullptr, 0);
}

// --------------------------- JPEG / debug -----------------------------------
static void hline(uint8_t* b, int x0, int x1, int y, uint8_t c) {
  if (y < 0 || y >= g_h) return;
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) if (x >= 0 && x < g_w) b[y * g_w + x] = c;
}
static void vline(uint8_t* b, int x, int y0, int y1, uint8_t c) {
  if (x < 0 || x >= g_w) return;
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; y++) if (y >= 0 && y < g_h) b[y * g_w + x] = c;
}
static void rectangle(uint8_t* b, int x, int y, int w, int h, uint8_t c) {
  hline(b, x, x + w, y, c); hline(b, x, x + w, y + h, c);
  vline(b, x, y, y + h, c); vline(b, x + w, y, y + h, c);
}
static void fillRect(uint8_t* b, int x, int y, int w, int h, uint8_t c) {
  for (int j = 0; j < h; j++) hline(b, x, x + w, y + j, c);
}

bool ocrJpegSnapshot(uint8_t** buf, size_t* len) {
  if (!g_gray) return false;
  return fmt2jpg(g_gray, (size_t)g_w * g_h, g_w, g_h,
                 PIXFORMAT_GRAYSCALE, 70, buf, len);
}

bool ocrJpegDebug(uint8_t** buf, size_t* len) {
  if (!g_gray || !g_scratch) return false;
  size_t sz = (size_t)g_w * g_h;
  memcpy(g_scratch, g_gray, sz);
  uint8_t* b = g_scratch;

  OcrResult snap; ocrGetLast(snap);
  int n   = g_cellN;
  int cw  = g_cellW, cy = g_cellY, chh = g_cellH;

  // deteccion cruda (lo que agrupo el localizador): rectangulo tenue
  if (g_cellAuto && g_rawX1 > g_rawX0)
    rectangle(b, g_rawX0, g_rawY0, g_rawX1 - g_rawX0, g_rawY1 - g_rawY0, 110);

  if (n > 0 && cw > 1) {
    for (int slot = 0; slot < n && slot < 8; slot++) {
      float cx = g_cellX[slot];
      rectangle(b, (int)cx, cy, cw, chh, 200);
      for (int s = 0; s < 7; s++) {
        float rx0 = SEG_BOX[s][0], ry0 = SEG_BOX[s][1];
        float rx1 = SEG_BOX[s][2], ry1 = SEG_BOX[s][3];
        float ix = (rx1 - rx0) * 0.15f, iy = (ry1 - ry0) * 0.15f;
        rx0 += ix; rx1 -= ix; ry0 += iy; ry1 -= iy;
        int x0 = (int)(cx + rx0 * cw + ocr.slant * (0.5f - ry0) * cw);
        int x1 = (int)(cx + rx1 * cw + ocr.slant * (0.5f - ry1) * cw);
        int y0 = (int)(cy + ry0 * chh);
        int y1 = (int)(cy + ry1 * chh);
        bool on = (snap.segmask[slot] >> s) & 1;
        if (on) fillRect(b, x0, y0, x1 - x0, y1 - y0, 255);
        else    rectangle(b, x0, y0, x1 - x0, y1 - y0, 90);
      }
    }
  } else {
    hline(b, g_w / 2 - 40, g_w / 2 + 40, g_h / 2, 255);   // no localizado
  }
  return fmt2jpg(b, sz, g_w, g_h, PIXFORMAT_GRAYSCALE, 60, buf, len);
}

// ===========================================================================
#else   // ENABLE_OCR == 0  ->  stubs para que el resto enlace
// ===========================================================================
#include "ocr7seg.h"
#include <math.h>

OcrParams ocr;
static OcrResult g_none = {};

bool  ocrBegin()                          { return false; }
void  ocrStartTask()                      {}
bool  ocrRead(OcrResult&)                 { return false; }
void  ocrGetLast(OcrResult& out)          { out = g_none; }
void  ocrLoadParams()                     {}
void  ocrSaveParams()                     {}
void  ocrApplyCam()                       {}
int   ocrAutoThr()                        { return 0; }
int   ocrLearn(const char*)               { return -9; }
void  ocrTemplatesClear()                 {}
int   ocrTemplatesMask()                  { return 0; }
bool  ocrJpegSnapshot(uint8_t**, size_t*) { return false; }
bool  ocrJpegDebug(uint8_t**, size_t*)    { return false; }

#endif  // ENABLE_OCR
