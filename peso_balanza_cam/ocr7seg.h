#pragma once
#include <Arduino.h>

// Resultado de una lectura OCR del display PESO.
struct OcrResult {
  bool     valid;              // true = 'value' es fiable
  float    value;              // peso con el punto decimal aplicado
  char     text[20];           // cadena cruda por digito ("  17.20", " 1?.0 ", ...)
  int      digit[8];           // -2 apagado, -3 signo, -1 '?', 0..9
  uint8_t  segmask[8];         // mascara a..g detectada por digito
  uint8_t  segBright[8][7];    // brillo medio 0..255 de cada segmento
  uint8_t  thr, bMin, bMax;    // umbral y rango de brillo dentro de la caja
  uint8_t  ncell;              // nº de celdas (digitos) localizadas
  int      autoThr;            // umbral del localizador automatico
  uint32_t frameMs;
};

// Parametros ajustables en caliente (se cargan de NVS o de config.h).
struct OcrParams {
  bool  autoMode;     // true = localiza los digitos sola (sin caja)
  float digitAspect;  // ancho de un digito = alto de la banda * esto
  float digitPitch;   // separacion entre digitos = ancho de digito * esto
  int   autoBright;   // umbral "encendido" absoluto; 0 = automatico
  int   boxX, boxY, boxW, boxH;   // caja manual (modo auto=0)
  int   digitGap;
  float slant;
  float onRatio;
  int   minContrast;
  int   numDigits;
  int   decimals;
  int   aecValue;     // exposicion de la camara
  int   contrast;     // -2..2
  bool  flashLed;
};
extern OcrParams ocr;

#include "weighlog.h"          // struct Weighing + historial de pesajes (comun)

bool  ocrBegin();
void  ocrStartTask();                 // arranca el bucle de lectura en su tarea (core 0)
bool  ocrRead(OcrResult& out);        // una lectura suelta (la usa la tarea)
void  ocrGetLast(OcrResult& out);     // copia protegida de la ultima lectura

//  El historial de pesajes y su estado estan en weighlog.h (weigh*()).

void  ocrLoadParams();
void  ocrSaveParams();                 // aplica + guarda en NVS
void  ocrApplyCam();                   // re-aplica exposicion/contraste al sensor (sin guardar)
int   ocrAutoThr();                    // umbral que eligio el localizador automatico

// --- plantillas (reconocimiento por forma) ---
int   ocrLearn(const char* shown);     // 'shown' = valor visible AHORA ("6.50").
                                       //  >0 = nº de cifras aprendidas ; <0 = error
void  ocrTemplatesClear();
int   ocrTemplatesMask();              // bit d = plantilla de la cifra d aprendida

// JPEG del ultimo frame (el llamante libera *buf con free())
bool  ocrJpegSnapshot(uint8_t** buf, size_t* len);
bool  ocrJpegDebug(uint8_t** buf, size_t* len);   // con las zonas dibujadas
