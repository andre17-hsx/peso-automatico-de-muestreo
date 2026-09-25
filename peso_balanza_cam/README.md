# peso_balanza_cam — ESP32-S3: sniffer del bus de la balanza + panel web

Lee **PESO, PRECIO y TOTAL** de la balanza cuenta-precio **escuchando de forma pasiva
el bus del display** (sniffer), guarda cada pesaje, los agrupa en **mallas y BIN** con
alertas para el operario, y lo publica por WiFi en un **panel web** (la propia placa
crea la red; no hace falta router ni internet).

Opcionalmente, y desactivado por defecto, también puede leer el display **con una
cámara** (OCR) — ver la sección 5. Ese método necesita una placa con conector de cámara.

Placa: **ESP32-S3 Super Mini** (chip ESP32-S3FH4R2: 4 MB flash + 2 MB PSRAM Quad,
un solo USB-C nativo, sin CH340). Antes se usaba un ESP32-S3-WROOM (N16R8) con
conector FFC de cámara y doble USB-C; se cambió de módulo por daño de hardware
(ver la sección 6, pines del sniffer).

---

## 1. Elegir el método — `config.h`

```c
#define ENABLE_OCR       0     // (opcional) leer el display con la cámara
#define ENABLE_SNIFFER   1     // escuchar el bus DA/SL del display  ← lo que se usa
```

| OCR | SNIFFER | resultado |
|:---:|:---:|---|
| 0 | 1 | **solo sniffer del bus** *(configuración actual)* |
| 1 | 0 | solo cámara — no hay que soldar ni abrir la balanza, pero exige una placa con conector de cámara (la Super Mini no lo tiene) |
| 1 | 1 | los dos; el panel web los compara |

Lo desactivado **no se compila** (ni reserva pines ni RAM).

`config.h` lleva las claves WiFi y **no está en el repositorio**: copia
`config.example.h` a `config.h` y pon tus claves antes de compilar.

---

## 2. Compilar (Arduino IDE)

- esp32 (Espressif) ≥ 3.0 · Placa: **ESP32S3 Dev Module** (genérico, sirve para el Super Mini)
- **PSRAM: `Disabled`** (o `QSPI PSRAM` si el menú obliga a elegir tipo) — con
  `ENABLE_OCR 0` no hace falta, era solo para los buffers de la cámara
- Flash Size: `4MB (32Mb)` · Partition: uno pensado para 4MB **que incluya SPIFFS**. Esa
  partición es donde se guarda el **historial completo** de pesajes (ver sección 3); sin ella
  el programa funciona igual, pero solo conserva los últimos 120 pesajes. El binario ocupa
  ~1,1 MB y no se usa OTA, así que cualquiera de estos sirve; la diferencia es cuántos
  pesajes caben (unos 24 bytes cada uno, usando el 75 % de la partición):

  | Esquema | SPIFFS | Pesajes |
  |---|---|---|
  | `Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)` | 128 KB | ~4 000 |
  | `Huge APP (3MB No OTA/1MB SPIFFS)` | 896 KB | ~28 000 |
  | `No OTA (2MB APP/2MB SPIFFS)` *(recomendado)* | 1,9 MB | ~61 000 |

  La primera vez que arranca con una partición nueva la formatea (unos segundos, más
  cuanto más grande sea) antes de levantar el WiFi.
- USB CDC On Boot: **`Enabled`** — esta placa NO tiene CH340, es un solo
  USB-C nativo; con `Disabled` no sale nada por el Monitor Serie
- Monitor Serie **115200** → sale la IP / la red creada.
- Si el puerto COM aparece y desaparece al conectar (típico de una placa nueva con el
  firmware de fábrica), **mantén presionado BOOT mientras conectas el USB**, suéltalo a
  los 2-3 s y sube el sketch; después ya arranca estable.

### WiFi — `config.h`

```c
#define WIFI_MODE  0     // 0 = SOLO AP (campo) · 1 = SOLO router · 2 = AP + router
```

| Modo | Qué hace |
|:---:|---|
| **0** *(actual)* | El ESP **crea su red** `WIFI_AP_SSID` / `WIFI_AP_PASS`. Panel en **`http://192.168.4.1/`**. Para campo, sin router. |
| 1 | Se une al router (`WIFI_SSID`/`WIFI_PASS`). Si no puede en 15 s y `WIFI_AP_FALLBACK 1`, crea su red. |
| 2 | Las dos: su red **siempre**, y además se une al router si lo ve (sin bloquear). Sirve para oficina. |

**Portal cautivo** (`WIFI_CAPTIVE 1`): al conectar el móvil a la red del ESP, el
sistema operativo abre solo el panel (como el login de un aeropuerto). Lo hace un
DNS "atrápalo-todo" en el ESP + un redirect a `192.168.4.1` para cualquier URL
desconocida. Si el mini-navegador del móvil va lento, ciérralo y abre el navegador
normal → `192.168.4.1`.

