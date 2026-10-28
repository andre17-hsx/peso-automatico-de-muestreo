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
#include <unistd.h>                    // truncate() (VFS de LittleFS)
#include <esp_partition.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "diaglog.h"

#define LFS_MOUNT "/littlefs"          // punto de montaje por defecto de LittleFS en Arduino-ESP32
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
static bool     s_mountFailed = false; // hay datos en la particion pero no monta
static SemaphoreHandle_t s_lock = nullptr;

bool histLock(uint32_t waitMs) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();      // 1ª llamada: en setup(), una sola tarea
  if (!s_lock) return false;
  TickType_t t = (waitMs == HIST_WAIT_FOREVER) ? portMAX_DELAY : pdMS_TO_TICKS(waitMs);
  return xSemaphoreTake(s_lock, t) == pdTRUE;
}
void histUnlock() { if (s_lock) xSemaphoreGive(s_lock); }
bool histMountFailed() { return s_mountFailed; }

// Recorta un archivo al ultimo registro completo (corte de luz a mitad de una
// escritura).  true si quedo alineado.
static bool trimTail(const char* path, size_t recSize) {
  File f = LittleFS.open(path, "r");
  if (!f) return true;                 // no existe: nada que recortar
  size_t size = f.size();
  f.close();
  size_t whole = size - (size % recSize);
  if (whole == size) return true;
  char full[48];
  snprintf(full, sizeof(full), LFS_MOUNT "%s", path);
  bool ok = (truncate(full, (off_t)whole) == 0);
  Serial.printf("[hist] %s: cola rota de %u bytes -> %s\n", path, (unsigned)(size - whole),
                ok ? "recortada" : "NO se pudo recortar");
  diagLog("ARCHIVO %s cola rota %uB %s", path, (unsigned)(size - whole), ok ? "recortada" : "SIN RECORTAR");
  return ok;
}

// ¿la particion de datos esta virgen (todo 0xFF en sus 2 primeros bloques, donde
// LittleFS guarda el superbloque)?  Solo entonces se formatea sin preguntar.
static bool partitionBlank() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                      ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, nullptr);
  if (!p) return false;
  uint8_t buf[256];
  for (size_t off = 0; off < 8192 && off < p->size; off += sizeof(buf)) {
    if (esp_partition_read(p, off, buf, sizeof(buf)) != ESP_OK) return false;
    for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != 0xFF) return false;
  }
  return true;
}

// Recalcula tamaño y ultimo id leyendo el archivo.  Si tiene una cola rota
// (bytes que no completan un registro) deja de escribir para no desalinear.
// (calcula en variables locales y asigna al final: quien lea histLastId() desde
//  otra tarea nunca ve un 0 a mitad de calculo)
static void syncFromFile() {
  size_t bytes = 0;
  uint32_t lastId = 0;
  File h = LittleFS.open(HIST_FILE, "r");
  if (!h) { s_histBytes = 0; s_lastId = 0; return; }
  size_t size  = h.size();
  size_t whole = size - (size % sizeof(HRec));
  bytes = whole;
  if (whole) {
    h.seek(whole - sizeof(HRec));
    HRec last;
    if (h.read((uint8_t*)&last, sizeof(last)) == sizeof(last)) lastId = last.id;
  }
  h.close();
  s_histBytes = bytes;
  s_lastId = lastId;
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
  if (!s_lock) s_lock = xSemaphoreCreateMutex();      // antes de que exista ninguna otra tarea
  s_ready = s_writable = s_full = false;
  s_cap = s_histBytes = s_delBytes = 0;
  s_lastId = 0;
  s_del.clear();

  s_mountFailed = false;
  // Nunca se formatea una particion con datos: si no monta (corte de luz a mitad de
  // una escritura de metadatos, por ejemplo) se AVISA y se sigue solo con el buffer
  // de los ultimos pesajes.  Solo una particion virgen (1ª vez) se formatea.
  if (!LittleFS.begin(false)) {
    if (partitionBlank() && LittleFS.begin(true)) {
      Serial.println("[hist] particion virgen: formateada (1ª vez)");
    } else {
      s_mountFailed = true;
      Serial.println("[hist] LittleFS NO monta y la particion tiene datos: NO se formatea"
                     " -> solo el buffer de los ultimos pesajes");
      diagLog("ARCHIVO no monta: NO se formatea; solo ultimos pesajes");
      return false;
    }
  }
  trimTail(HIST_FILE, sizeof(HRec));   // corte de luz a mitad de escritura -> se recorta
  bool delOk = trimTail(DEL_FILE, sizeof(uint32_t));  // (si /hist.bin no se pudo recortar, syncFromFile lo detecta)
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
  s_writable = delOk;                  // /del.bin con cola sin recortar: anadirle ids lo desalinearia
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
bool     histMountFailed()                     { return false; }
bool     histLock(uint32_t)                    { return true; }
void     histUnlock()                          {}
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
