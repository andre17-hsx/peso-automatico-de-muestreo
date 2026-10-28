#pragma once
// ===========================================================================
//  CONFIGURACION  -  Peso automatico de muestreo
//  Placa:  ESP32-S3 Super Mini (chip ESP32-S3FH4R2: 4 MB flash + 2 MB PSRAM
//          Quad, un solo USB-C nativo, SIN chip CH340).
//  (antes: DOIT ESP32-S3-WROOM N16R8 con FFC de camara -- se cambio de modulo
//   por daño de hardware; ver notas de cableado junto a SNIF_PIN_DA abajo)
// ===========================================================================

// --------------------------------------------------------------- WiFi -------
//  WIFI_MODE:  0 = SOLO AP  -> el ESP crea SU PROPIA red, sin router (CAMPO).
//              1 = SOLO STA -> se une a un router (AP de emergencia si falla).
//              2 = STA + AP  -> las dos: su red SIEMPRE + se une al router si lo ve.
//  Para conectarte SOLO al ESP con el movil (piscina, sin internet): pon 0.
//  (Con 2 tambien esta la red propia, ademas del router.)
#define WIFI_MODE        0

//  --- Red PROPIA del ESP32 (modo 0 y 2, y fallback del 1) ---
//  El movil se conecta a esta red WiFi y abre  http://192.168.4.1/
#define WIFI_AP_SSID     "balanza-aut"        // nombre de la red WiFi que crea el ESP
#define WIFI_AP_PASS     "cambia-esta-clave"        // clave (MINIMO 8 caracteres)
#define WIFI_AP_CHANNEL  6                    // canal: 1, 6 u 11 (los que no se pisan)
//  Potencia de transmision del WiFi.  Menos potencia = menos corriente y menos calor en
//  el regulador de la placa, pero menos alcance.  En campo hubo celulares a -80/-90 dBm:
//  si con esto se desconectan mas, sube a WIFI_POWER_17dBm o WIFI_POWER_19_5dBm.
#define WIFI_TX_POWER    WIFI_POWER_15dBm
//  Portal cautivo: al conectarte a la red del ESP, el movil abre solo el panel
//  (como el login de un aeropuerto).  Necesita un DNS "atrapatodo" en el ESP, que
//  tambien atrae el trafico de fondo de las apps de los celulares hacia el ESP.
//  Con 0 la red NO anuncia servidor DNS a los celulares y Android la trata como rota:
//  probado el 2026-10-08, la pagina no cargaba (ni con los datos moviles apagados).
#define WIFI_CAPTIVE     1

//  --- Router externo (solo modo 1 y 2) ---
#define WIFI_SSID        "NombreDeTuRouter"
#define WIFI_PASS        "ClaveDeTuRouter"
#define WIFI_AP_FALLBACK 1                    // modo 1: si no conecta en 15 s, crea su red

#define WIFI_HOSTNAME    "balanza-aut"        // -> http://balanza-aut.local/  (mDNS, poco fiable en el AP)

//  Hora por NTP (solo si hay internet).  Sin internet, el movil del operario
//  pone la hora al abrir el panel (POST /settime); si no, el historial usa
//  solo el orden.
#define ENABLE_NTP       1
#define NTP_SERVER       "pool.ntp.org"

//  Zona horaria (cadena POSIX TZ).  El historial se GUARDA en UTC pero se
//  MUESTRA en esta hora local.
//    Ecuador / Colombia / Peru (UTC-5, sin horario de verano): "<-05>5"
//    Mexico centro (UTC-6): "<-06>6"      Argentina/Chile (UTC-3): "<-03>3"
//    Espana (UTC+1 con verano): "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_POSIX         "<-05>5"