Sin internet no hay NTP: el **móvil del operario pone la hora** al abrir el panel
(`/settime`), así el historial lleva hora real. Si tampoco, queda solo el orden.

**Recuperación automática del AP** (modos 0 y 2): si el driver de WiFi detiene la red
propia (evento `AP_STOP`, o el modo deja de ser AP), el firmware la vuelve a levantar
solo. Ignora los avisos de los primeros 5 s tras levantarla (`AP_GRACE_MS`), que son
ruido del arranque. El contador `APrec` del latido por Serial cuenta las recuperaciones
reales: en funcionamiento normal debe quedar en **0**.

### Pines de la cámara (solo si `ENABLE_OCR 1`)

`config.h` trae el pinout **ESP32-S3-EYE** (el que copian casi todas las placas
"ESP32-S3-WROOM CAM"). Si al arrancar sale `esp_camera_init fallo 0x...`, busca el
pinout exacto de tu placa y corrige los `CAM_PIN_*`. Con el sniffer activo y el OCR
apagado, esos pines no se usan.

### Pines del sniffer (solo si `ENABLE_SNIFFER 1`)

| Balanza | | ESP32-S3 |
|---|---|---|
| `SL` | `— [470 Ω] —` | `GPIO1` |
| `DA` | `— [470 Ω] — [diodo Schottky] —` | `GPIO4` |
| `GND` | `———` | `GND` (masa común **obligatoria**) |

DA lleva además un **diodo Schottky** en serie (BAT54 / 1N5817 / 1N5819; ánodo hacia la
balanza, cátodo hacia el GPIO) — protege contra una fuga interna del ESP devolviéndose
hacia el bus de la balanza (así se descubrió que el módulo WROOM anterior tenía 2 GPIO
con fuga hacia el riel de 5V, y terminó reemplazándose). Usa Schottky y no un diodo de
silicio: la lógica es de ~3 V y los 0,6 V de un 1N4148 dejarían el nivel alto demasiado
justo.

No conectar `VCC_DIS`. `GPIO1` y `GPIO4` están limpios en esta placa; **no
`GPIO2`** (tiene el LED "ON" de la placa, que carga la línea). Evitar UART0
(`43/44`), USB nativo (`19/20`) y strapping S3 (`0/3/45/46`) — son del silicio,
valen para cualquier placa. El ISR lee `GPIO_IN_REG` → usa pines ≤ 31.

### Alimentación y montaje

- **La carcasa metálica de la balanza bloquea el WiFi** (jaula de Faraday): con la
  placa dentro y la tapa cerrada la red no aparece. El ESP32-S3 va en una **caja de
  plástico externa**, con solo los 3 cables del sniffer más la alimentación.
- Alimentación prevista desde la **batería de la propia balanza** (~4,2 V a plena carga,
  una celda de Li-ion): un convertidor **buck-boost** (TPS63020 / TPS63802, no un buck
  simple, porque la celda baja por debajo de 3,3 V al descargarse) ajustado a **3,3 V** y
  conectado al pin **`3V3`** de la placa.
- **No conectes nada a `BATTERY+` / `BATTERY-`** de la Super Mini: tiene su propio
  cargador de batería y no debe pelearse con el de la balanza.
- Recomendado un condensador de salida (electrolítico 220–470 µF + cerámico 100 nF) lo
  más cerca posible del pin `3V3`: los picos de corriente del WiFi (~0,4–0,5 A) son más
  rápidos de lo que responde el regulador.
- Consumo de referencia con el WiFi siempre activo (el firmware no duerme): ~150–250 mA
  de media → la autonomía en horas ≈ mAh de la batería ÷ mA. Es una estimación, no una
  medición; conviene medirla en la placa real.

---

## 3. Cuándo se guarda un pesaje — máquina de estados

`weighlog.cpp`, común a los dos métodos:

```
 VACIA ──(peso > WEIGH_ZERO_THRESH)──▶ CON CARGA
 CON CARGA (mientras el peso esté ESTABLE se RECUERDA su valor)
   └─ la plataforma cae por debajo del umbral ──▶ CONFIRMANDO (~WEIGH_CONFIRM_MS)
        · el peso BAJÓ de verdad (pasó por valores WEIGH_PARTIAL menores, o
          < WEIGH_PARTIAL_FRAC del estable si el neto es pequeño; con carga aún
          presente) ──────────────────────────────▶ se GUARDA  + weighOnCommit
        · salto LIMPIO a 0  ó  plataforma en NEGATIVO ──▶ es una TARA, NO se guarda
        · vuelve a haber carga (cambio de gaveta) ──▶ guarda el pendiente y sigue
```

- **Distingue una TARA de un retiro real** por *cómo* llega el peso a cero: un
  retiro hace oscilar la celda de carga (pasa por valores intermedios); una tara
  es un salto instantáneo y/o deja la plataforma en negativo.
