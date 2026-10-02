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
#define WEIGH_STABLE_MS     700     // (solo con WEIGH_PLATEAU 0) numero quieto este tiempo -> estable
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
#define WEIGH_ARCHIVE       1       // 1 = ademas del buffer de WEIGH_LOG_SIZE, guarda TODOS los pesajes
                                    //   en un archivo en flash (LittleFS, particion "spiffs") hasta que
                                    //   se pulse "Borrar todo".  Si la particion no existe o se llena, el
                                    //   programa sigue con el buffer.  0 = comportamiento anterior.

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

// --- captura por VENTANA TOLERANTE (gavetas con agua que escurre, <= ~2 s puestas) ---
//  Antes el valor solo se daba por "estable" si NO cambiaba nada durante ~0,25 s
//  (y 4 refrescos iguales); con un escurrido de 0,1 lb/s eso casi nunca ocurria.
//  Ahora se da por ASENTADO si las lecturas de los ultimos WEIGH_WIN_MS varian menos
//  de WEIGH_BAND.  Garantia de "nada de la bajada": la referencia es el primer valor
//  asentado de ESA gaveta (se reinicia con cada gaveta) y despues solo se aceptan
//  valores asentados a menos de max(WEIGH_TOL_ABS, WEIGH_TOL_PCT %) de ella; la
//  caida al retirar la gaveta baja decenas de lb en fracciones de segundo, asi que
//  ni forma un tramo asentado ni entra en ese margen.
//  Durante los primeros WEIGH_PLACE_MS con carga la referencia si puede subir
//  (gaveta posada despacio, sostenida por la mano).
//  Pon WEIGH_PLATEAU 0 para volver EXACTAMENTE al criterio anterior.
#define WEIGH_PLATEAU      1
#define WEIGH_WIN_MS       250      // ventana de lecturas (ms)
#define WEIGH_BAND         0.15f    // max-min dentro de la ventana (lb; la balanza va de 0,1 en 0,1)
#define WEIGH_TOL_ABS      1.0f     // margen respecto a la referencia (lb) ...
#define WEIGH_TOL_PCT      3.0f     // ... o este % de ella, lo que sea MAYOR
#define WEIGH_PLACE_MS     1000     // fase de colocacion: la referencia puede subir

//  El valor asentado manda siempre, salvo que DESPUES de la ultima vez que se dio por
//  asentado el peso suba por encima de valor / WEIGH_PLAUS_FRAC: entonces era una pausa
//  en mitad de la colocacion (caso real: 5.3 lb) y se descarta (PERDIDO, cap>0 en /log).
//  Un golpe al poner no lo activa (su pico es anterior al valor asentado).  0 = desactivado.
#define WEIGH_PLAUS_FRAC   0.7f

//  RESCATE: una gaveta que se RETIRO de verdad pero no dejo ningun valor asentado valido
//  (nunca asento, o se descarto por el chequeo de arriba) no se pierde: se guarda
//  WEIGH_RESCUE_FRAC x el maximo visto, marcada "e" (ESTIMADO) en /log.  Solo si estuvo
//  puesta >= WEIGH_RESCUE_MIN_MS, su maximo es >= WEIGH_RESCUE_MIN_LB (descarta roces de
//  la mano) y el peso bajo por intermedios tras el maximo (una tara no cuenta).
//  WEIGH_RESCUE_FRAC 0 = desactivado.
#define WEIGH_RESCUE_FRAC    0.92f
#define WEIGH_RESCUE_MIN_LB  15.0f
#define WEIGH_RESCUE_MIN_MS  600

// --- registro de diagnostico (/log) ---
//  Guarda en RAM (no en flash) los ultimos eventos: pesajes guardados/descartados y
//  por que, conexiones/desconexiones de celulares con su señal, bloqueos del bucle
//  principal y el motivo del ultimo reinicio.  Se descarga desde el movil: /log
//  Se pierde si el ESP se reinicia o se apaga: descargalo antes.  0 = desactivado.
#define DIAG_LOG           1

//  Cuantas lineas (eventos) conserva el registro en RAM; al llenarse, se va
//  borrando la mas vieja.  Cada linea pesa 64 bytes fijos (ver diaglog.cpp),
//  asi que el costo en RAM es DIAG_LOG_LINES*64.  De una jornada real (pesca
//  2026-09-30, ~75 min) salieron ~4 lineas/min -> con 1024 alcanza para unas
//  4 horas seguidas antes de empezar a perder las mas antiguas; si una
//  jornada dura mas, descarga /log a mitad de camino.
#define DIAG_LOG_LINES     1024

