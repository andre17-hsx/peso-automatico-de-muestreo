#pragma once
#include <Arduino.h>

// Mapa RAM(16 bytes) -> digitos.  Editable en caliente desde /config.
struct SnifParams {
  int8_t  pesoAddr[6];   int8_t pesoLen;
  int8_t  precioAddr[6]; int8_t precioLen;
  int8_t  totalAddr[6];  int8_t totalLen;
  uint8_t segBit[8];             // bit del byte que lleva a,b,c,d,e,f,g,punto
  int8_t  pesoDP, precioDP, totalDP;   // decimales fijos; -1 = usar bit de punto
};
extern SnifParams snf;

struct SnifState {
  uint8_t  shadow[16];
  bool     valid;
  bool     stable;                     // PESO sin cambios SNIF_STABLE_N refrescos
  char     peso[16], precio[16], total[16];
  float    pesoVal, precioVal, totalVal;
  uint32_t txCount, refreshCount;
  float    slKHz;                       // media (incluye huecos de reposo)
  float    slKHzPeak;                   // reloj dentro de la rafaga (periodo minimo)
  uint32_t slFastN;                     // nº de periodos SL < 2 us (reloj > 500 kHz)
};

void   snifBegin();
void   snifLoop();                      // llamar en loop(): consolida tramas
bool   snifGet(SnifState& out);
void   snifLoadParams();
void   snifSaveParams();                // guarda el mapa actual en NVS
void   snifResetParams();               // borra el mapa de NVS -> vuelve a config.h
void   snifSetEnabled(bool en);         // pausar/reanudar la captura (OCR_PAUSE_SNIFFER)

// --- auto-resolvedor del mapa (ingenieria inversa asistida) ---
// Capturas N estados con su valor conocido; luego resuelve.
int    snifCapture(const char* peso, const char* precio, const char* total);  // -> nº capturas, -1 lleno
void   snifCapClear();
int    snifCapCount();
int    snifSolve(char* report, int replen);   // >=0 posiciones resueltas, <0 error (con texto en report)

// volcado crudo de flancos (para decodificar offline si la trama sale con basura)
void     snifArmRaw();
int      snifRawCount();
void     snifRawGet(int i, uint32_t* dCyc, uint8_t* sl, uint8_t* da);
uint32_t snifCpuMHz();

// diagnostico de cableado (cuando /sniffer marca 0 tramas / 0 flancos)
void snifDiag(uint8_t* slLvl, uint8_t* daLvl, uint32_t* slEdges, uint32_t* daEdges);
void snifPinProbe(char* out, int len);   // fuerza pull-up/down y dice si el pin esta al aire
void snifResetStats();                   // borra medidas de reloj/flancos (pico limpio)