- Un retiro **rápido** se sigue guardando (la celda siempre oscila al descargarse).
- Re-tarar a mitad de sesión funciona solo: ese salto a 0 se ignora, las
  siguientes gavetas se guardan normal.
- "Estable" = **captura por ventana tolerante** (`WEIGH_PLATEAU 1`, ver abajo). Con OCR se
  usa "número quieto `WEIGH_STABLE_MS`". El punto verde del panel sigue siendo el bit
  ESTABLE del propio bus: es solo visual y ya no decide qué valor se guarda.
- **PRECIO y TOTAL** se toman en cada muestra estable (si re-tecleas el precio con
  el peso puesto, se guarda el precio **nuevo**).
- Monitor Serie: `[pesaje] carga detectada...` · `[pesaje] retirado -> GUARDADO
  PESO 1.30 ...` · `[pesaje] TARA (...) -> pendiente descartado`.

Ajustes en `config.h` (los números son **absolutos, en la unidad de la balanza**: el
firmware no convierte nada, así que la balanza debe estar en **libras**):

| Parámetro | Qué es | Por defecto |
|---|---|---|
| `WEIGH_ZERO_THRESH` | por debajo = plataforma vacía | `0.06` |
| `WEIGH_CONFIRM_MS` | tiempo de comprobación "¿retiro o tara?" antes de guardar | `1000` |
| `WEIGH_PARTIAL` | cuánto tiene que bajar el peso con carga para contar como retiro real | `0.085` |
| `WEIGH_PARTIAL_FRAC` | …o a qué fracción del valor estable (para netos pequeños con tara) | `0.55` |
| `WEIGH_ZERO_MS` | cuánto dura el negativo para confirmar una tara | `400` |
| `WEIGH_STABLE_MS` | (solo OCR) número quieto → "estable" | `700` |
| `WEIGH_INVALID_MS` | display apagado este tiempo → cierra | `2500` |
| `WEIGH_LOG_SIZE` | pesajes recientes que guarda en el buffer de memoria/NVS | `120` |
| `WEIGH_ARCHIVE` | `1` = además del buffer, guarda **todos** los pesajes en un archivo en flash hasta *Borrar todo*; `0` = solo el buffer | `1` |
| `WEIGH_V2` | `0` = comportamiento anterior a la ronda ag (sin rebote/anti-meneo/commit rápido) | `1` |
| `WEIGH_DIP` | el neto rebota a menos de `-esto` al retirar de verdad (la celda rebota; una tara nunca baja de 0) | `0.05` |
| `WEIGH_SETTLE_MS` | con señal de retiro, guarda a los ~350 ms en vez de `WEIGH_CONFIRM_MS` | `350` |
| `WEIGH_FEED_MS` | cada cuánto se alimenta la máquina de estados (más muestras = pilla mejor el retiro rápido) | `20` |
| `WEIGH_PLATEAU` | `1` = captura por ventana tolerante (abajo); `0` = criterio anterior (quieto 250 ms + bit ESTABLE del bus) | `1` |
| `WEIGH_WIN_MS` | ventana en la que las lecturas deben variar poco para dar un valor por asentado | `250` |
| `WEIGH_BAND` | cuánto puede variar el peso dentro de esa ventana (el escurrido, ~0,1 lb/s, cabe de sobra) | `0.15` |
| `WEIGH_TOL_ABS` / `WEIGH_TOL_PCT` | tras el primer valor asentado solo se aceptan otros a menos de `max(TOL_ABS, TOL_PCT %)` de él | `1.0` / `3.0` |
| `WEIGH_PLACE_MS` | durante este tiempo desde que se detecta la carga, la referencia todavía puede SUBIR (gaveta posada despacio o sostenida con la mano) | `1000` |
| `DIAG_LOG` | `1` = registro de diagnóstico `/log` (ver sección 8); `0` = desactivado | `1` |

**Captura por ventana tolerante (`WEIGH_PLATEAU 1`).** El criterio anterior exigía que el
número estuviera *quieto* 250 ms y coincidiera con el bit ESTABLE del bus; con el escurrido
del producto (el peso baja ~0,1 lb/s) o una gaveta que se apoya con rebote, muchas veces
no se cumplía en los ~2 s que la gaveta está en la balanza y el pesaje se perdía. Ahora:

1. Un valor se da por **asentado** si las lecturas de los últimos `WEIGH_WIN_MS` (250 ms)
   varían menos de `WEIGH_BAND` (0,15 lb) — con al menos 5 lecturas y la ventana completa.
2. El **primer valor asentado de ESA gaveta** es su referencia (se borra con cada gaveta:
   una de 50 lb seguida de una de 29 lb no se contamina). A partir de ahí solo se aceptan
   valores asentados a menos de `max(1 lb, 3 %)` de la referencia.
