#pragma once
#include <Arduino.h>

// ===========================================================================
//  weighsim  -  SIMULADOR de pesajes para pruebas en oficina (WEIGH_SIM en config.h)
// ---------------------------------------------------------------------------
//  Genera gavetas falsas con curvas realistas (subida con el filtro del indicador,
//  goteo, golpes, pausas a mitad de colocacion, retiros rapidos, gavetas livianas y
//  "mover la balanza" sin gaveta, con o sin tara: WEIGH_SIM_TARE) y las entrega como
//  si vinieran de la balanza.  Livianas (< WEIGH_MIN_LOAD) y "mover" NO deben guardarse.  Pasan por la maquina de
//  pesaje REAL, asi que se prueba todo: tarea, guardado en NVS, archivo LittleFS.
//  Cada gaveta se anota en /log como "SIM #n W=<peso real> <tipo>" para comparar.
// ===========================================================================
void simBegin();
void simSample(uint32_t now, float* peso, float* precio, float* total);
