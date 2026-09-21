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
//  Portal cautivo: al conectarte a la red del ESP, el movil abre solo el panel
//  (como el login de un aeropuerto).  Necesita un DNS "atrapatodo" en el ESP.
#define WIFI_CAPTIVE     1

//  --- Router externo (solo modo 1 y 2) ---
#define WIFI_SSID        "NombreDeTuRouter"
#define WIFI_PASS        "ClaveDeTuRouter"
#define WIFI_AP_FALLBACK 1                    // modo 1: si no conecta en 15 s, crea su red

#define WIFI_HOSTNAME    "balanza-aut"        // -> http://balanza-aut.local/  (mDNS, poco fiable en el AP)

//  Hora por NTP (solo si hay internet).  Sin internet, el movil del operario
//  pone la hora al abrir el panel (POST /settime); si no, el historial usa
//  solo el orden y "hace N s" de la sesion actual.
#define ENABLE_NTP       1
#define NTP_SERVER       "pool.ntp.org"

//  Zona horaria (cadena POSIX TZ).  El historial se GUARDA en UTC pero se
//  MUESTRA en esta hora local.
//    Ecuador / Colombia / Peru (UTC-5, sin horario de verano): "<-05>5"
//    Mexico centro (UTC-6): "<-06>6"      Argentina/Chile (UTC-3): "<-03>3"
//    Espana (UTC+1 con verano): "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_POSIX         "<-05>5"

// ===========================================================================
//  MODO DE PRUEBA  -  elige que metodo(s) compilar
// ===========================================================================
//    ENABLE_OCR      ENABLE_SNIFFER     resultado
//    -----------     -------------      --------------------------------------
//        1                0             SOLO camara (no hay que soldar nada)  <= empieza por aqui
//        0                1             SOLO sniffer del bus DA/SL
//        1                1             los dos, y el panel web los compara
//
//  Lo que queda desactivado NI se compila NI reserva pines/RAM.
#define ENABLE_OCR       0
#define ENABLE_SNIFFER   1

// ===========================================================================
//  SNIFFER del bus del display  (TM1640 / CS2540 ,  DA = datos , SL = reloj)
// ===========================================================================
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
//  CAPTURA DE PESAJES  (maquina de estados comun a sniffer y OCR)
// ===========================================================================
//  Escenario: el operario sube la gaveta -> el peso se estabiliza -> quita la
//  gaveta -> vuelve a ~0  =>  se guarda.  UNA entrada por gaveta.
//  Un pesaje solo se guarda si el peso BAJO de verdad (paso por valores
//  intermedios) antes de llegar a 0.  Un salto LIMPIO a 0 (= TARA) no se guarda.
//
//  UNIDAD: el panel muestra "lb".  El firmware NO convierte nada -> el numero es
//  el que muestra la balanza tal cual.  La balanza debe estar puesta en LIBRAS.
//  (Para volver a "kg" busca "lb" en web_ui.cpp -> hay 6 sitios en el panel.)
//  OJO: estos umbrales son ABSOLUTOS, en la unidad de la balanza.  Estaban
//  puestos para kg; al pasar a LIBRAS (numeros ~2.2x mas chicos) hay que
//  bajarlos, si no los pesos livianos no se detectan bien.
#define WEIGH_ZERO_THRESH   0.06f   // por debajo de esto = plataforma vacia (en lb)
#define WEIGH_STABLE_MS     700     // (solo OCR) numero quieto este tiempo -> estable
#define WEIGH_CONFIRM_MS    1000    // tras vaciarse la plataforma, se comprueba
                                    //   este tiempo si fue retiro real antes de guardar
#define WEIGH_PARTIAL       0.085f  // el peso tiene que haber bajado AL MENOS esto (lb)
                                    //   (con carga aun presente) para contar como
                                    //   retiro real; si no -> se toma como TARA.
                                    //   Subir = menos "filas de mas" por menear el peso,
                                    //   pero mas riesgo de perder retiros livianos.
#define WEIGH_PARTIAL_FRAC  0.55f   // ...o haber bajado por debajo de esta FRACCION
                                    //   del valor estable (fraccion -> no depende de
                                    //   la unidad).  Vale el criterio mas laxo.
#define WEIGH_ZERO_MS       400     // cuanto tiene que durar el NEGATIVO para
                                    //   confirmar que hubo una tara
#define WEIGH_INVALID_MS    2500    // display apagado este tiempo -> pesaje cerrado
#define WEIGH_LOG_SIZE      120     // ultimos N pesajes (en RAM y en flash/NVS)
#define WEIGH_EPS           0.005f  // diferencia de peso considerada "el mismo valor"
#define WEIGH_PERSIST       1       // 1 = guarda el historial en NVS (sobrevive apagon)

// --- ronda ag: retiro rapido mas fiable + menos latencia -------------------
//  Si algo va mal y hay que volver al comportamiento anterior:
//    - rapido:  WEIGH_V2 = 0  y  WEIGH_FEED_MS = 40   (deja el codigo nuevo pero inerte)
//    - total:   copia los 4 ficheros de  ../_backup_pre_rebote/  al sketch y reflashea
#define WEIGH_V2           1        // 0 = comportamiento de antes de la ronda ag
#define WEIGH_DIP          0.05f    // el neto rebota a MENOS de -esto al retirar de
                                    //   verdad (la celda rebota); una TARA nunca baja
                                    //   de 0.  (en lb).  Subir si alguna tara se cuela;
                                    //   bajar si algun retiro rapido se sigue perdiendo.
#define WEIGH_SETTLE_MS    350      // con señal de retiro (intermedios O rebote) se
                                    //   guarda a los ~350 ms en vez de WEIGH_CONFIRM_MS.
                                    //   Ponlo = WEIGH_CONFIRM_MS para desactivar.
