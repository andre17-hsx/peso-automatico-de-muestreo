// ===========================================================================
//  weighlog.cpp  -  maquina de estados del pesaje + historial (RAM + NVS)
//                   guarda los 3 campos de la balanza: PESO, PRECIO, TOTAL
// ===========================================================================
#include "config.h"
#include "weighlog.h"
#include <math.h>
#include <time.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// -------- parametros por defecto (ajustables en config.h) ------------------
#ifndef WEIGH_ZERO_THRESH
#define WEIGH_ZERO_THRESH   0.10f
#endif
#ifndef WEIGH_STABLE_MS
#define WEIGH_STABLE_MS     700
#endif
#ifndef WEIGH_ZERO_MS
#define WEIGH_ZERO_MS       400
#endif
#ifndef WEIGH_CONFIRM_MS
#define WEIGH_CONFIRM_MS    1000
#endif
#ifndef WEIGH_PARTIAL
#define WEIGH_PARTIAL       0.15f
#endif
#ifndef WEIGH_PARTIAL_FRAC
#define WEIGH_PARTIAL_FRAC  0.55f   // ...o que baje a menos de esta fraccion del
#endif                              //    valor estable (para pesos netos pequeños)
#ifndef WEIGH_INVALID_MS
#define WEIGH_INVALID_MS    2500
#endif
#ifndef WEIGH_LOG_SIZE
#define WEIGH_LOG_SIZE      120
#endif
#ifndef WEIGH_EPS
#define WEIGH_EPS           0.005f
#endif
#ifndef WEIGH_PERSIST
#define WEIGH_PERSIST       1
#endif
#ifndef WEIGH_V2
#define WEIGH_V2            1
#endif
#ifndef WEIGH_DIP
#define WEIGH_DIP           0.12f
#endif
#ifndef WEIGH_SETTLE_MS
#define WEIGH_SETTLE_MS     350
#endif

// -------------------------- historial (ring buffer) -----------------------
static portMUX_TYPE  g_mux = portMUX_INITIALIZER_UNLOCKED;
static Weighing      g_log[WEIGH_LOG_SIZE];
static int           g_count = 0;      // en el buffer (<= WEIGH_LOG_SIZE)
static uint32_t      g_total = 0;      // total desde el primer arranque (monotono)
static int           g_head  = 0;      // proxima posicion a escribir
static float         g_latched = NAN;
static double        g_sumAll = 0;     // suma de TODOS los pesos desde el ultimo Borrar

// -------------------- agrupacion MALLA / BIN (ver weighlog.h) --------------
static uint16_t g_mallaId = 1, g_binId = 1;             // grupo EN CURSO (arranca en 1)
static double   g_mallaSum = 0, g_binSum = 0;           // acumulado del grupo EN CURSO
static float    g_mallaTarget = 0, g_binTarget = 0;      // <=0 = sin objetivo
static float    g_mallaLast = NAN, g_binLast = NAN;      // total del ultimo grupo CERRADO

static WeighCommitCb g_commitCb = nullptr;

enum WState { WS_EMPTY, WS_RISING, WS_STABLE };
static volatile int  g_state = WS_EMPTY;

// --- estado de la maquina de estados del pesaje (ver mas abajo) ---
enum { PH_EMPTY, PH_LOAD, PH_CONFIRM };
static int      g_phase    = PH_EMPTY;
static float    g_sP  = NAN, g_sPr = NAN, g_sT = NAN;   // ultimo valor ESTABLE
static float    g_lastP    = 0;       // ultimo peso (para el "quieto" interno)
static uint32_t g_pMoveMs  = 0;       // millis del ultimo cambio de g_lastP
static uint32_t g_badMs    = 0;       // inicio de la racha ilegible
static float    g_minLoad  = NAN;     // peso MINIMO con carga desde la ultima lectura estable
static uint32_t g_confirmMs = 0;      // millis al entrar en CONFIRMANDO
static uint32_t g_negMs    = 0;       // millis desde que la plataforma esta negativa
static float    g_dip      = 0;       // peso MAS NEGATIVO visto en CONFIRMANDO (rebote de la celda)

