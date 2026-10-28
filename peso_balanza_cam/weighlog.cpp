// ===========================================================================
//  weighlog.cpp  -  maquina de estados del pesaje + historial (RAM + NVS)
//                   guarda los 3 campos de la balanza: PESO, PRECIO, TOTAL
// ===========================================================================
#include "config.h"
#include "weighlog.h"
#include "histarch.h"
#include "diaglog.h"
#include <math.h>
#include <time.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// -------- parametros por defecto (ajustables en config.h) ------------------
#ifndef WEIGH_ZERO_THRESH
#define WEIGH_ZERO_THRESH   0.06f   // <= esto: la gaveta paso por cero (o quedo negativa) -> se cierra
#endif
#ifndef WEIGH_MIN_LOAD
#define WEIGH_MIN_LOAD      12.0f   // una gaveta empieza al llegar a este peso; menos = tara / nada
#endif
#ifndef WEIGH_MAX_LOAD
#define WEIGH_MAX_LOAD      80.0f   // lecturas por encima = fuera de rango (prueba de display 888.8, presion fuerte)
#endif
#ifndef WEIGH_MIN_MS
#define WEIGH_MIN_MS        600     // una gaveta tiene que estar puesta al menos esto para guardarse
#endif
#ifndef WEIGH_ZERO_N
#define WEIGH_ZERO_N        3       // REFRESCOS NUEVOS del display seguidos en cero para cerrar (~3 x 55 ms):
                                    // se cuentan refrescos y no vueltas de 20 ms, que repiten el mismo refresco
#endif
#ifndef WEIGH_FRAME_TIMEOUT_MS
#define WEIGH_FRAME_TIMEOUT_MS 1000 // sin un refresco NUEVO del display en este tiempo = display congelado
#endif
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
#ifndef HIST_BATCH
#define HIST_BATCH          8      // cada cuantos pesajes se pasan al archivo permanente (en lote)
#endif
#ifndef WEIGH_FEED_MS
#define WEIGH_FEED_MS       20
#endif
// captura por ventana tolerante (ver config.h)
#ifndef WEIGH_WIN_MS
#define WEIGH_WIN_MS        250
#endif
#ifndef WEIGH_BAND
#define WEIGH_BAND          0.15f
#endif
#ifndef WEIGH_TOL_ABS
#define WEIGH_TOL_ABS       1.0f
#endif
#ifndef WEIGH_TOL_PCT
#define WEIGH_TOL_PCT       3.0f
#endif
#ifndef WEIGH_PLACE_MS
#define WEIGH_PLACE_MS      1000
#endif
#ifndef WEIGH_PLAUS_FRAC
#define WEIGH_PLAUS_FRAC    0.7f
#endif
#ifndef WEIGH_PLAUS_HOLD_MS
#define WEIGH_PLAUS_HOLD_MS 400     // un estable quieto esto o mas en total NO se descarta (presion != pausa)
#endif
#ifndef WEIGH_RESCUE_FRAC
#define WEIGH_RESCUE_FRAC   0.92f
#endif

// -------------------------- historial (ring buffer) -----------------------
static portMUX_TYPE  g_mux = portMUX_INITIALIZER_UNLOCKED;
static Weighing      g_log[WEIGH_LOG_SIZE];
static int           g_count = 0;      // en el buffer (<= WEIGH_LOG_SIZE)
static uint32_t      g_total = 0;      // total desde el primer arranque (monotono)
static int           g_head  = 0;      // proxima posicion a escribir
static float         g_latched = NAN;
static double        g_sumAll = 0;     // suma de TODOS los pesos desde el ultimo Borrar

// ---- totales sobre TODO el historial (archivo + buffer), guardados en RAM ----
//  Sirven para mostrar el nº de muestras y los subtotales de BIN/malla sin
//  releer el archivo en cada peticion.  Se reconstruyen al arrancar.
struct GAgg { uint16_t bin, malla; uint32_t cnt; double sum; };
#define AGG_MAX 800                     // ultimas 800 mallas; si se llena se descartan las mas antiguas
static GAgg     g_agg[AGG_MAX];
static int      g_aggN  = 0;
static uint32_t g_alive = 0;            // pesajes NO borrados desde el ultimo Borrar (todo el historial)

// busca desde el final: los grupos recientes son los ultimos de la tabla
static GAgg* aggFind(uint16_t bin, uint16_t malla) {
  for (int i = g_aggN - 1; i >= 0; i--)
    if (g_agg[i].bin == bin && g_agg[i].malla == malla) return &g_agg[i];
  return nullptr;
}
static void aggAdd(uint16_t bin, uint16_t malla, float peso) {
  GAgg* g = aggFind(bin, malla);
  if (!g) {
    if (g_aggN >= AGG_MAX) {            // tabla llena: se descarta la malla mas antigua (el panel
      memmove(&g_agg[0], &g_agg[1],     // solo muestra las recientes, que son las que importan)
              sizeof(GAgg) * (AGG_MAX - 1));
      g_aggN = AGG_MAX - 1;
    }
    g = &g_agg[g_aggN++];
    g->bin = bin; g->malla = malla; g->cnt = 0; g->sum = 0;
  }
  g->cnt++;
  if (!isnan(peso)) g->sum += peso;
}
static void aggSub(uint16_t bin, uint16_t malla, float peso) {
  GAgg* g = aggFind(bin, malla);
  if (!g || g->cnt == 0) return;
  g->cnt--;
  if (!isnan(peso)) g->sum -= peso;
  if (g->cnt == 0) g->sum = 0;          // sin restos de redondeo
}