3. **Nunca se guarda un valor de la caída a cero** al retirar la gaveta: esa bajada
   recorre decenas de lb en fracciones de segundo, ni forma un tramo asentado ni entra en
   la banda de la referencia. Lo mismo si el operario presiona la gaveta un instante antes
   de sacarla (esa subida se ignora).
4. Solo durante el primer `WEIGH_PLACE_MS` la referencia puede **subir** (la gaveta se posa
   despacio o la sostiene la mano un instante).
5. El valor guardado es el **último** asentado dentro de la banda (el escurrido se refleja
   como una bajada de décimas), y sigue rigiendo la lógica de tara/retiro de arriba.

Simulado offline con curvas de gaveta realistas (rebote de la celda, escurrido, display a
5–20 Hz): guarda ~90 % de las gavetas frente a ~20–50 % del criterio anterior; todas las que
guardaba el anterior las guarda también el nuevo (diferencia media de valor 0,05 lb); una
tara no se guarda; el peor valor guardado quedó a 0,35 lb por debajo del peso real. Las
gavetas que se retiran en menos de ~0,5 s tras posarse siguen sin poder guardarse (no llegan
a asentar), pero ahora **quedan anotadas** en `/log` como `PERDIDO`.

Si un pesaje real se sigue perdiendo, mira el Monitor Serie: la línea
`cero limpio (dip X.XX) sin descarga` dice cuánto rebotó — baja `WEIGH_DIP`
hacia ese valor. Si alguna **tara** se cuela, súbelo. Para volver del todo al
comportamiento anterior: `WEIGH_V2 0`.

### Mallas y BIN

El operario escribe en el panel el peso objetivo de una **malla** y de un **BIN** (en
lb). La estructura es: **cada BIN contiene varias mallas, y cada malla varios pesajes**.

- Cada pesaje se suma **completo** (una gaveta es un solo objeto, nunca se reparte) al
  acumulado de la malla en curso y al del BIN en curso.
- Una **malla se cierra** cuando su acumulado **iguala o supera** el objetivo. Ese
  pesaje es el último de esa malla, aunque se pase; el siguiente empieza otra desde 0.
- Un **BIN se cierra** solo en el mismo pesaje en que se cierra una malla y su acumulado
  iguala o supera el objetivo del BIN — así una malla nunca queda partida entre dos BIN.
  Si no hay objetivo de malla, el BIN se evalúa en cada pesaje.
- Los sobrantes **no se recortan ni se pasan al siguiente grupo**: si malla 1 se pasa por
  *a*, malla 2 por *b* y malla 3 por *c*, el BIN se pasa por *a+b+c* y cierra igual.
- **La malla se reinicia a 1 en cada BIN** (BIN 1: mallas 1, 2, 3… · BIN 2: mallas 1, 2…).
- El **peso total acumulado** es aparte y no cambia: sigue siendo la suma de toda la sesión.
- Objetivo en `0` o vacío = ese nivel no cierra grupos.
- Los objetivos, las etiquetas de malla/BIN de cada pesaje y los contadores se guardan en
  NVS (sobreviven a apagones). **Borrar todo** vuelve a BIN 1 / Malla 1 pero conserva los
  objetivos.

Limitaciones conocidas:
- **Borrar una fila suelta** del historial ajusta el peso total acumulado y los subtotales
  de su malla/BIN, pero **no** reajusta los acumulados de la malla/BIN *en curso* ni reabre
  grupos ya cerrados.
- Solo se pueden borrar filas sueltas de las **últimas 120** (las del buffer); las más
  antiguas quedan en el archivo y en el CSV.

### Panel `/` (para el operario)

Orden fijo: **pantalla LED** (PESO grande, PRECIO UNITARIO e IMPORTE TOTAL debajo) +
indicador ESTABLE → **Nombre de sector** y **Número de piscina** → **objetivos "malla" /
"BIN"** → **último pesaje guardado** → **peso total acumulado**. El historial completo
queda plegado bajo **"Historial"**; ahí ya no se muestra info técnica (bus/WiFi/RSSI,
enlaces a `/config` y `/sniffer`), que sigue disponible entrando a esas rutas directo.

- **Sector y piscina**: dos campos de texto que el operario rellena al empezar. Se guardan
  en la placa (NVS), se ven igual desde cualquier móvil y sobreviven a reinicios y a
  *Borrar todo*. Son datos de la **jornada**, no de cada pesaje: el CSV repite el valor que
  haya en el momento de exportar.
- **Objetivos malla / BIN**: cada tarjeta muestra el avance del grupo en curso
  (`acumulado / objetivo · faltan X lb`). Al cerrarse un grupo sale un **pop-up** grande
  con el total de esa malla/BIN, más pitido y vibración (un solo aviso, el del BIN, si
  cierran los dos a la vez).
- **Historial agrupado**: las filas van bajo cabeceras `BIN n` y `Malla m` con su
  subtotal. Cada fila se **borra deslizándola** hacia la izquierda (`GET /weighings/del?id=N`);
  el `#` se renumera sin huecos.
