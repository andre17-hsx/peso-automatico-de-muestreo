/*
 * ============================================================================
 *  sniffer_tm1640  -  Escucha pasiva del bus del display de la balanza
 * ============================================================================
 *
 *  Balanza:  placa principal ACS-JC36CV28  --->  placa display WP-91110-2540-V7
 *            El chip del display es un CS2540 (equivalente a TM1640).
 *            Bus de 2 hilos:  DA = datos ,  SL = reloj.
 *            16 digitos en total:  PESO(5) + PRECIO(5) + TOTAL(6).
 *
 *  Este sketch NO controla nada: solo escucha DA y SL, decodifica el
 *  protocolo tipo TM1640 y vuelca por USB-serie la "RAM de display"
 *  (16 bytes, 1 byte por digito).  Con eso se deduce el mapa de segmentos.
 *
 * ----------------------------------------------------------------------------
 *  CONEXION  (¡solo 3 cables!)
 * ----------------------------------------------------------------------------
 *    Balanza DA   --- [470 ohm] ---  GPIO PIN_DA  del ESP32
 *    Balanza SL   --- [470 ohm] ---  GPIO PIN_SL  del ESP32
 *    Balanza GND  ---------------- -  GND del ESP32   (masa comun OBLIGATORIA)
 *
 *    NO conectar VCC_DIS.
 *    Logica de 3 V -> entra directa al GPIO del ESP32-S3 (no hace falta
 *    level-shifter).  Las 470 ohm son solo proteccion.
 *
 *    Durante la prueba, alimenta la balanza con SU bateria (no cargador a
 *    la red) y el ESP32 por USB, para evitar bucles de masa.
 *
 * ----------------------------------------------------------------------------
 *  USO
 * ----------------------------------------------------------------------------
 *    1. Ajusta PIN_DA / PIN_SL a dos GPIO libres de tu placa (0..31).
 *    2. Carga el sketch. Abre el Monitor Serie a 115200 baudios.
 *    3. Enciende la balanza -> se captura el autotest de segmentos.
 *    4. Ve poniendo valores CONOCIDOS y apunta que muestra cada pantalla:
 *         - vacia y en cero
 *         - peso patron (p.ej. 500 g)  -> 0.500
 *         - otro peso                  -> 1.234
 *         - precio por teclado: 1.00 , 9.99 , 12.50
 *         - plataforma inestable  vs  estable   (icono ESTABLE)
 *         - tara / cero
 *    5. Copia TODO el log del Monitor Serie y mandalo para construir el
 *       mapa (que byte = que digito, que bit = que segmento, que bits = iconos).
 *
 *  Comandos por serie (escribe la letra y Enter):
 *    d  ->  modo normal: imprime la RAM solo cuando cambia   (por defecto)
 *    c  ->  imprime las proximas ~40 transacciones crudas (ver estructura)
 *    f  ->  estadisticas: frecuencia de SL, nº de transacciones
 *    r  ->  volcado CRUDO de flancos (~200 ms) para analisis offline
 *           (usar solo si la decodificacion normal sale con basura)
 * ============================================================================
 */

#include <Arduino.h>
#include "soc/gpio_reg.h"

// ==================== CONFIG ====================
#define PIN_DA   4        // linea DA (datos).  GPIO 0..31
#define PIN_SL   5        // linea SL (reloj).  GPIO 0..31
#define BAUD     115200

// ==================== lectura rapida de GPIO dentro de ISR ====================
static inline uint32_t IRAM_ATTR gpioIn() { return REG_READ(GPIO_IN_REG); }
#define DA_OF(io)  (((io) >> PIN_DA) & 1u)
#define SL_OF(io)  (((io) >> PIN_SL) & 1u)