// -------------------- agrupacion MALLA / BIN (ver weighlog.h) --------------
static uint16_t g_mallaId = 1, g_binId = 1;             // grupo EN CURSO (arranca en 1)
static double   g_mallaSum = 0, g_binSum = 0;           // acumulado del grupo EN CURSO
static float    g_mallaTarget = 0, g_binTarget = 0;      // <=0 = sin objetivo
static float    g_mallaLast = NAN, g_binLast = NAN;      // total del ultimo grupo CERRADO

static WeighCommitCb g_commitCb = nullptr;

enum WState { WS_EMPTY, WS_RISING, WS_STABLE };
static volatile int  g_state = WS_EMPTY;

// --- estado de la maquina de estados del pesaje (ver mas abajo) ---
enum { PH_EMPTY, PH_LOAD };
static int      g_phase    = PH_EMPTY;
static float    g_sP  = NAN, g_sPr = NAN, g_sT = NAN;   // ultimo valor ESTABLE de la gaveta en curso
static uint32_t g_badMs    = 0;       // inicio de la racha ilegible
static uint8_t  g_zeroN    = 0;       // refrescos seguidos en cero con una gaveta puesta
static uint32_t g_lastFreshMs = 0;    // ultimo refresco NUEVO del display (0 = aun ninguno)
static bool     g_waitZero = false;   // tras cerrar por display sin datos: no aceptar otra gaveta
                                      // hasta ver el peso en cero (la misma gaveta seguiria encima)

// --- datos de la gaveta en curso, para el registro de diagnostico (/log) ---
// weighClear() corre en la tarea del SERVIDOR WEB; la maquina de estados de
// aqui abajo es propiedad exclusiva de la tarea del SNIFFER (weighTaskFn).
// Por eso no se llama smReset() directamente desde weighClear(): se pide con
// esta bandera y la aplica weighFeed() al principio de su siguiente vuelta.
static volatile bool g_resetPending = false;

static uint32_t g_loadMs = 0;         // millis al empezar la carga
static uint32_t g_capMs  = 0;         // millis de la 1ª lectura asentada (0 = ninguna aun)
static int      g_capN   = 0;         // cuantas veces se dio por asentada
static int      g_ldN    = 0;         // lecturas con carga
static int      g_rejN   = 0;         // tramos asentados RECHAZADOS por salirse de la banda
static float    g_ldMax  = NAN, g_ldMin = NAN;
static float    g_maxPost = NAN;      // maximo visto DESPUES de la ultima lectura asentada
static float    g_ldPr    = NAN;      // ultimo precio visto con carga (para el valor estimado)
static uint32_t g_lastAccMs = 0;      // millis de la ULTIMA lectura asentada (con g_capMs: cuanto duro el estable)

// Cuanto tiempo paso la lectura en cada valor (casillas de 0,5 lb entre WEIGH_MIN_LOAD y
// WEIGH_MAX_LOAD): sin valor estable, se guarda el valor donde mas tiempo estuvo.  Una
// presion de la mano es un pico corto y no gana.
#define SUS_STEP 0.5f
#define SUS_BINS ((int)((WEIGH_MAX_LOAD - WEIGH_MIN_LOAD) / SUS_STEP) + 1)
static uint32_t g_susN[SUS_BINS];      // laxo: cada 20 ms (cuenta tambien la subida y la bajada)
static float    g_susSum[SUS_BINS];
static float    g_prevP = NAN;        // lectura anterior con carga (para saber si estaba quieta)
// Estricto: solo refrescos NUEVOS del display (~55 ms) iguales al refresco anterior, o sea
// el peso se mantuvo al menos 2 refrescos.  Si no hay ninguno se usa el laxo (como antes).
static uint32_t g_qN[SUS_BINS];
static float    g_qSum[SUS_BINS];
static float    g_prevF = NAN;        // refresco anterior con carga

// Ventana de las ultimas lecturas con carga: se da por ASENTADO un valor si las
// lecturas de los ultimos WEIGH_WIN_MS varian menos de WEIGH_BAND (ver config.h).
#define WIN_MAX 64
static uint32_t g_winT[WIN_MAX];
static float    g_winP[WIN_MAX];
static int      g_winN = 0, g_winHead = 0;
static float    g_refP = NAN;         // referencia de ESTA gaveta (primer valor asentado)

static void winPush(uint32_t t, float p) {
  g_winT[g_winHead] = t;
  g_winP[g_winHead] = p;
  g_winHead = (g_winHead + 1) % WIN_MAX;
  if (g_winN < WIN_MAX) g_winN++;
}

