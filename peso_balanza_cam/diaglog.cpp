// ===========================================================================
//  diaglog.cpp  -  registro de diagnostico en RAM (ver diaglog.h)
// ===========================================================================
#include "diaglog.h"

DiagCounters g_diagCnt = {};

#if DIAG_LOG

#include <stdarg.h>
#include <time.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_app_desc.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "freertos/FreeRTOS.h"

#ifndef DIAG_LOG_LINES
#define DIAG_LOG_LINES 1024     // config.h lo puede ajustar (costo: *64 bytes de RAM)
#endif
#define DIAG_LINES DIAG_LOG_LINES   // lineas que se conservan (las mas recientes)
#define DIAG_LEN   64               // caracteres por linea, con la hora incluida

static char         s_lines[DIAG_LINES][DIAG_LEN];
static uint16_t     s_head  = 0;
static uint16_t     s_n     = 0;
static uint32_t     s_total = 0;
static portMUX_TYPE s_mux   = portMUX_INITIALIZER_UNLOCKED;

// ---- caja negra: copia de las ultimas BB_LINES lineas en memoria RTC ----
// RTC_NOINIT: el arranque NO la borra, asi que tras un cuelgue / watchdog / reinicio
// por software conserva lo que paso justo antes.  Al quitar la alimentacion se pierde
// (queda basura: por eso el numero magico).
#define BB_LINES 96
// el numero magico lleva el formato (lineas x largo): un firmware con otra caja negra no lee basura
#define BB_MAGIC (0xB1AC0000u ^ ((uint32_t)BB_LINES << 8) ^ (uint32_t)DIAG_LEN)
struct BlackBox {
  uint32_t magic;
  uint16_t head, n;
  uint32_t lastMs;               // millis() de la ultima linea (cuanto duro la sesion)
  int64_t  lastEpoch;            // hora de la ultima linea (0 = el movil aun no la puso)
  char     lines[BB_LINES][DIAG_LEN];
};
RTC_NOINIT_ATTR static BlackBox s_bb;

static String s_report;          // informe del reinicio anterior (hasta guardarlo en flash)
static bool   s_cdErase = false; // hay un coredump leido: se borra SOLO despues de guardar el informe
static uint32_t s_bootNo = 0;    // nº de arranque (NVS "diag"/"boots"): identifica cada informe
static bool   s_repSaved = false;// se guardo un informe en ESTE arranque (falta anotarle la fecha)
static const char* REP_FILE     = "/reinicios.txt";
static const char* REP_FILE_OLD = "/reinicios_ant.txt";
#define REP_MAX_BYTES 12288      // al pasar de esto se rota (se conservan ~2 archivos)

// "12:03:41 " si el movil ya puso la hora; si no, "+723s " (segundos desde el arranque)
static int stamp(char* out, size_t n) {
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    return snprintf(out, n, "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
  return snprintf(out, n, "+%lus ", (unsigned long)(millis() / 1000));
}

void diagLog(const char* fmt, ...) {
  char buf[DIAG_LEN];
  int n = stamp(buf, sizeof(buf));
  if (n < 0 || n >= (int)sizeof(buf) - 1) n = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  time_t now = time(nullptr);
  portENTER_CRITICAL(&s_mux);                    // copiar 2 x 64 bytes: microsegundos
  memcpy(s_lines[s_head], buf, DIAG_LEN);
  s_head = (s_head + 1) % DIAG_LINES;
  if (s_n < DIAG_LINES) s_n++;
  s_total++;
  if (s_bb.magic == BB_MAGIC) {
    memcpy(s_bb.lines[s_bb.head], buf, DIAG_LEN);
    s_bb.head = (uint16_t)((s_bb.head + 1) % BB_LINES);
    if (s_bb.n < BB_LINES) s_bb.n++;
    s_bb.lastMs = millis();
    s_bb.lastEpoch = (now > 1700000000) ? (int64_t)now : 0;
  }
  portEXIT_CRITICAL(&s_mux);
}

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "ENCENDIDO";
    case ESP_RST_EXT:       return "PIN-RESET";
    case ESP_RST_SW:        return "REINICIO-SW";
    case ESP_RST_PANIC:     return "PANIC/CUELGUE";
    case ESP_RST_INT_WDT:   return "WDT-INT";
    case ESP_RST_TASK_WDT:  return "WDT-TAREA";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "CAIDA-DE-TENSION";
    default:                return "OTRO";
  }
}

