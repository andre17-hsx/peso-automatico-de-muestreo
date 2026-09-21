// ===========================================================================
//  sniffer_tm1640.cpp  -  Escucha PASIVA del bus DA/SL del display de la balanza
//
//  El chip del display (CS2540 == familia TM1640) recibe por 2 hilos:
//    START  = DA baja con SL alto        (tipo I2C, sin ACK)
//    bit    = valido en flanco de subida de SL,  LSB primero, 8 bits/byte
//    STOP   = DA sube con SL alto
//  Comandos:  0x40/0x44 = modo escritura (auto-incr / direccion fija)
//             0xC0|addr  = fija direccion 0..15 y a continuacion vienen datos
//             0x80..0x8F = display on/off + brillo
//
//  Este modulo NO controla nada: solo escucha, reconstruye los 16 bytes de
//  RAM del display y los traduce a PESO / PRECIO / TOTAL con el mapa de config.
//
//  Todo este fichero solo se compila si  ENABLE_SNIFFER == 1  (ver config.h).
// ===========================================================================
#include "config.h"

#if ENABLE_SNIFFER
// ===========================================================================

#include "seg7.h"
#include "sniffer_tm1640.h"
#include "soc/gpio_reg.h"
#include <Preferences.h>
#include <algorithm>
#include <string.h>

#define PIN_DA  SNIF_PIN_DA
#define PIN_SL  SNIF_PIN_SL

SnifParams snf;

static inline uint32_t IRAM_ATTR gpioIn() { return REG_READ(GPIO_IN_REG); }
#define DA_OF(io)  (((io) >> PIN_DA) & 1u)
#define SL_OF(io)  (((io) >> PIN_SL) & 1u)

// ------------------------- estado del decodificador ------------------------
static volatile uint8_t  dCur = 0, dBits = 0, dLen = 0;
static volatile bool     dIn  = false;
static volatile uint8_t  dBuf[80];
static volatile uint8_t  g_shadow[16];
static volatile uint32_t g_txCount  = 0;   // transacciones START..STOP
static volatile uint32_t g_stopSeq  = 0;   // ++ en cada STOP que escribe RAM
static volatile bool     g_enabled  = true;

// diagnostico de cableado: cuenta CUALQUIER flanco visto en cada pin, aunque
// el protocolo no decodifique nada.  Si estos no suben -> problema electrico.
static volatile uint32_t g_slRawEdges = 0;
static volatile uint32_t g_daRawEdges = 0;

// medicion del reloj SL
static volatile uint32_t slLastCyc = 0, slMinP = 0xFFFFFFFFu, slEdges = 0;
static volatile uint64_t slSumP = 0;
static volatile uint32_t slFastN = 0;          // periodos SL < 2 us  (reloj > 500 kHz)
static uint32_t          g_fastThr = 480;      // ciclos = 2 us @ 240 MHz (se ajusta en begin)

// volcado crudo de flancos
struct RawE { uint32_t cyc; uint8_t lv; };
#define RAW_MAX 4000
static volatile RawE  rawArr[RAW_MAX];
static volatile int   rawN = 0;
static volatile bool  rawArm = false;
static uint32_t       rawStartMs = 0;

static SnifState g_state;

// ---------------------------------------------------------------------------
//  ISR: flanco de SUBIDA de SL  ->  latch de 1 bit
// ---------------------------------------------------------------------------
void IRAM_ATTR isrSlRise() {
  g_slRawEdges++;
  if (!g_enabled) return;
  uint32_t io = gpioIn();
  uint8_t  da = DA_OF(io);

  uint32_t c = ESP.getCycleCount();
  if (rawArm && rawN < RAW_MAX) { rawArr[rawN].cyc = c; rawArr[rawN].lv = 0x02 | da; rawN++; }
  if (slLastCyc) {
    uint32_t p = c - slLastCyc;
    if (p < slMinP) slMinP = p;
    slSumP += p; slEdges++;
    if (p < g_fastThr) slFastN++;
  }
  slLastCyc = c;

  if (!dIn) return;
  dCur |= (uint8_t)(da << dBits);
  if (++dBits >= 8) {
    if (dLen < sizeof(dBuf)) dBuf[dLen++] = dCur;
    dCur = 0; dBits = 0;
  }
}

