// ===========================================================================
//  histarch.cpp  -  archivo permanente de pesajes en LittleFS (ver histarch.h)
// ===========================================================================
#include "config.h"
#include "histarch.h"

#ifndef WEIGH_ARCHIVE
#define WEIGH_ARCHIVE 1
#endif
#ifndef WEIGH_PERSIST
#define WEIGH_PERSIST 1
#endif

#if WEIGH_ARCHIVE && WEIGH_PERSIST

#include <LittleFS.h>
#include <vector>
#include <algorithm>

static_assert(sizeof(HRec) == 24, "HRec debe medir 24 bytes");

static const char* HIST_FILE = "/hist.bin";
static const char* DEL_FILE  = "/del.bin";

// Se usa como maximo este % de la particion: LittleFS necesita bloques libres
// para reescribir el ultimo bloque y compactar metadatos.
#define HIST_CAP_PCT 75

static bool     s_ready    = false;    // montado y legible
static bool     s_writable = false;    // se puede seguir añadiendo
static bool     s_full     = false;
static size_t   s_cap      = 0;        // bytes usables (registros + borrados)
static size_t   s_histBytes = 0;       // bytes de registros COMPLETOS en /hist.bin
static size_t   s_delBytes  = 0;       // tamaño de /del.bin
static uint32_t s_lastId   = 0;        // mayor id archivado
static std::vector<uint32_t> s_del;    // ids borrados, ORDENADO

// Recalcula tamaño y ultimo id leyendo el archivo.  Si tiene una cola rota
// (bytes que no completan un registro) deja de escribir para no desalinear.
static void syncFromFile() {
  s_histBytes = 0;
  s_lastId = 0;
  File h = LittleFS.open(HIST_FILE, "r");
  if (!h) return;
  size_t size  = h.size();
  size_t whole = size - (size % sizeof(HRec));
  s_histBytes = whole;
  if (whole) {
    h.seek(whole - sizeof(HRec));
    HRec last;
    if (h.read((uint8_t*)&last, sizeof(last)) == sizeof(last)) s_lastId = last.id;
  }
  h.close();
  if (size != whole) {
    s_writable = false;
    Serial.printf("[hist] AVISO: /hist.bin con cola rota (%u bytes sobrantes) -> solo lectura\n",
                  (unsigned)(size - whole));
  }
}

static void updateFull() {
  s_full = (s_histBytes + s_delBytes + sizeof(HRec) > s_cap);
}

bool histBegin() {
  s_ready = s_writable = s_full = false;
  s_cap = s_histBytes = s_delBytes = 0;
  s_lastId = 0;
  s_del.clear();

  if (!LittleFS.begin(true)) {         // true = formatea si aun no tiene formato (1ª vez)
    Serial.println("[hist] LittleFS no monta (¿esquema de particiones sin SPIFFS?)"
                   " -> solo el buffer de los ultimos pesajes");
    return false;
  }
  s_cap = (size_t)(((uint64_t)LittleFS.totalBytes() * HIST_CAP_PCT) / 100);

  {                                    // borrados
    File d = LittleFS.open(DEL_FILE, "r");
    if (d) {
      s_delBytes = d.size();
      uint32_t id;
      while (d.read((uint8_t*)&id, sizeof(id)) == sizeof(id)) s_del.push_back(id);
      d.close();
      std::sort(s_del.begin(), s_del.end());
      s_del.erase(std::unique(s_del.begin(), s_del.end()), s_del.end());
    }
  }

  s_ready = true;
  s_writable = true;
  syncFromFile();                      // puede poner s_writable = false si hay cola rota
  updateFull();
  Serial.printf("[hist] archivo listo: %lu registros, ultimo id %lu, %u borrados, "
                "capacidad ~%u registros (%u%% usado)\n",
                (unsigned long)histCount(), (unsigned long)s_lastId,
                (unsigned)s_del.size(), (unsigned)(s_cap / sizeof(HRec)),
                (unsigned)histUsedPct());
  return true;
}

bool     histReady()    { return s_ready; }
bool     histWritable() { return s_ready && s_writable; }
bool     histFull()     { return s_full; }
uint32_t histCount()    { return (uint32_t)(s_histBytes / sizeof(HRec)); }
uint32_t histLastId()   { return s_lastId; }