// ==================== estado del decodificador TM1640 ====================
// Protocolo: tipo I2C SIN ACK.  START = DA baja con SL alto.
//            STOP  = DA sube con SL alto.  Bits LSB primero, validos en
//            flanco de subida de SL.  8 bits por byte.
// Comandos:  0x40/0x44 = escritura (auto-incr / direccion fija)
//            0xC0|addr  = fija direccion 0..15,  luego vienen los datos
//            0x80..0x8F = display on/off + brillo
static volatile uint8_t  dCur = 0, dBits = 0, dLen = 0;
static volatile bool     dIn  = false;
static volatile uint8_t  dBuf[80];

static volatile uint8_t  shadow[16];          // imagen de la RAM del display
static volatile bool     shadowDirty = false;
static volatile uint32_t txCount = 0;         // transacciones START..STOP vistas

// --- descubrimiento de estructura: firmas (primer byte, longitud) ya vistas
static volatile uint16_t sigSeen[24];
static volatile uint8_t  sigN = 0;

// --- modo 'c': imprimir N transacciones crudas
static volatile uint8_t  rawTxBuf[48][20];
static volatile uint8_t  rawTxLen[48];
static volatile uint8_t  rawTxHead = 0, rawTxTail = 0;
static volatile int      cBudget = 0;

// --- medicion de reloj SL
static volatile uint32_t slLastCyc = 0, slMinP = 0xFFFFFFFFu, slEdges = 0;
static volatile uint64_t slSumP = 0;

// --- volcado crudo de flancos (fallback)
struct RawE { uint32_t cyc; uint8_t lv; };
#define RAW_MAX 6000
static volatile RawE  rawArr[RAW_MAX];
static volatile int   rawN = 0;
static volatile bool  rawArm = false;
static uint32_t       rawStartMs = 0;

static bool modeAll = false;

// ---------------------------------------------------------------------------
//  ISR: flanco de SUBIDA de SL  ->  latch de 1 bit
// ---------------------------------------------------------------------------
void IRAM_ATTR isrSlRise() {
  uint32_t io = gpioIn();
  uint8_t  da = DA_OF(io);

  uint32_t c = ESP.getCycleCount();
  if (rawArm && rawN < RAW_MAX) { rawArr[rawN].cyc = c; rawArr[rawN].lv = 0x02 | da; rawN++; }
  if (slLastCyc) { uint32_t p = c - slLastCyc; if (p < slMinP) slMinP = p; slSumP += p; slEdges++; }
  slLastCyc = c;

  if (!dIn) return;
  dCur |= (uint8_t)(da << dBits);
  if (++dBits >= 8) {
    if (dLen < sizeof(dBuf)) dBuf[dLen++] = dCur;
    dCur = 0; dBits = 0;
  }
}

// ---------------------------------------------------------------------------
//  ISR: cualquier cambio en DA  ->  posible START / STOP (si SL esta alto)
// ---------------------------------------------------------------------------
void IRAM_ATTR isrDaChange() {
  uint32_t io = gpioIn();
  uint8_t  sl = SL_OF(io), da = DA_OF(io);

  if (rawArm && rawN < RAW_MAX) {
    rawArr[rawN].cyc = ESP.getCycleCount();
    rawArr[rawN].lv  = (sl << 1) | da;
    rawN++;
  }

  if (!sl) return;                       // DA cambio con SL bajo -> es un bit de datos

  if (da == 0) {                         // ---- START ----
    dIn = true; dCur = 0; dBits = 0; dLen = 0;
    return;
  }

  // ---- STOP ----
  if (dIn && dLen > 0) {
    txCount++;
    uint8_t cmd = dBuf[0];

    // actualizar imagen de RAM si es una trama de datos
    if ((cmd & 0xC0) == 0xC0) {
      uint8_t a = cmd & 0x0F;
      for (uint8_t i = 1; i < dLen; i++) {
        uint8_t idx = a + (i - 1);
        if (idx < 16) shadow[idx] = dBuf[i];
      }
      if (dLen > 1) shadowDirty = true;
    }

    // registrar firma (primer byte + longitud) si es nueva
    uint16_t sig = ((uint16_t)cmd << 8) | dLen;
    bool known = false;
    for (uint8_t i = 0; i < sigN; i++) if (sigSeen[i] == sig) { known = true; break; }
    if (!known && sigN < 24) sigSeen[sigN++] = sig;

    // encolar para modo 'c'
    if (cBudget > 0) {
      uint8_t nh = (rawTxHead + 1) % 48;
      if (nh != rawTxTail) {
        uint8_t n = dLen; if (n > 20) n = 20;
        for (uint8_t i = 0; i < n; i++) rawTxBuf[rawTxHead][i] = dBuf[i];
        rawTxLen[rawTxHead] = n;
        rawTxHead = nh;
        cBudget--;
      }
    }
  }
  dIn = false;
}

