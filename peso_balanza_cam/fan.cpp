// ===========================================================================
//  fan.cpp  -  ventilacion de la caja por histeresis (ver fan.h)
// ===========================================================================
#include "config.h"
#include "fan.h"
#include "diaglog.h"
#include <math.h>

#ifndef FAN_ENABLE
#define FAN_ENABLE 0
#endif
#ifndef FAN1_PIN
#define FAN1_PIN 5
#endif
#ifndef FAN2_PIN
#define FAN2_PIN 6                 // -1 = solo un pin (p. ej. los dos MOSFET en el mismo GPIO)
#endif
#ifndef FAN_ACTIVE_HIGH
#define FAN_ACTIVE_HIGH 1          // 1 = el pin en ALTO enciende (MOSFET de canal N en el lado de tierra)
#endif
#ifndef FAN_ON_C
#define FAN_ON_C 60.0f
#endif
#ifndef FAN_OFF_C
#define FAN_OFF_C 52.0f
#endif
#ifndef FAN_CHECK_MS
#define FAN_CHECK_MS 5000
#endif
#ifndef FAN_AVG_N
#define FAN_AVG_N 6                // media de las ultimas N lecturas (6 x 5 s = 30 s)
#endif
#ifndef FAN_MIN_ON_MS
#define FAN_MIN_ON_MS 60000UL      // una vez encendidos, al menos esto
#endif

#if FAN_ENABLE

static bool     s_on        = false;
static uint32_t s_onSince   = 0;
static uint32_t s_lastCheck = 0;
static bool     s_checked   = false;
static float    s_hist[FAN_AVG_N];
static int      s_n = 0, s_i = 0;
static float    s_avg       = NAN;

static void setOutputs(bool on) {
  int lv = (on == (FAN_ACTIVE_HIGH != 0)) ? HIGH : LOW;
  digitalWrite(FAN1_PIN, lv);
  if (FAN2_PIN >= 0) digitalWrite(FAN2_PIN, lv);
}

static_assert(FAN1_PIN >= 0, "FAN1_PIN tiene que ser un GPIO valido");

// En el nucleo 3.x digitalWrite no actua antes de pinMode: se configura el pin y se
// apaga enseguida.  Lo que garantiza "apagado" durante el arranque (antes de setup) es
// el pulldown de 100 k en la puerta de cada MOSFET.
void fanBegin() {
  pinMode(FAN1_PIN, OUTPUT);
  if (FAN2_PIN >= 0) pinMode(FAN2_PIN, OUTPUT);
  setOutputs(false);
}

void fanUpdate(uint32_t now) {
  if (s_checked && now - s_lastCheck < (uint32_t)FAN_CHECK_MS) return;
  s_checked = true;
  s_lastCheck = now;

  float t = temperatureRead();
  bool bad = isnan(t) || t < -20.0f || t > 125.0f;   // sensor sin dato o absurdo
  if (bad) {
    s_n = 0; s_i = 0; s_avg = NAN;                   // al volver el dato, la media empieza de cero
  } else {
    if (t > g_diagCnt.tempMaxC) g_diagCnt.tempMaxC = t;   // el maximo de /log, sin esperar al 1er resumen ST
    s_hist[s_i] = t;
    s_i = (s_i + 1) % FAN_AVG_N;
    if (s_n < FAN_AVG_N) s_n++;
    float sum = 0;
    for (int k = 0; k < s_n; k++) sum += s_hist[k];
    s_avg = sum / s_n;
  }

  bool want = s_on;
  if (bad)                                       want = true;   // a prueba de fallos: sin dato, ventilar
  else if (!s_on && s_avg >= FAN_ON_C)           want = true;
  else if (s_on && s_avg <= FAN_OFF_C &&
           now - s_onSince >= (uint32_t)FAN_MIN_ON_MS) want = false;
  if (want == s_on) return;

  if (want) {
    s_onSince = now;
  } else {
    g_diagCnt.fanOnS += (now - s_onSince) / 1000;
  }
  s_on = want;
  g_diagCnt.fanOn = want;
  setOutputs(want);
  diagLog("VENT %s t=%.0f%s", want ? "ON" : "OFF", (double)s_avg, bad ? " (sensor sin dato)" : "");
}

bool  fanIsOn()    { return s_on; }
float fanTempAvg() { return s_avg; }

#else   // FAN_ENABLE 0: todo inerte

void  fanBegin()          {}
void  fanUpdate(uint32_t) {}
bool  fanIsOn()           { return false; }
float fanTempAvg()        { return NAN; }

#endif