// ¿la ventana esta llena (cubre WEIGH_WIN_MS, con al menos 5 lecturas) y todas sus
// lecturas caben en WEIGH_BAND?
static bool winFlat(uint32_t now) {
  if (g_winN < 5) return false;
  uint32_t cutoff = now - WEIGH_WIN_MS;
  float mn = 1e9f, mx = -1e9f;
  int n = 0;
  uint32_t oldest = now;
  for (int k = 0; k < g_winN; k++) {
    int i = (g_winHead - 1 - k + WIN_MAX) % WIN_MAX;
    if ((int32_t)(g_winT[i] - cutoff) < 0) break;            // mas viejo que la ventana
    n++;
    if (g_winP[i] < mn) mn = g_winP[i];
    if (g_winP[i] > mx) mx = g_winP[i];
    oldest = g_winT[i];
  }
  if (n < 5) return false;
  if (oldest - cutoff > 3 * WEIGH_FEED_MS) return false;     // la ventana no esta completa
  return (mx - mn) <= WEIGH_BAND;
}

static void loadDiagReset() {
  g_capMs = 0; g_lastAccMs = 0; g_capN = 0; g_ldN = 0; g_rejN = 0; g_maxPost = NAN; g_ldPr = NAN;
  g_ldMax = g_ldMin = NAN;
  g_winN = 0; g_winHead = 0;
  g_refP = NAN;
  memset(g_susN, 0, sizeof(g_susN));
  memset(g_susSum, 0, sizeof(g_susSum));
  g_prevP = NAN;
  memset(g_qN, 0, sizeof(g_qN));
  memset(g_qSum, 0, sizeof(g_qSum));
  g_prevF = NAN;
}