// Causas de excepcion Xtensa mas comunes (EXCCAUSE)
static const char* excName(uint32_t c) {
  switch (c) {
    case 0:  return "IllegalInstruction";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 6:  return "IntegerDivideByZero";
    case 9:  return "LoadStoreAlignment";
    case 28: return "LoadProhibited (puntero invalido al leer)";
    case 29: return "StoreProhibited (puntero invalido al escribir)";
    default: return "otra";
  }
}

// Motivos que NO son un reinicio inesperado: encendido normal y la carga por USB.
static bool expectedReset(esp_reset_reason_t r) {
  return r == ESP_RST_POWERON || r == ESP_RST_USB || r == ESP_RST_DEEPSLEEP;
}

// Arma s_report con el motivo, el fallo del coredump (si lo hay) y la caja negra.
static void buildReport(esp_reset_reason_t r) {
  bool bbOk = s_bb.magic == BB_MAGIC && s_bb.head < BB_LINES && s_bb.n <= BB_LINES;
  bool crash = false, unreadable = false;
  esp_core_dump_summary_t* cd = nullptr;
  if (esp_core_dump_image_check() == ESP_OK) {
    cd = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    crash = cd && esp_core_dump_get_summary(cd) == ESP_OK;
    unreadable = !crash;                         // hay coredump pero no se pudo leer
    s_cdErase = true;
  }
  if (expectedReset(r) && !crash && !unreadable) { free(cd); return; }   // nada que contar

  char tmp[160];
  s_report.reserve(1024 + (bbOk ? s_bb.n * (DIAG_LEN + 1) : 0));
  snprintf(tmp, sizeof(tmp), "== REINICIO motivo=%s(%d) · arranque #%lu ==\n", resetName(r), (int)r,
           (unsigned long)s_bootNo);
  s_report += tmp;
  if (bbOk) {
    snprintf(tmp, sizeof(tmp), "la sesion anterior duro ~%lu s", (unsigned long)(s_bb.lastMs / 1000));
    s_report += tmp;
    if (s_bb.lastEpoch > 1700000000LL && s_bb.lastEpoch < 4102444800LL) {   // 2023..2100: si no, basura
      time_t t = (time_t)s_bb.lastEpoch; struct tm tmv;
      if (localtime_r(&t, &tmv)) {
        strftime(tmp, sizeof(tmp), "; ultima linea %Y-%m-%d %H:%M:%S", &tmv);
        s_report += tmp;
      }
    }
    s_report += "\n";
  } else {
    s_report += "(sin caja negra: se perdio la alimentacion o es el primer arranque)\n";
  }
  if (crash) {
    snprintf(tmp, sizeof(tmp), "FALLO en tarea '%.16s' pc=0x%08lx causa=%lu %s dir=0x%08lx\n",
             cd->exc_task, (unsigned long)cd->exc_pc, (unsigned long)cd->ex_info.exc_cause,
             excName(cd->ex_info.exc_cause), (unsigned long)cd->ex_info.exc_vaddr);
    s_report += tmp;
    s_report += "backtrace:";
    for (uint32_t i = 0; i < cd->exc_bt_info.depth && i < 16; i++) {
      snprintf(tmp, sizeof(tmp), " 0x%08lx", (unsigned long)cd->exc_bt_info.bt[i]);
      s_report += tmp;
    }
    s_report += cd->exc_bt_info.corrupted ? " (incompleto)\n" : "\n";
    char why[96] = "";
    if (esp_core_dump_get_panic_reason(why, sizeof(why)) == ESP_OK && why[0]) {
      s_report += "motivo del sistema: "; s_report += why; s_report += "\n";
    }
    char mine[17] = "";
    esp_app_get_elf_sha256(mine, sizeof(mine));
    char theirs[17];
    memcpy(theirs, cd->app_elf_sha256, 16); theirs[16] = 0;
    if (strncmp(mine, theirs, 16) == 0)
      s_report += "firmware: el mismo que esta cargado (las direcciones valen con su .elf)\n";
    else {
      snprintf(tmp, sizeof(tmp), "firmware: OTRO (%.16s, cargado %.16s): fallo de un firmware anterior\n",
               theirs, mine);
      s_report += tmp;
    }
  }
  if (unreadable) s_report += "hay un coredump pero no se pudo leer (de otro firmware o danado)\n";
  free(cd);
  if (bbOk && s_bb.n) {
    snprintf(tmp, sizeof(tmp), "-- ultimas %u lineas antes del reinicio --\n", (unsigned)s_bb.n);
    s_report += tmp;
    uint16_t start = (uint16_t)((s_bb.head + BB_LINES - s_bb.n) % BB_LINES);
    char line[DIAG_LEN];
    for (uint16_t i = 0; i < s_bb.n; i++) {
      memcpy(line, s_bb.lines[(start + i) % BB_LINES], DIAG_LEN);
      line[DIAG_LEN - 1] = 0;
      s_report += line;
      s_report += "\n";
    }
  }
  s_report += "\n";
}