static void smReset() {
  g_phase = PH_EMPTY;
  g_sP = g_sPr = g_sT = NAN;
  g_minLoad = NAN;
  g_lastP = 0;
  g_pMoveMs = g_badMs = g_confirmMs = g_negMs = 0;
  g_dip = 0;
  g_state = WS_EMPTY;
}

// recalcula g_latched = PESO del pesaje NO borrado mas reciente (llamar con g_mux tomado)
static void relatch() {
  g_latched = NAN;
  for (int k = 0; k < g_count; k++) {
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    if (g_log[idx].epoch != -1) { g_latched = g_log[idx].peso; return; }
  }
}

// -------------------------- persistencia (NVS) ----------------------------
#if WEIGH_PERSIST
struct PSlot { float peso; float precio; float total; int32_t epoch; uint16_t malla; uint16_t bin; };
#define PSLOT_V1_SIZE 16     // tamaño de PSlot ANTES de agregar malla/bin (sin ellos)
struct PMeta { uint32_t total; int32_t head; int32_t count; };
// mallaId/binId/sumas/objetivos/ultimo-cerrado EMPAQUETADOS en un solo blob
// (antes eran 8 claves NVS sueltas -> 8 escrituras a flash por pesaje, cada
// una con su propio "congelon" de los dos nucleos; ahora es 1 sola).
struct PGroups { uint16_t mallaId, binId; float mallaSum, binSum, mallaTarget, binTarget, mallaLast, binLast; };
static Preferences        g_pr;
static bool               g_prOk = false;
static SemaphoreHandle_t  g_prMux = nullptr;

static void persistGroups() {
  if (!g_prOk) return;
  PGroups g;
  portENTER_CRITICAL(&g_mux);
  g.mallaId = g_mallaId; g.binId = g_binId;
  g.mallaSum = (float)g_mallaSum; g.binSum = (float)g_binSum;
  g.mallaTarget = g_mallaTarget; g.binTarget = g_binTarget;
  g.mallaLast = g_mallaLast; g.binLast = g_binLast;
  portEXIT_CRITICAL(&g_mux);
  if (g_prMux) xSemaphoreTake(g_prMux, portMAX_DELAY);
  g_pr.putBytes("grp", &g, sizeof(g));
  if (g_prMux) xSemaphoreGive(g_prMux);
}

static void persistSlot(int slot) {
  if (!g_prOk) return;
  PSlot s; PMeta m; float sv;
  portENTER_CRITICAL(&g_mux);
  s.peso   = g_log[slot].peso;
  s.precio = g_log[slot].precio;
  s.total  = g_log[slot].total;
  s.epoch  = (int32_t)g_log[slot].epoch;
  s.malla  = g_log[slot].malla;
  s.bin    = g_log[slot].bin;
  m.total = g_total; m.head = g_head; m.count = g_count;
  sv = (float)g_sumAll;
  portEXIT_CRITICAL(&g_mux);
  char k[8]; snprintf(k, sizeof(k), "s%03d", slot);
  if (g_prMux) xSemaphoreTake(g_prMux, portMAX_DELAY);
  g_pr.putBytes(k, &s, sizeof(s));
  g_pr.putBytes("meta", &m, sizeof(m));
  g_pr.putFloat("wsum", sv);                 // suma acumulada (clave aparte: no rompe el historial viejo)
  if (g_prMux) xSemaphoreGive(g_prMux);
}

static void persistClear() {
  if (!g_prOk) return;
  if (g_prMux) xSemaphoreTake(g_prMux, portMAX_DELAY);
  g_pr.clear();
  if (g_prMux) xSemaphoreGive(g_prMux);
}

