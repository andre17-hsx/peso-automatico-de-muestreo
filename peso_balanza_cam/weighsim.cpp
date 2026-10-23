// ===========================================================================
//  weighsim.cpp  -  simulador de pesajes (ver weighsim.h).  Solo con WEIGH_SIM 1.
// ===========================================================================
#include "config.h"
#include "weighsim.h"

#ifndef WEIGH_SIM
#define WEIGH_SIM 0
#endif

#if WEIGH_SIM
#include <math.h>
#include <esp_random.h>
#include "diaglog.h"

#ifndef WEIGH_SIM_GAP_MS
#define WEIGH_SIM_GAP_MS 4000
#endif
#ifndef WEIGH_SIM_TARE
#define WEIGH_SIM_TARE 9
#endif
#define SIM_TARE   ((float)WEIGH_SIM_TARE)   // 9: la plataforma vacia marca -9 (campo); 0: marca ~0
#define SIM_PRECIO 1.50f

static float urand(float a, float b) { return a + (b - a) * (esp_random() / 4294967296.0f); }
static float gauss(float sd) {
  float u1 = urand(1e-6f, 1.0f), u2 = urand(0.0f, 1.0f);
  return sd * sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

// K_MOVER: no hay gaveta; alguien mueve/toca la balanza (picos de 0,3-8 lb): NO debe guardarse.
enum Kind { K_NORMAL, K_RAPIDO, K_GOLPE, K_PAUSA, K_LIVIANO, K_MOVER };
static const char* const KNAME[] = { "normal", "rapido", "golpe", "pausa", "liviano", "mover" };

struct Tray {
  Kind  k;
  float W, tauUp, drip, ov, dec;       // peso real, subida (filtro), goteo lb/s, golpe
  float pv, pt0, plen, climb;          // pausa a mitad de colocacion
  float hold, tauDn;                   // ms puesta antes de retirar; bajada al retirar
};
static Tray     s_tr;
static bool     s_inTray = false;
static uint32_t s_t0 = 0, s_gapEnd = 0, s_n = 0;

// valor neto (sin ruido) mientras la gaveta esta puesta, t = ms desde que se empezo a poner
static float shape(const Tray& r, float t) {
  if (r.k == K_MOVER) {                // golpes cortos de la mano / balanza movida, siempre < 12 lb
    float ph = fmodf(t, r.pt0);        // pt0 = periodo entre picos
    return -SIM_TARE + (ph < r.plen ? r.pv : 0.0f);
  }
  if (r.k == K_PAUSA) {
    if (t < r.pt0)                     return -SIM_TARE + (r.pv + SIM_TARE) * (t / r.pt0);
    if (t < r.pt0 + r.plen)            return r.pv;
    if (t < r.pt0 + r.plen + r.climb)  return r.pv + (r.W - r.pv) * ((t - r.pt0 - r.plen) / r.climb);
    return r.W;
  }
  float v = -SIM_TARE + (r.W + SIM_TARE) * (1.0f - expf(-t / r.tauUp)) - r.drip * t / 1000.0f;
  if (r.k == K_GOLPE) v += r.W * r.ov * expf(-t / r.dec) * (1.0f - expf(-t / 60.0f));
  return v;
}

static void newTray() {
  Tray r = {};
  float d = urand(0, 100);
  r.k = d < 60 ? K_NORMAL : d < 72 ? K_RAPIDO : d < 82 ? K_GOLPE : d < 87 ? K_PAUSA
      : d < 93 ? K_LIVIANO : K_MOVER;
  r.W     = (r.k == K_LIVIANO) ? urand(4, 11) : (r.k == K_MOVER) ? 0.0f : urand(22, 60);
  r.tauUp = urand(120, 300);
  static const float drips[] = { 0.0f, 0.05f, 0.10f, 0.15f };
  r.drip  = drips[esp_random() % 4];
  r.tauDn = urand(350, 900);                      // cola de retiro ~0,6-1,5 s, como en el log real
  switch (r.k) {
    case K_RAPIDO: r.hold = urand(600, 1300); break;
    case K_GOLPE:  r.ov = urand(0.2f, 0.4f); r.dec = urand(200, 500); r.hold = urand(1500, 3000); break;
    case K_PAUSA:  r.drip = 0; r.W = urand(30, 60); r.pv = urand(13, 20);   // pausa >= 12 lb (las menores ya no cuentan)
                   r.pt0 = urand(150, 350); r.plen = urand(260, 320);
                   r.climb = urand(400, 900); r.hold = r.pt0 + r.plen + r.climb + urand(0, 300); break;
    case K_MOVER:  r.pv = urand(0.3f, 8.0f); r.pt0 = urand(300, 700); r.plen = urand(40, 250);
                   r.hold = urand(1500, 3000); r.tauDn = 50; break;
    default:       r.hold = urand(1500, 3500); break;
  }
  s_tr = r;
  s_n++;
  diagLog("SIM #%lu W=%.1f %s", (unsigned long)s_n, (double)r.W, KNAME[r.k]);
}

void simBegin() {
  diagLog("SIMULADOR DE PESAJES ACTIVO (no lee la balanza)");
  Serial.println("[sim] *** SIMULADOR DE PESAJES ACTIVO: los pesajes son FALSOS ***");
  s_gapEnd = millis() + 3000;
}

void simSample(uint32_t now, float* peso, float* precio, float* total) {
  float v = -SIM_TARE;
  if (!s_inTray && (int32_t)(now - s_gapEnd) >= 0) { newTray(); s_inTray = true; s_t0 = now; }
  if (s_inTray) {
    float t = (float)(now - s_t0);
    if (t < s_tr.hold) {
      v = shape(s_tr, t);
    } else {                                      // retiro: baja hacia -tara
      float td = t - s_tr.hold;
      v = -SIM_TARE + (shape(s_tr, s_tr.hold) + SIM_TARE) * expf(-td / s_tr.tauDn);
      if (td > 6.0f * s_tr.tauDn) {
        s_inTray = false;
        s_gapEnd = now + (uint32_t)(urand(0.5f, 1.5f) * WEIGH_SIM_GAP_MS);
      }
    }
  }
  // ruido: sin tara la plataforma vacia baila +-0,1 alrededor de 0 y a veces da un pico
  // suelto de 0,1-0,4 lb (asi fue en la prueba real del 2026-10-07)
  float sd = (SIM_TARE == 0.0f && !s_inTray) ? 0.06f : 0.02f;
  if (SIM_TARE == 0.0f && !s_inTray && urand(0, 1) < 0.005f) v += urand(0.1f, 0.4f);
  v = roundf((v + gauss(sd)) * 10.0f) / 10.0f;      // la balanza va de 0,1 en 0,1
  *peso   = v;
  *precio = SIM_PRECIO;
  *total  = roundf(v * SIM_PRECIO * 100.0f) / 100.0f;
}

#else
void simBegin() {}
void simSample(uint32_t, float* p, float* pr, float* t) { *p = NAN; *pr = NAN; *t = NAN; }
#endif