static void smReset() {
  g_phase = PH_EMPTY;
  g_sP = g_sPr = g_sT = NAN;
  g_badMs = 0;
  g_zeroN = 0;
  g_state = WS_EMPTY;
  loadDiagReset();
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
// Contadores + suma + grupos en UN solo blob ("st"): antes eran 3 claves ("meta",
// "wsum", "grp") = 3 escrituras a flash por pesaje; ahora 1 (+ la del slot).  Menos
// desgaste de NVS (~5000 pesajes/dia) y menos pausas del bus durante la escritura.
struct PState { uint32_t total; int32_t head; int32_t count; float wsum; PGroups g; };
static Preferences        g_pr;
static bool               g_prOk = false;
static SemaphoreHandle_t  g_prMux = nullptr;
static uint32_t           g_nvsErrMs = 0;   // ultimo aviso de error de NVS en /log (1 por minuto)

static void nvsError(const char* what) {
  uint32_t now = millis();
  if (g_nvsErrMs && now - g_nvsErrMs < 60000) return;
  g_nvsErrMs = now ? now : 1;
  diagLog("NVS ERROR al guardar %s", what);
}

static void fillState(PState* st) {           // llamar con g_mux tomado
  st->total = g_total; st->head = g_head; st->count = g_count;
  st->wsum = (float)g_sumAll;
  st->g.mallaId = g_mallaId; st->g.binId = g_binId;
  st->g.mallaSum = (float)g_mallaSum; st->g.binSum = (float)g_binSum;
  st->g.mallaTarget = g_mallaTarget; st->g.binTarget = g_binTarget;
  st->g.mallaLast = g_mallaLast; st->g.binLast = g_binLast;
}

// La instantanea se toma con g_prMux YA tomado: asi dos tareas (pesaje y web) nunca
// escriben una instantanea vieja encima de una nueva.
static bool writeStateLocked() {
  PState st;
  portENTER_CRITICAL(&g_mux);
  fillState(&st);
  portEXIT_CRITICAL(&g_mux);
  return g_pr.putBytes("st", &st, sizeof(st)) == sizeof(st);
}
static bool persistState() {
  if (!g_prOk) return false;
  if (g_prMux) xSemaphoreTake(g_prMux, portMAX_DELAY);
  bool ok = writeStateLocked();
  if (g_prMux) xSemaphoreGive(g_prMux);
  if (!ok) nvsError("estado");
  return ok;
}
static void persistGroups() { persistState(); }   // objetivos / grupos: van dentro de "st"

// Un pesaje: su slot + el estado (2 escrituras a flash).
static void persistSlot(int slot) {
  if (!g_prOk) return;
  PSlot s;
  if (g_prMux) xSemaphoreTake(g_prMux, portMAX_DELAY);
  portENTER_CRITICAL(&g_mux);
  s.peso   = g_log[slot].peso;
  s.precio = g_log[slot].precio;
  s.total  = g_log[slot].total;
  s.epoch  = (int32_t)g_log[slot].epoch;
  s.malla  = g_log[slot].malla;
  s.bin    = g_log[slot].bin;
  portEXIT_CRITICAL(&g_mux);
  char k[8]; snprintf(k, sizeof(k), "s%03d", slot);
  bool ok   = g_pr.putBytes(k, &s, sizeof(s)) == sizeof(s);
  bool stOk = writeStateLocked();
  if (g_prMux) xSemaphoreGive(g_prMux);
  if (!ok) nvsError("pesaje");
  if (!stOk) nvsError("estado");
}

static void persistClear() {
  if (!g_prOk) return;
  if (g_prMux) xSemaphoreTake(g_prMux, portMAX_DELAY);
  g_pr.clear();
  if (g_prMux) xSemaphoreGive(g_prMux);
}

static void loadSlots(const PMeta& m);
static void loadSumAndGroups(const PState* st);
static void persistLoad() {
  g_prMux = xSemaphoreCreateMutex();
  g_prOk  = g_pr.begin("weigh", false);
  if (!g_prOk) {
    Serial.println("[pesaje] NVS no abre: el historial NO se guardara al apagar");
    diagLog("NVS no abre: pesajes solo en RAM y archivo");
    return;
  }
  PState st;
  bool haveSt = g_pr.getBytes("st", &st, sizeof(st)) == sizeof(st);
  PMeta m = {};
  bool haveMeta = haveSt;
  if (haveSt) { m.total = st.total; m.head = st.head; m.count = st.count; }
  else        haveMeta = g_pr.getBytes("meta", &m, sizeof(m)) == sizeof(m);   // formato anterior
  if (haveMeta && (m.count < 0 || m.count > WEIGH_LOG_SIZE)) haveMeta = false; // basura -> ignora
  if (haveMeta) loadSlots(m);
  loadSumAndGroups(haveSt ? &st : nullptr);
  if (!haveSt) {                       // formato anterior (o vacio): se migra YA al blob "st"
    if (persistState()) {
      static const char* old[] = { "meta", "wsum", "grp", "mId", "bId", "mSum", "bSum",
                                   "mTgt", "bTgt", "mLast", "bLast" };
      for (const char* k : old) if (g_pr.isKey(k)) g_pr.remove(k);
      if (haveMeta) Serial.println("[pesaje] NVS migrado al formato agrupado (\"st\")");
    }
  }
  Serial.printf("[pesaje] historial cargado de NVS: %d en buffer, %lu total, suma %.2f\n",
                g_count, (unsigned long)g_total, (double)g_sumAll);
}

// buffer de pesajes (slots) segun los contadores
static void loadSlots(const PMeta& m) {
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
}

// suma acumulada y grupos: del blob "st" si existe; si no, de las claves anteriores
static void loadSumAndGroups(const PState* st) {
  if (st) {
    g_sumAll = st->wsum;
    g_mallaId = st->g.mallaId ? st->g.mallaId : 1; g_binId = st->g.binId ? st->g.binId : 1;
    g_mallaSum = st->g.mallaSum; g_binSum = st->g.binSum;
    g_mallaTarget = st->g.mallaTarget; g_binTarget = st->g.binTarget;
    g_mallaLast = st->g.mallaLast; g_binLast = st->g.binLast;
    return;
  }
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
  }
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

// ---------------------- archivo permanente (ver histarch.h) ----------------------
// Pasa al archivo los pesajes del buffer que todavia no estan (ids > ultimo
// archivado), del mas viejo al mas nuevo y con UNA sola escritura.  Se llama en
// lotes (HIST_BATCH) y al arrancar.  Mientras esperan, esos pesajes ya estan a
// salvo en el buffer (NVS): si se apaga antes, se archivan en el proximo arranque.
// Si el archivo no esta disponible, no hace nada.
static void archiveFlushLocked();
static void archiveFlushPending() {
  if (!histWritable()) return;
  if (!histLock(0)) return;            // la web esta usando el archivo: queda para el proximo lote
  archiveFlushLocked();
  histUnlock();
}
static void archiveFlushLocked() {
  if (!histWritable()) return;
  uint32_t lastId = histLastId();
  HRec* tmp = (HRec*)malloc(sizeof(HRec) * WEIGH_LOG_SIZE);
  if (!tmp) return;
  int cnt = 0;
  portENTER_CRITICAL(&g_mux);
  for (int k = g_count - 1; k >= 0; k--) {                       // del mas viejo al mas nuevo
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    uint32_t id = (uint32_t)(g_total - k);
    if (id <= lastId || g_log[idx].epoch == -1) continue;        // ya archivado / borrado antes de archivar
    HRec& r = tmp[cnt++];
    r.id     = id;
    r.peso   = g_log[idx].peso;
    r.precio = g_log[idx].precio;
    r.total  = g_log[idx].total;
    r.epoch  = (int32_t)g_log[idx].epoch;
    r.malla  = g_log[idx].malla;
    r.bin    = g_log[idx].bin;
  }
  portEXIT_CRITICAL(&g_mux);
  if (cnt > 0) {
    uint32_t t0 = millis();
    size_t saved = histAppendMany(tmp, (size_t)cnt);
    if (saved > 0 || !histFull())            // lleno: ya se aviso, no repetir en cada pesaje
      Serial.printf("[hist] +%u de %d pesajes al archivo (%lu ms; %lu registros, %u%% usado)\n",
                    (unsigned)saved, cnt, (unsigned long)(millis() - t0),
                    (unsigned long)histCount(), (unsigned)histUsedPct());
  }
  free(tmp);
}

// Recalcula g_alive y la tabla de subtotales: todo el archivo (sin borrados) + lo
// del buffer que aun no esta archivado.  Sin archivo, queda solo lo del buffer.
static bool rebuildVisit(const HRec& r, void*) {
  aggAdd(r.bin, r.malla, r.peso);
  g_alive++;
  return true;
}
static void rebuildTotals() {
  g_aggN = 0;
  g_alive = 0;
  histForEach(rebuildVisit, nullptr);
  uint32_t lastId = histLastId();
  for (int k = g_count - 1; k >= 0; k--) {
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    uint32_t id = (uint32_t)(g_total - k);
    if (id > lastId && g_log[idx].epoch != -1) {
      aggAdd(g_log[idx].bin, g_log[idx].malla, g_log[idx].peso);
      g_alive++;
    }
  }
}

// Monta el archivo y lo deja consistente con el buffer cargado de NVS.
static void archiveBoot() {
  if (!histBegin()) return;                                      // sin archivo: todo como antes
  if (histLastId() > g_total) {
    // El contador de NVS va por detras del archivo (NVS danado o borrado por fuera).
    // Nunca se descarta el archivo: se continua la numeracion desde su ultimo id.
    // El buffer de NVS queda atras (sus pesajes ya estan en el archivo) y se vacia.
    Serial.printf("[hist] AVISO: contador NVS (%lu) por detras del archivo (%lu) -> se continua desde el archivo\n",
                  (unsigned long)g_total, (unsigned long)histLastId());
    portENTER_CRITICAL(&g_mux);
    g_count = 0; g_head = 0; g_total = histLastId();
    portEXIT_CRITICAL(&g_mux);
  }
  for (int k = g_count - 1; k >= 0; k--) {                       // borrados que el buffer sabe y el archivo no
    int idx = (g_head - 1 - k + WEIGH_LOG_SIZE * 2) % WEIGH_LOG_SIZE;
    uint32_t id = (uint32_t)(g_total - k);
    if (g_log[idx].epoch == -1 && id <= histLastId()) histMarkDeleted(id);
  }
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
  aggAdd(curBin, curMalla, peso);             // subtotales de grupo sobre todo el historial
  g_alive++;
  w = g_log[slot];
  portEXIT_CRITICAL(&g_mux);
  persistSlot(slot);                          // slot + estado (contadores, suma, grupos)
  // al archivo permanente en lotes: asi hay pocas escrituras a flash.  Mientras
  // tanto el pesaje ya esta a salvo en el buffer (NVS).
  if (histWritable() && (uint32_t)(w.id - histLastId()) >= HIST_BATCH) archiveFlushPending();
  return w;
}

// ------------------------------- API lectura ------------------------------
void weighBegin() {
  persistLoad();          // buffer + contadores desde NVS
  archiveBoot();          // monta el archivo permanente (si se puede)
  rebuildTotals();        // nº de muestras y subtotales sobre TODO el historial
  archiveFlushPending();  // lo del buffer que aun no estaba en el archivo
}

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
  portENTER_CRITICAL(&g_mux);
  uint32_t n = g_alive;                          // todo el historial, no solo el buffer
  portEXIT_CRITICAL(&g_mux);
  return (int)n;
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
    aggSub(g_log[target].bin, g_log[target].malla, g_log[target].peso);
    if (g_alive > 0) g_alive--;
    g_log[target].epoch = -1;                    // marca de borrada
    relatch();
  }
  portEXIT_CRITICAL(&g_mux);
  if (target >= 0) {
    persistSlot(target);
    // con el candado: si la tarea del pesaje estaba archivando justo esta fila, se
    // espera a que termine y entonces ya figura como archivada -> se marca borrada
    histLock(HIST_WAIT_FOREVER);
    if (id <= histLastId()) histMarkDeleted(id); // ya estaba en el archivo: se marca como borrado
    histUnlock();
    Serial.printf("[web] fila #%lu borrada\n", (unsigned long)id);
  }
  return target >= 0;
}