static void persistLoad() {
  g_prMux = xSemaphoreCreateMutex();
  g_prOk  = g_pr.begin("weigh", false);
  if (!g_prOk) return;
  PMeta m;
  if (g_pr.getBytes("meta", &m, sizeof(m)) != sizeof(m)) return;   // aun no hay nada
  if (m.count < 0 || m.count > WEIGH_LOG_SIZE) return;             // basura -> ignora
  g_total = m.total;
  g_head  = (m.head >= 0 && m.head < WEIGH_LOG_SIZE) ? m.head : 0;
  g_count = m.count;
  for (int i = 0; i < WEIGH_LOG_SIZE; i++) {
    PSlot s = {};                                 // cero -> malla/bin=0 si el registro es viejo
    char k[8]; snprintf(k, sizeof(k), "s%03d", i);
    size_t n = g_pr.getBytes(k, &s, sizeof(s));
    if (n == sizeof(s) || n == PSLOT_V1_SIZE) {    // formato nuevo completo, o viejo (sin malla/bin)
      g_log[i].peso   = s.peso;
      g_log[i].precio = s.precio;
      g_log[i].total  = s.total;
      g_log[i].epoch  = s.epoch;
      g_log[i].malla  = s.malla;                  // 0 si venia del formato viejo (ver arriba)
      g_log[i].bin    = s.bin;
      g_log[i].ms     = 0;                        // millis de otra sesion -> desconocido
    }
  }
  relatch();                                   // g_latched = ultimo pesaje NO borrado
  // suma acumulada: la clave "wsum" si existe; si no (instalacion vieja), se
  // reconstruye con lo que haya en el buffer (>= los ultimos WEIGH_LOG_SIZE).
  float sv = g_pr.getFloat("wsum", NAN);
  if (isnan(sv)) {
    double acc = 0;
    for (int i = 0; i < g_count; i++) {
      int idx = (g_head - 1 - i + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
      if (g_log[idx].epoch != -1 && !isnan(g_log[idx].peso)) acc += g_log[idx].peso;
    }
    sv = (float)acc;
  }
  g_sumAll = sv;

  // agrupacion MALLA/BIN: primero se intenta el blob nuevo ("grp"); si no
  // esta (equipo que ya tenia guardadas las 8 claves sueltas de ANTES de
  // empaquetarlas, o instalacion nueva sin nada aun) se leen esas claves
  // sueltas con sus valores de siempre, y de paso se escribe YA el blob
  // nuevo para que el proximo arranque entre directo por la via rapida.
  PGroups g = {};
  if (g_pr.getBytes("grp", &g, sizeof(g)) == sizeof(g)) {
    g_mallaId = g.mallaId; g_binId = g.binId;
    g_mallaSum = g.mallaSum; g_binSum = g.binSum;
    g_mallaTarget = g.mallaTarget; g_binTarget = g.binTarget;
    g_mallaLast = g.mallaLast; g_binLast = g.binLast;
  } else {
    g_mallaId     = g_pr.getUShort("mId", 1);   if (g_mallaId == 0) g_mallaId = 1;
    g_binId       = g_pr.getUShort("bId", 1);   if (g_binId   == 0) g_binId   = 1;
    g_mallaSum    = g_pr.getFloat ("mSum", 0.0f);
    g_binSum      = g_pr.getFloat ("bSum", 0.0f);
    g_mallaTarget = g_pr.getFloat ("mTgt", 0.0f);
    g_binTarget   = g_pr.getFloat ("bTgt", 0.0f);
    g_mallaLast   = g_pr.getFloat ("mLast", NAN);
    g_binLast     = g_pr.getFloat ("bLast", NAN);
    persistGroups();     // migra YA al blob nuevo (si eran claves viejas; si no habia nada, guarda los defaults)
  }

  Serial.printf("[pesaje] historial cargado de NVS: %d en buffer, %lu total, suma %.2f\n",
                g_count, (unsigned long)g_total, (double)g_sumAll);
}
#else
static void persistSlot(int)  {}
static void persistClear()    {}
static void persistLoad()     {}
static void persistGroups()   {}
#endif

// ------------------------------- utilidades -------------------------------
static long nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (long)t : 0;         // hora real (post-2023) o 0
}