// ---------------------------------------------------------------------------
//  Decodificacion 7 seg con mapa ESTANDAR (a=bit0 ... g=bit6, punto=bit7).
//  Puede NO coincidir con el ruteo real: sirve solo de orientacion.
// ---------------------------------------------------------------------------
int seg7std(uint8_t b) {
  switch (b & 0x7F) {
    case 0x3F: return 0;  case 0x06: return 1;  case 0x5B: return 2;  case 0x4F: return 3;
    case 0x66: return 4;  case 0x6D: return 5;  case 0x7D: return 6;  case 0x07: return 7;
    case 0x7F: return 8;  case 0x6F: return 9;  case 0x00: return -2;  // -2 = apagado
    default:   return -1;                                             // -1 = desconocido
  }
}

void printBin8(uint8_t b) { for (int i = 7; i >= 0; i--) Serial.print((b >> i) & 1); }

String groupGuess(const uint8_t *s, int a, int b) {
  String r = "";
  for (int i = a; i <= b; i++) {
    int d = seg7std(s[i]);
    r += (d >= 0) ? char('0' + d) : (d == -2 ? ' ' : '?');
    if (s[i] & 0x80) r += '.';
  }
  return r;
}

void dumpShadow() {
  uint8_t s[16];
  noInterrupts();
  for (int i = 0; i < 16; i++) s[i] = shadow[i];
  interrupts();

  Serial.println();
  Serial.println("==== RAM display (16 bytes) ====");
  for (int i = 0; i < 16; i++) {
    char line[48];
    snprintf(line, sizeof(line), "D%02d = 0x%02X  ", i, s[i]);
    Serial.print(line);
    printBin8(s[i]);
    int d = seg7std(s[i]);
    Serial.print("  est=");
    if      (d == -2) Serial.print("' '");
    else if (d == -1) Serial.print(" ? ");
    else            { Serial.print(' '); Serial.print(d); Serial.print(' '); }
    if (s[i] & 0x80) Serial.print("  (punto)");
    Serial.println();
  }
  Serial.println("---- lectura adivinada (orden de digitos SIN confirmar) ----");
  Serial.print("  grupo A [D00..D04] ~ "); Serial.println(groupGuess(s, 0, 4));
  Serial.print("  grupo B [D05..D09] ~ "); Serial.println(groupGuess(s, 5, 9));
  Serial.print("  grupo C [D10..D15] ~ "); Serial.println(groupGuess(s, 10, 15));
  Serial.println("(anota aqui que mostraban PESO / PRECIO / TOTAL en este momento)");
  Serial.println();
}

String slKHz() {
  if (slEdges < 10) return "?";
  float mhz = (float)getCpuFrequencyMhz();
  float avgCyc = (float)slSumP / (float)slEdges;
  float f = (avgCyc > 0) ? (mhz * 1000.0f / avgCyc) : 0;   // kHz
  float fmax = (slMinP > 0) ? (mhz * 1000.0f / slMinP) : 0;
  char b[48];
  snprintf(b, sizeof(b), "%.0f (pico %.0f)", f, fmax);
  return String(b);
}