#define WEIGH_FEED_MS      20       // cada cuanto se alimenta la maquina de estados
                                    //   (era 40).  Mas muestras = pilla mejor el
                                    //   transitorio.  Vuelve a 40 para revertir.

// ===========================================================================
//  OCR de 7 segmentos  (pantalla PESO vista por la camara)
// ===========================================================================
#define OCR_FRAMESIZE     FRAMESIZE_VGA   // 640 x 480 en escala de grises
#define OCR_NUM_DIGITS    5               // digitos de la pantalla PESO
#define OCR_DECIMALS      2               // 17.20 -> 2 decimales (punto fijo)

//  --- Ritmo de lectura de la camara -------------------------------------
//  La lectura corre en su PROPIA tarea (nucleo 0), independiente del servidor
//  web: aunque el video del navegador vaya lento, esto sigue leyendo rapido.
//  (La captura de pesajes usa los parametros WEIGH_* de mas arriba.)
#define OCR_PERIOD_MS     200      // una lectura cada 200 ms  (~5 por segundo)

//  1 = detiene el sniffer mientras la camara captura.  En la S3 el periferico
//  LCD_CAM casi no sufre por las interrupciones, asi que normalmente sobra;
//  ponlo a 1 solo si al activar el OCR el bus se decodifica con basura.
#define OCR_PAUSE_SNIFFER 0

//  --- Localizacion de los digitos ---------------------------------------
//  OCR_AUTO 1 = la camara busca sola donde estan los 7-seg dentro del video;
//               NO hace falta encuadrarlos en una caja.  Funciona porque los
//               LEDs encendidos son lo mas brillante y estan sobre fondo negro.
//  OCR_AUTO 0 = usa la caja manual de abajo (se calibra en /config).
#define OCR_AUTO         1
#define OCR_DIGIT_ASPECT 0.62f           // ancho de un digito = alto de la banda * esto
#define OCR_DIGIT_PITCH  1.50f           // separacion entre digitos  = ancho digito * esto

//  --- Segmento "encendido": por COBERTURA, no por brillo medio -----------
//  Un segmento realmente encendido esta brillante a lo LARGO de todo el trazo;
//  el "sangrado" de un segmento vecino solo ilumina una punta.  Se cuenta el
//  % del trazo que supera el umbral.  Esto es lo que arregla 3 vs 5, 6 vs 8...
#define OCR_SEG_COVERAGE 55             // % del trazo encendido para contar "ON"

//  --- Reconocimiento por PLANTILLA (experimental, DESACTIVADO) ------------
//  Compara la forma completa del digito.  En teoria no confunde 3/5/6/8/9,
//  pero es MUY sensible a que la rejilla auto se mueva 1-2 px.  Deja 0 salvo
//  que quieras experimentar (se ensena en /config, seccion "Plantillas").
#define OCR_USE_TEMPLATES 0
#define OCR_TPL_W        14
#define OCR_TPL_H        22
#define OCR_TPL_MIN_NCC  0.60f
#define OCR_TPL_MARGIN   0.06f

//  OCR_AUTO_BRIGHT: nivel (0..255) a partir del cual un pixel cuenta como
//  "encendido".  0 = automatico (casi saturado).  Con LUZ AMBIENTE el bisel
//  blanco tambien satura; si estorba, fijalo a mano ~245 (aqui o en /config
//  campo "brillo ON") para que solo cuenten los LEDs del display.
#define OCR_AUTO_BRIGHT  0
//  Distancia (px) a la que busca "negro" alrededor de un trazo.  Subelo si los
//  digitos se ven MUY grandes (camara muy cerca); bajalo si muy pequenos.
#define OCR_DARK_DIST    12

//  Caja manual (px) - solo se usa si OCR_AUTO 0.  Se calibra en /config.
#define OCR_BOX_X        100
#define OCR_BOX_Y        150
#define OCR_BOX_W        440
#define OCR_BOX_H        180
#define OCR_DIGIT_GAP    6               // separacion entre celdas (px)
#define OCR_SLANT        0.10f           // inclinacion de los digitos (0 = recto)
#define OCR_ON_RATIO     0.45f           // umbral segmento = min + (max-min) * ratio
#define OCR_MIN_CONTRAST 35              // si (max-min) < esto -> display apagado

//  Camara: exposicion/ganancia MANUAL para que el brillo no cambie solo
#define OCR_AEC_VALUE    400             // exposicion 0..1200 (sube si oscuro)
#define OCR_CONTRAST     2               // -2..2

//  Esta placa no trae LED de flash (el display ya emite luz propia).
//  Si conectas uno a un GPIO libre, ponlo aqui; -1 = desactivado.
#define OCR_FLASH_PIN    -1

// --------------------------------------------------------------------------
//  PINES DE LA CAMARA  (conector FFC)
// --------------------------------------------------------------------------
//  VALORES POR DEFECTO = pinout ESP32-S3-EYE, que es el que copian casi todas
//  las placas "ESP32-S3-WROOM CAM" genericas.  Si la camara no inicializa
//  (esp_camera_init fallo 0x...), busca el pinout EXACTO de tu placa (pagina
//  del vendedor / esquematico) y corrige estos numeros.
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4      // SCCB SDA
#define CAM_PIN_SIOC     5      // SCCB SCL
#define CAM_PIN_D7      16      // Y9
#define CAM_PIN_D6      17      // Y8
#define CAM_PIN_D5      18      // Y7
#define CAM_PIN_D4      12      // Y6
#define CAM_PIN_D3      10      // Y5
#define CAM_PIN_D2       8      // Y4
#define CAM_PIN_D1       9      // Y3
#define CAM_PIN_D0      11      // Y2
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13