// ---------------------------------------------------------------------------
//  ISR: cambio en DA  ->  START / STOP  (solo si SL esta alto)
// ---------------------------------------------------------------------------
void IRAM_ATTR isrDaChange() {
  g_daRawEdges++;
  if (!g_enabled) return;
  uint32_t io = gpioIn();
  uint8_t  sl = SL_OF(io), da = DA_OF(io);

  if (rawArm && rawN < RAW_MAX) {
    rawArr[rawN].cyc = ESP.getCycleCount();
    rawArr[rawN].lv  = (sl << 1) | da;
    rawN++;
  }
  if (!sl) return;                       // DA cambio con SL bajo -> bit de datos

  if (da == 0) {                         // ---- START ----
    dIn = true; dCur = 0; dBits = 0; dLen = 0;
    return;
  }
  // ---- STOP ----
  if (dIn && dLen > 0) {
    g_txCount++;
    uint8_t cmd = dBuf[0];
    if ((cmd & 0xC0) == 0xC0) {          // trama de direccion + datos
      uint8_t a = cmd & 0x0F;
      for (uint8_t i = 1; i < dLen; i++) {
        uint8_t idx = a + (i - 1);
        if (idx < 16) g_shadow[idx] = dBuf[i];
      }
      if (dLen > 1) g_stopSeq++;
    }
  }
  dIn = false;
}

// --------------------------- parametros / mapa -----------------------------
static void snifDefaults() {
  int8_t  p[]  = SNIF_PESO_ADDR;
  int8_t  pr[] = SNIF_PRECIO_ADDR;
  int8_t  t[]  = SNIF_TOTAL_ADDR;
  uint8_t sb[] = SNIF_SEG_BIT;
  memset(&snf, 0, sizeof(snf));
  snf.pesoLen   = (int8_t)sizeof(p);   memcpy(snf.pesoAddr,   p,  sizeof(p));
  snf.precioLen = (int8_t)sizeof(pr);  memcpy(snf.precioAddr, pr, sizeof(pr));
  snf.totalLen  = (int8_t)sizeof(t);   memcpy(snf.totalAddr,  t,  sizeof(t));
  memcpy(snf.segBit, sb, 8);
  snf.pesoDP = SNIF_PESO_DEC; snf.precioDP = SNIF_PRECIO_DEC; snf.totalDP = SNIF_TOTAL_DEC;
}

static int parseCSV(const char* s, int8_t* out, int maxn) {
  int n = 0; const char* p = s;
  while (*p && n < maxn) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    int v = 0; bool got = false;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; got = true; }
    if (got) out[n++] = (int8_t)(sign * v);
  }
  return n;
}

// ¿tiene el campo al menos 3 direcciones validas (0..15)?  Si no, esta roto.
static bool fieldSane(const int8_t* a, int8_t len) {
  if (len < 3 || len > 6) return false;
  int good = 0;
  for (int i = 0; i < len; i++) if (a[i] >= 0 && a[i] < 16) good++;
  return good >= 3;
}