// añade UNA entrada nueva al historial y devuelve una copia (con su epoch/ms)
static Weighing logAppend(float peso, float precio, float total, uint32_t ms) {
  long ep = nowEpoch();
  Weighing w;
  portENTER_CRITICAL(&g_mux);
  int slot = g_head;
  g_head = (g_head + 1) % WEIGH_LOG_SIZE;
  if (g_count < WEIGH_LOG_SIZE) g_count++;
  g_total++;

  // -------- agrupacion MALLA / BIN: este pesaje se suma COMPLETO (nunca
  //          partido) al grupo EN CURSO de cada nivel; si con eso se alcanza
  //          (o supera) el objetivo, este pesaje queda como ULTIMO miembro de
  //          ese grupo y el siguiente pesaje arranca uno nuevo.
  //          Como un BIN esta formado por VARIAS MALLAS completas, el BIN
  //          solo puede cerrar en el mismo pesaje en que cierra una malla
  //          (nunca a mitad de una) -- si no, una malla podria terminar
  //          repartida entre dos BIN.  Si no hay objetivo de malla puesto,
  //          el BIN no depende de ella y se evalua en cada pesaje como antes.
  //          El nº de malla se reinicia a 1 en cada BIN nuevo (Malla 1, 2, 3...
  //          dentro de cada BIN), por eso el par (bin, malla) es la clave unica.
  bool haveP = !isnan(peso);
  if (haveP) { g_mallaSum += peso; g_binSum += peso; }
  uint16_t curMalla = g_mallaId, curBin = g_binId;
  bool mallaClosed = false;
  if (haveP && g_mallaTarget > 0 && g_mallaSum >= g_mallaTarget) {
    g_mallaLast = (float)g_mallaSum;
    g_mallaSum  = 0;
    g_mallaId++;
    mallaClosed = true;
  }
  bool binCanClose = (g_mallaTarget <= 0) || mallaClosed;
  if (haveP && binCanClose && g_binTarget > 0 && g_binSum >= g_binTarget) {
    g_binLast = (float)g_binSum;
    g_binSum  = 0;
    g_binId++;
    g_mallaId  = 1;      // cada BIN nuevo arranca en su Malla 1
    g_mallaSum = 0;      // (si habia malla activa ya cerro en este mismo pesaje -> ya es 0)
  }

  g_log[slot].peso   = peso;
  g_log[slot].precio = precio;
  g_log[slot].total  = total;
  g_log[slot].ms     = ms;
  g_log[slot].epoch  = ep;
  g_log[slot].id     = g_total;               // solo RAM (weighGet lo recalcula al leer)
  g_log[slot].malla  = curMalla;
  g_log[slot].bin    = curBin;
  g_latched = peso;
  if (haveP) g_sumAll += peso;                // suma acumulada (total de TODA la sesion)
  w = g_log[slot];
  portEXIT_CRITICAL(&g_mux);
  persistSlot(slot);
  persistGroups();
  return w;
}

// ------------------------------- API lectura ------------------------------
void weighBegin() { persistLoad(); }

const char* weighStateName() {
  switch (g_state) { case WS_RISING: return "estabilizando";
                     case WS_STABLE: return "estable";
                     default:        return "vacia"; }
}

float weighLatched() {
  portENTER_CRITICAL(&g_mux);
  float v = g_latched;
  portEXIT_CRITICAL(&g_mux);
  return v;
}

float weighSum() {
  portENTER_CRITICAL(&g_mux);
  double v = g_sumAll;
  portEXIT_CRITICAL(&g_mux);
  return (float)v;
}