// ===========================================================================
//  SNIFFER del bus del display  (TM1640 / CS2540 ,  DA = datos , SL = reloj)
// ===========================================================================
//  Interruptor general: 0 lo apaga por completo (ni se compila ni reserva
//  pines/RAM).  No hay otro metodo de lectura; esto es solo para pruebas del
//  panel web sin la balanza conectada.
#define ENABLE_SNIFFER   1
#define SNIFFER_DEBUG    0     // 1 = rutas de calibracion del bus (/config, /snif_*): solo para una balanza nueva
//  Pines a evitar en el ESP32-S3 Super Mini (esta placa NO tiene conector de
//  camara ni de microSD, asi que ese bloqueo de la placa anterior ya no
//  aplica -- lo que queda es del SILICIO del chip, vale para cualquier placa):
//    - UART0 (monitor serie) : 43 (TX), 44 (RX)
//    - USB nativo : 19 (D-), 20 (D+)
//    - Strapping ESP32-S3 : 0, 3, 45, 46   -> NO usar como entrada del bus
//    - LED RGB WS2812 de la placa : 48
//  El ISR lee GPIO_IN_REG -> usa pines <= 31.
//  GPIO1 y GPIO4: limpios en esta placa, confirmados en uso.
//  GPIO8 y GPIO21 quedaron de pruebas con el modulo ANTERIOR (dañado, ya no
//  se usa) -> no tienen relacion con esta placa, ignoralos si los ves en
//  commits viejos.
//
//  DA lleva un diodo Schottky en serie ademas de la resistencia de 470 ohm
//  (anodo hacia la balanza, catodo hacia el GPIO) -- bloquea que una fuga
//  interna del ESP se devuelva hacia el bus de la balanza.  Ver el comentario
//  de cableado al inicio del .ino para el detalle completo.
#define SNIF_PIN_SL      1    // SL  (reloj)   <-- [470 ohm] <-- balanza SL
#define SNIF_PIN_DA      4    // DA  (datos)   <-- [470 ohm + diodo Schottky] <-- balanza DA
#define SNIF_STABLE_N    4    // refrescos seguidos con el mismo PESO -> "estable"

//  --- Mapa RAM(16 bytes) -> digitos fisicos.  Ajustable luego desde /config.
//  Cada lista va de IZQUIERDA a DERECHA (el primero = mas significativo).
//  PESO = 5 digitos , PRECIO = 5 , TOTAL = 6   (16 = 16 grids del TM1640).
//  Indices dentro de shadow[0..15].
//  NOTA (balanza ACS-JC36CV28): en TOTAL los dos ultimos grids estan
//  cruzados en la placa del display -> 15 y 14 en vez de 14 y 15
//  (fisico 13.80 se leia 13.08).  PESO y PRECIO van en orden natural.
#define SNIF_PESO_ADDR    { 0, 1, 2, 3, 4 }
#define SNIF_PRECIO_ADDR  { 5, 6, 7, 8, 9 }
#define SNIF_TOTAL_ADDR   { 10, 11, 12, 13, 15, 14 }

//  Sube este numero para que, al reflashear, se IGNORE el mapa guardado en NVS
//  (p. ej. de un "Resolver mapa" que salio mal) y se use el de aqui arriba.
//  Los ajustes que hagas luego en /config siguen mandando.
#define SNIF_MAP_VERSION  5

//  Decimales de cada display.
//    >= 0 -> FIJO ese numero, ignorando el bit de punto del display (asi un
//            icono NET/TARA que se encienda tras tarar NO descoloca el valor).
//    -1   -> lo marca el bit de punto (para campos que pueden ser entero O decimal).
//  ACS-JC36CV28: PESO y TOTAL siempre 2 decimales; PRECIO puede ser entero
//  ("65") o decimal ("1.50") -> -1.
#define SNIF_PESO_DEC    2
#define SNIF_PRECIO_DEC  -1
#define SNIF_TOTAL_DEC   2

//  Que bit del byte lleva cada segmento:   a, b, c, d, e, f, g, punto
//  (permutacion a deducir mirando /sniffer con valores conocidos)
#define SNIF_SEG_BIT     { 0, 1, 2, 3, 4, 5, 6, 7 }