void weighClear() {
  portENTER_CRITICAL(&g_mux);
  g_count = 0; g_head = 0; g_total = 0; g_latched = NAN; g_sumAll = 0;
  g_aggN = 0; g_alive = 0;
  g_mallaId = 1; g_binId = 1; g_mallaSum = 0; g_binSum = 0;
  g_mallaLast = NAN; g_binLast = NAN;
  // g_mallaTarget / g_binTarget NO se tocan: los objetivos puestos por el
  // operario se conservan aunque se borre todo el historial.
  portEXIT_CRITICAL(&g_mux);
  g_resetPending = true;     // la tarea del sniffer hace smReset() en su proxima vuelta
  histLock(HIST_WAIT_FOREVER);
  histClear();        // primero el archivo permanente: si se corta la luz a medias, en el peor caso
  histUnlock();
                      // reaparecen las ultimas WEIGH_LOG_SIZE filas del buffer, no todo el historial
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

// ---------------- historial completo (archivo + buffer) ----------------
struct AllCtx { WeighVisit cb; void* ctx; bool stop; };

static bool allVisit(const HRec& r, void* p) {
  AllCtx* a = (AllCtx*)p;
  Weighing w;
  w.peso = r.peso; w.precio = r.precio; w.total = r.total;
  w.ms = 0; w.epoch = r.epoch; w.id = r.id;
  w.malla = r.malla; w.bin = r.bin;
  if (!a->cb(w, a->ctx)) { a->stop = true; return false; }
  return true;
}

// Con el candado del archivo tomado durante todo el recorrido: la tarea del pesaje
// no archiva mientras tanto (lo deja para el proximo lote), asi que el corte por id
// entre "archivo" y "buffer" es exacto: ni faltan ni se repiten filas.
void weighForEachAll(WeighVisit cb, void* ctx) {
  AllCtx a = { cb, ctx, false };
  Weighing* w = (Weighing*)malloc(sizeof(Weighing) * WEIGH_LOG_SIZE);
  histLock(HIST_WAIT_FOREVER);
  uint32_t lastId = histLastId();                // corte: <= lastId sale del archivo, > del buffer
  histForEach(allVisit, &a);                     // 1) lo archivado, del mas viejo al mas nuevo
  if (!a.stop && w) {
    int n = weighGet(w, WEIGH_LOG_SIZE);         // 2) reciente primero, sin borrados, con id
    for (int i = n - 1; i >= 0; i--)
      if (w[i].id > lastId && !cb(w[i], ctx)) break;
  }
  histUnlock();
  free(w);
}

bool weighGroupSum(uint16_t bin, uint16_t malla, float* sum, uint32_t* cnt) {
  bool ok = false;
  portENTER_CRITICAL(&g_mux);
  GAgg* g = aggFind(bin, malla);
  if (g) { *sum = (float)g->sum; *cnt = g->cnt; ok = true; }
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

bool weighBinSum(uint16_t bin, float* sum, uint32_t* cnt) {
  double s = 0; uint32_t c = 0; bool ok = false;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < g_aggN; i++)
    if (g_agg[i].bin == bin) { s += g_agg[i].sum; c += g_agg[i].cnt; ok = true; }
  portEXIT_CRITICAL(&g_mux);
  if (ok) { *sum = (float)s; *cnt = c; }
  return ok;
}

void weighArchiveInfo(WeighArchiveInfo* out) {
  out->ok     = histWritable();
  out->full   = histFull();
  out->pct    = histUsedPct();
  out->stored = histCount();
}

// ===========================================================================
//  MAQUINA DE ESTADOS DEL PESAJE  (regla simple)
// ---------------------------------------------------------------------------
//   VACIA     : nada en la plataforma, o menos de WEIGH_MIN_LOAD (una gaveta pesa
//               siempre mas: por debajo es tara, ruido o mover la balanza).
//   CON CARGA : empieza al llegar a WEIGH_MIN_LOAD.  Se busca el valor ESTABLE
//               (ventana tolerante) y se recuerda el maximo.
//   Al pasar por CERO o quedar NEGATIVA (<= WEIGH_ZERO_THRESH durante WEIGH_ZERO_N
//   lecturas seguidas) se cierra la gaveta:
//     1) si hubo valor estable >= WEIGH_MIN_LOAD -> se guarda ese ('r' en /log);
//     2) si no lo hubo -> el valor donde la lectura MAS TIEMPO estuvo ('e ... sost'):
//        una presion de la mano antes de quitar la gaveta es un pico corto y no gana;
//     3) caso especial "pausa a mitad de colocacion": un estable que estuvo quieto muy
//        poco (< WEIGH_PLAUS_HOLD_MS) y despues el peso subio mucho (> 1/WEIGH_PLAUS_FRAC)
//        -> el real llego despues sin asentarse: WEIGH_RESCUE_FRAC x maximo ('e ... x0.92').
//   Todo solo si la gaveta estuvo puesta >= WEIGH_MIN_MS (un golpe de la mano no es
//   una gaveta).  Si no, queda como PERDIDO en /log.  Lecturas > WEIGH_MAX_LOAD se
//   tratan como display ilegible (prueba de display "888.8", presion muy fuerte).
//   En campo nunca se presiona TARA con una gaveta llena: no hace falta distinguir
//   "retiro" de "tara".
// ===========================================================================
// how: 0 = valor estable; 'q' = sostenido quieto (>= 2 refrescos); 's' = sostenido laxo; 'x' = estimado WEIGH_RESCUE_FRAC x maximo
static void commitWeighing(float peso, float precio, float total, char how, uint32_t dur) {
  if (isnan(peso) || peso <= 0) {                        // guarda defensiva: nunca un pesaje sin valor
    diagLog("DESCARTE sin valor carga=%lu", (unsigned long)dur);
    return;
  }
  Weighing w = logAppend(peso, precio, total, millis());
  Serial.printf("[pesaje] %s -> GUARDADO  PESO %.2f  PRECIO %.2f  TOTAL %.2f\n",
                how ? "ESTIMADO" : "estable", (double)w.peso, (double)w.precio, (double)w.total);
  g_diagCnt.traysOk++;
  if (how == 'q')
    diagLog("OK %.1f e carga=%lu max=%.1f quieto", (double)w.peso, (unsigned long)dur, (double)g_ldMax);
  else if (how == 's')
    diagLog("OK %.1f e carga=%lu max=%.1f sost", (double)w.peso, (unsigned long)dur, (double)g_ldMax);
  else if (how == 'x')
    diagLog("OK %.1f e carga=%lu max=%.1f x%.2f", (double)w.peso, (unsigned long)dur,
            (double)g_ldMax, (double)WEIGH_RESCUE_FRAC);
  else
    diagLog("OK %.1f r carga=%lu 1a=%lu n=%d rech=%d", (double)w.peso, (unsigned long)dur,
            (unsigned long)(g_capMs ? g_capMs - g_loadMs : 0), g_capN, g_rejN);
  if (g_commitCb) g_commitCb(w);
}

// La gaveta estuvo puesta pero no se pudo guardar: se deja constancia para afinar.
// where: corta (< WEIGH_MIN_MS) | liviano (estimado < WEIGH_MIN_LOAD) | display (se apago).
// cap=N : cuantas veces llego a asentar (0 = nunca).
static void logLost(const char* where, uint32_t dur) {
  if (g_ldN < 3) return;                                 // un roce de milisegundos, no una gaveta
  g_diagCnt.traysLost++;
  diagLog("PERDIDO(%s) carga=%lu max=%.1f n=%d r=%d cap=%d", where, (unsigned long)dur,
          (double)g_ldMax, g_ldN, g_rejN, g_capN);
}

// Caso "pausa a mitad de colocacion" (visto en campo el 2026-09-30 a 5.3 lb; hoy una pausa
// por debajo de WEIGH_MIN_LOAD ya no cuenta, esto cubre las de 12 lb o mas): el estable duro
// muy poco y despues el peso subio mucho por encima -> ese estable no era el peso real.
// Duracion del estable = tramo aceptado + la ventana que hizo falta para el 1er acepto.
// Si estuvo quieto WEIGH_PLAUS_HOLD_MS o mas, una subida posterior es una PRESION de la mano
// y no lo anula.
static bool isPauseValue() {
  return !isnan(g_sP) && !isnan(g_maxPost) && g_sP < WEIGH_PLAUS_FRAC * g_maxPost &&
         (g_lastAccMs - g_capMs) + WEIGH_WIN_MS < (uint32_t)WEIGH_PLAUS_HOLD_MS;
}

// Valor donde la lectura mas tiempo estuvo QUIETA (ventana de +-1 casilla = ~+-0,75 lb);
// NAN si nunca estuvo quieta.
static float sustainedValue(const uint32_t* cnt, const float* acc) {
  int best = -1; uint32_t bestN = 0;
  for (int i = 0; i < SUS_BINS; i++) {
    uint32_t n = cnt[i] + (i > 0 ? cnt[i - 1] : 0) + (i + 1 < SUS_BINS ? cnt[i + 1] : 0);
    if (n > bestN) { bestN = n; best = i; }
  }
  if (best < 0) return NAN;
  float sum = 0; uint32_t n = 0;
  for (int i = best - 1; i <= best + 1; i++)
    if (i >= 0 && i < SUS_BINS) { sum += acc[i]; n += cnt[i]; }
  return n ? roundf(sum / n * 10.0f) / 10.0f : NAN;
}

// Cierra la gaveta en curso y decide que se guarda (ver la regla arriba).
// passedZero = false cuando se cierra porque el display quedo ilegible: no paso por cero.
static void closeLoad(uint32_t now, bool passedZero) {
  // duracion: hasta ahora, o hasta que el display dejo de dar datos (sin pasar por cero)
  uint32_t dur = ((passedZero || !g_badMs) ? now : g_badMs) - g_loadMs;
  bool pause = isPauseValue();
  if (pause) g_sP = g_sPr = g_sT = NAN;
  float est = pause ? NAN : sustainedValue(g_qN, g_qSum);  // quieto >= 2 refrescos
  char how = 'q';
  if (!pause && isnan(est)) { est = sustainedValue(g_susN, g_susSum); how = 's'; }  // como antes
  if (isnan(est)) {                                        // pausa, o nunca estuvo quieta: x maximo
    if (WEIGH_RESCUE_FRAC > 0 && !isnan(g_ldMax)) est = roundf(g_ldMax * WEIGH_RESCUE_FRAC * 10.0f) / 10.0f;
    how = 'x';
  }
  if (!passedZero) {
    // display congelado o ilegible con la gaveta puesta: se guarda lo mejor que se vio
    // (el estable; si no hubo, el estimado) y no se acepta otra gaveta hasta ver cero.
    diagLog("DISPLAY sin datos %lu ms con gaveta: se cierra", (unsigned long)(now - g_badMs));
    g_waitZero = true;
  }
  if (dur < (uint32_t)WEIGH_MIN_MS) {
    logLost("corta", dur);                                 // un golpe de la mano, no una gaveta
  } else if (!isnan(g_sP)) {
    commitWeighing(g_sP, g_sPr, g_sT, 0, dur);             // valor estable (siempre >= WEIGH_MIN_LOAD)
  } else if (!isnan(est) && est >= WEIGH_MIN_LOAD) {
    float tot = isnan(g_ldPr) ? NAN : roundf(est * g_ldPr * 100.0f) / 100.0f;
    commitWeighing(est, g_ldPr, tot, how, dur);            // valor estimado
  } else {
    logLost("liviano", dur);                               // por debajo de WEIGH_MIN_LOAD: tara / nada
  }
  smReset();
}

static void smStartLoad(uint32_t now) {
  smReset();
  g_phase = PH_LOAD;
  g_state = WS_RISING;
  g_loadMs = now;
  Serial.println("[pesaje] carga detectada, esperando a que se estabilice...");
}

void weighFeed(float peso, float precio, float total, bool stableHint, bool valid, bool fresh) {
  (void)stableHint;
  if (g_resetPending) { g_resetPending = false; smReset(); g_waitZero = false; }   // pedido por weighClear()
  uint32_t now = millis();
  if (fresh) g_lastFreshMs = now;
  bool frozen = g_lastFreshMs && (now - g_lastFreshMs > (uint32_t)WEIGH_FRAME_TIMEOUT_MS);

  // ---- display ilegible / dormido / congelado (sin refrescos nuevos) ----
  if (!valid || frozen || isnan(peso) || peso > WEIGH_MAX_LOAD) {  // ilegible, fuera de rango (888.8) o congelado
    if (g_badMs == 0) g_badMs = now;
    if (g_phase == PH_LOAD && now - g_badMs >= WEIGH_INVALID_MS)
      closeLoad(now, false);                             // no paso por cero: no se guarda
    return;
  }
  g_badMs = 0;

  // ------------------------------------------------------------ VACIA
  if (g_phase == PH_EMPTY) {
    if (g_waitZero) {                                    // la gaveta cerrada por display puede seguir encima
      static uint32_t since = 0; static bool told = false;
      if (!since) since = now;
      if (peso <= WEIGH_ZERO_THRESH) { g_waitZero = false; since = 0; told = false; return; }
      if (!told && now - since > 10000) {                // aviso (una vez): no se aceptan gavetas aun
        told = true;
        diagLog("ESPERANDO CERO tras cierre por display (peso %.1f)", (double)peso);
      }
      return;
    }
    if (peso < WEIGH_MIN_LOAD) return;
    smStartLoad(now);                                    // y esta misma lectura ya cuenta (abajo)
  }

  // ------------------------------------------------------------ CON CARGA
  if (peso <= WEIGH_ZERO_THRESH) {                       // paso por cero o quedo negativa...
    if (fresh && ++g_zeroN >= WEIGH_ZERO_N) closeLoad(now, true); // ...varios REFRESCOS seguidos: no un glitch
    return;
  }
  g_zeroN = 0;
  g_ldN++;
  if (isnan(g_ldMax) || peso > g_ldMax) g_ldMax = peso;
  if (isnan(g_ldMin) || peso < g_ldMin) g_ldMin = peso;
  g_ldPr = precio;
  if (isnan(g_maxPost) || peso > g_maxPost) g_maxPost = peso;
  // tiempo pasado en cada valor, contando solo lecturas casi QUIETAS (la subida, la bajada
  // al retirar y el pico de una presion cambian rapido y no cuentan)
  if (peso >= WEIGH_MIN_LOAD && !isnan(g_prevP) && fabsf(peso - g_prevP) <= WEIGH_BAND) {
    int bi = (int)((peso - WEIGH_MIN_LOAD) / SUS_STEP);
    if (bi >= 0 && bi < SUS_BINS) { g_susN[bi]++; g_susSum[bi] += peso; }
  }
  g_prevP = peso;
  if (fresh) {
    if (peso >= WEIGH_MIN_LOAD && !isnan(g_prevF) && fabsf(peso - g_prevF) <= WEIGH_BAND) {
      int bi = (int)((peso - WEIGH_MIN_LOAD) / SUS_STEP);
      if (bi >= 0 && bi < SUS_BINS) { g_qN[bi]++; g_qSum[bi] += peso; }
    }
    g_prevF = peso;
  }

  // ---- captura por VENTANA TOLERANTE ----
  // Un valor se da por ASENTADO si las lecturas de los ultimos WEIGH_WIN_MS
  // varian menos de WEIGH_BAND (un escurrido de 0,1 lb/s cabe de sobra).
  // La referencia es el primer valor asentado de ESTA gaveta.  Despues solo se
  // aceptan valores asentados a menos de max(WEIGH_TOL_ABS, WEIGH_TOL_PCT %) de
  // ella: la caida al retirar la gaveta ni forma un tramo asentado ni entra en ese
  // margen -> nunca se guarda un valor de la bajada.  Solo en los primeros
  // WEIGH_PLACE_MS la referencia puede SUBIR (gaveta posada despacio).
  winPush(now, peso);
  // un valor por debajo del minimo nunca es "el estable" de una gaveta: asi la cola de la
  // bajada (0,1-0,3 lb un rato, sin tara) no puede ganarle al valor estimado
  if (peso < WEIGH_MIN_LOAD || !winFlat(now)) return;
  bool accept = false;
  if (isnan(g_refP)) {
    g_refP = peso;
    accept = true;
  } else {
    float tol = fmaxf(WEIGH_TOL_ABS, g_refP * (WEIGH_TOL_PCT / 100.0f));
    if (fabsf(peso - g_refP) <= tol) {
      accept = true;
    } else if (peso > g_refP + tol && (now - g_loadMs) <= WEIGH_PLACE_MS) {
      g_refP = peso;
      accept = true;
    } else {
      g_rejN++;                                          // fuera de banda: se ignora
    }
  }
  if (!accept) return;
  bool nuevo = isnan(g_sP) || fabsf(peso - g_sP) >= WEIGH_EPS;
  g_sP = peso; g_sPr = precio; g_sT = total;
  g_state = WS_STABLE;
  if (!g_capMs) g_capMs = now;
  g_lastAccMs = now;
  g_capN++;
  g_maxPost = NAN;
  if (nuevo)
    Serial.printf("[pesaje] peso estable: %.2f  (precio %.2f  total %.2f)\n",
                  (double)peso, (double)precio, (double)total);
}