- Botón **Borrar todo** → tira de confirmación (Cancelar / Sí, borrar). También
  `GET/POST /weighings/clear`.
- **`/weighings.csv`**: descarga el historial ordenado del pesaje **1 al n** (del más
  antiguo al más reciente) con las columnas
  `n;peso;precio_unit;total;malla;bin;hora_local;sector;piscina`. En el panel,
  **"Copiar CSV"** lo pone en el portapapeles (útil en iPhone, donde la ventanita
  automática de la red no deja descargar archivos — para el archivo, abre Safari y entra
  a `192.168.4.1`).
- **Aviso de SIN SEÑAL**: el ESP atiende a un solo cliente a la vez, así que si el móvil
  tiene poca señal las peticiones se atascan y la pantalla se quedaba con el último valor
  como si fuera actual. Ahora cada petición del panel tiene un límite de 2 s, nunca hay dos
  iguales a la vez, y si pasan ~4 s sin una respuesta buena sale una **franja roja grande
  "SIN SEÑAL"** y la pantalla LED se apaga/atenúa (los números que se ven ya no son de
  fiar). En cuanto vuelve a contestar, se quita sola. El historial solo se pide al ESP
  cuando está **abierto** (al abrirlo, al entrar un pesaje nuevo y cada ~7 s), no todo el
  rato.
- Estilo *glassmorphism*; botón sol/luna arriba a la derecha para **modo claro / oscuro**
  (se recuerda en el navegador). Sin fuentes web ni librerías — funciona sin internet.
- Cada pesaje guarda los **3 campos**: `peso`, `precio unitario`, `total` (el precio/total
  tal como los mostraba la balanza al estabilizarse el peso).
- El panel muestra **lb**. Los umbrales `WEIGH_*` se interpretan en esa misma unidad.
- **El historial se guarda en dos niveles** y sobrevive a apagones / cambio de batería:
  - un **buffer de los últimos `WEIGH_LOG_SIZE` (120) pesajes** en NVS: es lo que muestra el
    panel y lo que se puede borrar fila a fila. Al reiniciar se recarga (`historial cargado
    de NVS: N`).
  - un **archivo permanente** en flash (LittleFS, partición `spiffs`, `histarch.cpp`) con
    **todos** los pesajes hasta que se pulse *Borrar todo*. Es de solo-añadir y se escribe en
    lotes de 8 pesajes (`HIST_BATCH`) para no desgastar la flash; entre lote y lote los
    pesajes ya están a salvo en el buffer y, si se apaga antes, se archivan al reiniciar.
    La capacidad depende de la partición SPIFFS elegida (~4 000 pesajes con *Minimal
    SPIFFS*, ~61 000 con *No OTA 2MB/2MB*; ver sección 2); al llegar al 85 % el panel avisa
    para que descargues el CSV. Si LittleFS no monta o el archivo se llena, todo sigue como
    antes con el buffer (el panel lo avisa).
  - El **CSV** trae siempre todo (archivo + buffer). El **peso total acumulado**, el nº de
    muestras y los **subtotales de BIN/malla** del panel se calculan sobre todo el
    historial, no solo sobre lo que se ve. El panel muestra las últimas 30 filas.
- `weighOnCommit(cb)` en `weighlog.h`: gancho para el **envío a un servidor** (pendiente) —
  salta 1 vez al confirmarse cada pesaje, con los 3 campos.

## 4. Rutas del servidor

| Ruta | | Qué hace |
|---|---|---|
| `/` | | panel (se adapta a lo que esté activo) |
| `/api` | | JSON estado en vivo (incluye acumulados y objetivos de malla/BIN, sector, piscina y el estado del archivo permanente `hist`) |
| `/weighings` · `/weighings.csv` | | historial: JSON de las últimas 30 filas con los subtotales de sus mallas/BIN (`grp`, `bins`) / CSV con **todo** el historial |
| `/weighings/del?id=N` | | borra una fila |
| `/weighings/clear` | | borra el historial (RAM + NVS); conserva objetivos, sector y piscina |
| `/targets?malla=200&bin=800` | | pone los objetivos en lb (`0` lo desactiva; si falta un parámetro, ese no cambia) |
| `/info?sector=A&piscina=12` | | pone sector / piscina (vacío los borra; si falta un parámetro, ese no cambia) |
| `/settime?epoch=…` | | el móvil le pasa la hora al ESP (sin internet) |
| `/log` · `/log?dl=1` | | registro de diagnóstico en texto (ver en pantalla / descargar). Enlace "Diagnóstico" al pie del panel |
| `/config` | | calibración OCR y/o mapa del sniffer (se guarda en NVS) |
| `/snapshot.jpg` · `/ocr_debug.jpg` | OCR | foto actual / con las zonas dibujadas |
| `/sniffer` · `/sniffer/raw` | SNIFFER | 16 bytes de RAM / volcado de flancos |