void diagBegin() {
  {                                              // contador de arranques (1 escritura NVS por arranque)
    Preferences p;
    if (p.begin("diag", false)) { s_bootNo = p.getULong("boots", 0) + 1; p.putULong("boots", s_bootNo); p.end(); }
  }
  esp_reset_reason_t r = esp_reset_reason();
  buildReport(r);
  memset(&s_bb, 0, sizeof(s_bb));                // la caja negra empieza de cero en esta sesion
  s_bb.magic = BB_MAGIC;
  diagLog("ARRANQUE #%lu motivo=%s(%d) heap=%lu", (unsigned long)s_bootNo, resetName(r), (int)r,
          (unsigned long)ESP.getFreeHeap());
  if (s_report.length()) diagLog("INFORME del reinicio anterior al final de /log");
}

// Guarda s_report en flash (LittleFS ya montado por weighBegin; si no, se intenta montar).
// Si no se puede, el informe queda en RAM y /log lo muestra igual hasta el proximo reinicio.
void diagBootReport() {
  if (!s_report.length()) return;
  Serial.println("[diag] informe del reinicio anterior:"); Serial.print(s_report);
  if (!LittleFS.begin(false)) { Serial.println("[diag] sin LittleFS: el informe queda solo en RAM"); return; }
  File f = LittleFS.open(REP_FILE, "r");
  size_t cur = f ? f.size() : 0;
  if (f) f.close();
  if (cur + s_report.length() > REP_MAX_BYTES) {   // rotar: el viejo se descarta
    LittleFS.remove(REP_FILE_OLD);
    LittleFS.rename(REP_FILE, REP_FILE_OLD);
  }
  f = LittleFS.open(REP_FILE, "a");
  if (!f) { Serial.println("[diag] no se pudo abrir el informe: queda solo en RAM"); return; }
  size_t w = f.write((const uint8_t*)s_report.c_str(), s_report.length());
  f.close();
  if (w == s_report.length()) {
    Serial.printf("[diag] informe del reinicio guardado (%u bytes)\n", (unsigned)w);
    s_report = String();                         // ya esta en flash: se libera la RAM
    s_repSaved = true;                           // falta la fecha: se anota cuando el movil ponga la hora
    // el coredump se borra SOLO ahora: si no se pudo guardar el informe, se conserva
    // y se vuelve a leer en el proximo arranque
    if (s_cdErase) { esp_core_dump_image_erase(); s_cdErase = false; }
  }
}

// El reloj no se conoce al arrancar (lo pone el movil al abrir el panel): cuando llega
// la hora por primera vez en este arranque, se anota la fecha debajo de su informe.
void diagClockSet() {
  if (!s_repSaved) return;
  s_repSaved = false;
  time_t t = time(nullptr);
  struct tm tmv;
  if (!localtime_r(&t, &tmv)) return;
  char line[96], d[24];
  strftime(d, sizeof(d), "%Y-%m-%d %H:%M", &tmv);
  snprintf(line, sizeof(line), "(arranque #%lu: el panel puso la hora %s, poco despues del reinicio)\n\n",
           (unsigned long)s_bootNo, d);
  File f = LittleFS.open(REP_FILE, "a");
  if (!f) return;
  f.write((const uint8_t*)line, strlen(line));
  f.close();
}

void diagClearReports() {
  LittleFS.remove(REP_FILE);
  LittleFS.remove(REP_FILE_OLD);
  s_report = String();
  s_repSaved = false;
  diagLog("INFORMES de reinicio borrados desde el panel");
}

static void dumpFile(const char* path, DiagSink sink, void* ctx) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  char buf[256];
  int n;
  while ((n = f.read((uint8_t*)buf, sizeof(buf))) > 0) sink(buf, (size_t)n, ctx);
  f.close();
}

