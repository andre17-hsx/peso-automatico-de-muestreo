/*
 * ===========================================================================
 *  peso_balanza_cam  -  Lee el PESO de la balanza cuenta-precio de 2 formas
 *                       y lo publica por WiFi (servidor web).
 * ===========================================================================
 *
 *  Placa:   ESP32-S3 Super Mini  (chip ESP32-S3FH4R2: 4MB flash + 2MB PSRAM Quad,
 *           un solo USB-C nativo -- SIN chip CH340).
 *           (antes: DOIT ESP32-S3-WROOM N16R8 con FFC de camara; se cambio de
 *            modulo por daño de hardware, ver notas de cableado mas abajo)
 *  Balanza: ACS-JC36CV28  -->  display WP-91110-2540-V7 (chip CS2540 == TM1640)
 *
 *  METODO 1 - OCR con la camara  (ENABLE_OCR):
 *     fotografia la pantalla PESO en escala de grises y decodifica los 7
 *     segmentos de cada digito.  NO hay que soldar ni abrir la balanza.
 *
 *  METODO 2 - Sniffer del bus del display  (ENABLE_SNIFFER):
 *     escucha pasiva de DA (datos) y SL (reloj), decodifica el protocolo tipo
 *     TM1640 y reconstruye los 16 bytes de RAM -> PESO / PRECIO / TOTAL exactos.
 *
 *  Se elige uno, otro o los dos en config.h (ENABLE_OCR / ENABLE_SNIFFER).
 *  Lo desactivado no se compila.  El servidor web muestra lo que este activo.
 *
 * ---------------------------------------------------------------------------
 *  CONEXION DEL SNIFFER  (solo si ENABLE_SNIFFER, 3 cables)  -- ver config.h
 * ---------------------------------------------------------------------------
 *     Balanza SL   --- [470 ohm] ------------------  GPIO1  (SNIF_PIN_SL)
 *     Balanza DA   --- [470 ohm] --- [diodo Schottky] --- GPIO4  (SNIF_PIN_DA)
 *                        (BAT54/1N5817/1N5819; anodo hacia la balanza, catodo
 *                         hacia el GPIO -- bloquea que una fuga interna del
 *                         ESP se devuelva hacia la balanza.  Ver README/chat:
 *                         asi se resolvio el bus "volviendose loco" con el
 *                         modulo anterior, que resulto tener 2 GPIO dañados
 *                         con fuga hacia el riel de 5V)
 *     Balanza GND  ------------------------------------------------  GND     (masa comun OBLIGATORIA)
 *
 *     NO conectar VCC_DIS.  Logica de 3 V -> entra directa al GPIO del S3.
 *     GPIO1 y GPIO4 limpios en esta placa.  Strapping S3 (0/3/45/46) y USB
 *     nativo (19/20) son del silicio, evitarlos sin importar la placa.  Esta
 *     placa no tiene conector de camara ni microSD, asi que ese bloqueo ya
 *     no aplica -- solo usa los 2 pines de arriba de todos modos.
 *     Durante la prueba: balanza con SU bateria, placa por USB-C.
 *
 * ---------------------------------------------------------------------------
 *  ARDUINO IDE   (chip ESP32-S3FH4R2 -- 4MB flash, 2MB PSRAM Quad)
 * ---------------------------------------------------------------------------
 *     Placa:              "ESP32S3 Dev Module"  (generico, sirve para esta placa)
 *     PSRAM:              "Disabled" (no se usa: la camara/OCR esta apagada,
 *                          que era lo unico que la necesitaba) o "QSPI PSRAM"
 *     Flash Size:         "4MB (32Mb)"
 *     Partition Scheme:   cualquiera pensado para 4MB, p.ej. "Minimal SPIFFS
 *                          (1.9MB APP/190KB SPIFFS)" -- confirma que el
 *                          binario compilado entra en el espacio disponible.
 *     USB CDC On Boot:    "Enabled"  (esta placa NO tiene CH340 -- solo el
 *                          USB-C nativo; si lo dejas en Disabled no vas a ver
 *                          NADA en el Monitor Serie)
 *     Core Debug Level:   "None"
 *
 *     Pon tu WiFi en config.h antes de compilar.
 * ===========================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdlib.h>

#include "config.h"
#include "ocr7seg.h"
#include "sniffer_tm1640.h"
#include "weighlog.h"
#include "web_ui.h"

#if WIFI_CAPTIVE
  #include <DNSServer.h>
  #include <esp_netif.h>
  DNSServer dnsServer;
  static bool g_apUp = false;      // hay un AP en marcha -> portal cautivo

  // Fuerza que el DHCP del AP entregue la IP del ESP como servidor DNS
  // (algunos cores no lo hacen solos y el portal cautivo no salta).
  static void apOfferOwnDns() {
    esp_netif_t* ni = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ni) return;
    esp_netif_dns_info_t dns; memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4("192.168.4.1", &dns.ip.u_addr.ip4);
    esp_netif_dhcps_stop(ni);
    esp_netif_set_dns_info(ni, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offerDns = 2;   // OFFER_DNS
    esp_netif_dhcps_option(ni, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns));
    esp_netif_dhcps_start(ni);
  }
#endif

WebServer server(80);

// Gracia tras levantar el AP: el driver puede soltar un AP_STOP espurio mientras
// termina de configurarse (y otro eco si lo relevantamos); no cuenta como caida.
#define AP_GRACE_MS 5000
static uint32_t g_apStartMs = 0;

// ---------------------------------------------------------------------------
static void wifiStartAP() {
  // IP fija 192.168.4.1; el DHCP del AP reparte esa misma IP como DNS -> hace
  // falta para el portal cautivo (el DNS "atrapatodo" del ESP).
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                    IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
#ifdef WIFI_AP_CHANNEL
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL);
#else
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
#endif
  Serial.printf("\n  ================ RED PROPIA DEL ESP32 ================\n"
                "   WiFi:   %s\n   Clave:  %s\n   Abrir:  http://%s/\n"
                "  =====================================================\n",
                WIFI_AP_SSID, WIFI_AP_PASS, WiFi.softAPIP().toString().c_str());
#if WIFI_CAPTIVE
  apOfferOwnDns();
  g_apUp = true;
#endif
  g_apStartMs = millis();      // arranca (o reinicia) la ventana de gracia
}

// ---------------------------------------------------------------------------
//  Auto-recuperacion del AP:  si el driver de WiFi lo llega a parar solo (p.
//  ej. por un bajon de tension momentaneo en la alimentacion), lo volvemos a
//  levantar.  Esto NO sustituye arreglar la alimentacion (C_out pendiente) --
//  es una red de seguridad para que el panel no se quede muerto hasta que
//  alguien reinicie el ESP a mano.
//
//  Dos vias, para cubrir los dos escenarios posibles:
//   1) El driver SI avisa (evento AP_STOP) -> reaccion inmediata.  El evento
//      llega en la tarea de WiFi, asi que aqui solo se marca una bandera; el
//      restart real lo hace loop() (evitar tocar el driver desde su propio
//      callback).
//   2) El driver NO avisa (se queda "zombie": el modo sigue en AP pero ya no
//      emite) -> red de seguridad barata: se revisa el modo cada 5 s,
//      aprovechando el heartbeat que ya existia (CERO temporizadores nuevos).
// ---------------------------------------------------------------------------
static volatile bool g_apNeedsRestart = false;
static uint32_t      g_apRecoverCount = 0;

static void wifiRecoverAP(const char* why) {
  g_apRecoverCount++;
  Serial.printf("[wifi] recuperando la red propia (%s) -> recuperacion #%lu\n",
                why, (unsigned long)g_apRecoverCount);
  wifiStartAP();
}

static void onWifiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_AP_STOP) g_apNeedsRestart = true;   // lo procesa loop()
}

//  llamado cada 5 s desde el heartbeat que ya existia en loop() (ver mas abajo)
static void wifiCheckHealth() {
#if WIFI_MODE == 0
  bool apOk = (WiFi.getMode() == WIFI_MODE_AP);
#elif WIFI_MODE == 2
  bool apOk = (WiFi.getMode() == WIFI_MODE_APSTA);
#else
  bool apOk = true;    // modo 1 (solo STA): la reconexion al router es otro tema, no se toca aqui
#endif
  if (!apOk) wifiRecoverAP("modo WiFi inesperado");
}

#if WIFI_MODE == 1
// intenta unirse al router; devuelve true si conecta antes de 15 s
static bool wifiJoinRouter() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] uniendo al router");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] router OK  ->  http://%s/   (%s)\n",
                  WiFi.localIP().toString().c_str(), WIFI_SSID);
#if ENABLE_NTP
    configTzTime(TZ_POSIX, NTP_SERVER);         // NTP + zona horaria local
#endif
    return true;
  }
  Serial.println("[wifi] router no disponible");
  return false;
}
#endif

static void wifiConnect() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.onEvent(onWifiEvent);       // armado ANTES de levantar el AP, por si se cae ya de arranque

#if WIFI_MODE == 0                                // ---- SOLO AP (campo) ----
  WiFi.mode(WIFI_AP);
  wifiStartAP();
#elif WIFI_MODE == 2                              // ---- STA + AP a la vez ----
  WiFi.mode(WIFI_AP_STA);
  wifiStartAP();                                  // la red propia esta lista YA
  WiFi.begin(WIFI_SSID, WIFI_PASS);               // y se une al router en segundo plano
  #if ENABLE_NTP
  configTzTime(TZ_POSIX, NTP_SERVER);             // SNTP + zona horaria local
  #endif
  Serial.println("[wifi] router en segundo plano (no bloquea; NTP si conecta)");
#else                                             // ---- SOLO STA (1) ----
  WiFi.mode(WIFI_STA);
  if (!wifiJoinRouter()) {
  #if WIFI_AP_FALLBACK
    WiFi.mode(WIFI_AP);
    wifiStartAP();
  #endif
  }
#endif

  if (MDNS.begin(WIFI_HOSTNAME))
    Serial.printf("[mdns] http://%s.local/\n", WIFI_HOSTNAME);

#if WIFI_CAPTIVE
  if (g_apUp) {
    // DNS "atrapatodo": CUALQUIER dominio -> la IP del ESP.  Con eso, la
    // deteccion de portal del movil (Android/iOS/Windows sondean una URL fija)
    // cae en nuestro servidor y este responde con un redirect al panel.
    dnsServer.setTTL(3600);
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.println("[wifi] portal cautivo ON (DNS *)  ->  el movil abre el panel solo");
  }
#endif
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(240);                      // maximo: camara + captura del bus
  setenv("TZ", TZ_POSIX, 1); tzset();           // hora local para MOSTRAR (se guarda en UTC)
  Serial.println("\n=== Peso automatico de muestreo (ESP32-S3) ===");
  Serial.printf("[modo] OCR=%d  SNIFFER=%d  WIFI_MODE=%d\n",
                ENABLE_OCR, ENABLE_SNIFFER, WIFI_MODE);

  weighBegin();                        // recupera el historial guardado en NVS

#if ENABLE_SNIFFER
  snifBegin();
  Serial.printf("[snif] escuchando el bus  SL=GPIO%d  DA=GPIO%d\n",
                SNIF_PIN_SL, SNIF_PIN_DA);
#endif

#if ENABLE_OCR
  ocrLoadParams();
  if (ocrBegin()) {
    Serial.println("[ocr] camara lista");
    ocrStartTask();                    // el bucle de lectura corre en su tarea (core 0)
  } else {
    Serial.println("[ocr] ERROR iniciando la camara (revisa PSRAM y CAM_PIN_*)");
  }
#endif

  wifiConnect();
  webBegin(server);
  server.begin();
  Serial.println("[web] servidor en marcha");
}

// ---------------------------------------------------------------------------
void loop() {
  if (g_apNeedsRestart) {
    g_apNeedsRestart = false;
    if (millis() - g_apStartMs >= AP_GRACE_MS) wifiRecoverAP("evento AP_STOP");   // dentro de la gracia: se ignora
  }

#if WIFI_CAPTIVE
  if (g_apUp) dnsServer.processNextRequest();
#endif
  server.handleClient();      // el OCR ya NO se hace aqui: va en su propia tarea

#if ENABLE_SNIFFER
  snifLoop();
  // alimenta la maquina de estados del pesaje con el PESO del bus.  Cada
  // WEIGH_FEED_MS (config.h, 20 ms ~= 50 Hz): mas muestras = pilla mejor el
  // transitorio al retirar la gaveta rapido.
  {
  #ifndef WEIGH_FEED_MS
  #define WEIGH_FEED_MS 20
  #endif
    static uint32_t tw = 0;
    if (millis() - tw >= WEIGH_FEED_MS) {
      tw = millis();
      SnifState s; snifGet(s);
      weighFeed(s.pesoVal, s.precioVal, s.totalVal,
                s.stable, s.valid && !isnan(s.pesoVal));
    }
  }
#endif

  static uint32_t hb = 0;
  if (millis() - hb > 5000) {
    hb = millis();
    Serial.print("[hb]");
#if ENABLE_OCR
    { OcrResult o; ocrGetLast(o);
      Serial.printf("  OCR:%s", o.valid ? o.text : "--"); }
#endif
#if ENABLE_SNIFFER
    { SnifState s; snifGet(s);
      Serial.printf("  BUS valid:%d stable:%d  peso\"%s\"=%.2f  precio\"%s\"=%.2f  total\"%s\"=%.2f",
        s.valid, s.stable,
        s.peso,   (double)s.pesoVal,
        s.precio, (double)s.precioVal,
        s.total,  (double)s.totalVal); }
#endif
    Serial.printf("  pesaje:%s cap:%.2f  rssi:%d  heap:%lu(min %lu)  APcli:%d  APrec:%lu\n",
                  weighStateName(), weighLatched(), (int)WiFi.RSSI(),
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
                  WiFi.softAPgetStationNum(), (unsigned long)g_apRecoverCount);
    wifiCheckHealth();     // red de seguridad: por si el AP quedo "zombie" sin avisar
  }
}
