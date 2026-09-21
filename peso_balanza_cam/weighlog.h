#pragma once
#include <Arduino.h>

// ===========================================================================
//  weighlog  -  Maquina de estados del pesaje + historial  (comun a los dos
//               metodos: la alimenta el sniffer o el OCR, el que este activo)
// ===========================================================================
//
//   VACIA : la balanza marca "-" (dormida) o ~0.
//   CARGA : hay peso > WEIGH_ZERO_THRESH.  Mientras este ESTABLE se va
//           recordando su valor (el ULTIMO estable).
//   La plataforma vuelve a ~0 (o el display se apaga) -> se GUARDA ese ultimo
//   valor estable en el historial y salta el callback.  Y vuelta a empezar.
//
//   UNA entrada por gaveta.  Simple y "time-based": da igual a que ritmo se
//   llame weighFeed.

struct Weighing {
  float    peso;      // kg  (== "value" antiguo)
  float    precio;    // precio unitario mostrado (NAN si no legible)
  float    total;     // suma total mostrada       (NAN si no legible)
  uint32_t ms;        // millis() al capturarlo
  long     epoch;     // hora UTC (s) si hay NTP, si no 0 ;  -1 = fila borrada
  uint32_t id;        // nº de pesaje (para borrar una fila concreta).  Solo RAM,
                      // lo rellena weighGet/weighLatchedFull; no se persiste.
  uint16_t malla;     // nº de MALLA a la que pertenece (0 = anterior a esta funcion)
  uint16_t bin;       // nº de BIN   a la que pertenece (0 = anterior a esta funcion)
};

// Agrupacion MALLA/BIN: cada pesaje que se GUARDA se suma COMPLETO (nunca
// partido) al grupo en curso; al llegar (o superar) el objetivo, ese grupo
// se cierra (queda con ese pesaje como ultimo miembro) y el siguiente pesaje
// empieza un grupo nuevo.  BIN contiene varias MALLA completas; MALLA
// contiene varios pesos individuales.  Por eso el BIN solo puede cerrar en
// el mismo pesaje en que cierra una malla (nunca a mitad de una) -- si no,
// una malla quedaria repartida entre dos BIN.  Como cada malla puede
// pasarse de su objetivo, el BIN hereda esos sobrantes (si malla1 se pasa
// por A, malla2 por B, etc., el BIN se pasa por A+B+...) -- es normal y
// esperado, no se recorta.  El nº de malla se REINICIA a 1 en cada BIN nuevo
// (BIN 1: mallas 1,2,3 · BIN 2: mallas 1,2,3...), asi que la clave de una
// malla es el par (bin, malla), no el nº de malla solo.
// El "peso total acumulado" (weighSum) NO cambia - sigue siendo el total de
// TODA la sesion; esto es una agrupacion aparte, encima de esos mismos pesajes.
struct WeighGroups {
  uint16_t mallaId,     binId;       // grupo EN CURSO (arranca en 1)
  float    mallaSum,    binSum;      // acumulado del grupo EN CURSO
  float    mallaTarget, binTarget;   // objetivo puesto por el operario (<=0 = sin objetivo)
  float    mallaLast,   binLast;     // total del ULTIMO grupo CERRADO (NAN si ninguno aun)
};
void weighGetGroups(WeighGroups* out);
void weighSetTargets(float mallaTarget, float binTarget);   // <=0 en cualquiera lo desactiva

void        weighBegin();              // carga el historial de NVS (llamar en setup)

// Alimentar con los 3 campos en vivo, a ritmo regular (p. ej. cada 100 ms).
//   peso    : dispara la maquina de estados
//   precio, total : se guardan junto al peso estable
//   stable  : bit ESTABLE del bus (si el metodo lo tiene; el OCR pasa false y
//             se usa un "quieto WEIGH_STABLE_MS" interno)
//   valid   : false si el display esta ilegible / apagado / dormido
void        weighFeed(float peso, float precio, float total, bool stable, bool valid);

const char* weighStateName();          // "vacia" / "estabilizando" / "estable"
float       weighLatched();            // PESO del ultimo pesaje NO borrado (NAN si ninguno)
bool        weighLatchedFull(Weighing* out);   // el ultimo pesaje entero; false si ninguno
uint32_t    weighTotal();              // total desde el arranque (monotono, cuenta los borrados)
float       weighSum();                // suma de los PESO NO borrados desde el ultimo Borrar
int         weighCount();              // pesajes NO borrados en TODO el historial (no solo el buffer)
int         weighGet(Weighing* out, int maxn);   // los ultimos maxn del BUFFER (sin borrados), reciente primero
void        weighClear();                         // borra TODO (buffer + archivo permanente)
bool        weighDeleteOne(uint32_t id);          // borra una fila por su id (solo las del buffer); false si no existe

// ---- historial COMPLETO (buffer + archivo permanente, ver histarch.h) ----
// El buffer guarda los ultimos WEIGH_LOG_SIZE pesajes; el archivo guarda TODOS
// hasta el proximo "Borrar todo".  Si el archivo no esta disponible, todo sigue
// funcionando con el buffer solo.
//
// Recorre TODOS los pesajes NO borrados, del mas viejo al mas nuevo.  cb
// devuelve false para parar.  (ms no se rellena en los que vienen del archivo.)
typedef bool (*WeighVisit)(const Weighing& w, void* ctx);
void        weighForEachAll(WeighVisit cb, void* ctx);

// Subtotal de una malla / de un BIN sobre TODO el historial.  false = ese grupo
// no se conoce (no hay pesajes, o es de las mallas mas antiguas: la tabla guarda
// las ultimas 800).
bool        weighGroupSum(uint16_t bin, uint16_t malla, float* sum, uint32_t* cnt);
bool        weighBinSum(uint16_t bin, float* sum, uint32_t* cnt);

// Estado del archivo permanente (para avisar al operario).
struct WeighArchiveInfo {
  bool     ok;        // montado y se puede seguir archivando
  bool     full;      // capacidad agotada: los pesajes nuevos ya no se archivan
  uint8_t  pct;       // % de la capacidad usado
  uint32_t stored;    // registros en el archivo
};
void        weighArchiveInfo(WeighArchiveInfo* out);

// Se invoca UNA vez cuando un pesaje queda CONFIRMADO (plataforma vaciada tras
// una lectura estable).  Aqui se engancha el envio permanente (MQTT / HTTP).
typedef void (*WeighCommitCb)(const Weighing& w);
void        weighOnCommit(WeighCommitCb cb);