void diagDump(DiagSink sink, void* ctx) {
  char hdr[1024];
  int n = snprintf(hdr, sizeof(hdr),
    "== Diagnostico (en RAM; los reinicios inesperados quedan al final, guardados en flash) ==\n"
    "encendido hace %lu s | heap libre %lu (min %lu) | temp chip max %.0f C\n"
    "pesajes guardados %lu | perdidos %lu\n"
    "ventiladores: %s ahora | %lu min encendidos (sin contar el periodo actual)\n"
    "celulares: conexiones %lu | desconexiones %lu\n"
    "bucle: %lu vueltas lentas | max %lu ms | max web %lu ms\n"
    "lineas escritas %lu (se conservan las ultimas %d)\n"
    "motivo WiFi: 8 el celular se fue | 3 deauth | 4 inactividad | 2 auth expirada | 15 handshake\n"
    "OK peso r|e = guardado al pasar por cero (r valor estable, e estimado = max x factor) | carga = ms puesta\n"
    "1a = ms hasta el 1er valor asentado | n = lecturas asentadas | rech = asentadas fuera de banda\n"
    "PERDIDO = gaveta (>= peso minimo) que no se pudo guardar: muy corta o liviana\n"
    "DISPLAY sin datos = display congelado/ilegible con gaveta: se guardo lo ultimo visto (linea OK siguiente)\n"
    "-----\n",
    (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
    (double)g_diagCnt.tempMaxC,
    (unsigned long)g_diagCnt.traysOk, (unsigned long)g_diagCnt.traysLost,
    g_diagCnt.fanOn ? "ON" : "OFF", (unsigned long)(g_diagCnt.fanOnS / 60),
    (unsigned long)g_diagCnt.wifiConn, (unsigned long)g_diagCnt.wifiDisc,
    (unsigned long)g_diagCnt.slowLoops, (unsigned long)g_diagCnt.maxLoopMs, (unsigned long)g_diagCnt.maxWebMs,
    (unsigned long)s_total, DIAG_LINES);
  if (n > 0) sink(hdr, (size_t)((n < (int)sizeof(hdr)) ? n : (int)sizeof(hdr) - 1), ctx);

  uint16_t count, start;
  portENTER_CRITICAL(&s_mux);
  count = s_n;
  start = (uint16_t)((s_head + DIAG_LINES - s_n) % DIAG_LINES);
  portEXIT_CRITICAL(&s_mux);

  char line[DIAG_LEN + 2];
  for (uint16_t i = 0; i < count; i++) {
    portENTER_CRITICAL(&s_mux);
    memcpy(line, s_lines[(start + i) % DIAG_LINES], DIAG_LEN);
    portEXIT_CRITICAL(&s_mux);
    line[DIAG_LEN - 1] = 0;                      // por si una linea quedo sin terminar
    size_t len = strlen(line);
    line[len++] = '\n';
    sink(line, len, ctx);
  }

  // informes de reinicios inesperados (flash), del mas viejo al mas nuevo
  bool inOld = LittleFS.exists(REP_FILE_OLD), inCur = LittleFS.exists(REP_FILE);
  if (!inOld && !inCur && !s_report.length()) {
    const char* m = "\n(ningun reinicio inesperado guardado)\n";
    sink(m, strlen(m), ctx);
    return;
  }
  const char* h2 = "\n===== REINICIOS INESPERADOS (guardados en flash) =====\n";
  sink(h2, strlen(h2), ctx);
  if (inOld) dumpFile(REP_FILE_OLD, sink, ctx);
  if (inCur) dumpFile(REP_FILE, sink, ctx);
  for (size_t i = 0; i < s_report.length(); i += 512) {                  // aun no guardado: a trozos
    size_t n = s_report.length() - i;
    sink(s_report.c_str() + i, n < 512 ? n : 512, ctx);
  }
}

#else   // DIAG_LOG 0: todo inerte

void diagBegin() {}
void diagBootReport() {}
void diagClockSet() {}
void diagClearReports() {}
void diagLog(const char*, ...) {}
void diagDump(DiagSink sink, void* ctx) {
  const char* m = "diagnostico desactivado (DIAG_LOG 0 en config.h)\n";
  sink(m, strlen(m), ctx);
}

#endif