---

## 5. Puesta a punto del OCR (solo si `ENABLE_OCR 1`)

Por defecto va en **modo automático** (`auto=1`): **no hay que encuadrar nada**.
Cada frame (5×/s) localiza la **banda** de brillo (los LEDs sobre negro) y el
**borde derecho del último LED** (= borde del dígito de las unidades, que
siempre está encendido). A partir de ahí monta la rejilla:
`ancho_digito = alto_banda × asp` , `separacion = ancho_digito × pit`.

**Robusto a que la cámara se mueva:**

- `asp` y `pit` son **proporciones**, no píxeles → si la cámara se acerca/aleja,
  todo escala solo (digitos más grandes → banda más alta → celdas más anchas).
  Los mismos `asp`/`pit` valen a cualquier distancia.
- Traslación (unos cm arriba/abajo/lados): la banda y el borde derecho se
  re-miden en cada frame; la rejilla los sigue.
- Vibración: suavizado que filtra el temblor pero **reengancha de golpe** si el
  salto supera el 20% del alto del display.
- Si `autoLocate` falla un instante (reflejo, alguien pasa), mantiene la última
  rejilla hasta 3 s.
- Rotación pequeña (±2–3°): la absorbe el promediado de cada segmento. Rotación
  grande necesitaría montaje más rígido.

En `/config` todo se ajusta **en vivo**. La línea de estado muestra la lectura
cruda (`cruda "130"`) o el valor, nº de celdas y umbrales. En la imagen:
rectángulo **tenue** = zona detectada; rectángulos **claros** = celdas.

- Celdas anchas/estrechas → `asp` (probado: **0.62**).
- Celdas juntas/separadas → `pit` (probado: **1.50**).
- Segmentos mal → `umbral seg` (0–1). Torcidos → `inclinacion`.
- **Guardar** para que persista al reinicio.

### Si confunde cifras (3/5, 6/8...)

Un segmento realmente encendido está brillante **a lo largo de todo el trazo**;
el "sangrado" de un segmento vecino solo ilumina una punta. Por eso el segmento
"encendido" se decide por **cobertura** (`OCR_SEG_COVERAGE` % del trazo por
encima del umbral), no por brillo medio. Ajustes:

- Sigue confundiendo → **enfoca la lente** de la OV2640 (gírala). Con la imagen
  borrosa, un 5 y un 6 se parecen demasiado y ningún algoritmo lo arregla.
- `OCR_SEG_COVERAGE` (`config.h`): baja a ~45 si se deja segmentos sin detectar,
  sube a ~65 si detecta de más.
- `umbral seg` (`onR` en `/config`).

> `OCR_USE_TEMPLATES` (config.h, por defecto `0`): reconocimiento por forma
> (plantillas aprendidas). Experimental — muy sensible a que la rejilla auto se
> mueva. Se entrena en `/config` → "Plantillas". Déjalo en `0` salvo pruebas.

### Luz ambiente

Con el display a oscuras funciona directo. **Con luz de sala** el bisel blanco
brilla como los LEDs; el detector lo descarta por su forma, pero si aún confunde
pon **`brillo ON`** (`abr` en `/config`, o `OCR_AUTO_BRIGHT` en `config.h`) a
**~245** → solo cuentan los LEDs. En caja fija se pone una vez.

**Si el modo auto no engancha:** `auto=0` + caja manual (botones ←→↑↓).

---

## 6. Sniffer del bus

En `config.h`: `#define ENABLE_SNIFFER 1` (y `ENABLE_OCR 0`; los dos a la vez solo si
tienes cámara y quieres compararlos). Cablea:

| Balanza (conector DIS) | | ESP32-S3 |
|---|---|---|
| `SL` | `— [470 Ω] —` | `GPIO1` |
| `DA` | `— [470 Ω] — [diodo Schottky] —` | `GPIO4` |
| `GND` | `———` | `GND` (masa común **obligatoria**) |

No conectes `VCC_DIS`. Balanza con su batería, placa por USB-C. Ver la sección
2 (pines del sniffer) para el detalle del diodo en la línea DA.

### Comprobar que capta

Abre `/sniffer`. Arriba hay una **réplica de la pantalla LED** (los 3 displays
con 7 segmentos) que debe reflejar lo que muestra la balanza. Debajo:
`tramas` subiendo, `SL ~X kHz media / Y kHz pico`. Si `Y` (pico) sale muy alto
(>300 kHz) pero baja al pulsar **"Reiniciar medida SL"** y "pulsos rápidos" se
queda en 0, era ruido.