void snifLoadParams() {
  snifDefaults();
  SnifParams def = snf;                 // copia de los defaults de config.h

  Preferences pr;
  pr.begin("snf", false);               // RW: puede que haya que reescribir "ver"

  // si el mapa guardado es de una version anterior de config.h -> borrarlo
  if (pr.getInt("ver", 0) != SNIF_MAP_VERSION) {
    pr.clear();
    pr.putInt("ver", SNIF_MAP_VERSION);
    pr.end();
    Serial.println("[snif] mapa NVS obsoleto -> uso el de config.h");
    return;
  }

  if (pr.isKey("peso"))   snf.pesoLen   = parseCSV(pr.getString("peso").c_str(),   snf.pesoAddr,   6);
  if (pr.isKey("precio")) snf.precioLen = parseCSV(pr.getString("precio").c_str(), snf.precioAddr, 6);
  if (pr.isKey("total"))  snf.totalLen  = parseCSV(pr.getString("total").c_str(),  snf.totalAddr,  6);
  if (pr.isKey("segbit")) {
    int8_t tmp[8]; int n = parseCSV(pr.getString("segbit").c_str(), tmp, 8);
    for (int i = 0; i < n && i < 8; i++) snf.segBit[i] = (uint8_t)tmp[i];
  }
  snf.pesoDP   = pr.getInt("pdec",  snf.pesoDP);
  snf.precioDP = pr.getInt("prdec", snf.precioDP);
  snf.totalDP  = pr.getInt("tdec",  snf.totalDP);
  pr.end();

  // sanidad: cualquier campo roto -> vuelve al de config.h
  if (!fieldSane(snf.pesoAddr,   snf.pesoLen))   { memcpy(snf.pesoAddr,   def.pesoAddr,   6); snf.pesoLen   = def.pesoLen; }
  if (!fieldSane(snf.precioAddr, snf.precioLen)) { memcpy(snf.precioAddr, def.precioAddr, 6); snf.precioLen = def.precioLen; }
  if (!fieldSane(snf.totalAddr,  snf.totalLen))  { memcpy(snf.totalAddr,  def.totalAddr,  6); snf.totalLen  = def.totalLen; }
}

// borra el mapa de NVS y vuelve al de config.h  (boton en /sniffer)
void snifResetParams() {
  Preferences pr;
  pr.begin("snf", false);
  pr.clear();
  pr.putInt("ver", SNIF_MAP_VERSION);
  pr.end();
  snifDefaults();
  Serial.println("[snif] mapa restaurado a config.h");
}

// --------------------------- decodificacion -------------------------------
//  Nº de decimales (segun SNIF_*_DEC):
//    decCfg >= 0  ->  FIJO ese numero.  Se IGNORA el bit de punto del display
//                     (asi un icono NET/TARA que encienda ese bit tras tarar NO
//                      descoloca el valor:  "-10.00" y "1.60" salen bien).
//    decCfg == -1 ->  lo marca el bit de punto de un digito ENCENDIDO; si no hay,
//                     0 decimales  (para PRECIO, que puede ser entero o decimal).
//  En los dos casos los digitos van pegados a la DERECHA del campo.
static void decodeGroup(const uint8_t* sh, const int8_t* addr, int len,
                        int decCfg, char* out, float* val) {
  char digs[10]; int di = 0;
  bool neg = false, any = false, unknown = false;
  int  dpPos = -1;                       // posicion (desde la derecha) del punto

  for (int i = 0; i < len && i < 8; i++) {
    int a = addr[i];
    bool ok = (a >= 0 && a < 16);
    uint8_t raw = ok ? sh[a] : 0;
    uint8_t std = 0;
    for (int k = 0; k < 7; k++) if ((raw >> snf.segBit[k]) & 1) std |= (1 << k);
    bool dpBit = ok && ((raw >> snf.segBit[7]) & 1);
    // posicion delantera (aun sin digitos): decode ESTRICTO, y si no cuadra se
    //   toma como blanco (suele ser un icono ESTABLE/kg/NET en un grid suelto).
    // posicion ya dentro del numero: decode TOLERANTE (1 segmento) por si un
    //   icono se solapa con un digito real.
    bool leading = !any;
    int dgt = !ok ? -1 : (leading ? seg7decode(std) : seg7decodeFuzzy(std, 1));
    if      (dgt >= 0)  { digs[di++] = '0' + dgt; any = true;
                          if (dpBit) dpPos = len - 1 - i; }   // punto solo si el digito esta ON
    else if (dgt == -2) { digs[di++] = '_'; }
    else if (dgt == -3) { digs[di++] = '-'; neg = true; }
    else if (leading)   { digs[di++] = '_'; }  // icono en posicion delantera -> blanco
    else                { digs[di++] = '?'; unknown = true; }
  }
  digs[di] = 0;

  // decCfg >= 0 -> FIJO (ignora el punto del display).  decCfg == -1 -> usa el punto.
  bool usePoint = (decCfg < 0) && (dpPos >= 0);
  int  dec = usePoint ? dpPos : (decCfg >= 0 ? decCfg : 0);
  if (dec < 0) dec = 0; else if (dec > 3) dec = 3;

  double v = 0;
  if (usePoint) {
    // --- por POSICION en el campo: el punto marca las unidades ---
    for (int k = 0; k < di; k++) {
      if (digs[k] < '0' || digs[k] > '9') continue;
      int e = (di - 1 - k) - dec;
      double m = 1.0;
      if (e > 0)      for (int j = 0; j < e;  j++) m *= 10.0;
      else if (e < 0) for (int j = 0; j < -e; j++) m /= 10.0;
      v += (digs[k] - '0') * m;
    }
  } else {
    // --- digitos juntos, el ultimo en el lugar 10^-dec ---
    long iv = 0;
    for (int k = 0; k < di; k++)
      if (digs[k] >= '0' && digs[k] <= '9') iv = iv * 10 + (digs[k] - '0');
    v = (double)iv;
    for (int j = 0; j < dec; j++) v /= 10.0;
  }
  if (neg) v = -v;

  if (any && !unknown) { snprintf(out, 16, "%.*f", dec, v); *val = (float)v; }
  else                 { strncpy(out, digs, 15); out[15] = 0; *val = NAN; }
}