// ===========================================================================
//  CAPTURA DE PESAJES  (maquina de estados del pesaje)
// ===========================================================================
//  Regla: el operario sube la gaveta -> el peso pasa de WEIGH_MIN_LOAD -> se busca
//  el valor ESTABLE -> quita la gaveta -> el peso pasa por CERO (o queda negativo)
//  => se guarda el valor estable o, si no lo hubo, el valor donde la lectura mas tiempo
//  estuvo quieta.
//  UNA entrada por gaveta.  Menos de WEIGH_MIN_LOAD = tara / nada (una gaveta pesa
//  siempre mas).  En campo nunca se presiona TARA con una gaveta llena.
//
//  UNIDAD: el panel muestra "lb".  El firmware NO convierte nada -> el numero es
//  el que muestra la balanza tal cual.  La balanza debe estar puesta en LIBRAS.
//  (Para volver a "kg" busca "lb" en web_ui.cpp -> hay 6 sitios en el panel.)
//  OJO: estos umbrales son ABSOLUTOS, en la unidad de la balanza.
#define WEIGH_ZERO_THRESH   0.06f   // <= esto: la gaveta paso por cero (o quedo negativa) -> se cierra
#define WEIGH_MIN_LOAD      12.0f   // una gaveta empieza al llegar a este peso; menos = tara / nada
#define WEIGH_MAX_LOAD      80.0f   // mas que esto = fuera de rango, se ignora (prueba de display
                                    //   "888.8" al encender, presion fuerte de la mano)
#define WEIGH_MIN_MS        600     // ms minimos puesta para guardarse (un golpe de la mano no es gaveta)
#define WEIGH_INVALID_MS    2500    // display ilegible/congelado este tiempo con gaveta puesta -> se cierra y se
                                    // guarda el ultimo estable (o el estimado); otra gaveta solo tras ver cero
#define WEIGH_FRAME_TIMEOUT_MS 1000 // sin un refresco NUEVO del display en este tiempo = display congelado
#define WEIGH_LOG_SIZE      120     // ultimos N pesajes (en RAM y en flash/NVS)
#define WEIGH_EPS           0.005f  // diferencia de peso considerada "el mismo valor"
#define WEIGH_PERSIST       1       // 1 = guarda el historial en NVS (sobrevive apagon)
#define WEIGH_ARCHIVE       1       // 1 = ademas del buffer de WEIGH_LOG_SIZE, guarda TODOS los pesajes
                                    //   en un archivo en flash (LittleFS, particion "spiffs") hasta que
                                    //   se pulse "Borrar todo".  Si la particion no existe o se llena, el
                                    //   programa sigue con el buffer.  0 = comportamiento anterior.
#define WEIGH_FEED_MS      20       // cada cuanto se alimenta la maquina de estados (ms)

// --- valor ESTABLE: captura por VENTANA TOLERANTE (gavetas con agua que escurre) ---
//  Se da por ASENTADO un valor si las lecturas de los ultimos WEIGH_WIN_MS varian
//  menos de WEIGH_BAND.  La referencia es el primer valor asentado de ESA gaveta y
//  despues solo se aceptan valores a menos de max(WEIGH_TOL_ABS, WEIGH_TOL_PCT %) de
//  ella: la caida al retirar la gaveta ni forma un tramo asentado ni entra en ese
//  margen.  Durante los primeros WEIGH_PLACE_MS la referencia si puede subir
//  (gaveta posada despacio, sostenida por la mano).
#define WEIGH_WIN_MS       250      // ventana de lecturas (ms)
#define WEIGH_BAND         0.15f    // max-min dentro de la ventana (lb; la balanza va de 0,1 en 0,1)
#define WEIGH_TOL_ABS      1.0f     // margen respecto a la referencia (lb) ...
#define WEIGH_TOL_PCT      3.0f     // ... o este % de ella, lo que sea MAYOR
#define WEIGH_PLACE_MS     1000     // fase de colocacion: la referencia puede subir

//  El valor ESTABLE manda siempre.  Excepcion "pausa a mitad de colocacion" (visto en
//  campo; hoy solo cuenta si la pausa es de 12 lb o mas): si el estable estuvo quieto en
//  total menos de WEIGH_PLAUS_HOLD_MS y despues el peso subio por
//  encima de valor / WEIGH_PLAUS_FRAC, ese estable no era el peso real y se guarda
//  WEIGH_RESCUE_FRAC x el maximo.  Si el estable duro mas, una subida posterior es una
//  PRESION de la mano y no lo anula.  WEIGH_PLAUS_FRAC 0 = desactivado.
#define WEIGH_PLAUS_FRAC     0.7f
#define WEIGH_PLAUS_HOLD_MS  400
#define WEIGH_RESCUE_FRAC    0.92f