Mapa por defecto (`config.h`): bytes **0-4 = PESO**, **5-9 = PRECIO UNITARIO**,
**10-15 = TOTAL**, segmentos estándar (`a`=bit0 … `g`=bit6, punto=bit7). Si tu
balanza coincide (probado con la `ACS-JC36CV28`), **no hay que resolver nada**.
El detalle hex/binario está en `/sniffer` → *"2. RAM del display (detalle
técnico)"*.

### Si marca `0 tramas / 0 refrescos / SL kHz` vacío

Cero flancos de reloj = problema **eléctrico**, no de software. En `/sniffer`,
sección **"0. Diagnóstico de cableado"**:

- Con la balanza encendida y el número cambiando (pon la mano en el plato), los
  contadores `flancos=` de SL y DA tienen que **subir**. Si siguen en 0:
- Pulsa **"Probar pines"**:
  - `FLOTANTE` → ese cable no llega a nada: pad equivocado, soldadura fría o
    hilo roto.
  - `ALTO fijo` → conectado y en reposo (bien) — el problema es que el bus no
    tiene tráfico (¿balanza en standby? ¿display apagado?).
  - `BAJO fijo` → cable a masa o a un segmento.
- Comprueba: **masa común** (multímetro en continuidad entre `GND` del ESP32 y
  `GND` de la balanza), resistencias de **470 Ω** (amarillo-violeta-**marrón**,
  no 470 k), y el pad correcto en el conector de la placa de display
  (`DIN`/`CLK`, a veces `SDA`/`SCL` o `DA`/`SL`). **No puentees la resistencia ni el
  diodo** para "descartarlos": son lo que protege al GPIO y al bus de la balanza.

### Si la balanza se descontrola al conectar DA

Si al conectar el cable de DA los LEDs de la balanza parpadean o la pantalla se apaga, el
ESP está **cargando el bus** en vez de solo escucharlo. Con todo apagado, mide con el
multímetro entre cada GPIO usado y los pines `5V` / `3V3` / `GND` del ESP: no debe haber
continuidad ni una lectura de unos pocos mV. Si la hay, ese pin (o el módulo) está dañado;
el diodo Schottky en DA contiene el problema, pero lo correcto es cambiar de pin o de
módulo.

### Si el mapa se descuadra (dígitos que faltan, PRECIO/TOTAL en blanco…)

El mapa `SNIF_*_ADDR` de `config.h` vale para la `ACS-JC36CV28`. Si un
**"Resolver mapa"** salió mal, quedó guardado en NVS y pisa al de `config.h`.
Arreglo:

- **Botón "Restaurar mapa por defecto"** en `/sniffer` → borra el de NVS (solo el mapa
  del sniffer; no toca el historial ni los objetivos).
- O sube `SNIF_MAP_VERSION` en `config.h` y reflashea → se borra solo.
- La línea `mapa PESO[…] PRECIO[…] TOTAL[…]` de `/sniffer` muestra el que hay.

### Nº de decimales (`SNIF_*_DEC` en `config.h`)

- **`>= 0`** → número FIJO de decimales, **ignorando el bit de punto del
  display**. Así un icono NET/TARA que encienda ese bit al tarar no descoloca
  el valor (`-10.00` y `1.60` salen bien, no `1000` ni `160`).
- **`-1`** → los decimales los marca el punto del propio display (para campos
  que pueden ser enteros *o* decimales).

`ACS-JC36CV28`: `SNIF_PESO_DEC 2`, `SNIF_TOTAL_DEC 2`, `SNIF_PRECIO_DEC -1`
(el precio puede ser `65` o `1.50`).

### Resolver el mapa (automático) — solo si tu balanza no coincide

En `/sniffer`, sección **"Resolver el mapa"**: pon un peso, mira qué marca,
escríbelo, **Capturar**; repite con 3–4 pesos MUY distintos; **Resolver mapa**.

---

## 7. Ficheros

| Fichero | Contenido |
|---|---|
| `peso_balanza_cam.ino` | setup/loop, WiFi, portal cautivo, recuperación del AP, orquestación |
| `config.example.h` | plantilla de configuración con claves de ejemplo → copiar a `config.h` |
| `config.h` | **todo lo ajustable**: modo, WiFi, TZ, pines y mapa del sniffer, umbrales (no se versiona) |
| `seg7.h` | tabla de 7 segmentos (compartida) |
| `weighlog.h/.cpp` | máquina de estados del pesaje + buffer de los últimos pesajes + agrupación malla/BIN (NVS) |
| `histarch.h/.cpp` | archivo permanente con **todos** los pesajes (LittleFS, solo-añadir) |
| `diaglog.h/.cpp` | registro de diagnóstico en RAM (`/log`) |
| `ocr7seg.h/.cpp` | cámara + análisis de segmentos (solo si `ENABLE_OCR`) |
| `sniffer_tm1640.h/.cpp` | ISR del bus DA/SL + TM1640 (solo si `ENABLE_SNIFFER`) |
| `web_ui.h/.cpp` | servidor web y páginas |

## 8. Notas y problemas conocidos