static float slKHzCalc() {
  uint32_t e; uint64_t s;
  noInterrupts(); e = slEdges; s = slSumP; interrupts();
  if (e < 10) return 0;
  float mhz = (float)getCpuFrequencyMhz();
  float avg = (float)s / (float)e;
  return avg > 0 ? (mhz * 1000.0f / avg) : 0;
}

// frecuencia de reloj DENTRO de una rafaga (periodo minimo visto) -> dice si el
// ISR puede seguir el ritmo del bus.  La media (slKHzCalc) sale baja porque
// incluye los huecos de reposo entre refrescos.
static float slKHzPeakCalc() {
  uint32_t mp;
  noInterrupts(); mp = slMinP; interrupts();
  if (mp == 0 || mp == 0xFFFFFFFFu) return 0;
  return (float)getCpuFrequencyMhz() * 1000.0f / (float)mp;
}

// --------------------------- guardar el mapa -----------------------------
static String csvI8(const int8_t* a, int n) {
  String s; for (int i = 0; i < n; i++) { if (i) s += ','; s += (int)a[i]; } return s;
}

void snifSaveParams() {
  Preferences pr;
  pr.begin("snf", false);
  pr.putString("peso",   csvI8(snf.pesoAddr,   snf.pesoLen));
  pr.putString("precio", csvI8(snf.precioAddr, snf.precioLen));
  pr.putString("total",  csvI8(snf.totalAddr,  snf.totalLen));
  { String s; for (int i = 0; i < 8; i++) { if (i) s += ','; s += (int)snf.segBit[i]; }
    pr.putString("segbit", s); }
  pr.putInt("pdec",  snf.pesoDP);
  pr.putInt("prdec", snf.precioDP);
  pr.putInt("tdec",  snf.totalDP);
  pr.end();
}

// ===========================================================================
//  AUTO-RESOLVEDOR DEL MAPA
//  Se capturan N estados del display con su valor CONOCIDO.  El solver prueba
//  todas las permutaciones de bits (a..g,punto -> los 8 bits del byte) y busca
//  la asignacion byte->digito que explica todas las capturas.
// ===========================================================================
#define SNIF_CAP_MAX 12
struct SnifCap { uint8_t b[16]; int8_t d[16]; };   // d: -1 blanco, 0-9, -2 desconocido
static SnifCap g_cap[SNIF_CAP_MAX];
static int     g_capN = 0;