bool weighLatchedFull(Weighing* out) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  for (int k = 0; k < g_count; k++) {
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    if (g_log[idx].epoch == -1) continue;
    *out = g_log[idx];
    out->id = (uint32_t)(g_total - k);
    ok = true;
    break;
  }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

uint32_t weighTotal() {
  portENTER_CRITICAL(&g_mux);
  uint32_t t = g_total;
  portEXIT_CRITICAL(&g_mux);
  return t;
}

int weighCount() {
  int n = 0;
  portENTER_CRITICAL(&g_mux);
  for (int k = 0; k < g_count; k++) {
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    if (g_log[idx].epoch != -1) n++;             // no cuenta las filas borradas
  }
  portEXIT_CRITICAL(&g_mux);
  return n;
}

int weighGet(Weighing* out, int maxn) {
  int j = 0;
  portENTER_CRITICAL(&g_mux);
  for (int k = 0; k < g_count && j < maxn; k++) {
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;   // reciente primero
    if (g_log[idx].epoch == -1) continue;                              // fila borrada
    out[j] = g_log[idx];
    out[j].id = (uint32_t)(g_total - k);                               // nº de pesaje (con huecos si hay borrados)
    j++;
  }
  portEXIT_CRITICAL(&g_mux);
  return j;
}

bool weighDeleteOne(uint32_t id) {
  int target = -1;
  portENTER_CRITICAL(&g_mux);
  for (int k = 0; k < g_count; k++) {
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    if (g_log[idx].epoch == -1) continue;
    if ((uint32_t)(g_total - k) == id) { target = idx; break; }
  }
  if (target >= 0) {
    if (!isnan(g_log[target].peso)) g_sumAll -= g_log[target].peso;
    g_log[target].epoch = -1;                    // marca de borrada
    relatch();
  }
  portEXIT_CRITICAL(&g_mux);
  if (target >= 0) { persistSlot(target); Serial.printf("[web] fila #%lu borrada\n", (unsigned long)id); }
  return target >= 0;
}

void weighClear() {
  portENTER_CRITICAL(&g_mux);
  g_count = 0; g_head = 0; g_total = 0; g_latched = NAN; g_sumAll = 0;
  g_mallaId = 1; g_binId = 1; g_mallaSum = 0; g_binSum = 0;
  g_mallaLast = NAN; g_binLast = NAN;
  // g_mallaTarget / g_binTarget NO se tocan: los objetivos puestos por el
  // operario se conservan aunque se borre todo el historial.
  portEXIT_CRITICAL(&g_mux);
  smReset();
  persistClear();     // borra TODO el namespace NVS...
  persistGroups();    // ...asi que hay que volver a guardar objetivos/ids ya mismo
}

void weighOnCommit(WeighCommitCb cb) { g_commitCb = cb; }

void weighGetGroups(WeighGroups* out) {
  portENTER_CRITICAL(&g_mux);
  out->mallaId     = g_mallaId;     out->binId     = g_binId;
  out->mallaSum    = (float)g_mallaSum;  out->binSum    = (float)g_binSum;
  out->mallaTarget = g_mallaTarget; out->binTarget = g_binTarget;
  out->mallaLast   = g_mallaLast;   out->binLast   = g_binLast;
  portEXIT_CRITICAL(&g_mux);
}

void weighSetTargets(float mallaTarget, float binTarget) {
  portENTER_CRITICAL(&g_mux);
  g_mallaTarget = (mallaTarget > 0) ? mallaTarget : 0;
  g_binTarget   = (binTarget   > 0) ? binTarget   : 0;
  portEXIT_CRITICAL(&g_mux);
  persistGroups();
}