- **Latido por Serial** (cada 5 s): `[hb] BUS valid… pesaje:… heap:libre(min …) APcli:N
  APrec:N`. `heap min` no debe bajar de forma sostenida; `APrec` debe quedarse en 0. El
  `rssi` sale siempre **0** en modo AP (mide el enlace como cliente, que no existe): para
  el alcance usa el indicador de señal del móvil.
- **Registro de diagnóstico `/log`** (pensado para pruebas de campo, sin cable ni Monitor
  Serie): abre `http://192.168.4.1/log` (o el enlace "Diagnóstico" al pie del panel; añade
  `?dl=1` para descargarlo como `balanza-log.txt`). Vive en **RAM** (~16 KB, últimas 256
  líneas): no escribe en flash ni usa el Serie, anotar una línea cuesta microsegundos y solo
  se hace en **eventos**, nunca por muestra → no ralentiza nada. **Se borra al apagar:
  descárgalo antes de desconectar la batería.** Abre el panel una vez tras encender para
  que el móvil ponga la hora (si no, las líneas llevan `+123s` = segundos desde el
  arranque). `DIAG_LOG 0` en `config.h` lo desactiva. Qué anota:
  - `ARRANQUE motivo=…`: por qué se reinició (`CAIDA-DE-TENSION` = brownout de la
    alimentación; `PANIC`/`WDT` = fallo del programa; `ENCENDIDO` = conexión normal).
  - `OK 41.3 r carga=1780 1a=520 n=31 rech=0`: pesaje guardado (peso; `r` retiro / `c`
    cambio de gaveta / `d` display cerrado; ms que estuvo puesta; ms hasta el 1er valor
    asentado; nº de lecturas asentadas; cuántas cayeron fuera de banda).
  - `PERDIDO(neg|cambio|cero|display) carga=… max=… min=…`: una gaveta estuvo puesta y **no
    llegó a asentar** (no se guardó). Con `carga` (ms puesta) y `max`/`min` se ve si fue
    por poco tiempo o por un peso que no paraba de moverse → sirve para afinar
    `WEIGH_WIN_MS` / `WEIGH_BAND`.
  - `TARA …` / `DESCARTE …`: tara o cambio descartado a propósito (no es una pérdida).
  - `WIFI + cliente AB:CD` / `WIFI - cliente AB:CD motivo=N`: un móvil entra o sale de la
    red (`8` se fue él · `3` deauth · `4` inactividad · `2` autenticación expirada · `15`
    fallo de handshake). Si un móvil sale y vuelve a entrar en el mismo instante, lo más
    probable es que lo haya provocado su sistema (Android a veces suelta una WiFi "sin
    internet"), no el ESP.
  - `ST cli=1 rssi[-63 ] heap=… fps=…` cada ~2 min: nº de móviles, **señal** de cada uno
    (dBm; peor que ~-80 es mala), memoria libre / mínima y refrescos por segundo del display.
  - `BUCLE lento 143 ms (web 138 ms)`: una vuelta del programa tardó ≥ 80 ms y cuánto de eso
    fue el servidor web; si el peso se pierde justo ahí, la causa es esa espera.
  - Cabecera con contadores (pesajes guardados / perdidos / taras, conexiones, vueltas
    lentas, la más lenta).
- **Alimentación y "SIN SEÑAL" en campo**: si el panel se queda congelado o se cae la red,
  comprueba primero el condensador de salida (~470 µF + 100 nF junto al pin `3V3`, ver
  sección 2) y el `motivo=` del último `ARRANQUE` en `/log`: si dice `CAIDA-DE-TENSION`, es
  la alimentación.
- **"waiting for download" en el Monitor Serie**: el chip arrancó en modo descarga porque
  `GPIO0` (botón BOOT) estaba en bajo al encender o resetear, así que no corre tu programa.
  Apaga y enciende **sin tocar BOOT**. Si algo lo aprieta (la caja, un cable, humedad),
  quítalo o cubre el botón. Solo se lee en el arranque; con el programa corriendo, BOOT no
  hace nada.
- **El banner de la red a veces no sale en el Monitor Serie** (con el USB nativo, el
  inicio del log puede perderse al resetear). No significa que la red no exista: míralo en
  la lista WiFi del móvil.
- **Descargar el CSV** recorre todo el historial y ocupa al ESP unos instantes (más cuantos
  más pesajes haya): no lo descargues mientras se está pesando.
- **La red desaparece con la caja cerrada**: ver *Alimentación y montaje* (sección 2).
- Si la placa se reinicia al conectar la WiFi (brownout), aliméntala por un
  USB-C con buena corriente.
- Con los dos métodos activos, si el bus se decodifica con basura pon
  `OCR_PAUSE_SNIFFER 1` en `config.h`.
- El sniffer es de solo lectura y no perturba el bus, siempre que el cableado sea el de la
  sección 2 (resistencia + diodo en DA).
