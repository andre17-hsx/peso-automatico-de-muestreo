#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
//  Decodificacion de 7 segmentos.
//  Mascara ESTANDAR:  bit0=a  bit1=b  bit2=c  bit3=d  bit4=e  bit5=f  bit6=g
//  Devuelve:  0..9  = digito
//            -1     = patron desconocido
//            -2     = todo apagado (digito en blanco)
//            -3     = solo el segmento g  (signo menos)
// ---------------------------------------------------------------------------
static inline int seg7decode(uint8_t m) {
  switch (m & 0x7F) {
    case 0x3F: return 0;  case 0x06: return 1;  case 0x5B: return 2;
    case 0x4F: return 3;  case 0x66: return 4;  case 0x6D: return 5;
    case 0x7D: return 6;  case 0x07: return 7;  case 0x7F: return 8;
    case 0x6F: return 9;
    case 0x00: return -2;
    case 0x40: return -3;
  }
  return -1;
}

// Version tolerante para el OCR: si no hay coincidencia exacta, acepta el
// digito mas parecido siempre que difiera como mucho en 'maxDist' segmentos.
static inline int seg7decodeFuzzy(uint8_t m, int maxDist) {
  int d = seg7decode(m);
  if (d != -1) return d;
  static const uint8_t C[10] =
    { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
  int best = -1, bd = 99;
  for (int k = 0; k < 10; k++) {
    int hd = __builtin_popcount((unsigned)((m & 0x7F) ^ C[k]));
    if (hd < bd) { bd = hd; best = k; }
  }
  return (bd <= maxDist) ? best : -1;
}