void printStats() {
  Serial.println();
  Serial.println("---- estadisticas ----");
  Serial.printf("  transacciones START..STOP : %u\n", (unsigned)txCount);
  Serial.printf("  frecuencia SL (kHz)       : %s\n", slKHz().c_str());
  Serial.printf("  CPU (MHz)                 : %u\n", getCpuFrequencyMhz());
  Serial.println("  firmas de transaccion vistas (primer byte / longitud):");
  noInterrupts();
  uint8_t n = sigN; uint16_t cp[24];
  for (uint8_t i = 0; i < n; i++) cp[i] = sigSeen[i];
  interrupts();
  for (uint8_t i = 0; i < n; i++)
    Serial.printf("    cmd=0x%02X  len=%u\n", cp[i] >> 8, cp[i] & 0xFF);
  Serial.println();
}

void startRaw() {
  Serial.println("\n[raw] capturando flancos ~200 ms ...");
  noInterrupts();
  rawN = 0; rawArm = true;
  interrupts();
  rawStartMs = millis();
}

void dumpRaw() {
  noInterrupts();
  int n = rawN; rawArm = false;
  interrupts();
  Serial.printf("\n[raw] n=%d  cpuMHz=%u   formato: <delta_ciclos> <SL> <DA>\n", n, (unsigned)getCpuFrequencyMhz());
  uint32_t prev = (n > 0) ? rawArr[0].cyc : 0;
  for (int i = 0; i < n; i++) {
    uint32_t d = rawArr[i].cyc - prev;
    prev = rawArr[i].cyc;
    Serial.printf("%lu %u %u\n", (unsigned long)d, (rawArr[i].lv >> 1) & 1, rawArr[i].lv & 1);
  }
  Serial.println("[raw] fin\n");
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(BAUD);
  delay(300);
  setCpuFrequencyMhz(240);

  pinMode(PIN_DA, INPUT);
  pinMode(PIN_SL, INPUT);

  for (int i = 0; i < 16; i++) shadow[i] = 0;

  attachInterrupt(digitalPinToInterrupt(PIN_SL), isrSlRise,  RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_DA), isrDaChange, CHANGE);

  Serial.println();
  Serial.println("=========================================================");
  Serial.println(" sniffer_tm1640  -  escucha pasiva del display de balanza");
  Serial.println("=========================================================");
  Serial.printf ("  PIN_DA = GPIO %d    PIN_SL = GPIO %d\n", PIN_DA, PIN_SL);
  Serial.println("  Enciende la balanza ahora para capturar el autotest.");
  Serial.println("  Comandos:  d=normal  c=crudo  f=stats  r=raw");
  Serial.println();
}

void loop() {
  // ---- comandos ----
  while (Serial.available()) {
    char k = Serial.read();
    if (k == 'c') { cBudget = 40; modeAll = true;  Serial.println("\n[modo] 40 transacciones crudas...\n"); }
    else if (k == 'd') { modeAll = false;          Serial.println("\n[modo] normal (solo cambios)\n"); }
    else if (k == 'f') printStats();
    else if (k == 'r') startRaw();
  }

  // ---- fin de volcado crudo ----
  if (rawArm && (rawN >= RAW_MAX || millis() - rawStartMs > 250)) dumpRaw();

  // ---- modo 'c': imprimir transacciones encoladas ----
  while (rawTxTail != rawTxHead) {
    uint8_t n = rawTxLen[rawTxTail];
    Serial.print("TX  ");
    for (uint8_t i = 0; i < n; i++) { char h[4]; snprintf(h, 4, "%02X ", rawTxBuf[rawTxTail][i]); Serial.print(h); }
    Serial.println();
    rawTxTail = (rawTxTail + 1) % 48;
  }
  if (modeAll && cBudget <= 0 && rawTxTail == rawTxHead) {
    modeAll = false;
    Serial.println("\n[modo] normal (solo cambios)\n");
  }

  // ---- imprimir RAM cuando cambia ----
  if (shadowDirty && !modeAll) {
    delay(40);                 // dejar que termine el refresco completo
    shadowDirty = false;
    dumpShadow();
  }

  // ---- heartbeat ----
  static uint32_t hb = 0;
  if (millis() - hb > 4000) {
    hb = millis();
    Serial.printf("[vivo] tx=%u  SL~%s kHz   (d/c/f/r)\n", (unsigned)txCount, slKHz().c_str());
  }
}