// "6.50" en un campo de 'width' -> d[off..] alineado a la DERECHA (blancos a la izq)
static void expandField(const char* s, int8_t* d, int off, int width) {
  for (int i = 0; i < width; i++) d[off + i] = -2;
  if (!s || !*s) return;                     // campo vacio -> desconocido
  char digs[8]; int n = 0;
  for (const char* p = s; *p && n < width; p++) if (*p >= '0' && *p <= '9') digs[n++] = *p;
  if (n == 0) return;
  int lead = width - n; if (lead < 0) lead = 0;
  for (int i = 0; i < lead; i++)          d[off + i] = -1;
  for (int i = 0; i < n && lead + i < width; i++) d[off + lead + i] = digs[i] - '0';
}

int snifCapture(const char* peso, const char* precio, const char* total) {
  if (g_capN >= SNIF_CAP_MAX) return -1;
  SnifCap& c = g_cap[g_capN];
  noInterrupts();
  for (int i = 0; i < 16; i++) c.b[i] = g_shadow[i];
  interrupts();
  expandField(peso,   c.d, 0,  5);
  expandField(precio, c.d, 5,  5);
  expandField(total,  c.d, 10, 6);
  return ++g_capN;
}
void snifCapClear() { g_capN = 0; }
int  snifCapCount() { return g_capN; }

