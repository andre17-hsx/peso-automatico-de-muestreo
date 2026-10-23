#pragma once
#include <Arduino.h>

// ===========================================================================
//  fan  -  ventilacion de la caja del ESP (FAN_ENABLE en config.h)
// ---------------------------------------------------------------------------
//  Dos ventiladores que trabajan JUNTOS (uno mete aire, el otro lo saca): se
//  encienden y apagan a la vez segun la temperatura interna del chip, con
//  histeresis (FAN_ON_C / FAN_OFF_C) para no prender y apagar sin parar.
//  Cada ventilador va con su MOSFET (el GPIO no da la corriente del motor).
//  Se llama desde loop(): nunca toca la tarea del pesaje.
// ===========================================================================
void     fanBegin();                 // pines como salida, ventiladores APAGADOS
void     fanUpdate(uint32_t now);    // llamar a menudo; mide cada FAN_CHECK_MS
bool     fanIsOn();
float    fanTempAvg();               // media de la temperatura que usa el control (NAN al inicio)