// ===========================================================================
//  MAQUINA DE ESTADOS DEL PESAJE
// ---------------------------------------------------------------------------
//   VACIA      : plataforma ~0 (o dormida).
//   CON CARGA  : hay peso; mientras este ESTABLE se recuerda su valor.
//   CONFIRMAND.: la plataforma volvio a ~0.  Se comprueba si fue RETIRO o TARA:
//                  · RETIRO real -> el peso BAJO de verdad (paso por valores
//                    intermedios: WEIGH_PARTIAL kg menos que el estable, O por
//                    debajo de WEIGH_PARTIAL_FRAC del estable si el neto es
//                    pequeño)  O  la celda REBOTO a negativo (< -WEIGH_DIP) al
//                    quitar el peso -> SE GUARDA (a los WEIGH_SETTLE_MS).
//                  · TARA / re-cero -> salto LIMPIO de un peso estable a 0 sin
//                    pasar por valores menores y SIN rebote  Y/O  la plataforma
//                    queda en NEGATIVO sostenido -> NO se guarda (a WEIGH_CONFIRM_MS).
//                Si vuelve a haber carga (cambio de gaveta) se cierra el
//                pendiente y empieza otro pesaje.
//                (Menear la gaveta sin quitarla NO cuenta: al volver el peso
//                 cerca del estable se olvida el bache -> ver PH_LOAD.)
//
//   "Estable" = el bit ESTABLE del bus (stableHint) O, si no lo hay (OCR),
//   el numero quieto durante WEIGH_STABLE_MS.
// ===========================================================================
static void commitPending(const char* why) {
  Weighing w = logAppend(g_sP, g_sPr, g_sT, millis());
  Serial.printf("[pesaje] %s -> GUARDADO  PESO %.2f  PRECIO %.2f  TOTAL %.2f\n",
                why, w.peso, (double)w.precio, (double)w.total);
  if (g_commitCb) g_commitCb(w);
}

static void smStartLoad(float peso) {
  g_phase = PH_LOAD;
  g_sP = g_sPr = g_sT = NAN;
  g_minLoad = peso;
  g_state = WS_RISING;
  Serial.println("[pesaje] carga detectada, esperando a que se estabilice...");
}

// ¿el peso BAJO de verdad (con carga) antes de llegar a 0?  -> retiro real.
// Cuanto tuvo que bajar = el MAYOR (mas facil de cumplir) de dos criterios:
//   - una caida ABSOLUTA de WEIGH_PARTIAL, o
//   - una caida RELATIVA (bajo por debajo de WEIGH_PARTIAL_FRAC del valor
//     estable).  Lo segundo es lo que hace que funcione con pesos NETOS
//     pequeños (tara activa), donde no caben 0.15 kg entre el valor estable y
//     el umbral de vacio.  Ambos terminos son < g_sP, asi que una TARA (salto
//     limpio: g_minLoad == g_sP) sigue sin contar como retiro.
static bool sawRealUnload() {
  if (isnan(g_sP) || isnan(g_minLoad)) return false;
  float need = g_sP - WEIGH_PARTIAL;
  float rel  = g_sP * WEIGH_PARTIAL_FRAC;
  if (rel > need) need = rel;
  return g_minLoad <= need;
}