int snifSolve(char* rep, int replen) {
  if (g_capN < 2) { snprintf(rep, replen, "necesito >= 2 capturas (mejor 3-4 con pesos MUY distintos, e incluye uno pesado de 4-5 cifras)"); return -1; }

  // posiciones "utiles": con >=2 digitos conocidos y que VARIAN entre capturas
  bool useful[16]; int nUseful = 0;
  for (int p = 0; p < 16; p++) {
    int known = 0, first = -9; bool varies = false;
    for (int k = 0; k < g_capN; k++) {
      int dv = g_cap[k].d[p];
      if (dv < -1) continue;
      known++;
      if (first == -9) first = dv; else if (dv != first) varies = true;
    }
    useful[p] = (known >= 2 && varies);
    if (useful[p]) nUseful++;
  }
  if (nUseful < 2) { snprintf(rep, replen, "las capturas no varian bastante: usa pesos MUY distintos entre si"); return -2; }

  // valores de byte distintos que aparecen (para no decodificar 256 cada vez)
  static uint8_t uv[256]; int nuv = 0;
  { bool seen[256]; memset(seen, 0, sizeof(seen));
    for (int k = 0; k < g_capN; k++)
      for (int b = 0; b < 16; b++) { uint8_t v = g_cap[k].b[b]; if (!seen[v]) { seen[v] = 1; uv[nuv++] = v; } } }

  uint8_t perm[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
  uint8_t bestSb[8]; int bestScore = -1; int bestAssign[16];
  static int8_t decv[256];                 // decode de cada valor de byte, para la perm actual

  do {
    // perm[0..6] = bit crudo que lleva el segmento a..g ; perm[7] = punto
    for (int u = 0; u < nuv; u++) {
      uint8_t v = uv[u], m = 0;
      for (int s = 0; s < 7; s++) if ((v >> perm[s]) & 1) m |= (1 << s);
      decv[v] = (int8_t)seg7decode(m);      // 0-9, -2 blanco, -3 signo, -1 invalido
    }

    static bool compat[16][16];
    for (int bb = 0; bb < 16; bb++)
      for (int p = 0; p < 16; p++) {
        if (!useful[p]) { compat[bb][p] = false; continue; }
        bool good = true;
        for (int k = 0; k < g_capN && good; k++) {
          int want = g_cap[k].d[p];
          int got  = decv[g_cap[k].b[bb]];
          if (want == -2) { if (got == -1) good = false; continue; }   // comodin, pero patron valido
          int wn = (want == -1) ? -2 : want;
          if (got != wn) good = false;
        }
        compat[bb][p] = good;
      }

    int assign[16]; for (int p = 0; p < 16; p++) assign[p] = -1;
    bool taken[16] = { false };
    int resolved = 0; bool progress = true;
    while (progress) {
      progress = false;
      for (int p = 0; p < 16; p++) {
        if (!useful[p] || assign[p] >= 0) continue;
        int cand = -1, nc = 0;
        for (int bb = 0; bb < 16; bb++) if (compat[bb][p] && !taken[bb]) { cand = bb; nc++; }
        if (nc == 1) { assign[p] = cand; taken[cand] = true; resolved++; progress = true; }
      }
    }
    if (resolved > bestScore) {
      bestScore = resolved;
      memcpy(bestSb, perm, 8);
      memcpy(bestAssign, assign, sizeof(assign));
    }
  } while (std::next_permutation(perm, perm + 8));

  if (bestScore <= 0) { snprintf(rep, replen, "no se pudo resolver ninguna posicion. Revisa que las capturas sean del PESO correcto y con cifras distintas."); return -3; }

  // aplicar
  memcpy(snf.segBit, bestSb, 8);
  int base[3]  = { 0, 5, 10 };
  int wid[3]   = { 5, 5, 6 };
  int8_t* dst[3] = { snf.pesoAddr, snf.precioAddr, snf.totalAddr };
  int8_t* dlen[3] = { &snf.pesoLen, &snf.precioLen, &snf.totalLen };
  for (int f = 0; f < 3; f++) {
    int tmp[6];
    for (int i = 0; i < wid[f]; i++) tmp[i] = bestAssign[base[f] + i];   // -1 si no resuelto
    int lead = 0; while (lead < wid[f] && tmp[lead] < 0) lead++;         // recorta blancos izq no resueltos
    int n = 0;
    for (int i = lead; i < wid[f]; i++) dst[f][n++] = (int8_t)tmp[i];
    *dlen[f] = (int8_t)(n > 0 ? n : wid[f]);
    if (n == 0) for (int i = 0; i < wid[f]; i++) dst[f][i] = -1;
  }
  snifSaveParams();

  snprintf(rep, replen,
    "OK: %d/%d posiciones resueltas. segBit(a..g,punto)= %u,%u,%u,%u,%u,%u,%u,%u . Guardado.",
    bestScore, nUseful,
    bestSb[0], bestSb[1], bestSb[2], bestSb[3], bestSb[4], bestSb[5], bestSb[6], bestSb[7]);
  return bestScore;
}

// borra las estadisticas de reloj/flancos (para medir el pico "limpio" sin
// arrastrar un glitch de cuando se manipulaba el cableado)
void snifResetStats() {
  noInterrupts();
  slLastCyc = 0; slMinP = 0xFFFFFFFFu; slEdges = 0; slSumP = 0; slFastN = 0;
  g_slRawEdges = 0; g_daRawEdges = 0;
  interrupts();
}

// ---------------------------------------------------------------------------
void snifBegin() {
  snifLoadParams();
  g_fastThr = getCpuFrequencyMhz() * 2;         // 2 us en ciclos de CPU
  pinMode(PIN_DA, INPUT);
  pinMode(PIN_SL, INPUT);
  for (int i = 0; i < 16; i++) g_shadow[i] = 0;
  memset(&g_state, 0, sizeof(g_state));
  attachInterrupt(digitalPinToInterrupt(PIN_SL), isrSlRise,  RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_DA), isrDaChange, CHANGE);
}

#ifndef SNIF_SETTLE_MS
#define SNIF_SETTLE_MS 12
#endif

