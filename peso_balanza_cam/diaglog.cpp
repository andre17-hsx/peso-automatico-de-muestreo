// ===========================================================================
//  diaglog.cpp  -  registro de diagnostico en RAM (ver diaglog.h)
// ===========================================================================
#include "diaglog.h"

DiagCounters g_diagCnt = {};

#if DIAG_LOG

#include <stdarg.h>
#include <time.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"

#define DIAG_LINES 256          // lineas que se conservan (las mas recientes)
#define DIAG_LEN   64           // caracteres por linea, con la hora incluida

static char         s_lines[DIAG_LINES][DIAG_LEN];
static uint16_t     s_head  = 0;
static uint16_t     s_n     = 0;
static uint32_t     s_total = 0;
static portMUX_TYPE s_mux   = portMUX_INITIALIZER_UNLOCKED;

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
  portENTER_CRITICAL(&s_mux);                    // solo copiar 64 bytes: microsegundos
  memcpy(s_lines[s_head], buf, DIAG_LEN);
  s_head = (s_head + 1) % DIAG_LINES;
  if (s_n < DIAG_LINES) s_n++;
  s_total++;
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

void diagBegin() {
  esp_reset_reason_t r = esp_reset_reason();
  diagLog("ARRANQUE motivo=%s(%d) heap=%lu", resetName(r), (int)r, (unsigned long)ESP.getFreeHeap());
}

void diagDump(DiagSink sink, void* ctx) {
  char hdr[1024];
  int n = snprintf(hdr, sizeof(hdr),
    "== Diagnostico (en RAM: se borra al reiniciar) ==\n"
    "encendido hace %lu s | heap libre %lu (min %lu)\n"
    "pesajes guardados %lu | perdidos (sin asentar) %lu | taras/descartes %lu\n"
    "celulares: conexiones %lu | desconexiones %lu\n"
    "bucle: %lu vueltas lentas | max %lu ms | max web %lu ms\n"
    "lineas escritas %lu (se conservan las ultimas %d)\n"
    "motivo WiFi: 8 el celular se fue | 3 deauth | 4 inactividad | 2 auth expirada | 15 handshake\n"
    "OK peso r|c|d = guardado (r retiro, c cambio de gaveta, d display cerrado) | carga = ms puesta\n"
    "1a = ms hasta el 1er valor asentado | n = lecturas asentadas | rech = asentadas fuera de banda\n"
    "PERDIDO = gaveta que no llego a asentar | DESCARTE/TARA = tara descartada a proposito\n"
    "-----\n",
    (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
    (unsigned long)g_diagCnt.traysOk, (unsigned long)g_diagCnt.traysLost, (unsigned long)g_diagCnt.tares,
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
}

#else   // DIAG_LOG 0: todo inerte

void diagBegin() {}
void diagLog(const char*, ...) {}
void diagDump(DiagSink sink, void* ctx) {
  const char* m = "diagnostico desactivado (DIAG_LOG 0 en config.h)\n";
  sink(m, strlen(m), ctx);
}

#endif