void weighFeed(float peso, float precio, float total, bool stableHint, bool valid) {
  uint32_t now = millis();
  bool ok = valid && !isnan(peso);

  // ---- display ilegible / dormido ----
  if (!ok) {
    if (g_badMs == 0) g_badMs = now;
    if (now - g_badMs < WEIGH_INVALID_MS) return;      // glitch breve -> ignorar
    if (g_phase != PH_EMPTY) {
      if      (sawRealUnload())  commitPending("display cerrado");
      else if (!isnan(g_sP))     Serial.println("[pesaje] display cerrado sin retiro -> no se guarda");
      smReset();
    }
    return;
  }
  g_badMs = 0;

  // seguimiento de "quieto" (fallback si no hay bit ESTABLE)
  if (fabsf(peso - g_lastP) >= WEIGH_EPS) { g_lastP = peso; g_pMoveMs = now; }

  bool loaded = (peso >= WEIGH_ZERO_THRESH);

  switch (g_phase) {

  // ------------------------------------------------------------ VACIA
  case PH_EMPTY:
    if (loaded) smStartLoad(peso);
    return;

  // ------------------------------------------------------------ CON CARGA
  case PH_LOAD:
    if (loaded) {
#if WEIGH_V2
      // si el peso VUELVE a subir cerca del valor estable, el bache era
      // manipulacion (menear la gaveta), no un descenso -> se olvida.  Asi una
      // TARA posterior no se cuela por un g_minLoad artificialmente bajo.
      if (!isnan(g_sP) && peso >= g_sP - WEIGH_PARTIAL)  g_minLoad = peso;
      else
#endif
      if (isnan(g_minLoad) || peso < g_minLoad)          g_minLoad = peso;   // sigue el minimo
      bool quiet = (now - g_pMoveMs >= 250);
      bool stbl  = quiet && (stableHint || now - g_pMoveMs >= WEIGH_STABLE_MS);
      if (stbl) {
        bool nuevo = isnan(g_sP) || fabsf(peso - g_sP) >= WEIGH_EPS;
        g_sP = peso; g_sPr = precio; g_sT = total;
        g_minLoad = peso;                                           // referencia nueva
        g_state = WS_STABLE;
        if (nuevo)
          Serial.printf("[pesaje] peso estable: %.2f  (precio %.2f  total %.2f)\n",
                        (double)peso, (double)precio, (double)total);
      }
      return;
    }
    // la plataforma cayo por debajo del umbral -> a CONFIRMANDO
    g_phase = PH_CONFIRM;
    g_confirmMs = now;
    g_negMs = 0;
    g_state = WS_EMPTY;
    return;

  // ------------------------------------------------------------ CONFIRMANDO
  case PH_CONFIRM: {
    if (peso < g_dip) g_dip = peso;                 // rebote de la celda al retirar

    // (a) volvio a haber carga -> cambio de gaveta
    if (loaded) {
      if      (sawRealUnload()) commitPending("retirado (cambio de gaveta)");
      else if (!isnan(g_sP))    Serial.println("[pesaje] carga tras un cero limpio (tara?) -> pendiente descartado");
      smStartLoad(peso);
      return;
    }
    // (b) plataforma en NEGATIVO sostenido -> aun habia peso bruto = hubo TARA
    if (peso <= -WEIGH_ZERO_THRESH) {
      if (g_negMs == 0) g_negMs = now;
      if (now - g_negMs >= WEIGH_ZERO_MS) {
        if (sawRealUnload()) commitPending("retirado (con tara activa)");  // habia producto real
        else Serial.printf("[pesaje] TARA (plataforma en %.2f) -> pendiente descartado\n", (double)peso);
        smReset();
      }
      return;
    }
    g_negMs = 0;
    // (c) plataforma en ~0.  ¿fue RETIRO?  -> paso por intermedios (sawRealUnload)
    //     O la celda reboto a negativo (g_dip).  Una TARA no cumple ninguna.
#if WEIGH_V2
    bool     realUnload = sawRealUnload() || (g_dip <= -WEIGH_DIP);
    uint32_t win        = realUnload ? WEIGH_SETTLE_MS : WEIGH_CONFIRM_MS;
#else
    bool     realUnload = sawRealUnload();
    uint32_t win        = WEIGH_CONFIRM_MS;
#endif
    if (now - g_confirmMs >= win) {
      if      (realUnload)   commitPending("retirado");
      else if (!isnan(g_sP)) Serial.printf("[pesaje] cero limpio (dip %.2f) sin descarga (tara?) -> no se guarda\n", (double)g_dip);
      else                   Serial.println("[pesaje] cero sin lectura estable -> nada");
      smReset();
    }
    return;
  }

  }  // switch
}