//  Valor ESTIMADO (sin valor estable, la gaveta se quito muy rapido): se guarda el valor
//  donde la lectura estuvo QUIETA mas tiempo (la subida, la bajada y una presion de la mano
//  cambian rapido y no cuentan), marcado "e ... sost" en /log.  Si nunca estuvo quieta:
//  WEIGH_RESCUE_FRAC x el maximo ("e ... x0.92").

//  SIMULADOR DE PESAJES (solo para pruebas en oficina).  1 = NO lee la balanza:
//  genera gavetas falsas cada pocos segundos (normales, retiros rapidos, golpes,
//  pausas a mitad de colocacion, livianas) y las pasa por el pesaje REAL.  Cada
//  una queda en /log como "SIM #n W=<peso real> <tipo>" para comparar con lo guardado.
//  OJO: los pesajes simulados SE GUARDAN en el historial: exporta antes los reales y
//  usa "Borrar todo" al terminar.  El panel muestra "MODO SIMULADOR".  NUNCA a campo con 1.
#define WEIGH_SIM            0
#define WEIGH_SIM_GAP_MS     2000   // pausa media entre gavetas simuladas (ms)
#define WEIGH_SIM_TARE       9      // plataforma vacia: 9 = tara activa como en campo (marca -9),
                                    //   0 = sin tara (marca ~0, con ruido)

// --- VENTILACION de la caja del ESP (dos ventiladores que trabajan JUNTOS) ---
//  Uno mete aire y el otro lo saca: se encienden y apagan a la vez segun la
//  temperatura interna del chip (media de ~30 s), con histeresis: ON al llegar a
//  FAN_ON_C, OFF al bajar a FAN_OFF_C (y al menos FAN_MIN_ON_MS encendidos).
//  Si el sensor no da dato, se encienden (a prueba de fallos).
//  HARDWARE: cada ventilador con su transistor NPN (2N2222) en el lado de tierra,
//  470 ohm a la base, 10 k de base a GND, diodo (1N5819) en paralelo
//  con el ventilador, y los ventiladores alimentados desde la BATERIA (no del pin 3V3:
//  el arranque del motor podria reiniciar el ESP).  NUNCA el ventilador directo al GPIO.
#define FAN_ENABLE         1
#define FAN1_PIN           5        // base del transistor del ventilador que mete aire
#define FAN2_PIN           6        // base del que lo saca (-1 si los dos van al mismo pin)
#define FAN_ACTIVE_HIGH    1        // 1 = pin en ALTO enciende (NPN o MOSFET canal N)
#define FAN_ON_C           68.0f    // enciende con la media >= esto (temperatura del CHIP, no del aire)
#define FAN_OFF_C          60.0f    // apaga con la media <= esto
#define FAN_CHECK_MS       5000     // cada cuanto se mide
#define FAN_MIN_ON_MS      60000    // una vez encendidos, al menos esto

// --- registro de diagnostico (/log) ---
//  Guarda en RAM (no en flash) los ultimos eventos: pesajes guardados/descartados y
//  por que, conexiones/desconexiones de celulares con su señal, bloqueos del bucle
//  principal y el motivo del ultimo reinicio.  Se descarga desde el movil: /log
//  Se pierde si el ESP se reinicia o se apaga: descargalo antes.  0 = desactivado.
#define DIAG_LOG           1

//  Cuantas lineas (eventos) conserva el registro en RAM; al llenarse, se va
//  borrando la mas vieja.  Cada linea pesa 64 bytes fijos (ver diaglog.cpp),
//  asi que el costo en RAM es DIAG_LOG_LINES*64.  De una jornada real (pesca
//  2026-09-30, ~75 min) salieron ~4 lineas/min -> con 256 alcanza para ~1 hora
//  antes de empezar a perder las mas antiguas (con 1024, ~4 h, pero cuesta 48 KB
//  mas de RAM, y la prueba del 2026-10-05 mostro el margen de memoria demasiado
//  justo al conectarse varios celulares); descarga /log a mitad de camino.
#define DIAG_LOG_LINES     256

