#pragma once
#include <Arduino.h>

// ===========================================================================
//  histarch  -  archivo PERMANENTE de pesajes (LittleFS, particion "spiffs")
// ---------------------------------------------------------------------------
//  El buffer circular de weighlog guarda solo los ultimos WEIGH_LOG_SIZE
//  pesajes.  Este modulo guarda TODOS (hasta que se pulse "Borrar todo") en un
//  archivo de solo-añadir:
//     /hist.bin : registros de 24 bytes (HRec), en orden de id
//     /del.bin  : ids borrados a mano (uint32 cada uno)
//  Es OPCIONAL y a prueba de fallos: si LittleFS no monta (esquema de
//  particiones sin "spiffs") o el archivo se llena, weighlog sigue funcionando
//  exactamente como antes con su buffer.  WEIGH_ARCHIVE 0 lo desactiva.
// ===========================================================================

struct __attribute__((packed)) HRec {
  uint32_t id;        // nº de pesaje (el mismo id que usa weighlog)
  float    peso;
  float    precio;    // NAN si no legible
  float    total;     // NAN si no legible
  int32_t  epoch;     // hora UTC (s), 0 = desconocida
  uint16_t malla;
  uint16_t bin;
};                    // 24 bytes

bool     histBegin();                      // monta LittleFS y carga los borrados; false = sin archivo
bool     histReady();                      // montado y legible
bool     histWritable();                   // se pueden seguir añadiendo registros
bool     histFull();                       // se alcanzo la capacidad reservada
uint8_t  histUsedPct();                    // % de la capacidad usado (0..100)
uint32_t histCount();                      // registros en el archivo (incluye los borrados)
uint32_t histLastId();                     // mayor id archivado (0 = vacio)

// Añade registros (ids ascendentes) con UNA sola escritura.  Devuelve cuantos
// quedaron guardados: si no caben todos guarda el prefijo que quepa, asi el
// archivo nunca tiene huecos.  Nunca bloquea ni rompe el pesaje si falla.
size_t   histAppendMany(const HRec* r, size_t n);

void     histMarkDeleted(uint32_t id);     // borrado a mano (se persiste)
bool     histIsDeleted(uint32_t id);
void     histClear();                      // borra archivo y borrados ("Borrar todo")

// Recorre en orden (mas viejo primero) los registros NO borrados.  cb devuelve
// false para parar.
typedef bool (*HistVisit)(const HRec& r, void* ctx);
void     histForEach(HistVisit cb, void* ctx);