void snifLoop() {
  static uint32_t lastSeq = 0, quietMs = 0;
  static bool     pend = false;
  static uint8_t  prevSh[16] = {0};        // frame crudo de la lectura anterior
  static bool     havePrev = false;
  static char     prevPeso[16] = "";
  static int      stableCount = 0;
  static int      differ = 0;              // lecturas seguidas TODAS distintas

  uint32_t now = millis();
  uint32_t seq = g_stopSeq;
  if (seq != lastSeq) { lastSeq = seq; quietMs = now; pend = true; }

  // leemos solo cuando el bus lleva SNIF_SETTLE_MS sin escribir -> el refresco
  // esta COMPLETO (nada de leer a medias)
  if (!pend || (now - quietMs) < SNIF_SETTLE_MS) return;
  pend = false;

  uint8_t sh[16];
  noInterrupts();
  for (int i = 0; i < 16; i++) sh[i] = g_shadow[i];
  interrupts();

  // Decodifica SIEMPRE, cada frame -> los VALORES numericos van a g_state sin
  // filtrar.  La maquina de estados del pesaje necesita ver el descenso
  // gradual del peso al retirar la gaveta (si no, un retiro parece una tara).
  char  tp[16], tpr[16], tt[16];
  float tpv, tprv, ttv;
  decodeGroup(sh, snf.pesoAddr,   snf.pesoLen,   snf.pesoDP,   tp,  &tpv);
  decodeGroup(sh, snf.precioAddr, snf.precioLen, snf.precioDP, tpr, &tprv);
  decodeGroup(sh, snf.totalAddr,  snf.totalLen,  snf.totalDP,  tt,  &ttv);
  g_state.pesoVal   = tpv;
  g_state.precioVal = tprv;
  g_state.totalVal  = ttv;
  g_state.refreshCount++;
  g_state.txCount = g_txCount;
  g_state.valid   = (g_state.refreshCount > 2);
  g_state.slKHz     = slKHzCalc();
  g_state.slKHzPeak = slKHzPeakCalc();
  { uint32_t fn; noInterrupts(); fn = slFastN; interrupts(); g_state.slFastN = fn; }

  // Anti-basura: SOLO para lo visual (shadow / strings / estable).  Un frame
  // corrompido por ruido difiere de sus vecinos; se descarta.  Si hay muchos
  // seguidos todos distintos (cambio real rapido) se acepta el ultimo.
  bool same = havePrev && (memcmp(sh, prevSh, 16) == 0);
  if (!same) {
    if (++differ < 6) { memcpy(prevSh, sh, 16); havePrev = true; return; }
  }
  differ = 0;
  memcpy(prevSh, sh, 16); havePrev = true;

  memcpy(g_state.shadow, sh, 16);
  strncpy(g_state.peso,   tp,  sizeof(g_state.peso) - 1);
  strncpy(g_state.precio, tpr, sizeof(g_state.precio) - 1);
  strncpy(g_state.total,  tt,  sizeof(g_state.total) - 1);
  g_state.peso[sizeof(g_state.peso) - 1] = 0;
  g_state.precio[sizeof(g_state.precio) - 1] = 0;
  g_state.total[sizeof(g_state.total) - 1] = 0;

  if (strcmp(prevPeso, g_state.peso) == 0) { if (stableCount < 9999) stableCount++; }
  else { stableCount = 0; strncpy(prevPeso, g_state.peso, sizeof(prevPeso) - 1); }
  g_state.stable = stableCount >= SNIF_STABLE_N;
}

bool snifGet(SnifState& out) { out = g_state; return g_state.valid; }

void snifSetEnabled(bool en) {
  if (en && !g_enabled) {                 // al reanudar, reinicia el decodificador
    noInterrupts();
    dIn = false; dBits = 0; dLen = 0; dCur = 0;
    interrupts();
  }
  g_enabled = en;
}

// --------------------------- volcado crudo -------------------------------
void snifArmRaw() {
  noInterrupts(); rawN = 0; rawArm = true; interrupts();
  rawStartMs = millis();
}

int snifRawCount() {
  if (rawArm && (rawN >= RAW_MAX || millis() - rawStartMs >= 300)) {
    noInterrupts(); rawArm = false; interrupts();
  }
  return rawN;
}

void snifRawGet(int i, uint32_t* dCyc, uint8_t* sl, uint8_t* da) {
  static uint32_t prev = 0;
  if (i <= 0) prev = (rawN > 0) ? rawArr[0].cyc : 0;
  uint32_t c = rawArr[i].cyc;
  *dCyc = c - prev;
  prev = c;
  *sl = (rawArr[i].lv >> 1) & 1;
  *da = rawArr[i].lv & 1;
}

