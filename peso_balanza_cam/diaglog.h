#pragma once
#include <Arduino.h>
#include "config.h"

#ifndef DIAG_LOG
#define DIAG_LOG 1                // un config.h antiguo, sin la opcion, lo deja activo
#endif

// ===========================================================================
//  diaglog  -  registro de diagnostico en RAM  (se descarga desde el movil: /log)
// ---------------------------------------------------------------------------
//  Pensado para NO estorbar al programa principal:
//    - vive en RAM (~16 KB estaticos); NO escribe a flash ni usa el Serial;
//    - anotar una linea cuesta unos microsegundos y solo se hace en EVENTOS
//      (pesaje guardado / perdido, celular que entra o sale, vuelta del bucle
//      demasiado lenta, cada ~2 min un resumen), nunca por muestra;
//    - se puede llamar desde cualquier tarea (eventos de WiFi incluidos).
//  Se pierde al reiniciar/apagar: descargalo antes.  DIAG_LOG 0 lo desactiva.
// ===========================================================================

struct DiagCounters {
  uint32_t traysOk;        // pesajes guardados
  uint32_t traysLost;      // gavetas puestas que NO se guardaron por no llegar a asentar
  uint32_t tares;          // taras / cambios descartados a proposito
  uint32_t wifiConn;       // celulares que se conectaron
  uint32_t wifiDisc;       // celulares que se desconectaron
  uint32_t slowLoops;      // vueltas del bucle principal mas lentas que el limite
  uint32_t maxLoopMs;      // la vuelta mas lenta
  uint32_t maxWebMs;       // lo mas que tardo server.handleClient() en una vuelta
};
extern DiagCounters g_diagCnt;

void diagBegin();                                         // anota el motivo del ultimo reinicio
void diagLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Vuelca el registro (cabecera con contadores + lineas, de la mas vieja a la mas
// nueva) llamando a sink(texto, longitud, ctx) por trozos.
typedef void (*DiagSink)(const char* s, size_t n, void* ctx);
void diagDump(DiagSink sink, void* ctx);