uint8_t histUsedPct() {
  if (!s_cap) return 0;
  size_t used = s_histBytes + s_delBytes;
  size_t pct  = (used * 100) / s_cap;
  return (uint8_t)(pct > 100 ? 100 : pct);
}

size_t histAppendMany(const HRec* r, size_t n) {
  if (!s_ready || !s_writable || n == 0) return 0;

  size_t used = s_histBytes + s_delBytes;
  size_t room = (s_cap > used) ? (s_cap - used) / sizeof(HRec) : 0;
  if (room < n) { s_full = true; n = room; }     // prefijo que quepa: sin huecos
  if (n == 0) return 0;

  size_t before = s_histBytes;
  size_t bytes  = n * sizeof(HRec);
  File f = LittleFS.open(HIST_FILE, "a");
  if (!f) {
    s_writable = false;
    Serial.println("[hist] ERROR: no se pudo abrir /hist.bin para añadir -> se deja de archivar");
    return 0;
  }
  size_t w = f.write((const uint8_t*)r, bytes);
  f.close();

  syncFromFile();                                // lo que de verdad quedo en el archivo
  if (s_histBytes != before + bytes) {
    s_writable = false;
    Serial.printf("[hist] ERROR: escritura incompleta (%u de %u bytes) -> se deja de archivar\n",
                  (unsigned)w, (unsigned)bytes);
  }
  updateFull();
  if (s_histBytes < before) return 0;            // no deberia pasar; evita restar por debajo de cero
  return (s_histBytes - before) / sizeof(HRec);
}

bool histIsDeleted(uint32_t id) {
  return std::binary_search(s_del.begin(), s_del.end(), id);
}

void histMarkDeleted(uint32_t id) {
  if (!s_ready) return;
  auto it = std::lower_bound(s_del.begin(), s_del.end(), id);
  if (it != s_del.end() && *it == id) return;    // ya estaba
  s_del.insert(it, id);
  if (!s_writable) return;                       // solo en RAM hasta el reinicio
  File f = LittleFS.open(DEL_FILE, "a");
  if (!f) return;
  size_t w = f.write((const uint8_t*)&id, sizeof(id));
  f.close();
  if (w == sizeof(id)) s_delBytes += sizeof(id);
}

void histClear() {
  s_del.clear();
  s_histBytes = s_delBytes = 0;
  s_lastId = 0;
  s_full = false;
  if (!s_ready) return;
  LittleFS.remove(HIST_FILE);
  LittleFS.remove(DEL_FILE);
  if (LittleFS.exists(HIST_FILE) || LittleFS.exists(DEL_FILE)) {
    s_writable = false;
    Serial.println("[hist] ERROR: no se pudo borrar el archivo -> se deja de archivar");
    return;
  }
  s_writable = true;                             // archivo nuevo, alineado
}

void histForEach(HistVisit cb, void* ctx) {
  if (!s_ready) return;
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) return;
  const size_t CH = 32;                          // 32 registros = 768 bytes de pila por lectura
  HRec buf[CH];
  size_t left = f.size() / sizeof(HRec);         // solo registros completos
  while (left) {
    size_t want = (left < CH) ? left : CH;
    size_t got  = f.read((uint8_t*)buf, want * sizeof(HRec)) / sizeof(HRec);
    if (!got) break;
    for (size_t i = 0; i < got; i++) {
      if (histIsDeleted(buf[i].id)) continue;
      if (!cb(buf[i], ctx)) { f.close(); return; }
    }
    left -= got;
  }
  f.close();
}

#else   // WEIGH_ARCHIVE 0 (o sin persistencia): todo inerte

bool     histBegin()                           { return false; }
bool     histReady()                           { return false; }
bool     histWritable()                        { return false; }
bool     histFull()                            { return false; }
uint8_t  histUsedPct()                         { return 0; }
uint32_t histCount()                           { return 0; }
uint32_t histLastId()                          { return 0; }
size_t   histAppendMany(const HRec*, size_t)   { return 0; }
void     histMarkDeleted(uint32_t)             {}
bool     histIsDeleted(uint32_t)               { return false; }
void     histClear()                           {}
void     histForEach(HistVisit, void*)         {}

#endif