uint32_t snifCpuMHz() { return getCpuFrequencyMhz(); }

// --------------------------- diagnostico de cableado --------------------------
void snifDiag(uint8_t* slLvl, uint8_t* daLvl, uint32_t* slEdges, uint32_t* daEdges) {
  *slLvl = (uint8_t)digitalRead(PIN_SL);
  *daLvl = (uint8_t)digitalRead(PIN_DA);
  noInterrupts();
  *slEdges = g_slRawEdges;
  *daEdges = g_daRawEdges;
  interrupts();
}

static const char* probeVerdict(int up, int dn) {
  if (up == 1 && dn == 0) return "FLOTANTE (el pin no esta conectado a nada)";
  if (up == 1 && dn == 1) return "ALTO fijo (reposo del bus, o cable a VCC)";
  if (up == 0 && dn == 0) return "BAJO fijo (cable a GND?)";
  return "activo (algo lo esta moviendo)";
}

// Fuerza pull-up y luego pull-down en cada pin y mira si la linea los sigue.
// Sirve para saber si el cable llega o el pin esta al aire.
void snifPinProbe(char* out, int len) {
  const int pins[2] = { PIN_SL, PIN_DA };
  int up[2], dn[2];
  detachInterrupt(digitalPinToInterrupt(PIN_SL));
  detachInterrupt(digitalPinToInterrupt(PIN_DA));
  for (int i = 0; i < 2; i++) {
    pinMode(pins[i], INPUT_PULLUP);   delayMicroseconds(400); up[i] = digitalRead(pins[i]);
    pinMode(pins[i], INPUT_PULLDOWN); delayMicroseconds(400); dn[i] = digitalRead(pins[i]);
    pinMode(pins[i], INPUT);
  }
  noInterrupts(); dIn = false; dBits = 0; dLen = 0; dCur = 0; interrupts();
  attachInterrupt(digitalPinToInterrupt(PIN_SL), isrSlRise,  RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_DA), isrDaChange, CHANGE);
  snprintf(out, len,
    "SL (GPIO%d): pullup=%d pulldown=%d -> %s\nDA (GPIO%d): pullup=%d pulldown=%d -> %s",
    PIN_SL, up[0], dn[0], probeVerdict(up[0], dn[0]),
    PIN_DA, up[1], dn[1], probeVerdict(up[1], dn[1]));
}

// ===========================================================================
#else   // ENABLE_SNIFFER == 0  ->  stubs para que el resto enlace
// ===========================================================================
#include "sniffer_tm1640.h"

SnifParams snf;
static SnifState g_state = {};

void   snifBegin()                          {}
void   snifLoop()                           {}
bool   snifGet(SnifState& out)              { out = g_state; return false; }
void   snifLoadParams()                     {}
void   snifSaveParams()                     {}
void   snifResetParams()                    {}
void   snifSetEnabled(bool)                 {}
void   snifArmRaw()                         {}
int    snifRawCount()                       { return 0; }
void   snifRawGet(int, uint32_t* d, uint8_t* s, uint8_t* a) { *d = 0; *s = 0; *a = 0; }
uint32_t snifCpuMHz()                       { return 0; }
void   snifDiag(uint8_t* sl, uint8_t* da, uint32_t* se, uint32_t* de) { *sl = 0; *da = 0; *se = 0; *de = 0; }
void   snifPinProbe(char* o, int n)        { if (n) snprintf(o, n, "sniffer desactivado"); }
void   snifResetStats()                    {}
int    snifCapture(const char*, const char*, const char*) { return -1; }
void   snifCapClear()                       {}
int    snifCapCount()                       { return 0; }
int    snifSolve(char* r, int n)            { if (n) snprintf(r, n, "sniffer desactivado"); return -9; }

#endif  // ENABLE_SNIFFER
